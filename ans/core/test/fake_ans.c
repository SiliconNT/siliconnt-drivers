/* SPDX-License-Identifier: MIT */
/*
 * The fake disk controller of the model test; see fake_ans.h. Register offsets and
 * message formats are those of m1n1 v1.9.9 and of the core; the behaviour it checks
 * is written down next to each check.
 */

#include "fake_ans.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ASC */
#define F_CPU_CONTROL 0x44
#define F_RUN         0x10u
#define F_A2I_CONTROL 0x8110
#define F_I2A_CONTROL 0x8114
#define F_A2I_SEND0   0x8800
#define F_A2I_SEND1   0x8808
#define F_I2A_RECV0   0x8830
#define F_I2A_RECV1   0x8838
#define F_FULL        (1u << 16)
#define F_EMPTY       (1u << 17)

/* NVMe */
#define F_CC              0x14
#define F_CSTS            0x1c
#define F_AQA             0x24
#define F_ASQ             0x28
#define F_ACQ             0x30
#define F_DB_ACQ          0x1004
#define F_DB_IOCQ         0x100c
#define F_IOQ_CMDS        0x1200
#define F_IOQ_CQES        0x1208
#define F_MAX_PEND        0x1210
#define F_BOOT_STATUS     0x1300
#define F_BOOT_OK         0xde71ce55u
#define F_FORBIDDEN       0x24008
#define F_LINEAR_SQ_CTRL  0x24908
#define F_DB_ASQ          0x2490c
#define F_DB_IOSQ         0x24910
#define F_NVMMU_NUM       0x28100
#define F_NVMMU_ASQ_BASE  0x28108
#define F_NVMMU_IOSQ_BASE 0x28110
#define F_TCB_INVAL       0x28118
#define F_TCB_STAT        0x28120

/* RTKit */
#define T(msg0)      ((uint32_t)(((msg0) >> 52) & 0xff))
#define PWR(msg0)    ((uint32_t)((msg0)&0xffff))
#define MK(type)     ((uint64_t)(type) << 52)
#define EP_MGMT      0
#define EP_CRASHLOG  1
#define EP_SYSLOG    2
#define EP_DEBUG     3
#define EP_IOREPORT  4
#define EP_OSLOG     8

#define QSIZE 64

static const uint32_t start_order[] = {EP_DEBUG, EP_CRASHLOG, EP_SYSLOG, EP_IOREPORT, EP_OSLOG};

/* Pages each system endpoint asks for: crashlog, syslog, ioreport. */
#define CRASHLOG_PAGES 2
#define SYSLOG_PAGES   4
#define IOREPORT_PAGES 1

void fake_violation(struct fake *f, const char *fmt, ...)
{
    va_list ap;
    char buf[512];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (f->violations == 0)
        snprintf(f->first_violation, sizeof(f->first_violation), "%s", buf);
    f->violations++;
    fprintf(stderr, "    fake: VIOLATION: %s\n", buf);
}

/* ------------------------------------------------------------------------------- */
/* SART                                                                              */

static void sart_layout(uint32_t version, uint32_t *paddr_base, uint32_t *size_base)
{
    switch (version) {
        case 3:
            *paddr_base = 0x40;
            *size_base = 0x80;
            break;
        case 4:
            *paddr_base = 0x60;
            *size_base = 0xc0;
            break;
        default: /* 0 and 2 keep the size in the config register */
            *paddr_base = 0x40;
            *size_base = 0;
            break;
    }
}

static void sart_decode(struct fake *f, int i, uint32_t *flags, uint64_t *paddr, uint64_t *size)
{
    uint32_t pb, sb;
    uint32_t cfg = f->sart[i];

    sart_layout(f->sart_version, &pb, &sb);
    *paddr = (uint64_t)f->sart[(pb + 4 * i) / 4] << 12;
    switch (f->sart_version) {
        case 0:
            *flags = (cfg >> 24) & 0x1f;
            *size = (uint64_t)(cfg & 0x7ffff) << 12;
            break;
        case 2:
            *flags = (cfg >> 24) & 0xff;
            *size = (uint64_t)(cfg & 0xffffff) << 12;
            break;
        default:
            *flags = cfg & 0xff;
            *size = (uint64_t)f->sart[(sb + 4 * i) / 4] << 12;
            break;
    }
}

static void sart_encode(struct fake *f, int i, uint32_t flags, uint64_t paddr, uint64_t size)
{
    uint32_t pb, sb;

    sart_layout(f->sart_version, &pb, &sb);
    f->sart[(pb + 4 * i) / 4] = (uint32_t)(paddr >> 12);
    switch (f->sart_version) {
        case 0:
            f->sart[i] = (flags << 24) | (uint32_t)(size >> 12);
            break;
        case 2:
            f->sart[i] = (flags << 24) | (uint32_t)(size >> 12);
            break;
        default:
            f->sart[i] = flags;
            f->sart[(sb + 4 * i) / 4] = (uint32_t)(size >> 12);
            break;
    }
}

/* Entries 0 and 1 are iBoot's; the core must leave them alone. */
#define IBOOT_ENTRIES 2

static bool sart_offset_entry(struct fake *f, uint32_t off, int *entry)
{
    uint32_t pb, sb;

    sart_layout(f->sart_version, &pb, &sb);
    if (off < 0x40) {
        *entry = (int)(off / 4);
        return true;
    }
    if (off >= pb && off < pb + 0x40) {
        *entry = (int)((off - pb) / 4);
        return true;
    }
    if (sb && off >= sb && off < sb + 0x40) {
        *entry = (int)((off - sb) / 4);
        return true;
    }
    return false;
}

bool fake_sart_covers(struct fake *f, uint64_t dva, uint64_t size)
{
    for (int i = IBOOT_ENTRIES; i < 16; i++) {
        uint32_t flags;
        uint64_t p, s;

        sart_decode(f, i, &flags, &p, &s);
        if (flags && p <= dva && dva + size <= p + s)
            return true;
    }
    return false;
}

uint32_t fake_sart_entries_in_use(struct fake *f)
{
    uint32_t n = 0;

    for (int i = 0; i < 16; i++) {
        uint32_t flags;
        uint64_t p, s;

        sart_decode(f, i, &flags, &p, &s);
        if (flags)
            n++;
    }
    return n;
}

bool fake_sart_unchanged_iboot(struct fake *f)
{
    return memcmp(f->sart, f->sart_at_start, sizeof(f->sart)) == 0;
}

/* ------------------------------------------------------------------------------- */
/* Memory                                                                            */

bool fake_mem_alloc(struct fake *f, size_t size, bool from_env, void **va, uint64_t *dva)
{
    if (f->nblocks == FAKE_MAX_BLOCKS || size == 0)
        return false;

    struct fake_block *b = &f->blocks[f->nblocks++];
    b->va = calloc(1, size);
    if (!b->va)
        return false;
    b->dva = f->next_dva;
    b->size = size;
    b->live = true;
    b->from_env = from_env;
    /* never reused, 16 KiB aligned, a gap between blocks */
    f->next_dva += ((size + 0x3fff) & ~(size_t)0x3fff) + 0x4000;
    *va = b->va;
    *dva = b->dva;
    return true;
}

void fake_mem_free(struct fake *f, uint64_t dva)
{
    for (uint32_t i = 0; i < f->nblocks; i++) {
        struct fake_block *b = &f->blocks[i];
        if (b->dva == dva && b->live) {
            /* The coprocessor or the controller must no longer reach it. */
            if (b->from_env && fake_sart_covers(f, b->dva, 1))
                fake_violation(f, "memory at %llx freed while the SART still lets it through",
                               (unsigned long long)dva);
            b->live = false;
            free(b->va);
            b->va = NULL;
            return;
        }
    }
    fake_violation(f, "free of unknown memory %llx", (unsigned long long)dva);
}

/* The controller's view: a live block holding [dva, dva + len). */
void *fake_mem_at(struct fake *f, uint64_t dva, size_t len)
{
    for (uint32_t i = 0; i < f->nblocks; i++) {
        struct fake_block *b = &f->blocks[i];
        if (dva >= b->dva && dva + len <= b->dva + b->size) {
            if (!b->live) {
                fake_violation(f, "device access to freed memory %llx", (unsigned long long)dva);
                return NULL;
            }
            return (char *)b->va + (dva - b->dva);
        }
    }
    fake_violation(f, "device access to memory nobody gave it: %llx+%llx",
                   (unsigned long long)dva, (unsigned long long)len);
    return NULL;
}

uint32_t fake_live_env_blocks(const struct fake *f)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < f->nblocks; i++)
        if (f->blocks[i].live && f->blocks[i].from_env)
            n++;
    return n;
}

/* ------------------------------------------------------------------------------- */
/* Set-up                                                                            */

static void nvme_reset(struct fake *f)
{
    f->nvme_cc = f->nvme_csts = f->nvme_aqa = 0;
    f->nvme_asq = f->nvme_acq = 0;
    f->linear_sq_ctrl = f->max_pend = f->nvmmu_num = 0;
    f->nvmmu_asq_base = f->nvmmu_iosq_base = 0;
    f->ioq_cmds = f->ioq_cqes = 0;
    f->iocq_made = f->iosq_made = false;
    for (int q = 0; q < 2; q++) {
        f->cq_tail[q] = 0;
        f->cq_head[q] = 0;
        f->cq_phase[q] = 1;
        f->npending[q] = 0;
        f->busy[q] = 0;
    }
}

void fake_init(struct fake *f, uint32_t sart_version)
{
    memset(f, 0, sizeof(*f));
    f->sart_version = sart_version;
    f->secure_bar = true;
    f->mdts = 8; /* 1 MiB */
    f->nsze = 122138132; /* 500.3 GB of 4 KiB blocks */
    f->lbads = 12;
    f->hang_admin_opcode = -1;
    f->fail_admin_opcode = -1;
    f->a2i_capacity = 8;
    f->pmgr = 0x100000ffu; /* auto-enable, PS_ACTUAL = PS_TARGET = active */
    f->next_dva = 0x10000000000ull;
    f->cp = CP_OFF;
    nvme_reset(f);

    /* iBoot's two entries */
    sart_encode(f, 0, sart_version == 0 ? 0xf : 0xff, 0x10e6da4000ull, 0x100000);
    sart_encode(f, 1, 0x3, 0x10f0000000ull, 0x4000);
    memcpy(f->sart_at_start, f->sart, sizeof(f->sart));
}

void fake_advance(struct fake *f, uint32_t usec)
{
    f->now_us += usec;
}

/* ------------------------------------------------------------------------------- */
/* Coprocessor                                                                       */

static void cp_send(struct fake *f, uint32_t ep, uint64_t msg0)
{
    if (f->i2a_n == FAKE_FIFO) {
        fake_violation(f, "coprocessor-to-host FIFO overflow");
        return;
    }
    f->i2a[(f->i2a_head + f->i2a_n) % FAKE_FIFO].msg0 = msg0;
    f->i2a[(f->i2a_head + f->i2a_n) % FAKE_FIFO].msg1 = ep;
    f->i2a_n++;
}

static void cp_crash(struct fake *f)
{
    uint32_t *log = fake_mem_at(f, f->crashlog_dva, 0x100);
    const char text[] = "fake crash test";

    if (log) {
        memset(log, 0, 0x100);
        log[0] = 0x434c4845u; /* 'CLHE' */
        log[1] = 1;
        log[2] = 0x100;
        /* entry at 32: 'Cstr', pad, flags, length; payload: number, 3 bytes, text */
        log[8] = 0x43737472u;
        log[11] = 16 + 4 + (uint32_t)sizeof(text); /* 36, so the next entry is aligned */
        ((uint8_t *)log)[48] = 7;
        memcpy((uint8_t *)log + 52, text, sizeof(text));
        log[(32 + 36) / 4] = 0x434c4845u; /* end */
    }
    cp_send(f, EP_CRASHLOG, MK(1) | ((uint64_t)CRASHLOG_PAGES << 44));
    f->cp = CP_CRASHED;
}

static void cp_unexpected(struct fake *f, const struct fake_msg *m)
{
    fake_violation(f, "coprocessor in state %d got unexpected message %016llx ep %u", f->cp,
                   (unsigned long long)m->msg0, m->msg1);
}

static bool cp_is_ack(struct fake *f, const struct fake_msg *m)
{
    if (f->cp < CP_WAIT_BUFFERS)
        return false;
    if ((m->msg1 == EP_SYSLOG && T(m->msg0) == 5) ||
        (m->msg1 == EP_IOREPORT && (T(m->msg0) == 8 || T(m->msg0) == 0xc))) {
        f->acks_seen++;
        return true;
    }
    return false;
}

static void cp_check_buffer(struct fake *f, const struct fake_msg *m)
{
    uint64_t pages = (m->msg0 >> 44) & 0xff;
    uint64_t iova = m->msg0 & ((1ull << 42) - 1);
    uint32_t want = m->msg1 == EP_CRASHLOG ? CRASHLOG_PAGES
                    : m->msg1 == EP_SYSLOG ? SYSLOG_PAGES
                                           : IOREPORT_PAGES;
    uint32_t bit = m->msg1 == EP_CRASHLOG ? 1 : m->msg1 == EP_SYSLOG ? 2 : 4;

    if (pages != want || iova == 0)
        fake_violation(f, "bad buffer reply %016llx ep %u", (unsigned long long)m->msg0, m->msg1);
    else if (!fake_sart_covers(f, iova, pages << 12))
        fake_violation(f, "buffer %llx given to the coprocessor is not in the SART",
                       (unsigned long long)iova);
    else if (!fake_mem_at(f, iova, pages << 12))
        fake_violation(f, "buffer %llx is not memory", (unsigned long long)iova);
    if (f->buffers_answered & bit)
        fake_violation(f, "second buffer reply for ep %u", m->msg1);
    f->buffers_answered |= bit;
    if (m->msg1 == EP_CRASHLOG)
        f->crashlog_dva = iova;
}

static void cp_message(struct fake *f, const struct fake_msg *m)
{
    uint32_t type = T(m->msg0);

    if (cp_is_ack(f, m))
        return;

    switch (f->cp) {
        case CP_OFF:
            fake_violation(f, "message to a stopped coprocessor");
            return;
        case CP_DEAD:
        case CP_CRASHED:
            return;
        case CP_WAIT_WAKE:
            if (m->msg1 != EP_MGMT || type != 6 || PWR(m->msg0) != 0x220)
                break;
            if (f->bad_version)
                cp_send(f, EP_MGMT, MK(1) | (14ull << 16) | 13);
            else
                cp_send(f, EP_MGMT, MK(1) | (12ull << 16) | 11);
            f->cp = CP_HELLO_SENT;
            return;
        case CP_HELLO_SENT:
            if (m->msg1 != EP_MGMT || type != 2 || (m->msg0 & 0xffffffff) != ((12u << 16) | 12))
                break;
            /* map part 1: endpoints 0, 1, 2, 3, 4, 8 */
            cp_send(f, EP_MGMT, MK(8) | (0ull << 32) | 0x11f);
            f->cp = CP_EPMAP_SENT;
            return;
        case CP_EPMAP_SENT:
            if (m->msg1 != EP_MGMT || type != 8 || ((m->msg0 >> 32) & 7) != 0 ||
                !(m->msg0 & 1) || (m->msg0 & (1ull << 51)))
                break;
            /* map part 2: endpoint 0x20, last */
            cp_send(f, EP_MGMT, MK(8) | (1ull << 51) | (1ull << 32) | 0x1);
            f->cp = CP_EPMAP_SENT2;
            return;
        case CP_EPMAP_SENT2:
            if (m->msg1 != EP_MGMT || type != 8 || ((m->msg0 >> 32) & 7) != 1 ||
                !(m->msg0 & (1ull << 51)))
                break;
            f->cp = CP_STARTING_EPS;
            f->eps_started = 0;
            return;
        case CP_STARTING_EPS: {
            uint32_t n = 0;
            while (n < 5 && (f->eps_started & (1u << start_order[n])))
                n++;
            if (m->msg1 != EP_MGMT || type != 5 || !(m->msg0 & 2) || n == 5 ||
                ((m->msg0 >> 32) & 0xff) != start_order[n])
                break;
            f->eps_started |= 1u << start_order[n];
            if (n + 1 < 5)
                return;
            uint64_t own = f->buffer_at_own_addr ? 0x20000000000ull >> 2 : 0;
            cp_send(f, EP_CRASHLOG, MK(1) | ((uint64_t)CRASHLOG_PAGES << 44) | own);
            cp_send(f, EP_SYSLOG, MK(1) | ((uint64_t)SYSLOG_PAGES << 44));
            cp_send(f, EP_SYSLOG, MK(8) | (0x80ull << 24) | 16);
            if (!f->no_ioreport_buffer)
                cp_send(f, EP_IOREPORT, MK(1) | ((uint64_t)IOREPORT_PAGES << 44));
            f->cp = CP_WAIT_BUFFERS;
            return;
        }
        case CP_WAIT_BUFFERS:
            if (type != 1 || (m->msg1 != EP_CRASHLOG && m->msg1 != EP_SYSLOG &&
                              m->msg1 != EP_IOREPORT))
                break;
            cp_check_buffer(f, m);
            if (f->buffers_answered != (f->no_ioreport_buffer ? 3u : 7u))
                return;
            if (f->crash_at_boot) {
                cp_crash(f);
                return;
            }
            cp_send(f, EP_MGMT, MK(7) | 0x20);
            if (f->extra_messages) {
                cp_send(f, EP_SYSLOG, MK(5) | 3);
                cp_send(f, EP_IOREPORT, MK(8) | 0x1234);
                cp_send(f, 0x20, MK(0x42)); /* an endpoint of the controller's own */
                f->acks_expected += 2;
            }
            f->cp = CP_WAIT_AP_ON;
            return;
        case CP_WAIT_AP_ON:
            if (m->msg1 != EP_MGMT || type != 0xb || PWR(m->msg0) != 0x20)
                break;
            cp_send(f, EP_MGMT, MK(0xb) | 0x20);
            f->cp = CP_RUNNING;
            return;
        case CP_RUNNING:
            if (m->msg1 != EP_MGMT || type != 0xb || PWR(m->msg0) != 0x10)
                break;
            if (!f->no_sleep)
                cp_send(f, EP_MGMT, MK(0xb) | 0x10);
            f->cp = CP_QUIESCED;
            return;
        case CP_QUIESCED:
            if (m->msg1 != EP_MGMT || type != 6 || PWR(m->msg0) != 0x1)
                break;
            cp_send(f, EP_MGMT, MK(7) | 0x1);
            f->cp = CP_ASLEEP;
            return;
        case CP_ASLEEP:
            break;
    }
    cp_unexpected(f, m);
}

static void cp_run_changed(struct fake *f, bool run)
{
    if (run) {
        nvme_reset(f);
        f->buffers_answered = 0;
        f->eps_started = 0;
        f->a2i_n = 0;
        f->cp = (f->needs_reset || f->silent) ? CP_DEAD : CP_WAIT_WAKE;
    } else {
        /* R32: stopped without the reset afterwards, the next start gets no HELLO */
        f->cp = CP_OFF;
        f->needs_reset = true;
        f->a2i_n = 0;
    }
}

/* ------------------------------------------------------------------------------- */
/* NVMe                                                                              */

bool fake_whitelisted(int queue, uint8_t opcode, uint32_t nsid, uint32_t cdw10, uint32_t cdw11,
                      uint8_t flags)
{
    if (flags)
        return false;
    if (queue == 0) {
        switch (opcode) {
            case 0x05:
                return (cdw10 & 0xffff) == 1 && nsid == 0;
            case 0x01:
                return (cdw10 & 0xffff) == 1 && (cdw11 >> 16) == 1 && nsid == 0;
            case 0x06:
                return (cdw10 == 1 && nsid == 0) || (cdw10 == 0 && nsid == 1);
            case 0x00:
            case 0x04:
                return cdw10 == 1 && nsid == 0;
            default:
                return false;
        }
    }
    return opcode == 0x02 && nsid == 1;
}

uint8_t fake_disk_byte(uint64_t lba, uint32_t block_size, uint32_t i)
{
    uint64_t x = lba * block_size + i;
    x ^= x >> 13;
    x *= 0x9e3779b97f4a7c15ull;
    return (uint8_t)(x >> 56);
}

/* Writes bytes from the controller into the memory the PRPs describe. */
static bool prp_write(struct fake *f, uint64_t prp1, uint64_t prp2, uint32_t bytes,
                      void (*fill)(struct fake *, uint8_t *, uint32_t off, uint32_t len, void *),
                      void *ctx)
{
    uint32_t done = 0;
    uint32_t first = 4096 - (uint32_t)(prp1 & 0xfff);
    uint64_t *list = NULL;
    uint32_t pages_left, idx = 0;

    if (prp1 & 3) {
        fake_violation(f, "PRP1 %llx not dword aligned", (unsigned long long)prp1);
        return false;
    }
    if (first > bytes)
        first = bytes;
    uint8_t *p = fake_mem_at(f, prp1, first);
    if (!p)
        return false;
    fill(f, p, 0, first, ctx);
    done = first;

    pages_left = (bytes - done + 4095) / 4096;
    if (pages_left == 0) {
        if (prp2)
            fake_violation(f, "PRP2 %llx set for a one-page transfer", (unsigned long long)prp2);
        return true;
    }
    if (pages_left > 1) {
        if (prp2 & 0xfff) {
            fake_violation(f, "PRP list %llx not page aligned", (unsigned long long)prp2);
            return false;
        }
        if (pages_left > 512) {
            fake_violation(f, "transfer needs a chained PRP list");
            return false;
        }
        list = fake_mem_at(f, prp2, (size_t)pages_left * 8);
        if (!list)
            return false;
    }
    while (done < bytes) {
        uint64_t page = list ? list[idx++] : prp2;
        uint32_t n = bytes - done < 4096 ? bytes - done : 4096;
        if (page & 0xfff) {
            fake_violation(f, "PRP entry %llx not page aligned", (unsigned long long)page);
            return false;
        }
        p = fake_mem_at(f, page, n);
        if (!p)
            return false;
        fill(f, p, done, n, ctx);
        done += n;
    }
    return true;
}

struct read_ctx {
    uint64_t slba;
    uint32_t bs;
};

static void fill_disk(struct fake *f, uint8_t *p, uint32_t off, uint32_t len, void *vctx)
{
    struct read_ctx *c = vctx;
    (void)f;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t o = off + i;
        p[i] = fake_disk_byte(c->slba + o / c->bs, c->bs, o % c->bs);
    }
}

static void put_str(uint8_t *p, const char *s, uint32_t len)
{
    size_t n = strlen(s);
    for (uint32_t i = 0; i < len; i++)
        p[i] = i < n ? (uint8_t)s[i] : ' ';
}

static void fill_id_ctrl(struct fake *f, uint8_t *p, uint32_t off, uint32_t len, void *ctx)
{
    uint8_t page[4096];
    (void)ctx;
    memset(page, 0, sizeof(page));
    put_str(page + 4, "FAKE0123456789ABCDEF", 20);
    put_str(page + 24, "APPLE SSD AP0512Z", 40);
    put_str(page + 64, "2000.1.0", 8);
    page[77] = f->mdts;
    memcpy(p, page + off, len);
}

static void fill_id_ns(struct fake *f, uint8_t *p, uint32_t off, uint32_t len, void *ctx)
{
    uint8_t page[4096];
    (void)ctx;
    memset(page, 0, sizeof(page));
    memcpy(page, &f->nsze, 8);
    memcpy(page + 8, &f->nsze, 8);
    page[25] = 0; /* one LBA format */
    page[26] = 0; /* using format 0 */
    uint32_t lbaf = f->lbads << 16;
    memcpy(page + 128, &lbaf, 4);
    memcpy(p, page + off, len);
}

void fake_post_cqe(struct fake *f, int q, uint32_t tag, uint32_t status)
{
    uint64_t cq = q == 0 ? f->nvme_acq : f->iocq_dva;
    uint32_t *e;

    if ((f->cq_tail[q] + 1) % QSIZE == f->cq_head[q]) {
        fake_violation(f, "completion queue %d overflows: host does not move its head", q);
        return;
    }
    e = fake_mem_at(f, cq + (uint64_t)f->cq_tail[q] * 16, 16);
    if (!e)
        return;
    e[0] = e[1] = e[2] = 0;
    e[3] = tag | ((f->cq_phase[q] | (status << 1)) << 16);
    f->cq_tail[q]++;
    if (f->cq_tail[q] == QSIZE) {
        f->cq_tail[q] = 0;
        f->cq_phase[q] ^= 1;
    }
    f->busy[q] &= ~(1ull << tag);
    f->irq = true;
}

static void execute(struct fake *f, int q, uint32_t tag)
{
    uint64_t sq = q == 0 ? f->nvme_asq : f->iosq_dva;
    uint64_t tcbs = q == 0 ? f->nvmmu_asq_base : f->nvmmu_iosq_base;
    uint32_t *c = fake_mem_at(f, sq + (uint64_t)tag * 64, 64);
    uint8_t *t = fake_mem_at(f, tcbs + (uint64_t)tag * 128, 128);
    uint32_t status = 0;

    if (!c || !t)
        return;

    uint8_t opcode = (uint8_t)c[0], flags = (uint8_t)(c[0] >> 8);
    uint32_t ctag = c[0] >> 16, nsid = c[1];
    uint64_t prp1, prp2;
    memcpy(&prp1, (uint8_t *)c + 0x18, 8);
    memcpy(&prp2, (uint8_t *)c + 0x20, 8);
    uint32_t cdw10 = c[10], cdw11 = c[11], cdw12 = c[12];

    if (ctag != tag)
        fake_violation(f, "command in slot %u carries tag %u", tag, ctag);

    /* The command table entry must describe the same transfer (m1n1, Linux). */
    uint8_t want_flags = !prp1 ? 0 : (opcode & 1) ? 2 : 1;
    uint16_t len;
    uint64_t tprp1, tprp2;
    memcpy(&len, t + 4, 2);
    memcpy(&tprp1, t + 0x18, 8);
    memcpy(&tprp2, t + 0x20, 8);
    bool rest_zero = true;
    for (int i = 6; i < 128; i++)
        if ((i < 0x18 || i >= 0x28) && t[i])
            rest_zero = false;
    if (t[0] != 0 || t[1] != want_flags || t[2] != tag || t[3] != 0 || len != (cdw12 & 0xffff) ||
        tprp1 != prp1 || tprp2 != prp2 || !rest_zero)
        fake_violation(f, "command table entry for tag %u does not match the command", tag);

    if (f->ncmds < FAKE_MAX_CMDS) {
        f->cmds[f->ncmds].queue = q;
        f->cmds[f->ncmds].opcode = opcode;
        f->cmds[f->ncmds].nsid = nsid;
        f->cmds[f->ncmds].cdw10 = cdw10;
        f->ncmds++;
    }
    if (!fake_whitelisted(q, opcode, nsid, cdw10, cdw11, flags)) {
        fake_violation(f, "controller received a command off the whitelist: queue %d opcode "
                          "0x%02x nsid %u",
                       q, opcode, nsid);
        fake_post_cqe(f, q, tag, 0x1); /* invalid opcode */
        return;
    }

    if (q == 0) {
        if (opcode == f->hang_admin_opcode)
            return;
        if (opcode == f->fail_admin_opcode) {
            fake_post_cqe(f, q, tag, 0x2);
            return;
        }
        switch (opcode) {
            case 0x05:
                if (f->iocq_made || (cdw10 >> 16) != QSIZE - 1 || cdw11 != 3 ||
                    !fake_mem_at(f, prp1, QSIZE * 16)) {
                    fake_violation(f, "bad create CQ: cdw10 %x cdw11 %x", cdw10, cdw11);
                    status = 0x2;
                    break;
                }
                f->iocq_made = true;
                f->iocq_dva = prp1;
                f->cq_tail[1] = f->cq_head[1] = 0;
                f->cq_phase[1] = 1;
                break;
            case 0x01:
                if (f->iosq_made || !f->iocq_made || (cdw10 >> 16) != QSIZE - 1 ||
                    (cdw11 & 0xffff) != 1 || !fake_mem_at(f, prp1, QSIZE * 64)) {
                    fake_violation(f, "bad create SQ: cdw10 %x cdw11 %x", cdw10, cdw11);
                    status = 0x2;
                    break;
                }
                f->iosq_made = true;
                f->iosq_dva = prp1;
                break;
            case 0x06:
                if (!prp_write(f, prp1, prp2, 4096, cdw10 == 1 ? fill_id_ctrl : fill_id_ns, NULL))
                    status = 0x2;
                break;
            case 0x00:
                if (!f->iosq_made)
                    status = 0x101; /* invalid queue identifier */
                f->iosq_made = false;
                break;
            case 0x04:
                if (!f->iocq_made || f->iosq_made)
                    status = 0x10c; /* invalid queue deletion */
                else
                    f->iocq_made = false;
                break;
        }
        fake_post_cqe(f, q, tag, status);
        return;
    }

    /* read */
    struct read_ctx rc;
    uint32_t nlb = (cdw12 & 0xffff) + 1;
    rc.slba = cdw10 | ((uint64_t)cdw11 << 32);
    rc.bs = 1u << f->lbads;
    if (rc.slba + nlb > f->nsze || (cdw12 >> 16)) {
        fake_violation(f, "read of %u blocks at %llu outside the namespace", nlb,
                       (unsigned long long)rc.slba);
        fake_post_cqe(f, q, tag, 0x80);
        return;
    }
    if (f->hang_reads)
        return;
    if (f->fail_read_status) {
        fake_post_cqe(f, q, tag, (uint32_t)f->fail_read_status);
        return;
    }
    if (!prp_write(f, prp1, prp2, nlb * rc.bs, fill_disk, &rc))
        status = 0x2;
    fake_post_cqe(f, q, tag, status);
}

static void ring(struct fake *f, int q, uint32_t tag)
{
    f->doorbells++;
    if (!(f->nvme_cc & 1) || !(f->nvme_csts & 1)) {
        fake_violation(f, "doorbell while the controller is not enabled");
        return;
    }
    if (!(f->linear_sq_ctrl & 1) || f->nvmmu_num != QSIZE - 1 || !f->nvmmu_asq_base ||
        !f->nvmmu_iosq_base) {
        fake_violation(f, "doorbell before the NVMMU is set up");
        return;
    }
    if (tag >= QSIZE || (q == 0 && tag != 0)) {
        fake_violation(f, "doorbell for tag %u on queue %d", tag, q);
        return;
    }
    if (q == 1) {
        if (!f->iosq_made) {
            fake_violation(f, "I/O doorbell before the I/O queue exists");
            return;
        }
        if (f->secure_bar && (f->ioq_cmds != f->iosq_dva || f->ioq_cqes != f->iocq_dva)) {
            /* m1n1: without these the ANS crashes */
            fake_violation(f, "I/O queue registers 0x1200/0x1208 not set on an M4");
            return;
        }
    }
    if (f->busy[q] & (1ull << tag)) {
        fake_violation(f, "tag %u rung again while in flight", tag);
        return;
    }
    f->busy[q] |= 1ull << tag;
    f->pending[q][f->npending[q]++] = (int)tag;
}

void fake_tick(struct fake *f)
{
    /* the coprocessor takes in what it was sent */
    for (uint32_t i = 0; i < f->a2i_n; i++)
        cp_message(f, &f->a2i[i]);
    f->a2i_n = 0;
    if (f->crash_now && f->cp == CP_RUNNING) {
        f->crash_now = false;
        cp_crash(f);
    }

    /* controller status */
    if (f->cp == CP_RUNNING || f->cp == CP_QUIESCED) {
        if ((f->nvme_cc & 1) && !(f->nvme_csts & 1) && !f->never_ready && f->nvme_asq &&
            f->nvme_acq && f->nvme_aqa == (((QSIZE - 1) << 16) | (QSIZE - 1)))
            f->nvme_csts |= 1;
        if (((f->nvme_cc >> 14) & 3) == 1)
            f->nvme_csts = (f->nvme_csts & ~0xcu) | (2u << 2);
    }
    if (!(f->nvme_cc & 1))
        f->nvme_csts &= ~1u;

    /* commands */
    if (f->cp == CP_RUNNING) {
        for (int q = 0; q < 2; q++) {
            uint32_t n = f->npending[q];
            f->npending[q] = 0;
            for (uint32_t i = 0; i < n; i++)
                execute(f, q, (uint32_t)f->pending[q][i]);
        }
    }
}

/* ------------------------------------------------------------------------------- */
/* Registers                                                                         */

/* The NVMe registers are there once the coprocessor has finished the RTKit handshake. */
static bool nvme_up(struct fake *f)
{
    return f->cp >= CP_WAIT_AP_ON && f->cp != CP_CRASHED; /* after a crash: never */
}

static uint64_t asc_read(struct fake *f, uint32_t off, unsigned width)
{
    switch (off) {
        case F_CPU_CONTROL:
            return f->cpu_control;
        case F_A2I_CONTROL:
            return (f->a2i_n >= f->a2i_capacity ? F_FULL : 0) | (f->a2i_n == 0 ? F_EMPTY : 0);
        case F_I2A_CONTROL:
            return (f->i2a_n == 0 ? F_EMPTY : 0) | (f->i2a_n == FAKE_FIFO ? F_FULL : 0);
        case F_I2A_RECV0:
            if (width != 8 || f->i2a_n == 0) {
                fake_violation(f, "mailbox receive with width %u, %u waiting", width, f->i2a_n);
                return 0;
            }
            f->i2a_recv0_read = true;
            return f->i2a[f->i2a_head].msg0;
        case F_I2A_RECV1: {
            if (width != 8 || f->i2a_n == 0 || !f->i2a_recv0_read) {
                fake_violation(f, "mailbox receive out of order");
                return 0;
            }
            uint32_t v = f->i2a[f->i2a_head].msg1;
            f->i2a_head = (f->i2a_head + 1) % FAKE_FIFO;
            f->i2a_n--;
            f->i2a_recv0_read = false;
            return v;
        }
    }
    fake_violation(f, "read of unknown ASC register %x", off);
    return 0;
}

static void asc_write(struct fake *f, uint32_t off, unsigned width, uint64_t val)
{
    switch (off) {
        case F_CPU_CONTROL: {
            bool was = (f->cpu_control & F_RUN) != 0, now = (val & F_RUN) != 0;
            if (width != 4 || (val & ~(uint64_t)F_RUN)) {
                fake_violation(f, "odd write %llx to the CPU control register",
                               (unsigned long long)val);
            }
            f->cpu_control = (uint32_t)val;
            if (was != now) {
                if (now)
                    f->run_sets++;
                cp_run_changed(f, now);
            }
            return;
        }
        case F_A2I_SEND0:
            if (width != 8)
                break;
            f->a2i_send0 = val;
            f->a2i_send0_written = true;
            return;
        case F_A2I_SEND1:
            if (width != 8 || !f->a2i_send0_written)
                break;
            if (f->a2i_n >= f->a2i_capacity) {
                fake_violation(f, "mailbox send while the FIFO is full");
                return;
            }
            f->a2i[f->a2i_n].msg0 = f->a2i_send0;
            f->a2i[f->a2i_n].msg1 = (uint32_t)val;
            f->a2i_n++;
            f->a2i_send0_written = false;
            return;
    }
    fake_violation(f, "write %llx of width %u to ASC register %x", (unsigned long long)val, width,
                   off);
}

static uint32_t *nvme_reg(struct fake *f, uint32_t off, bool *lo_hi_high, uint64_t **wide)
{
    *lo_hi_high = false;
    *wide = NULL;
    switch (off & ~4u) {
        case F_ASQ:
            *wide = &f->nvme_asq;
            break;
        case F_ACQ:
            *wide = &f->nvme_acq;
            break;
        case F_NVMMU_ASQ_BASE:
            *wide = &f->nvmmu_asq_base;
            break;
        case F_NVMMU_IOSQ_BASE:
            *wide = &f->nvmmu_iosq_base;
            break;
        case F_IOQ_CMDS:
            *wide = &f->ioq_cmds;
            break;
        case F_IOQ_CQES:
            *wide = &f->ioq_cqes;
            break;
    }
    if (*wide) {
        *lo_hi_high = (off & 4) != 0;
        return NULL;
    }
    switch (off) {
        case F_CC:
            return &f->nvme_cc;
        case F_CSTS:
            return &f->nvme_csts;
        case F_AQA:
            return &f->nvme_aqa;
        case F_LINEAR_SQ_CTRL:
            return &f->linear_sq_ctrl;
        case F_MAX_PEND:
            return &f->max_pend;
        case F_NVMMU_NUM:
            return &f->nvmmu_num;
    }
    return NULL;
}

static uint64_t nvme_read(struct fake *f, uint32_t off)
{
    bool hi;
    uint64_t *wide;
    uint32_t *r = nvme_reg(f, off, &hi, &wide);

    if (off == F_BOOT_STATUS)
        return (f->cp >= CP_RUNNING && f->cp <= CP_ASLEEP && !f->never_boot_status) ? F_BOOT_OK
                                                                                     : 0;
    if (off == F_TCB_STAT)
        return f->tcb_stat;
    if (wide)
        return hi ? (*wide >> 32) : (uint32_t)*wide;
    if (r)
        return *r;
    fake_violation(f, "read of unknown NVMe register %x", off);
    return 0;
}

static void nvme_write(struct fake *f, uint32_t off, uint32_t val)
{
    bool hi;
    uint64_t *wide;
    uint32_t *r = nvme_reg(f, off, &hi, &wide);

    switch (off) {
        case F_DB_ASQ:
            ring(f, 0, val);
            return;
        case F_DB_IOSQ:
            ring(f, 1, val);
            return;
        case F_DB_ACQ:
        case F_DB_IOCQ:
            if (val >= QSIZE)
                fake_violation(f, "completion doorbell %u", val);
            f->cq_head[off == F_DB_ACQ ? 0 : 1] = val;
            return;
        case F_TCB_INVAL:
            f->last_inval = val;
            return;
        case F_CC:
            if ((val & 1) && !(f->nvme_cc & 1) && f->nvme_csts & 1)
                fake_violation(f, "CC.EN set before CSTS.RDY cleared");
            break;
        case F_AQA:
        case F_ASQ:
        case F_ASQ + 4:
        case F_ACQ:
        case F_ACQ + 4:
            if (f->nvme_cc & 1)
                fake_violation(f, "admin queue registers written while enabled");
            break;
        case F_IOQ_CMDS:
        case F_IOQ_CMDS + 4:
        case F_IOQ_CQES:
        case F_IOQ_CQES + 4:
            if (!f->secure_bar)
                fake_violation(f, "0x1200/0x1208 written on a controller without them");
            break;
        case F_MAX_PEND:
            if (val != (((QSIZE - 1) << 16) | (QSIZE - 1)))
                fake_violation(f, "max pending commands %x", val);
            break;
        case F_NVMMU_NUM:
            if (val != QSIZE - 1)
                fake_violation(f, "NVMMU_NUM %x", val);
            break;
    }
    if (wide) {
        if (hi)
            *wide = (*wide & 0xffffffffull) | ((uint64_t)val << 32);
        else
            *wide = (*wide & ~0xffffffffull) | val;
        return;
    }
    if (r) {
        if (off != F_CSTS)
            *r = val;
        return;
    }
    fake_violation(f, "write %x to unknown NVMe register %x", val, off);
}

static void pmgr_write(struct fake *f, uint32_t val)
{
    uint32_t old = f->pmgr;
    uint32_t changed = old ^ val;

    if (f->pmgr_inactive)
        fake_violation(f, "power state register written while the controller is not active");
    if (changed & 0xf)
        fake_violation(f, "power state target changed: %08x -> %08x", old, val);
    if (changed & ~((1u << 10) | (1u << 31)))
        fake_violation(f, "power state register bits other than 10 and 31 changed");
    f->pmgr = val;

    bool dis = (val >> 10) & 1, rst = (val >> 31) & 1;
    switch (f->reset_step) {
        case 0:
            if (dis && !rst) {
                /* R40: the reset is done with the coprocessor out of RUN */
                if (f->cpu_control & F_RUN)
                    fake_violation(f, "reset while the RUN bit is set");
                f->reset_step = 1;
                return;
            }
            break;
        case 1:
            if (dis && rst) {
                f->reset_step = 2;
                f->reset_t0 = f->now_us;
                return;
            }
            break;
        case 2:
            if (dis && !rst) {
                if (f->now_us - f->reset_t0 < 10)
                    fake_violation(f, "reset held for less than 10 us");
                f->reset_step = 3;
                return;
            }
            break;
        case 3:
            if (!dis && !rst) {
                f->reset_step = 0;
                f->resets++;
                f->needs_reset = false;
                return;
            }
            break;
    }
    fake_violation(f, "reset steps out of order at step %d: %08x", f->reset_step, val);
}

uint64_t fake_reg_read(struct fake *f, int rgn, uint32_t off, unsigned width)
{
    switch (rgn) {
        case 0:
            if (off == F_FORBIDDEN) {
                fake_violation(f, "read of the forbidden register 0x24008");
                return 0;
            }
            if (width != 4 || !nvme_up(f)) {
                fake_violation(f, "NVMe register %x read (width %u) while the coprocessor is "
                                  "not running",
                               off, width);
                return 0;
            }
            return nvme_read(f, off);
        case 1:
            if (width == 4 && (off == F_I2A_RECV0 || off == F_I2A_RECV1)) {
                fake_violation(f, "32-bit mailbox receive");
                return 0;
            }
            return asc_read(f, off, width);
        case 2: {
            int e;
            if (width != 4 || !sart_offset_entry(f, off, &e)) {
                fake_violation(f, "read of unknown SART register %x", off);
                return 0;
            }
            return f->sart[off / 4];
        }
        case 3:
            if (width != 4 || off != 0) {
                fake_violation(f, "bad power state register access");
                return 0;
            }
            if (f->pmgr_all_ones)
                return 0xffffffffu;
            return f->pmgr_inactive ? (f->pmgr & ~0xf0u) | 0x40u : f->pmgr;
    }
    fake_violation(f, "read of region %d", rgn);
    return 0;
}

void fake_reg_write(struct fake *f, int rgn, uint32_t off, unsigned width, uint64_t val)
{
    f->reg_writes++;
    switch (rgn) {
        case 0:
            if (off == F_FORBIDDEN) {
                fake_violation(f, "write of the forbidden register 0x24008");
                return;
            }
            if (width != 4 || !nvme_up(f)) {
                fake_violation(f, "NVMe register %x written (width %u) while the coprocessor is "
                                  "not running",
                               off, width);
                return;
            }
            nvme_write(f, off, (uint32_t)val);
            return;
        case 1:
            asc_write(f, off, width, val);
            return;
        case 2: {
            int e;
            if (width != 4 || !sart_offset_entry(f, off, &e)) {
                fake_violation(f, "write of unknown SART register %x", off);
                return;
            }
            if (e < IBOOT_ENTRIES) {
                fake_violation(f, "write to iBoot's SART entry %d", e);
                return;
            }
            if (off >= 0x40) {
                /* an address or size register: its entry must be off while it changes */
                uint32_t flags;
                uint64_t p, sz;
                sart_decode(f, e, &flags, &p, &sz);
                if (flags)
                    fake_violation(f, "SART entry %d changed while it is on", e);
            }
            f->sart[off / 4] = (uint32_t)val;
            return;
        }
        case 3:
            if (width != 4 || off != 0) {
                fake_violation(f, "bad power state register access");
                return;
            }
            pmgr_write(f, (uint32_t)val);
            return;
    }
    fake_violation(f, "write to region %d", rgn);
}
