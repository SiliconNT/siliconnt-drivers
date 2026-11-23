/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/rtkit.c; see LICENSE.m1n1.
 * Changed for SiliconNT: SART only (no DART, no SRAM), no device tree, memory through
 * the environment, and boot and shutdown are steps of a state machine driven by the
 * messages, so the core can take the messages from an interrupt handler and answer
 * them later. See SiliconNT findings/0072.
 */

#include "ans_internal.h"

#define RTKIT_EP_MGMT     0
#define RTKIT_EP_CRASHLOG 1
#define RTKIT_EP_SYSLOG   2
#define RTKIT_EP_DEBUG    3
#define RTKIT_EP_IOREPORT 4
#define RTKIT_EP_OSLOG    8

#define MGMT_TYPE GENMASK(59, 52)

#define MGMT_PWR_STATE GENMASK(15, 0)

#define MSG_BUFFER_REQUEST      1
#define MSG_BUFFER_REQUEST_SIZE GENMASK(51, 44)
#define MSG_BUFFER_REQUEST_IOVA GENMASK(41, 0)

#define MSG_SYSLOG_INIT           8
#define MSG_SYSLOG_INIT_ENTRYSIZE GENMASK(39, 24)
#define MSG_SYSLOG_INIT_COUNT     GENMASK(15, 0)
#define MSG_SYSLOG_LOG            5

#define MGMT_MSG_HELLO        1
#define MGMT_MSG_HELLO_ACK    2
#define MGMT_MSG_HELLO_MINVER GENMASK(15, 0)
#define MGMT_MSG_HELLO_MAXVER GENMASK(31, 16)

#define MGMT_MSG_IOP_PWR_STATE     6
#define MGMT_MSG_IOP_PWR_STATE_ACK 7

#define MGMT_MSG_EPMAP        8
#define MGMT_MSG_EPMAP_DONE   BIT(51)
#define MGMT_MSG_EPMAP_BASE   GENMASK(34, 32)
#define MGMT_MSG_EPMAP_BITMAP GENMASK(31, 0)

#define MGMT_MSG_EPMAP_REPLY      8
#define MGMT_MSG_EPMAP_REPLY_DONE BIT(51)
#define MGMT_MSG_EPMAP_REPLY_MORE BIT(0)

#define MGMT_MSG_AP_PWR_STATE     0xb
#define MGMT_MSG_AP_PWR_STATE_ACK 0xb

#define MGMT_MSG_START_EP      5
#define MGMT_MSG_START_EP_IDX  GENMASK(39, 32)
#define MGMT_MSG_START_EP_FLAG BIT(1)

#define RTKIT_MIN_VERSION 11
#define RTKIT_MAX_VERSION 12

enum rtkit_power_state {
    RTKIT_POWER_OFF = 0x00,
    RTKIT_POWER_SLEEP = 0x01,
    RTKIT_POWER_QUIESCED = 0x10,
    RTKIT_POWER_ON = 0x20,
    RTKIT_POWER_INIT = 0x220,
};

/* 'CLHE' and 'Cstr' as m1n1's multi-character constants evaluate with GCC. */
#define CRASHLOG_TYPE_CLHE 0x434c4845u
#define CRASHLOG_TYPE_CSTR 0x43737472u

/* Crash log layout: a 32-byte header, then entries of a 16-byte head and a payload. */
#define CRASHLOG_HDR_SIZE       32
#define CRASHLOG_ENTRY_HDR_SIZE 16
#define CRASHLOG_MAX_ENTRIES    64
#define CRASHLOG_MAX_STRING     120

#define rtkit_log(core, ...) ans_log((core), "rtkit: " __VA_ARGS__)

void ans_rtkit_init(struct ans_core *core)
{
    struct ans_rtkit *rtk = &core->rtkit;

    rtk->phase = RTKIT_OFF;
    rtk->iop_power = RTKIT_POWER_OFF;
    rtk->ap_power = RTKIT_POWER_OFF;
    rtk->endpoints = 0;
    rtk->syslog_count = rtk->syslog_size = 0;
    for (int i = 0; i < RTKIT_BUF_COUNT; i++)
        rtk->buf[i].held = false;
}

static bool rtkit_send(struct ans_core *core, uint8_t ep, uint64_t msg0)
{
    struct asc_message msg;

    msg.msg0 = msg0;
    msg.msg1 = ep;
    return ans_asc_send(core, &msg);
}

static void rtkit_fail(struct ans_core *core, const char *why)
{
    rtkit_log(core, "%s", why);
    core->rtkit.phase = RTKIT_FAILED;
}

static void rtkit_free_buffer(struct ans_core *core, struct rtkit_buffer *bfr)
{
    struct ans_dma dma = bfr->dma;

    if (!bfr->held)
        return;
    /* first out of the core's record, then out of the SART, then back to the environment */
    bfr->held = false;
    if (!ans_sart_remove(core, dma.dva, dma.size))
        rtkit_log(core, "sart_remove_allowed_region failed (%016llx, 0x%llx)",
                  (unsigned long long)dma.dva, (unsigned long long)dma.size);
    ans_env_dma_free(core->env, &dma);
}

/* Gives back every buffer the coprocessor was given. */
void ans_rtkit_free(struct ans_core *core)
{
    for (int i = 0; i < RTKIT_BUF_COUNT; i++)
        rtkit_free_buffer(core, &core->rtkit.buf[i]);
}

/*
 * The coprocessor asks for a buffer: allocate it, let the coprocessor reach it through
 * the SART, and tell it the address. Buffers at an address the coprocessor chose
 * itself are refused (m1n1 translated those through a DART, which this controller does
 * not have). Allocation is only allowed inside ans_start() and ans_stop(). A request
 * the core cannot answer is counted and fails the conversation: left unanswered, the
 * coprocessor may hang.
 */
static void rtkit_refuse(struct ans_core *core, uint8_t ep, const char *why)
{
    core->stats.msgs_refused++;
    rtkit_log(core, "ep 0x%x: %s; not answered, giving up", ep, why);
    core->rtkit.phase = RTKIT_FAILED;
}

static void rtkit_handle_buffer_request(struct ans_core *core, uint8_t ep, uint64_t msg0,
                                        struct rtkit_buffer *bfr, bool can_alloc)
{
    uint64_t n_4kpages = FIELD_GET(MSG_BUFFER_REQUEST_SIZE, msg0);
    uint64_t addr = FIELD_GET(MSG_BUFFER_REQUEST_IOVA, msg0);
    uint64_t sz = ALIGN_UP(n_4kpages << 12, 0x4000ull);

    if (addr) {
        rtkit_log(core, "ep 0x%x asked for a buffer at its own address %016llx", ep,
                  (unsigned long long)addr);
        rtkit_refuse(core, ep, "a buffer at its own address");
        return;
    }
    if (bfr->held) {
        rtkit_refuse(core, ep, "asked for a second buffer");
        return;
    }
    if (!can_alloc) {
        rtkit_log(core, "ep 0x%x asked for a buffer of 0x%llx bytes", ep,
                  (unsigned long long)sz);
        rtkit_refuse(core, ep, "a buffer while running");
        return;
    }
    if (n_4kpages == 0) {
        rtkit_fail(core, "buffer request of size 0");
        return;
    }

    if (!ans_env_dma_alloc(core->env, (size_t)sz, &bfr->dma)) {
        rtkit_log(core, "unable to allocate 0x%llx byte buffer", (unsigned long long)sz);
        core->rtkit.phase = RTKIT_FAILED;
        return;
    }
    if (bfr->dma.dva & ~MSG_BUFFER_REQUEST_IOVA) {
        rtkit_log(core, "buffer at %016llx is out of the coprocessor's reach",
                  (unsigned long long)bfr->dma.dva);
        ans_env_dma_free(core->env, &bfr->dma);
        core->rtkit.phase = RTKIT_FAILED;
        return;
    }
    if (!ans_sart_add(core, bfr->dma.dva, sz)) {
        rtkit_log(core, "sart_add_allowed_region failed (%016llx, 0x%llx)",
                  (unsigned long long)bfr->dma.dva, (unsigned long long)sz);
        ans_env_dma_free(core->env, &bfr->dma);
        core->rtkit.phase = RTKIT_FAILED;
        return;
    }
    bfr->held = true;

    uint64_t reply = FIELD_PREP(MGMT_TYPE, MSG_BUFFER_REQUEST);
    reply |= FIELD_PREP(MSG_BUFFER_REQUEST_SIZE, n_4kpages);
    reply |= FIELD_PREP(MSG_BUFFER_REQUEST_IOVA, bfr->dma.dva);
    if (!rtkit_send(core, ep, reply)) {
        rtkit_free_buffer(core, bfr);
        rtkit_fail(core, "unable to send buffer reply");
    }
}

/* Logs what the coprocessor wrote into its crash log buffer. */
static void rtkit_crashed(struct ans_core *core)
{
    const struct ans_dma *dma = &core->rtkit.buf[RTKIT_BUF_CRASHLOG].dma;
    size_t size = dma->size;

    core->rtkit.phase = RTKIT_CRASHED;
    rtkit_log(core, "IOP crashed!");

    uint32_t type = ans_dma_read32(core, dma, 0);
    if (type != CRASHLOG_TYPE_CLHE) {
        rtkit_log(core, "bad crashlog header 0x%x", type);
        return;
    }

    rtkit_log(core, "== CRASH INFO ==");
    size_t p = CRASHLOG_HDR_SIZE;
    for (int n = 0; n < CRASHLOG_MAX_ENTRIES; n++) {
        if (p + CRASHLOG_ENTRY_HDR_SIZE > size || (p & 3)) {
            rtkit_log(core, "  crashlog entry at 0x%llx outside the buffer",
                      (unsigned long long)p);
            return;
        }
        type = ans_dma_read32(core, dma, p);
        if (type == CRASHLOG_TYPE_CLHE)
            return;
        uint32_t len = ans_dma_read32(core, dma, p + 12);

        if (type == CRASHLOG_TYPE_CSTR) {
            char text[CRASHLOG_MAX_STRING + 1];
            size_t i = 0;

            /* payload byte 0 is the message number, the text starts at payload byte 4 */
            while (i < CRASHLOG_MAX_STRING && p + 20 + i < size && 20 + i < len) {
                uint8_t c = ans_dma_read8(core, dma, p + 20 + i);
                if (!c)
                    break;
                text[i++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
            text[i] = '\0';
            rtkit_log(core, "  Message %u: %s", ans_dma_read8(core, dma, p + 16), text);
        } else {
            rtkit_log(core, "  0x%x", type);
        }

        if (len < CRASHLOG_ENTRY_HDR_SIZE) {
            rtkit_log(core, "  crashlog entry of length %u, stopping", len);
            return;
        }
        p += len;
    }
}

/* Starts the conversation: lets the coprocessor run and wakes it up. */
bool ans_rtkit_boot(struct ans_core *core)
{
    struct ans_rtkit *rtk = &core->rtkit;

    rtk->phase = RTKIT_WAIT_HELLO;
    ans_asc_cpu_start(core);
    /* can be sent unconditionally to wake up a possibly sleeping IOP */
    if (!rtkit_send(core, RTKIT_EP_MGMT,
                    FIELD_PREP(MGMT_TYPE, MGMT_MSG_IOP_PWR_STATE) |
                        FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_INIT))) {
        rtkit_fail(core, "unable to send wakeup message");
        return false;
    }
    return true;
}

/*
 * Starts putting the coprocessor to sleep. AP power must always go to QUIESCED
 * first, otherwise rebooting doesn't work.
 */
bool ans_rtkit_sleep(struct ans_core *core)
{
    struct ans_rtkit *rtk = &core->rtkit;

    if (rtk->phase != RTKIT_RUNNING)
        return false;

    rtk->phase = RTKIT_WAIT_QUIESCED;
    if (!rtkit_send(core, RTKIT_EP_MGMT,
                    FIELD_PREP(MGMT_TYPE, MGMT_MSG_AP_PWR_STATE) |
                        FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_QUIESCED))) {
        rtkit_fail(core, "unable to send shutdown message");
        return false;
    }
    return true;
}

static void rtkit_handle_hello(struct ans_core *core, const struct asc_message *msg)
{
    if (msg->msg1 != RTKIT_EP_MGMT) {
        rtkit_log(core, "expected HELLO but got message for EP 0x%x", msg->msg1);
        core->rtkit.phase = RTKIT_FAILED;
        return;
    }

    uint32_t msgtype = (uint32_t)FIELD_GET(MGMT_TYPE, msg->msg0);
    if (msgtype != MGMT_MSG_HELLO) {
        rtkit_log(core, "expected HELLO but got message with type 0x%02x", msgtype);
        core->rtkit.phase = RTKIT_FAILED;
        return;
    }

    uint32_t min_ver = (uint32_t)FIELD_GET(MGMT_MSG_HELLO_MINVER, msg->msg0);
    uint32_t max_ver = (uint32_t)FIELD_GET(MGMT_MSG_HELLO_MAXVER, msg->msg0);
    uint32_t want_ver = max_ver < RTKIT_MAX_VERSION ? max_ver : RTKIT_MAX_VERSION;

    if (min_ver > RTKIT_MAX_VERSION || max_ver < RTKIT_MIN_VERSION) {
        rtkit_log(core, "supported versions [%d,%d] must overlap versions [%u,%u]",
                  RTKIT_MIN_VERSION, RTKIT_MAX_VERSION, min_ver, max_ver);
        core->rtkit.phase = RTKIT_FAILED;
        return;
    }

    rtkit_log(core, "booting with version %u", want_ver);

    if (!rtkit_send(core, RTKIT_EP_MGMT,
                    FIELD_PREP(MGMT_TYPE, MGMT_MSG_HELLO_ACK) |
                        FIELD_PREP(MGMT_MSG_HELLO_MINVER, want_ver) |
                        FIELD_PREP(MGMT_MSG_HELLO_MAXVER, want_ver))) {
        rtkit_fail(core, "couldn't send HELLO ack");
        return;
    }
    core->rtkit.phase = RTKIT_WAIT_EPMAP;
}

static bool rtkit_start_ep(struct ans_core *core, uint8_t ep)
{
    if (!rtkit_send(core, RTKIT_EP_MGMT,
                    FIELD_PREP(MGMT_TYPE, MGMT_MSG_START_EP) | MGMT_MSG_START_EP_FLAG |
                        FIELD_PREP(MGMT_MSG_START_EP_IDX, ep))) {
        rtkit_log(core, "unable to start endpoint 0x%02x", ep);
        return false;
    }
    return true;
}

static void rtkit_handle_epmap(struct ans_core *core, const struct asc_message *msg)
{
    struct ans_rtkit *rtk = &core->rtkit;

    if (msg->msg1 != RTKIT_EP_MGMT) {
        rtkit_log(core, "expected management message while waiting for endpoint map but got "
                        "message for endpoint 0x%x",
                  msg->msg1);
        rtk->phase = RTKIT_FAILED;
        return;
    }

    uint32_t msgtype = (uint32_t)FIELD_GET(MGMT_TYPE, msg->msg0);
    if (msgtype != MGMT_MSG_EPMAP) {
        rtkit_log(core, "expected endpoint map message but got 0x%x instead", msgtype);
        rtk->phase = RTKIT_FAILED;
        return;
    }

    uint32_t bitmap = (uint32_t)FIELD_GET(MGMT_MSG_EPMAP_BITMAP, msg->msg0);
    uint32_t base = (uint32_t)FIELD_GET(MGMT_MSG_EPMAP_BASE, msg->msg0);
    for (unsigned int i = 0; i < 32; i++) {
        if (!(bitmap & (1U << i)))
            continue;
        uint32_t ep_idx = 32 * base + i;
        if (ep_idx >= 0x20)
            continue;
        switch (ep_idx) {
            case RTKIT_EP_MGMT:
            case RTKIT_EP_CRASHLOG:
            case RTKIT_EP_DEBUG:
            case RTKIT_EP_IOREPORT:
            case RTKIT_EP_SYSLOG:
            case RTKIT_EP_OSLOG:
                rtk->endpoints |= 1u << ep_idx;
                break;
            default:
                rtkit_log(core, "unknown system endpoint 0x%02x", ep_idx);
        }
    }

    bool done = (msg->msg0 & MGMT_MSG_EPMAP_DONE) != 0;
    uint64_t reply = FIELD_PREP(MGMT_TYPE, MGMT_MSG_EPMAP_REPLY) |
                     FIELD_PREP(MGMT_MSG_EPMAP_BASE, base);
    reply |= done ? MGMT_MSG_EPMAP_REPLY_DONE : MGMT_MSG_EPMAP_REPLY_MORE;
    if (!rtkit_send(core, RTKIT_EP_MGMT, reply)) {
        rtkit_fail(core, "couldn't reply to endpoint map");
        return;
    }
    if (!done)
        return;

    /* start all required system endpoints, in m1n1's order */
    static const uint8_t order[] = {RTKIT_EP_DEBUG, RTKIT_EP_CRASHLOG, RTKIT_EP_SYSLOG,
                                    RTKIT_EP_IOREPORT, RTKIT_EP_OSLOG};
    for (unsigned int i = 0; i < sizeof(order); i++) {
        if ((rtk->endpoints & (1u << order[i])) && !rtkit_start_ep(core, order[i])) {
            rtk->phase = RTKIT_FAILED;
            return;
        }
    }
    rtk->phase = RTKIT_WAIT_POWER_ON;
}

static void rtkit_handle_mgmt(struct ans_core *core, const struct asc_message *msg)
{
    struct ans_rtkit *rtk = &core->rtkit;
    uint32_t msgtype = (uint32_t)FIELD_GET(MGMT_TYPE, msg->msg0);

    switch (msgtype) {
        case MGMT_MSG_IOP_PWR_STATE_ACK:
            rtk->iop_power = (uint32_t)FIELD_GET(MGMT_PWR_STATE, msg->msg0);
            if (rtk->phase == RTKIT_WAIT_POWER_ON && rtk->iop_power == RTKIT_POWER_ON) {
                /* this enables syslog */
                if (!rtkit_send(core, RTKIT_EP_MGMT,
                                FIELD_PREP(MGMT_TYPE, MGMT_MSG_AP_PWR_STATE) |
                                    FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_ON))) {
                    rtkit_fail(core, "unable to send AP power message");
                    return;
                }
                rtk->phase = RTKIT_RUNNING;
            } else if (rtk->phase == RTKIT_WAIT_SLEEP && rtk->iop_power == RTKIT_POWER_SLEEP) {
                rtk->phase = RTKIT_ASLEEP;
            }
            break;
        case MGMT_MSG_AP_PWR_STATE_ACK:
            rtk->ap_power = (uint32_t)FIELD_GET(MGMT_PWR_STATE, msg->msg0);
            if (rtk->phase == RTKIT_WAIT_QUIESCED && rtk->ap_power == RTKIT_POWER_QUIESCED) {
                if (!rtkit_send(core, RTKIT_EP_MGMT,
                                FIELD_PREP(MGMT_TYPE, MGMT_MSG_IOP_PWR_STATE) |
                                    FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_SLEEP))) {
                    rtkit_fail(core, "unable to send shutdown message");
                    return;
                }
                rtk->phase = RTKIT_WAIT_SLEEP;
            }
            break;
        default:
            core->stats.msgs_ignored++;
            rtkit_log(core, "unknown management message %x", msgtype);
    }
}

/* One message, once the endpoint map is through (m1n1's rtkit_recv()). */
static void rtkit_handle_system(struct ans_core *core, const struct asc_message *msg,
                                bool can_alloc)
{
    struct ans_rtkit *rtk = &core->rtkit;
    uint8_t ep = (uint8_t)msg->msg1;
    uint32_t msgtype = (uint32_t)FIELD_GET(MGMT_TYPE, msg->msg0);

    switch (ep) {
        case RTKIT_EP_MGMT:
            rtkit_handle_mgmt(core, msg);
            break;
        case RTKIT_EP_SYSLOG:
            switch (msgtype) {
                case MSG_BUFFER_REQUEST:
                    rtkit_handle_buffer_request(core, ep, msg->msg0, &rtk->buf[RTKIT_BUF_SYSLOG],
                                                can_alloc);
                    break;
                case MSG_SYSLOG_INIT:
                    rtk->syslog_count = (uint32_t)FIELD_GET(MSG_SYSLOG_INIT_COUNT, msg->msg0);
                    rtk->syslog_size = (uint32_t)FIELD_GET(MSG_SYSLOG_INIT_ENTRYSIZE, msg->msg0);
                    break;
                case MSG_SYSLOG_LOG:
                    if (!ans_asc_send(core, msg))
                        rtkit_log(core, "failed to ack syslog");
                    break;
                default:
                    core->stats.msgs_ignored++;
                    rtkit_log(core, "unknown syslog message %x", msgtype);
            }
            break;
        case RTKIT_EP_CRASHLOG:
            switch (msgtype) {
                case MSG_BUFFER_REQUEST:
                    if (!rtk->buf[RTKIT_BUF_CRASHLOG].held)
                        rtkit_handle_buffer_request(core, ep, msg->msg0,
                                                    &rtk->buf[RTKIT_BUF_CRASHLOG], can_alloc);
                    else
                        rtkit_crashed(core);
                    break;
                default:
                    core->stats.msgs_ignored++;
                    rtkit_log(core, "unknown crashlog message %x", msgtype);
            }
            break;
        case RTKIT_EP_IOREPORT:
            switch (msgtype) {
                case MSG_BUFFER_REQUEST:
                    rtkit_handle_buffer_request(core, ep, msg->msg0,
                                                &rtk->buf[RTKIT_BUF_IOREPORT], can_alloc);
                    break;
                /* unknown but must be ACKed */
                case 0x8:
                case 0xc:
                    if (!ans_asc_send(core, msg))
                        rtkit_log(core, "unable to ACK unknown ioreport message");
                    break;
                default:
                    core->stats.msgs_ignored++;
                    rtkit_log(core, "unknown ioreport message %x", msgtype);
            }
            break;
        case RTKIT_EP_OSLOG:
            core->stats.msgs_ignored++;
            rtkit_log(core, "unknown oslog message %llx", (unsigned long long)msg->msg0);
            break;
        default:
            core->stats.msgs_ignored++;
            rtkit_log(core, "message to unknown system endpoint 0x%02x: %llx", ep,
                      (unsigned long long)msg->msg0);
    }
}

/* A message lost in either ring may have needed an answer: the conversation is over. */
static void rtkit_check_lost(struct ans_core *core)
{
    if (core->asc.lost && core->rtkit.phase != RTKIT_FAILED &&
        core->rtkit.phase != RTKIT_CRASHED && core->rtkit.phase != RTKIT_OFF)
        rtkit_fail(core, "mailbox messages were lost, giving up");
}

/*
 * Handles the messages waiting in the receive ring, in order. can_alloc is true only
 * when called from inside ans_start() or ans_stop().
 */
void ans_rtkit_service(struct ans_core *core, bool can_alloc)
{
    struct ans_rtkit *rtk = &core->rtkit;
    struct asc_message msg;

    rtkit_check_lost(core);
    while (rtk->phase != RTKIT_FAILED && rtk->phase != RTKIT_CRASHED &&
           ans_asc_recv(core, &msg)) {
        switch (rtk->phase) {
            case RTKIT_OFF:
            case RTKIT_ASLEEP:
                core->stats.msgs_ignored++;
                rtkit_log(core, "message while the IOP is not running: %llx ep 0x%x",
                          (unsigned long long)msg.msg0, msg.msg1);
                continue;
            case RTKIT_WAIT_HELLO:
                rtkit_handle_hello(core, &msg);
                continue;
            case RTKIT_WAIT_EPMAP:
                rtkit_handle_epmap(core, &msg);
                continue;
            default:
                break;
        }

        if (msg.msg1 >= 0x100) {
            core->stats.msgs_ignored++;
            rtkit_log(core, "WARNING: received message for invalid endpoint %x >= 0x100",
                      msg.msg1);
            continue;
        }
        if (msg.msg1 >= 0x20) {
            /* The disk controller's own endpoints say nothing the core needs. */
            core->stats.msgs_ignored++;
            rtkit_log(core, "unexpected message to non-system endpoint 0x%02x: %llx", msg.msg1,
                      (unsigned long long)msg.msg0);
            continue;
        }
        rtkit_handle_system(core, &msg, can_alloc);
    }
    rtkit_check_lost(core);
}
