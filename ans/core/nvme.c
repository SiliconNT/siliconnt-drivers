/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/nvme.c; see LICENSE.m1n1.
 * Changed for SiliconNT: no device tree and no heap, registers and memory through the
 * environment, many reads in flight completed from an interrupt handler, identify,
 * PRP lists, an NVMe command whitelist, the reset in one place, and the interface in
 * ans_core.h. The differences and why are in SiliconNT findings/0072.
 */

#include "ans_internal.h"

/* Timeouts in microseconds, as m1n1 has them. */
#define NVME_TIMEOUT          1000000
#define NVME_ENABLE_TIMEOUT   5000000
#define NVME_SHUTDOWN_TIMEOUT 5000000
#define RTKIT_TIMEOUT         1000000 /* m1n1: USEC_PER_SEC for HELLO and the endpoint map */
#define BOOT_STATUS_TIMEOUT   1000000

/* How long one wait for the coprocessor or the controller lasts. */
#define ANS_WAIT_STEP 50

#define NVME_QUEUE_SIZE 64

#define NVME_CC            0x14
#define NVME_CC_SHN        GENMASK(15, 14)
#define NVME_CC_SHN_NORMAL 1
#define NVME_CC_EN         BIT(0)

#define NVME_CSTS             0x1c
#define NVME_CSTS_SHST        GENMASK(3, 2)
#define NVME_CSTS_SHST_DONE   2
#define NVME_CSTS_RDY         BIT(0)

#define NVME_AQA 0x24
#define NVME_ASQ 0x28
#define NVME_ACQ 0x30

#define NVME_DB_ACQ  0x1004
#define NVME_DB_IOCQ 0x100c

#define NVME_BOOT_STATUS    0x1300
#define NVME_BOOT_STATUS_OK 0xde71ce55

#define NVME_LINEAR_SQ_CTRL    0x24908
#define NVME_LINEAR_SQ_CTRL_EN BIT(0)

#define NVME_IOQ_CMDS           0x1200
#define NVME_IOQ_CQES           0x1208
#define NVME_MAX_PEND_CMDS_CTRL 0x1210
#define NVME_DB_LINEAR_ASQ      0x2490c
#define NVME_DB_LINEAR_IOSQ     0x24910

#define NVMMU_NUM       0x28100
#define NVMMU_ASQ_BASE  0x28108
#define NVMMU_IOSQ_BASE 0x28110
#define NVMMU_TCB_INVAL 0x28118
#define NVMMU_TCB_STAT  0x28120 /* m1n1: 0x29120; Linux, OpenBSD, U-Boot: 0x28120 */

#define NVME_ADMIN_CMD_DELETE_SQ 0x00
#define NVME_ADMIN_CMD_CREATE_SQ 0x01
#define NVME_ADMIN_CMD_DELETE_CQ 0x04
#define NVME_ADMIN_CMD_CREATE_CQ 0x05
#define NVME_ADMIN_CMD_IDENTIFY  0x06
#define NVME_QUEUE_CONTIGUOUS    BIT(0)
#define NVME_CQ_IRQ_ENABLED      BIT(1)

#define NVME_ID_CNS_NS   0x00
#define NVME_ID_CNS_CTRL 0x01

#define NVME_CMD_READ 0x02

/* The only namespace the core reads. */
#define NVME_NSID 1

#define NVMMU_TCB_DMA_FROM_DEVICE BIT(0)
#define NVMMU_TCB_DMA_TO_DEVICE   BIT(1)

/* Sizes and offsets of the structures the controller reads and writes. */
#define NVME_CMD_SIZE      64
#define NVME_CQE_SIZE      16
#define NVMMU_TCB_SIZE     128
#define NVME_PAGE          4096u
#define NVME_PRP_LIST_ENTS (NVME_PAGE / 8)

/* struct nvme_command: opcode, flags, tag (u8; normal NVMe has a u16), rsvd, ... */
#define CMD_DW0   0x00
#define CMD_NSID  0x04
#define CMD_PRP1  0x18
#define CMD_PRP2  0x20
#define CMD_CDW10 0x28

/* struct nvme_completion: result (u64), rsvd (u32), tag (u16), status (u16) */
#define CQE_TAG_STATUS 0x0c

/* struct apple_nvmmu_tcb: opcode, dma_flags, slot_id, unk0, length (u16), ..., prp1, prp2 */
#define TCB_HEAD 0x00
#define TCB_PRP1 0x18
#define TCB_PRP2 0x20

/* ----------------------------------------------------------------------------- */
/* Device memory                                                                   */

/*
 * The environment may give up its lock inside ans_env_dma_alloc() and _free(), and
 * the interrupt handler may then run: the core's own record changes only before or
 * after, never half way.
 */
static bool alloc_dma(struct ans_core *core, struct ans_dma *dma, size_t size)
{
    struct ans_dma tmp;

    dma->va = NULL;
    if (!ans_env_dma_alloc(core->env, size, &tmp)) {
        ans_log(core, "nvme: cannot allocate 0x%llx bytes of device memory",
                (unsigned long long)size);
        return false;
    }
    if (!tmp.va || (tmp.dva & 0x3fff) || tmp.size < size) {
        ans_log(core, "nvme: environment gave device memory at %016llx that breaks the rules",
                (unsigned long long)tmp.dva);
        ans_env_dma_free(core->env, &tmp);
        return false;
    }
    *dma = tmp;
    return true;
}

static void free_dma(struct ans_core *core, struct ans_dma *dma)
{
    struct ans_dma tmp = *dma;

    if (!tmp.va)
        return;
    dma->va = NULL;
    ans_env_dma_free(core->env, &tmp);
}

static bool alloc_queue(struct ans_core *core, struct ans_nvme_queue_mem *q)
{
    q->cq_head = 0;
    q->cq_phase = 1;
    return alloc_dma(core, &q->tcbs, NVME_QUEUE_SIZE * NVMMU_TCB_SIZE) &&
           alloc_dma(core, &q->cmds, NVME_QUEUE_SIZE * NVME_CMD_SIZE) &&
           alloc_dma(core, &q->cqes, NVME_QUEUE_SIZE * NVME_CQE_SIZE);
}

static void free_queue(struct ans_core *core, struct ans_nvme_queue_mem *q)
{
    free_dma(core, &q->cmds);
    free_dma(core, &q->tcbs);
    free_dma(core, &q->cqes);
}

static void free_all_memory(struct ans_core *core)
{
    struct ans_nvme *nvme = &core->nvme;

    ans_rtkit_free(core);
    ans_sart_remove_all(core);
    free_queue(core, &nvme->q[ANS_NVME_IOQ]);
    free_queue(core, &nvme->q[ANS_NVME_ADMINQ]);
    free_dma(core, &nvme->prp);
    free_dma(core, &nvme->identify);
}

/* ----------------------------------------------------------------------------- */
/* Submitting commands                                                             */

/*
 * The whitelist (findings/0071 conclusion 22). Looked up by queue and opcode, because
 * the two queues number their opcodes separately. Nothing that writes, formats,
 * erases, changes features or firmware, and no vendor command can pass.
 */
bool ans_nvme_allowed(enum ans_nvme_queue queue, const struct ans_nvme_cmd *cmd)
{
    if (cmd->flags != 0)
        return false;

    if (queue == ANS_NVME_ADMINQ) {
        switch (cmd->opcode) {
            case NVME_ADMIN_CMD_CREATE_CQ: /* I/O completion queue 1 */
                return (cmd->cdw10 & 0xffff) == 1 && cmd->nsid == 0;
            case NVME_ADMIN_CMD_CREATE_SQ: /* I/O submission queue 1 on completion queue 1 */
                return (cmd->cdw10 & 0xffff) == 1 && (cmd->cdw11 >> 16) == 1 && cmd->nsid == 0;
            case NVME_ADMIN_CMD_IDENTIFY: /* the controller, or namespace 1 */
                return (cmd->cdw10 == NVME_ID_CNS_CTRL && cmd->nsid == 0) ||
                       (cmd->cdw10 == NVME_ID_CNS_NS && cmd->nsid == NVME_NSID);
            case NVME_ADMIN_CMD_DELETE_SQ: /* I/O submission queue 1 */
            case NVME_ADMIN_CMD_DELETE_CQ: /* I/O completion queue 1 */
                return cmd->cdw10 == 1 && cmd->nsid == 0;
            default:
                return false;
        }
    }

    if (queue == ANS_NVME_IOQ)
        return cmd->opcode == NVME_CMD_READ && cmd->nsid == NVME_NSID;

    return false;
}

/*
 * The only function that puts a command into a queue and rings a doorbell. A command
 * that is not on the whitelist does not reach the queue, the command table or the
 * doorbell.
 */
int ans_nvme_submit(struct ans_core *core, enum ans_nvme_queue queue, uint16_t tag,
                    const struct ans_nvme_cmd *cmd)
{
    if (!ans_nvme_allowed(queue, cmd)) {
        ans_log(core, "nvme: denied %s command opcode 0x%02x nsid %u cdw10 0x%x",
                queue == ANS_NVME_ADMINQ ? "admin" : "I/O", cmd->opcode, cmd->nsid, cmd->cdw10);
        return ANS_ERR_DENIED;
    }

    if (core->hw < ANS_HW_MEMORY) {
        ans_log(core, "nvme: no queues to submit to");
        return ANS_ERR_STATE;
    }
    if ((queue == ANS_NVME_ADMINQ && tag != 0) ||
        (queue == ANS_NVME_IOQ && tag >= core->config.io_depth)) {
        ans_log(core, "nvme: tag %u out of range", tag);
        return ANS_ERR_INVALID;
    }

    struct ans_nvme_queue_mem *q = &core->nvme.q[queue];
    size_t c = (size_t)tag * NVME_CMD_SIZE;
    size_t t = (size_t)tag * NVMMU_TCB_SIZE;
    uint32_t dma_flags;

    ans_dma_write32(core, &q->cmds, c + CMD_DW0,
                    cmd->opcode | ((uint32_t)cmd->flags << 8) | ((uint32_t)tag << 16));
    ans_dma_write32(core, &q->cmds, c + CMD_NSID, cmd->nsid);
    ans_dma_write64(core, &q->cmds, c + 0x08, 0); /* cdw2, cdw3 */
    ans_dma_write64(core, &q->cmds, c + 0x10, 0); /* metadata */
    ans_dma_write64(core, &q->cmds, c + CMD_PRP1, cmd->prp1);
    ans_dma_write64(core, &q->cmds, c + CMD_PRP2, cmd->prp2);
    ans_dma_write32(core, &q->cmds, c + CMD_CDW10 + 0x00, cmd->cdw10);
    ans_dma_write32(core, &q->cmds, c + CMD_CDW10 + 0x04, cmd->cdw11);
    ans_dma_write32(core, &q->cmds, c + CMD_CDW10 + 0x08, cmd->cdw12);
    ans_dma_write32(core, &q->cmds, c + CMD_CDW10 + 0x0c, cmd->cdw13);
    ans_dma_write32(core, &q->cmds, c + CMD_CDW10 + 0x10, cmd->cdw14);
    ans_dma_write32(core, &q->cmds, c + CMD_CDW10 + 0x14, cmd->cdw15);

    if (!cmd->prp1)
        dma_flags = 0;
    else if (cmd->opcode & 1)
        dma_flags = NVMMU_TCB_DMA_TO_DEVICE;
    else
        dma_flags = NVMMU_TCB_DMA_FROM_DEVICE;

    for (size_t i = 0; i < NVMMU_TCB_SIZE; i += 8)
        ans_dma_write64(core, &q->tcbs, t + i, 0);
    /* opcode 0 (as m1n1 and Linux), dma_flags, slot_id, length = blocks - 1 (16 bits) */
    ans_dma_write64(core, &q->tcbs, t + TCB_HEAD,
                    ((uint64_t)dma_flags << 8) | ((uint64_t)tag << 16) |
                        ((uint64_t)(cmd->cdw12 & 0xffff) << 32));
    ans_dma_write64(core, &q->tcbs, t + TCB_PRP1, cmd->prp1);
    ans_dma_write64(core, &q->tcbs, t + TCB_PRP2, cmd->prp2);

    /* make sure ANS2 can see the command and tcb before triggering it */
    ans_env_wmb(core->env);

    if (queue == ANS_NVME_ADMINQ)
        ans_write32(core, ANS_RGN_NVME, NVME_DB_LINEAR_ASQ, tag);
    else
        ans_write32(core, ANS_RGN_NVME, NVME_DB_LINEAR_IOSQ, tag);
    return ANS_OK;
}

/* ----------------------------------------------------------------------------- */
/* Completions                                                                     */

/*
 * Takes every new entry off one completion queue: for each, invalidate the tag in the
 * NVMMU and check, move the head, ring the completion doorbell, then hand the result
 * on (findings/0071 conclusion 19). Never waits.
 */
static bool nvme_handle_cq(struct ans_core *core, enum ans_nvme_queue queue, ans_done_fn *done,
                           void *ctx)
{
    struct ans_nvme *nvme = &core->nvme;
    struct ans_nvme_queue_mem *q = &nvme->q[queue];
    bool handled = false;

    for (int n = 0; n < NVME_QUEUE_SIZE; n++) {
        /* we need a DMA read barrier here since the CQ will be updated using DMA */
        ans_env_rmb(core->env);
        uint32_t dw = ans_dma_read32(core, &q->cqes,
                                     (size_t)q->cq_head * NVME_CQE_SIZE + CQE_TAG_STATUS);
        if (((dw >> 16) & 1) != q->cq_phase)
            break;

        uint16_t tag = (uint16_t)(dw & 0xffff);
        int status = (int)((dw >> 17) & 0x7fff);
        handled = true;

        ans_write32(core, ANS_RGN_NVME, NVMMU_TCB_INVAL, tag);
        if (ans_read32(core, ANS_RGN_NVME, NVMMU_TCB_STAT)) {
            core->stats.nvmmu_inval_failed++;
            ans_log(core, "nvme: NVMMU invalidation for tag %u failed", tag);
        }

        /* increment head and switch phase once the end of the queue has been reached */
        q->cq_head++;
        if (q->cq_head == NVME_QUEUE_SIZE) {
            q->cq_head = 0;
            q->cq_phase ^= 1;
        }
        ans_write32(core, ANS_RGN_NVME, queue == ANS_NVME_ADMINQ ? NVME_DB_ACQ : NVME_DB_IOCQ,
                    q->cq_head);

        if (queue == ANS_NVME_ADMINQ) {
            if (tag == 0 && nvme->admin == ANS_ADMIN_WAITING) {
                nvme->admin = ANS_ADMIN_DONE;
                nvme->admin_status = (uint32_t)status;
            } else {
                core->stats.stray_completions++;
                ans_log(core, "nvme: unexpected admin completion, tag %u status 0x%x", tag,
                        status);
            }
        } else if (tag < core->config.io_depth && (nvme->inflight & BIT(tag))) {
            nvme->inflight &= ~BIT(tag);
            if (status)
                ans_log(core, "nvme: read tag %u failed with status 0x%x", tag, status);
            if (done)
                done(ctx, tag, status);
        } else {
            core->stats.stray_completions++;
            ans_log(core, "nvme: completion for tag %u, which is not in flight", tag);
        }
    }
    return handled;
}

/* ----------------------------------------------------------------------------- */
/* Waiting                                                                         */

static bool rtkit_crashed(struct ans_core *core)
{
    return core->rtkit.phase == RTKIT_CRASHED;
}

/* What ans_start() and ans_stop() do while they wait: move messages and completions. */
static void ans_pump(struct ans_core *core)
{
    ans_asc_flush(core);
    ans_asc_drain(core);
    ans_rtkit_service(core, true);
    ans_asc_flush(core);
    if (core->hw >= ANS_HW_NVME_EN && !rtkit_crashed(core))
        nvme_handle_cq(core, ANS_NVME_ADMINQ, NULL, NULL);
}

typedef bool ans_cond_fn(struct ans_core *core);

/*
 * Waits until cond() holds, for at most timeout_us as the environment counts time.
 * Gives up at once when the coprocessor crashes or when ans_halt() ran meanwhile.
 */
static int ans_wait(struct ans_core *core, ans_cond_fn *cond, uint32_t timeout_us,
                    const char *what)
{
    uint32_t halts = core->halts;
    uint32_t left = timeout_us;

    for (;;) {
        ans_pump(core);
        if (rtkit_crashed(core)) {
            ans_log(core, "nvme: coprocessor crashed while waiting for %s", what);
            core->state = ANS_CRASHED;
            return ANS_ERR_CRASHED;
        }
        if (core->halts != halts) {
            ans_log(core, "nvme: stopped waiting for %s, the core was halted", what);
            return ANS_ERR_STATE;
        }
        if (cond(core))
            return ANS_OK;
        if (left == 0) {
            ans_log(core, "nvme: timeout while waiting for %s", what);
            return ANS_ERR_TIMEOUT;
        }

        uint32_t spent = ans_env_wait(core->env, left < ANS_WAIT_STEP ? left : ANS_WAIT_STEP);
        if (spent == 0)
            spent = 1;
        left = spent >= left ? 0 : left - spent;
    }
}

static bool rtkit_past_hello(struct ans_core *core)
{
    return core->rtkit.phase != RTKIT_WAIT_HELLO;
}

static bool rtkit_past_epmap(struct ans_core *core)
{
    return core->rtkit.phase != RTKIT_WAIT_EPMAP;
}

static bool rtkit_running(struct ans_core *core)
{
    return core->rtkit.phase != RTKIT_WAIT_POWER_ON;
}

static bool rtkit_asleep(struct ans_core *core)
{
    return core->rtkit.phase != RTKIT_WAIT_QUIESCED && core->rtkit.phase != RTKIT_WAIT_SLEEP;
}

/* Waits for the coprocessor to leave a step; fails unless it reached want. */
static int rtkit_wait(struct ans_core *core, ans_cond_fn *cond, enum rtkit_phase want,
                      const char *what)
{
    int r = ans_wait(core, cond, RTKIT_TIMEOUT, what);
    if (r)
        return r;
    if (core->rtkit.phase == RTKIT_FAILED || core->rtkit.phase < want) {
        ans_log(core, "nvme: coprocessor failed while waiting for %s", what);
        return ANS_ERR_DEVICE;
    }
    return ANS_OK;
}

static bool boot_status_ok(struct ans_core *core)
{
    return ans_read32(core, ANS_RGN_NVME, NVME_BOOT_STATUS) == NVME_BOOT_STATUS_OK;
}

static bool csts_ready(struct ans_core *core)
{
    return (ans_read32(core, ANS_RGN_NVME, NVME_CSTS) & NVME_CSTS_RDY) != 0;
}

static bool csts_not_ready(struct ans_core *core)
{
    return !csts_ready(core);
}

static bool csts_shutdown_done(struct ans_core *core)
{
    return FIELD_GET(NVME_CSTS_SHST, ans_read32(core, ANS_RGN_NVME, NVME_CSTS)) ==
           NVME_CSTS_SHST_DONE;
}

static bool admin_done(struct ans_core *core)
{
    return core->nvme.admin == ANS_ADMIN_DONE;
}

/* ----------------------------------------------------------------------------- */
/* Controller steps                                                                */

static int nvme_ctrl_disable(struct ans_core *core)
{
    ans_clear32(core, ANS_RGN_NVME, NVME_CC, NVME_CC_EN);
    return ans_wait(core, csts_not_ready, NVME_TIMEOUT, "CSTS.RDY to clear");
}

static int nvme_ctrl_enable(struct ans_core *core)
{
    ans_mask32(core, ANS_RGN_NVME, NVME_CC, NVME_CC_SHN, NVME_CC_EN);
    return ans_wait(core, csts_ready, NVME_ENABLE_TIMEOUT, "CSTS.RDY to be set");
}

static int nvme_ctrl_shutdown(struct ans_core *core)
{
    ans_mask32(core, ANS_RGN_NVME, NVME_CC, NVME_CC_SHN,
               FIELD_PREP(NVME_CC_SHN, NVME_CC_SHN_NORMAL));
    return ans_wait(core, csts_shutdown_done, NVME_SHUTDOWN_TIMEOUT, "controller shutdown");
}

/*
 * Runs one admin command and waits for it (only inside ans_start() and ans_stop()).
 * Every admin command is tag 0. Once one went out and no completion came back, a late
 * completion could not be told apart from the next command's, and slot 0 might still
 * be in the controller's hands: no further admin command is sent.
 */
static int nvme_admin(struct ans_core *core, const struct ans_nvme_cmd *cmd, const char *what)
{
    struct ans_nvme *nvme = &core->nvme;

    if (nvme->admin == ANS_ADMIN_LOST) {
        ans_log(core, "nvme: %s command not sent, an earlier one never completed", what);
        return ANS_ERR_DEVICE;
    }
    nvme->admin = ANS_ADMIN_WAITING;
    int r = ans_nvme_submit(core, ANS_NVME_ADMINQ, 0, cmd);
    if (r) {
        nvme->admin = ANS_ADMIN_IDLE;
    } else {
        r = ans_wait(core, admin_done, NVME_TIMEOUT, what);
        nvme->admin = r ? ANS_ADMIN_LOST : ANS_ADMIN_IDLE;
        if (r == ANS_OK && nvme->admin_status) {
            ans_log(core, "nvme: %s command failed with status 0x%x", what, nvme->admin_status);
            r = ANS_ERR_DEVICE;
        }
    }
    if (r)
        ans_log(core, "nvme: %s command failed", what);
    return r;
}

static void nvme_cmd_zero(struct ans_nvme_cmd *cmd)
{
    cmd->opcode = 0;
    cmd->flags = 0;
    cmd->nsid = 0;
    cmd->prp1 = cmd->prp2 = 0;
    cmd->cdw10 = cmd->cdw11 = cmd->cdw12 = cmd->cdw13 = cmd->cdw14 = cmd->cdw15 = 0;
}

static int nvme_delete_queue(struct ans_core *core, uint8_t opcode, const char *what)
{
    struct ans_nvme_cmd cmd;

    nvme_cmd_zero(&cmd);
    cmd.opcode = opcode;
    cmd.cdw10 = 1; /* queue id */
    return nvme_admin(core, &cmd, what);
}

/* Copies an identify string out of device memory, printable ASCII only. */
static void identify_string(struct ans_core *core, size_t off, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = ans_dma_read8(core, &core->nvme.identify, off + i);
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
    }
    out[len] = '\0';
}

static int nvme_identify(struct ans_core *core)
{
    struct ans_nvme *nvme = &core->nvme;
    struct ans_info *info = &core->info;
    struct ans_nvme_cmd cmd;
    int r;

    nvme_cmd_zero(&cmd);
    cmd.opcode = NVME_ADMIN_CMD_IDENTIFY;
    cmd.prp1 = nvme->identify.dva;
    cmd.cdw10 = NVME_ID_CNS_CTRL;
    r = nvme_admin(core, &cmd, "identify controller");
    if (r)
        return r;

    /* serial number 4-23, model 24-63, firmware revision 64-71, MDTS 77 */
    identify_string(core, 4, 20, info->serial);
    identify_string(core, 24, 40, info->model);
    identify_string(core, 64, 8, info->firmware);
    uint8_t mdts = ans_dma_read8(core, &nvme->identify, 77);
    /* MDTS is a power of two in units of the minimum page size, 4 KiB here */
    info->mdts_bytes = (mdts == 0 || mdts > 19) ? 0 : (NVME_PAGE << mdts);

    nvme_cmd_zero(&cmd);
    cmd.opcode = NVME_ADMIN_CMD_IDENTIFY;
    cmd.nsid = NVME_NSID;
    cmd.prp1 = nvme->identify.dva;
    cmd.cdw10 = NVME_ID_CNS_NS;
    r = nvme_admin(core, &cmd, "identify namespace");
    if (r)
        return r;

    /* NSZE 0-7, NLBAF 25, FLBAS 26, LBA formats from 128, 4 bytes each, LBADS in 16-23 */
    uint64_t nsze = ans_dma_read64(core, &nvme->identify, 0);
    uint8_t nlbaf = ans_dma_read8(core, &nvme->identify, 25);
    uint8_t format = ans_dma_read8(core, &nvme->identify, 26) & 0xf;
    if (format > nlbaf) {
        ans_log(core, "nvme: namespace 1 uses LBA format %u of %u", format, nlbaf + 1);
        return ANS_ERR_DEVICE;
    }
    uint32_t lbaf = ans_dma_read32(core, &nvme->identify, 128 + 4 * (size_t)format);
    uint32_t lbads = (lbaf >> 16) & 0xff;
    if (nsze == 0 || lbads < 9 || lbads > 12) {
        ans_log(core, "nvme: namespace 1 has %llu blocks of 2^%u bytes, not usable",
                (unsigned long long)nsze, lbads);
        return ANS_ERR_DEVICE;
    }
    info->blocks = nsze;
    info->block_size = 1u << lbads;

    ans_log(core, "nvme: model \"%s\" serial \"%s\" firmware \"%s\"", info->model, info->serial,
            info->firmware);
    ans_log(core, "nvme: namespace 1: %llu blocks of %u bytes, MDTS %u bytes",
            (unsigned long long)info->blocks, info->block_size, info->mdts_bytes);

    if (info->mdts_bytes && info->mdts_bytes < core->config.max_transfer) {
        ans_log(core, "nvme: controller takes at most %u bytes per command, %u wanted",
                info->mdts_bytes, core->config.max_transfer);
        return ANS_ERR_DEVICE;
    }
    return ANS_OK;
}

/* ----------------------------------------------------------------------------- */
/* Bringing the controller up and down                                             */

/*
 * The reset through the power manager goes in two places, both with the RUN bit clear
 * (R40, SiliconNT lab record 2026-11-22-nvme-reset-before-bringup-plan): here, right
 * after the coprocessor is taken out of RUN, whenever the controller is brought down
 * (ans_stop(), a failed ans_start(), ans_halt()); and in start_steps() before RUN is
 * set.
 */
static int ans_hw_off(struct ans_core *core)
{
    ans_asc_cpu_stop(core);
    return ans_pmgr_reset(core);
}

static void reset_state(struct ans_core *core)
{
    struct ans_nvme *nvme = &core->nvme;
    struct ans_info *info = &core->info;

    ans_asc_init(core);
    ans_rtkit_init(core);
    core->sart.protected_mask = 0;
    core->sart.added_mask = 0;
    nvme->inflight = 0;
    nvme->admin = ANS_ADMIN_IDLE;
    nvme->admin_status = 0;
    info->blocks = 0;
    info->block_size = 0;
    info->mdts_bytes = 0;
    info->serial[0] = info->model[0] = info->firmware[0] = '\0';
}

static int start_steps(struct ans_core *core)
{
    struct ans_nvme *nvme = &core->nvme;
    struct ans_nvme_cmd cmd;
    uint32_t halts = core->halts;
    int r;

    if (!alloc_queue(core, &nvme->q[ANS_NVME_ADMINQ]) ||
        !alloc_queue(core, &nvme->q[ANS_NVME_IOQ]) ||
        !alloc_dma(core, &nvme->prp, (size_t)core->config.io_depth * NVME_PAGE) ||
        !alloc_dma(core, &nvme->identify, NVME_PAGE)) {
        core->hw = ANS_HW_MEMORY;
        return ANS_ERR_NOMEM;
    }
    core->hw = ANS_HW_MEMORY;
    /* the environment may have given up its lock while allocating: was the core halted? */
    if (core->halts != halts) {
        ans_log(core, "nvme: halted while allocating, not starting");
        return ANS_ERR_STATE;
    }

    r = ans_sart_init(core, core->config.sart_version);
    if (r)
        return r;

    /* RUN is clear here (ans_start() checked); a reset the controller refuses fails the start */
    r = ans_pmgr_reset(core);
    if (r) {
        ans_log(core, "nvme: reset before start failed");
        return r;
    }

    core->hw = ANS_HW_CPU_RUN;
    if (!ans_rtkit_boot(core))
        return ANS_ERR_DEVICE;
    r = rtkit_wait(core, rtkit_past_hello, RTKIT_WAIT_EPMAP, "HELLO");
    if (r)
        return r;
    r = rtkit_wait(core, rtkit_past_epmap, RTKIT_WAIT_POWER_ON, "endpoint map");
    if (r)
        return r;
    r = rtkit_wait(core, rtkit_running, RTKIT_RUNNING, "IOP power on");
    if (r)
        return r;
    core->hw = ANS_HW_RTKIT;

    r = ans_wait(core, boot_status_ok, BOOT_STATUS_TIMEOUT, "ANS boot status");
    if (r) {
        ans_log(core, "nvme: ANS did not boot correctly.");
        return r;
    }

    /* setup controller and NVMMU for linear submission queue */
    ans_set32(core, ANS_RGN_NVME, NVME_LINEAR_SQ_CTRL, NVME_LINEAR_SQ_CTRL_EN);
    ans_write32(core, ANS_RGN_NVME, NVME_MAX_PEND_CMDS_CTRL,
                ((NVME_QUEUE_SIZE - 1) << 16) | (NVME_QUEUE_SIZE - 1));
    ans_write32(core, ANS_RGN_NVME, NVMMU_NUM, NVME_QUEUE_SIZE - 1);
    ans_write64_lo_hi(core, ANS_RGN_NVME, NVMMU_ASQ_BASE, nvme->q[ANS_NVME_ADMINQ].tcbs.dva);
    ans_write64_lo_hi(core, ANS_RGN_NVME, NVMMU_IOSQ_BASE, nvme->q[ANS_NVME_IOQ].tcbs.dva);

    /* setup admin queue */
    r = nvme_ctrl_disable(core);
    if (r)
        return r;
    ans_write64_lo_hi(core, ANS_RGN_NVME, NVME_ASQ, nvme->q[ANS_NVME_ADMINQ].cmds.dva);
    ans_write64_lo_hi(core, ANS_RGN_NVME, NVME_ACQ, nvme->q[ANS_NVME_ADMINQ].cqes.dva);
    ans_write32(core, ANS_RGN_NVME, NVME_AQA,
                ((NVME_QUEUE_SIZE - 1) << 16) | (NVME_QUEUE_SIZE - 1));
    core->hw = ANS_HW_NVME_EN;
    r = nvme_ctrl_enable(core);
    if (r)
        return r;

    r = nvme_identify(core);
    if (r)
        return r;

    /* setup IO queue */
    nvme_cmd_zero(&cmd);
    cmd.opcode = NVME_ADMIN_CMD_CREATE_CQ;
    cmd.prp1 = nvme->q[ANS_NVME_IOQ].cqes.dva;
    cmd.cdw10 = 1; /* cq id */
    cmd.cdw10 |= (NVME_QUEUE_SIZE - 1) << 16;
    cmd.cdw11 = NVME_QUEUE_CONTIGUOUS | NVME_CQ_IRQ_ENABLED; /* vector 0 */
    r = nvme_admin(core, &cmd, "create cq");
    if (r)
        return r;
    core->hw = ANS_HW_IOCQ;

    nvme_cmd_zero(&cmd);
    cmd.opcode = NVME_ADMIN_CMD_CREATE_SQ;
    cmd.prp1 = nvme->q[ANS_NVME_IOQ].cmds.dva;
    cmd.cdw10 = 1; /* sq id */
    cmd.cdw10 |= (NVME_QUEUE_SIZE - 1) << 16;
    cmd.cdw11 = NVME_QUEUE_CONTIGUOUS;
    cmd.cdw11 |= 1 << 16; /* cq id for this sq */
    r = nvme_admin(core, &cmd, "create sq");
    if (r)
        return r;
    core->hw = ANS_HW_IOSQ;

    if (core->config.ioq_registers) {
        /*
         * Extra write required to set up the IO queues on M4+. Otherwise, the ANS
         * crashes and the crashlog says `I/O SQ  : 0x000000000` / `I/O CQ  : 0x000000000`.
         */
        ans_write64_lo_hi(core, ANS_RGN_NVME, NVME_IOQ_CQES, nvme->q[ANS_NVME_IOQ].cqes.dva);
        ans_write64_lo_hi(core, ANS_RGN_NVME, NVME_IOQ_CMDS, nvme->q[ANS_NVME_IOQ].cmds.dva);
    }
    return ANS_OK;
}

/*
 * Brings down whatever start_steps() brought up, step by step from where the hardware
 * is, and gives back all memory. Steps that need the coprocessor are skipped once it
 * has crashed; RUN is cleared and the reset done whenever RUN may have been set.
 * Leaves the state ANS_OFF when every step worked, ANS_FAILED when one did not (or the
 * core was halted, or had failed before), ANS_CRASHED after a crash.
 */
static void ans_teardown(struct ans_core *core)
{
    struct ans_nvme *nvme = &core->nvme;
    bool clean = core->state != ANS_FAILED;

    if (core->hw >= ANS_HW_IOSQ && !rtkit_crashed(core) &&
        nvme_delete_queue(core, NVME_ADMIN_CMD_DELETE_SQ, "delete sq"))
        clean = false;
    if (core->hw >= ANS_HW_IOCQ && !rtkit_crashed(core) &&
        nvme_delete_queue(core, NVME_ADMIN_CMD_DELETE_CQ, "delete cq"))
        clean = false;
    if (core->hw >= ANS_HW_NVME_EN && !rtkit_crashed(core)) {
        if (nvme_ctrl_shutdown(core))
            clean = false;
        if (!rtkit_crashed(core) && nvme_ctrl_disable(core))
            clean = false;
    }
    if (core->hw >= ANS_HW_RTKIT && !rtkit_crashed(core)) {
        if (!ans_rtkit_sleep(core) ||
            rtkit_wait(core, rtkit_asleep, RTKIT_ASLEEP, "IOP sleep"))
            clean = false;
    }
    if (core->hw >= ANS_HW_CPU_RUN) {
        if (ans_hw_off(core))
            clean = false;
        if (!rtkit_crashed(core))
            core->rtkit.phase = RTKIT_OFF;
    }
    /*
     * The queues are gone from the core's record before their memory is: the interrupt
     * handler may run while the environment frees it.
     */
    if (core->hw >= ANS_HW_MEMORY) {
        core->hw = ANS_HW_MEMORY;
        nvme->inflight = 0;
        free_all_memory(core);
    }

    core->hw = ANS_HW_NONE;
    nvme->inflight = 0;
    ans_asc_init(core);

    if (rtkit_crashed(core) || core->state == ANS_CRASHED)
        core->state = ANS_CRASHED;
    else if (!clean || core->state == ANS_FAILED)
        core->state = ANS_FAILED;
    else
        core->state = ANS_OFF;
}

static const char *state_name(enum ans_state state)
{
    switch (state) {
        case ANS_OFF:
            return "off";
        case ANS_STARTING:
            return "starting";
        case ANS_READY:
            return "ready";
        case ANS_STOPPING:
            return "stopping";
        case ANS_FAILED:
            return "failed";
        case ANS_CRASHED:
            return "crashed";
    }
    return "?";
}

/* ----------------------------------------------------------------------------- */
/* The interface                                                                   */

int ans_probe(struct ans_core *core, struct ans_env *env)
{
    if (core->state != ANS_OFF) {
        ans_env_log(env, "ans: probe while %s", state_name(core->state));
        return ANS_ERR_STATE;
    }
    core->env = env;

    if (ans_asc_cpu_running(core)) {
        ans_log(core, "ans: coprocessor is running: the previous owner did not stop the "
                      "controller, not taking it over");
        return ANS_ERR_RUNNING;
    }
    uint32_t n = ans_asc_discard(core);
    if (n)
        ans_log(core, "ans: discarded %u stale mailbox messages", n);
    return ANS_OK;
}

int ans_start(struct ans_core *core, struct ans_env *env, const struct ans_config *config)
{
    if (core->state != ANS_OFF) {
        ans_env_log(env, "ans: start while %s", state_name(core->state));
        return ANS_ERR_STATE;
    }
    core->env = env;

    if (config->io_depth < 1 || config->io_depth > ANS_MAX_IO_DEPTH ||
        config->max_transfer < 1 || config->max_transfer > ANS_MAX_TRANSFER ||
        (config->sart_version != 0 && config->sart_version != 2 && config->sart_version != 3 &&
         config->sart_version != 4)) {
        ans_log(core, "ans: bad configuration: io_depth %u max_transfer %u sart_version %u",
                config->io_depth, config->max_transfer, config->sart_version);
        return ANS_ERR_INVALID;
    }
    core->config = *config;
    reset_state(core);

    /* findings/0071 conclusion 30: a running coprocessor is not taken over */
    if (ans_asc_cpu_running(core)) {
        ans_log(core, "ans: coprocessor is running: the previous owner did not stop the "
                      "controller, not taking it over");
        return ANS_ERR_RUNNING;
    }
    /* what the previous owner left in the mailbox would be taken for the answer to HELLO */
    uint32_t stale = ans_asc_discard(core);
    if (stale)
        ans_log(core, "ans: discarded %u stale mailbox messages", stale);

    core->state = ANS_STARTING;
    core->hw = ANS_HW_NONE;
    int r = start_steps(core);
    if (r == ANS_OK && core->state == ANS_STARTING) {
        core->state = ANS_READY;
        ans_log(core, "ans: ready");
        return ANS_OK;
    }
    if (r == ANS_OK)
        r = ANS_ERR_STATE;

    /*
     * A start that failed half way is not tried again: brought down (RUN cleared, then
     * the reset) and left failed, so only a new boot starts the controller again.
     */
    ans_log(core, "ans: start failed (%d), bringing the controller down again", r);
    ans_teardown(core);
    if (core->state != ANS_CRASHED)
        core->state = ANS_FAILED;
    ans_log(core, "ans: controller %s", state_name(core->state));
    return r;
}

void ans_stop(struct ans_core *core)
{
    switch (core->state) {
        case ANS_OFF:
            return;
        case ANS_STARTING:
        case ANS_STOPPING:
            ans_log(core, "ans: stop while %s, ignored", state_name(core->state));
            return;
        case ANS_READY:
            if (core->nvme.inflight)
                ans_log(core, "ans: stop with reads in flight (%016llx)",
                        (unsigned long long)core->nvme.inflight);
            core->state = ANS_STOPPING;
            break;
        case ANS_FAILED:
        case ANS_CRASHED:
            break;
    }

    ans_teardown(core);
    ans_log(core, "ans: controller %s", state_name(core->state));
}

/*
 * Stops the controller's DMA now, without the coprocessor's help: CC.EN off, RUN off,
 * then the reset. For ScsiStopAdapter and HwResetBus (findings/0071 conclusions 26,
 * 28). Never waits, allocates or frees, so it may be called at any IRQL with the lock
 * held, also while ans_start() or ans_stop() is waiting. Reads in flight never
 * complete; the caller completes them. ans_stop() afterwards gives back the memory.
 */
void ans_halt(struct ans_core *core)
{
    if (core->state == ANS_OFF)
        return;

    ans_log(core, "ans: halt while %s", state_name(core->state));
    core->halts++; /* a start or stop that is waiting gives up */
    /*
     * The NVMe registers only once the RTKit handshake is through and the admin queue
     * was set up; before that the coprocessor has not brought them up.
     */
    if (core->hw >= ANS_HW_NVME_EN && !rtkit_crashed(core))
        ans_clear32(core, ANS_RGN_NVME, NVME_CC, NVME_CC_EN);
    if (core->hw >= ANS_HW_CPU_RUN) {
        if (ans_hw_off(core))
            ans_log(core, "ans: reset during halt failed");
        if (!rtkit_crashed(core))
            core->rtkit.phase = RTKIT_OFF;
    }
    if (core->hw > ANS_HW_MEMORY)
        core->hw = ANS_HW_MEMORY;
    core->nvme.inflight = 0;
    if (core->state != ANS_CRASHED)
        core->state = ANS_FAILED;
}

/*
 * Describes the read's memory with PRPs. The first piece may start anywhere 4-byte
 * aligned; every other piece starts on a 4 KiB boundary, and every piece but the last
 * ends on one (findings/0071 conclusion 20). One PRP list page per tag, not chained.
 */
static int nvme_build_prp(struct ans_core *core, uint16_t tag, uint64_t bytes,
                          const struct ans_segment *segs, uint32_t nsegs, uint64_t *prp1,
                          uint64_t *prp2)
{
    const struct ans_dma *list = &core->nvme.prp;
    size_t list_off = (size_t)tag * NVME_PAGE;
    uint64_t total = 0, second = 0;
    uint32_t pages = 0;

    if (!segs || nsegs == 0) {
        ans_log(core, "nvme: read without memory");
        return ANS_ERR_INVALID;
    }

    for (uint32_t i = 0; i < nsegs; i++) {
        uint64_t addr = segs[i].addr;
        uint64_t end = addr + segs[i].len;

        if (segs[i].len == 0 || end < addr || (i == 0 && (addr & 3)) ||
            (i > 0 && (addr & (NVME_PAGE - 1))) ||
            (i + 1 < nsegs && (end & (NVME_PAGE - 1)))) {
            ans_log(core, "nvme: piece %u of read tag %u (%016llx, 0x%x) breaks the PRP rules",
                    i, tag, (unsigned long long)addr, segs[i].len);
            return ANS_ERR_PRP;
        }
        total += segs[i].len;

        for (uint64_t page = addr & ~(uint64_t)(NVME_PAGE - 1); page < end; page += NVME_PAGE) {
            if (pages == 0) {
                *prp1 = addr;
            } else {
                if (pages - 1 >= NVME_PRP_LIST_ENTS) {
                    ans_log(core, "nvme: read tag %u needs more than one PRP list page", tag);
                    return ANS_ERR_PRP;
                }
                if (pages == 1)
                    second = page;
                ans_dma_write64(core, list, list_off + 8 * (size_t)(pages - 1), page);
            }
            pages++;
        }
    }

    if (total != bytes) {
        ans_log(core, "nvme: read tag %u has 0x%llx bytes of memory for 0x%llx bytes", tag,
                (unsigned long long)total, (unsigned long long)bytes);
        return ANS_ERR_INVALID;
    }

    if (pages == 1)
        *prp2 = 0;
    else if (pages == 2)
        *prp2 = second;
    else
        *prp2 = list->dva + list_off;
    return ANS_OK;
}

int ans_read(struct ans_core *core, uint16_t tag, uint64_t lba, uint32_t blocks,
             const struct ans_segment *segs, uint32_t nsegs)
{
    struct ans_nvme *nvme = &core->nvme;
    struct ans_nvme_cmd cmd;
    uint64_t prp1 = 0, prp2 = 0;
    int r;

    if (core->state == ANS_CRASHED)
        return ANS_ERR_CRASHED;
    if (core->state != ANS_READY) {
        if (core->env) /* never started: no environment to log to */
            ans_log(core, "nvme: read refused while %s", state_name(core->state));
        return ANS_ERR_STATE;
    }
    if (tag >= core->config.io_depth) {
        ans_log(core, "nvme: read tag %u out of range", tag);
        return ANS_ERR_INVALID;
    }
    if (nvme->inflight & BIT(tag)) {
        ans_log(core, "nvme: read tag %u is in flight already", tag);
        return ANS_ERR_BUSY;
    }

    uint64_t bytes = (uint64_t)blocks * core->info.block_size;
    if (blocks == 0 || blocks > 0x10000 || bytes > core->config.max_transfer ||
        lba >= core->info.blocks || blocks > core->info.blocks - lba) {
        ans_log(core, "nvme: read of %u blocks at %llu refused", blocks, (unsigned long long)lba);
        return ANS_ERR_INVALID;
    }

    r = nvme_build_prp(core, tag, bytes, segs, nsegs, &prp1, &prp2);
    if (r)
        return r;

    nvme_cmd_zero(&cmd);
    cmd.opcode = NVME_CMD_READ;
    cmd.nsid = NVME_NSID;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = blocks - 1; /* #blocks, 0-based */

    nvme->inflight |= BIT(tag);
    r = ans_nvme_submit(core, ANS_NVME_IOQ, tag, &cmd);
    if (r)
        nvme->inflight &= ~BIT(tag);
    return r;
}

/*
 * For the interrupt handler, or a polling loop: empties the mailbox FIFO (which keeps
 * its line high while it holds anything) and takes the completions off both queues,
 * calling done for each read. When no one will answer the coprocessor (off, failed,
 * crashed) its messages are thrown away. Never waits. done may call ans_read().
 */
bool ans_isr(struct ans_core *core, ans_done_fn *done, void *ctx)
{
    bool handled = false;

    if (!core->env)
        return false;

    switch (core->state) {
        case ANS_STARTING:
        case ANS_READY:
        case ANS_STOPPING:
            ans_asc_flush(core);
            if (ans_asc_drain(core))
                handled = true;
            break;
        default:
            if (ans_asc_discard(core))
                handled = true;
            break;
    }

    /*
     * After a crash the completions are still taken off the queues, so the controller's
     * line does not stay high, but not reported: the caller completes those reads.
     */
    if (core->state == ANS_CRASHED || rtkit_crashed(core))
        done = NULL;
    if (core->hw >= ANS_HW_NVME_EN && nvme_handle_cq(core, ANS_NVME_ADMINQ, NULL, NULL))
        handled = true;
    if (core->hw >= ANS_HW_IOCQ && nvme_handle_cq(core, ANS_NVME_IOQ, done, ctx))
        handled = true;
    return handled;
}

/*
 * Handles the coprocessor's messages that ans_isr() took in. Only while the core is
 * ready: during ans_start() and ans_stop() the waiting thread handles them itself,
 * because answering may need memory.
 */
void ans_mbox_service(struct ans_core *core)
{
    if (core->state != ANS_READY)
        return;

    ans_asc_flush(core);
    ans_rtkit_service(core, false);
    ans_asc_flush(core);

    if (rtkit_crashed(core)) {
        ans_log(core, "ans: coprocessor crashed");
        core->state = ANS_CRASHED;
    } else if (core->rtkit.phase == RTKIT_FAILED) {
        ans_log(core, "ans: coprocessor protocol failed while running");
        core->state = ANS_FAILED;
    }
}

enum ans_state ans_get_state(const struct ans_core *core)
{
    return core->state;
}

const struct ans_info *ans_get_info(const struct ans_core *core)
{
    return &core->info;
}

const struct ans_stats *ans_get_stats(const struct ans_core *core)
{
    return &core->stats;
}
