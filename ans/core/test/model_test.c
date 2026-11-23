/* SPDX-License-Identifier: MIT */
/*
 * The model test of the core: the core runs against the fake controller (fake_ans.c)
 * through the model environment (env_model.c). Each test starts from a fresh fake,
 * environment and core. After every test the fake must have seen no violation, the
 * environment must have refused no access, and a stopped core must have given back
 * all memory and every SART entry it added.
 *
 * Where the reset goes is R40's answer: before the coprocessor is started, and after
 * RUN is cleared whenever the controller is brought down; never with RUN set.
 *
 *   model_test            run everything
 *   model_test -v         also print the core's log
 *   model_test <name>     run the tests whose name contains <name>
 */

#include "../ans_internal.h"
#include "env_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;
static bool g_verbose;
static bool g_test_failed;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            g_test_failed = true;                                                      \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                              \
    } while (0)

static void fail_here(const char *file, int line, const char *what)
{
    g_checks++;
    g_failures++;
    g_test_failed = true;
    printf("    FAIL %s:%d: %s\n", file, line, what);
}

#define FAIL(what) fail_here(__FILE__, __LINE__, (what))

#define KIB 1024u
#define MAX_XFER (252u * KIB) /* the Windows miniport's MaximumTransferLength */

/* ------------------------------------------------------------------------------- */

struct rig {
    struct fake f;
    struct ans_env env;
    struct ans_core core;
    struct ans_config cfg;
    uint32_t expected_violations;
};

struct done_rec {
    uint32_t count;
    bool done[64];
    int status[64];
};

static void on_done(void *ctx, uint16_t tag, int status)
{
    struct done_rec *d = ctx;
    if (tag < 64) {
        if (d->done[tag])
            printf("    tag %u completed twice\n", tag);
        d->done[tag] = true;
        d->status[tag] = status;
    }
    d->count++;
}

static struct rig *rig_new(uint32_t sart_version, bool interrupts)
{
    struct rig *r = calloc(1, sizeof(*r));
    if (!r) {
        printf("out of memory\n");
        exit(2);
    }
    fake_init(&r->f, sart_version);
    env_init(&r->env, &r->f, &r->core);
    r->env.verbose = g_verbose;
    r->env.interrupts = interrupts;
    r->cfg.ioq_registers = true;
    r->cfg.sart_version = sart_version;
    r->cfg.io_depth = ANS_MAX_IO_DEPTH;
    r->cfg.max_transfer = MAX_XFER;
    return r;
}

/* The explicit whitelist of the test, written apart from the core's and the fake's. */
static bool test_whitelisted(const struct fake_cmd_record *c)
{
    if (c->queue == 1)
        return c->opcode == 0x02 && c->nsid == 1;
    switch (c->opcode) {
        case 0x00: /* delete I/O SQ 1 */
        case 0x04: /* delete I/O CQ 1 */
            return c->cdw10 == 1 && c->nsid == 0;
        case 0x01: /* create I/O SQ 1 */
        case 0x05: /* create I/O CQ 1 */
            return (c->cdw10 & 0xffff) == 1 && c->nsid == 0;
        case 0x06: /* identify controller, namespace 1 */
            return (c->cdw10 == 1 && c->nsid == 0) || (c->cdw10 == 0 && c->nsid == 1);
    }
    return false;
}

static void rig_finish(struct rig *r)
{
    struct fake *f = &r->f;

    CHECK(f->violations == r->expected_violations);
    if (f->violations != r->expected_violations)
        printf("    fake saw %u violations, expected %u; first: %s\n", f->violations,
               r->expected_violations, f->first_violation);
    CHECK(r->env.refused == 0);
    CHECK(r->env.contract_broken == 0);
    for (uint32_t i = 0; i < f->ncmds; i++) {
        if (!test_whitelisted(&f->cmds[i])) {
            FAIL("a command off the whitelist reached the controller");
            printf("    queue %d opcode 0x%02x nsid %u\n", f->cmds[i].queue, f->cmds[i].opcode,
                   f->cmds[i].nsid);
        }
    }

    enum ans_state s = ans_get_state(&r->core);
    if (s == ANS_OFF || s == ANS_FAILED || s == ANS_CRASHED) {
        CHECK(fake_live_env_blocks(f) == 0);
        CHECK(fake_sart_unchanged_iboot(f));
        CHECK((f->cpu_control & 0x10) == 0);
    }

    for (uint32_t i = 0; i < f->nblocks; i++)
        if (f->blocks[i].live)
            free(f->blocks[i].va);
    free(r);
}

/* The calls that must not wait, allocate or free: the environment checks. */
static bool t_isr(struct rig *r, ans_done_fn *done, void *ctx)
{
    r->env.no_wait++;
    bool ret = ans_isr(&r->core, done, ctx);
    r->env.no_wait--;
    return ret;
}

static void t_mbox(struct rig *r)
{
    r->env.no_wait++;
    ans_mbox_service(&r->core);
    r->env.no_wait--;
}

static int t_read(struct rig *r, uint16_t tag, uint64_t lba, uint32_t blocks,
                  const struct ans_segment *segs, uint32_t nsegs)
{
    r->env.no_wait++;
    int ret = ans_read(&r->core, tag, lba, blocks, segs, nsegs);
    r->env.no_wait--;
    return ret;
}

static void t_halt(struct rig *r)
{
    r->env.no_wait++;
    ans_halt(&r->core);
    r->env.no_wait--;
}

static int t_probe(struct rig *r)
{
    r->env.no_wait++;
    int ret = ans_probe(&r->core, &r->env);
    r->env.no_wait--;
    return ret;
}

/* What happens outside the core between calls: time passes, the device works. */
static void poll(struct rig *r, struct done_rec *d)
{
    fake_advance(&r->f, 10);
    fake_tick(&r->f);
    t_isr(r, on_done, d);
    t_mbox(r);
}

static int start(struct rig *r)
{
    struct done_rec dummy;
    memset(&dummy, 0, sizeof(dummy));
    r->env.done = on_done;
    r->env.done_ctx = &dummy;
    int ret = ans_start(&r->core, &r->env, &r->cfg);
    r->env.done = NULL;
    r->env.done_ctx = NULL;
    return ret;
}

static void stop(struct rig *r)
{
    ans_stop(&r->core);
}

/* Memory a read goes to, as the environment would hand it over. */
struct buf {
    void *va;
    uint64_t dva;
    size_t size;
};

static struct buf buf_new(struct rig *r, size_t size)
{
    struct buf b;
    if (!fake_mem_alloc(&r->f, size, false, &b.va, &b.dva)) {
        printf("out of fake memory\n");
        exit(2);
    }
    b.size = size;
    return b;
}

static bool data_ok(const uint8_t *p, uint64_t lba, uint32_t bytes, uint32_t bs)
{
    for (uint32_t i = 0; i < bytes; i++)
        if (p[i] != fake_disk_byte(lba + i / bs, bs, i % bs))
            return false;
    return true;
}

static uint32_t count_cmds(struct fake *f, int queue, uint8_t opcode)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < f->ncmds; i++)
        if (f->cmds[i].queue == queue && f->cmds[i].opcode == opcode)
            n++;
    return n;
}

/* ------------------------------------------------------------------------------- */
/* Bring-up, identify, bring-down                                                    */

static void check_start_stop(bool interrupts)
{
    struct rig *r = rig_new(3, interrupts);
    struct fake *f = &r->f;

    CHECK(t_probe(r) == ANS_OK);
    CHECK(start(r) == ANS_OK);
    CHECK(ans_get_state(&r->core) == ANS_READY);
    CHECK(f->cp == CP_RUNNING);
    CHECK(f->iocq_made && f->iosq_made);
    CHECK(fake_sart_entries_in_use(f) == 2 + 3);

    const struct ans_info *info = ans_get_info(&r->core);
    CHECK(info->blocks == f->nsze);
    CHECK(info->block_size == 4096);
    CHECK(info->mdts_bytes == 1024 * KIB);
    CHECK(strcmp(info->serial, "FAKE0123456789ABCDEF") == 0);
    CHECK(strncmp(info->model, "APPLE SSD AP0512Z   ", 20) == 0 && strlen(info->model) == 40);
    CHECK(strcmp(info->firmware, "2000.1.0") == 0);

    /* identify controller, identify namespace, create CQ, create SQ, in that order */
    CHECK(f->ncmds == 4);
    CHECK(f->cmds[0].opcode == 0x06 && f->cmds[0].cdw10 == 1);
    CHECK(f->cmds[1].opcode == 0x06 && f->cmds[1].cdw10 == 0 && f->cmds[1].nsid == 1);
    CHECK(f->cmds[2].opcode == 0x05);
    CHECK(f->cmds[3].opcode == 0x01);
    CHECK(f->resets == 1u);

    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_OFF);
    CHECK(f->cp == CP_OFF);
    CHECK((f->cpu_control & 0x10) == 0);
    CHECK(f->ncmds == 6);
    CHECK(f->cmds[4].opcode == 0x00 && f->cmds[5].opcode == 0x04);
    CHECK(!f->iocq_made && !f->iosq_made);
    CHECK(fake_sart_entries_in_use(f) == 2);
    CHECK(fake_live_env_blocks(f) == 0);
    CHECK(f->resets == 1u + 1u);
    /* m1n1's own driver does not reset before it starts: it needs us to have reset */
    CHECK(!f->needs_reset);

    /* stopping again does nothing */
    uint32_t writes = f->reg_writes;
    stop(r);
    CHECK(f->reg_writes == writes);
    rig_finish(r);
}

static void test_start_stop_polling(void)
{
    check_start_stop(false);
}

static void test_start_stop_interrupts(void)
{
    check_start_stop(true);
}

static void test_sart_versions(void)
{
    static const uint32_t versions[] = {0, 2, 3, 4};
    for (unsigned i = 0; i < 4; i++) {
        struct rig *r = rig_new(versions[i], false);
        CHECK(start(r) == ANS_OK);
        CHECK(fake_sart_entries_in_use(&r->f) == 2 + 3);
        stop(r);
        CHECK(ans_get_state(&r->core) == ANS_OFF);
        CHECK(fake_sart_entries_in_use(&r->f) == 2);
        rig_finish(r);
    }
}

static void test_without_secure_bar(void)
{
    struct rig *r = rig_new(2, false);
    r->f.secure_bar = false;
    r->cfg.ioq_registers = false;
    CHECK(start(r) == ANS_OK);
    CHECK(r->f.ioq_cmds == 0 && r->f.ioq_cqes == 0);
    struct buf b = buf_new(r, 4096);
    struct ans_segment seg = {b.dva, 4096};
    struct done_rec d;
    memset(&d, 0, sizeof(d));
    CHECK(t_read(r, 0, 0, 1, &seg, 1) == ANS_OK);
    poll(r, &d);
    CHECK(d.done[0] && d.status[0] == 0);
    stop(r);
    rig_finish(r);
}

/* R32: brought down and up again in the same boot, twice. */
static void test_restart_same_boot(void)
{
    struct rig *r = rig_new(3, false);
    for (int round = 0; round < 3; round++) {
        CHECK(start(r) == ANS_OK);
        struct buf b = buf_new(r, 4096);
        struct ans_segment seg = {b.dva, 4096};
        struct done_rec d;
        memset(&d, 0, sizeof(d));
        CHECK(t_read(r, 5, 1, 1, &seg, 1) == ANS_OK);
        poll(r, &d);
        CHECK(d.done[5] && d.status[5] == 0 && data_ok(b.va, 1, 4096, 4096));
        stop(r);
        CHECK(ans_get_state(&r->core) == ANS_OFF);
    }
    CHECK(r->f.resets == 6u);
    rig_finish(r);
}

/* The owner before us stopped the controller without the reset (R32 round 3). */
static void test_previous_owner_did_not_reset(void)
{
    struct rig *r = rig_new(3, false);
    r->f.needs_reset = true;

    /* R40: the reset before the start brings it back */
    CHECK(start(r) == ANS_OK);
    CHECK(ans_get_state(&r->core) == ANS_READY);
    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_OFF);
    rig_finish(r);
}

/* ------------------------------------------------------------------------------- */
/* Reads                                                                             */

static void check_one_read(struct rig *r, uint16_t tag, uint64_t lba, uint32_t blocks,
                           struct ans_segment *segs, uint32_t nsegs, uint8_t **vas)
{
    struct done_rec d;
    uint32_t bs = 4096;

    memset(&d, 0, sizeof(d));
    CHECK(t_read(r, tag, lba, blocks, segs, nsegs) == ANS_OK);
    poll(r, &d);
    CHECK(d.count == 1 && d.done[tag] && d.status[tag] == 0);

    /* the data, piece by piece, in order */
    uint64_t off = 0;
    for (uint32_t i = 0; i < nsegs; i++) {
        for (uint32_t j = 0; j < segs[i].len; j++) {
            uint64_t o = off + j;
            if (vas[i][j] != fake_disk_byte(lba + o / bs, bs, (uint32_t)(o % bs))) {
                FAIL("read data differs");
                return;
            }
        }
        off += segs[i].len;
    }
}

static void check_reads(bool interrupts)
{
    struct rig *r = rig_new(3, interrupts);
    CHECK(start(r) == ANS_OK);

    /* one page */
    struct buf b1 = buf_new(r, 4096);
    struct ans_segment s1 = {b1.dva, 4096};
    uint8_t *v1[] = {b1.va};
    check_one_read(r, 0, 0, 1, &s1, 1, v1);

    /* two pages: PRP2 is the second page */
    struct buf b2 = buf_new(r, 8192);
    struct ans_segment s2 = {b2.dva, 8192};
    uint8_t *v2[] = {b2.va};
    check_one_read(r, 1, 100, 2, &s2, 1, v2);

    /* starting in the middle of a page: three pages, PRP list */
    struct buf b3 = buf_new(r, 3 * 4096);
    struct ans_segment s3 = {b3.dva + 0x10, 8192};
    uint8_t *v3[] = {(uint8_t *)b3.va + 0x10};
    check_one_read(r, 2, 7, 2, &s3, 1, v3);

    /* three pieces: ends on a page boundary, whole pages, ends in a page */
    struct buf b4a = buf_new(r, 4096), b4b = buf_new(r, 3 * 4096), b4c = buf_new(r, 4096);
    struct ans_segment s4[] = {{b4a.dva + 0x800, 0x800}, {b4b.dva, 0x3000}, {b4c.dva, 0x800}};
    uint8_t *v4[] = {(uint8_t *)b4a.va + 0x800, b4b.va, b4c.va};
    check_one_read(r, 3, 1000, 4, s4, 3, v4);

    /* the largest read, 252 KiB, from an odd place: 64 pages */
    struct buf b5 = buf_new(r, MAX_XFER + 4096);
    struct ans_segment s5 = {b5.dva + 0x204, MAX_XFER};
    uint8_t *v5[] = {(uint8_t *)b5.va + 0x204};
    check_one_read(r, 62, 5000, MAX_XFER / 4096, &s5, 1, v5);

    /* the last block */
    struct ans_segment s6 = {b1.dva, 4096};
    uint8_t *v6[] = {b1.va};
    check_one_read(r, 4, r->f.nsze - 1, 1, &s6, 1, v6);

    stop(r);
    rig_finish(r);
}

static void test_reads_polling(void)
{
    check_reads(false);
}

static void test_reads_interrupts(void)
{
    check_reads(true);
}

/* All 63 tags in flight at once, completed in one go. */
static void test_reads_in_flight(void)
{
    struct rig *r = rig_new(3, false);
    struct buf bufs[ANS_MAX_IO_DEPTH];
    struct done_rec d;

    CHECK(start(r) == ANS_OK);
    memset(&d, 0, sizeof(d));
    for (uint16_t t = 0; t < ANS_MAX_IO_DEPTH; t++) {
        bufs[t] = buf_new(r, 2 * 4096);
        struct ans_segment s = {bufs[t].dva, 2 * 4096};
        CHECK(t_read(r, t, 3000u + 17u * t, 2, &s, 1) == ANS_OK);
    }
    /* a tag in flight cannot be used again */
    struct ans_segment s = {bufs[0].dva, 4096};
    CHECK(t_read(r, 0, 0, 1, &s, 1) == ANS_ERR_BUSY);

    for (int i = 0; i < 3 && d.count < ANS_MAX_IO_DEPTH; i++)
        poll(r, &d);
    CHECK(d.count == ANS_MAX_IO_DEPTH);
    for (uint16_t t = 0; t < ANS_MAX_IO_DEPTH; t++)
        CHECK(d.done[t] && d.status[t] == 0 && data_ok(bufs[t].va, 3000u + 17u * t, 8192, 4096));

    /* the completion queue wraps: 64 entries, so do it again */
    memset(&d, 0, sizeof(d));
    for (uint16_t t = 0; t < ANS_MAX_IO_DEPTH; t++) {
        struct ans_segment s2 = {bufs[t].dva, 4096};
        CHECK(t_read(r, t, t, 1, &s2, 1) == ANS_OK);
    }
    poll(r, &d);
    CHECK(d.count == ANS_MAX_IO_DEPTH);
    for (uint16_t t = 0; t < ANS_MAX_IO_DEPTH; t++)
        CHECK(d.done[t] && data_ok(bufs[t].va, t, 4096, 4096));

    stop(r);
    rig_finish(r);
}

/* Reads beyond block 2^32 put the high half of the LBA into CDW11. */
static void test_read_high_lba(void)
{
    struct rig *r = rig_new(3, false);
    r->f.nsze = 1ull << 33;
    CHECK(start(r) == ANS_OK);
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    uint8_t *v[] = {b.va};
    check_one_read(r, 9, (1ull << 32) + 5, 1, &s, 1, v);
    stop(r);
    rig_finish(r);
}

static void test_read_error_status(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    r->f.fail_read_status = 0x281; /* media error: unrecovered read */
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    memset(&d, 0, sizeof(d));
    CHECK(t_read(r, 1, 10, 1, &s, 1) == ANS_OK);
    poll(r, &d);
    CHECK(d.done[1] && d.status[1] == 0x281);
    CHECK(ans_get_state(&r->core) == ANS_READY);
    stop(r);
    rig_finish(r);
}

static void test_read_refused(void)
{
    struct rig *r = rig_new(3, false);
    struct buf b = buf_new(r, 4 * 4096);
    struct ans_segment one = {b.dva, 4096};

    /* not started */
    CHECK(t_read(r, 0, 0, 1, &one, 1) == ANS_ERR_STATE);
    CHECK(start(r) == ANS_OK);
    uint32_t bells = r->f.doorbells;

    CHECK(t_read(r, ANS_MAX_IO_DEPTH, 0, 1, &one, 1) == ANS_ERR_INVALID); /* tag */
    CHECK(t_read(r, 0, 0, 0, &one, 1) == ANS_ERR_INVALID);                /* 0 blocks */
    CHECK(t_read(r, 0, r->f.nsze, 1, &one, 1) == ANS_ERR_INVALID);       /* past end */
    CHECK(t_read(r, 0, r->f.nsze - 1, 2, &one, 1) == ANS_ERR_INVALID);
    CHECK(t_read(r, 0, 0, 2, &one, 1) == ANS_ERR_INVALID); /* memory too short */
    CHECK(t_read(r, 0, 0, 1, NULL, 0) == ANS_ERR_INVALID);

    /* longer than max_transfer */
    struct buf big = buf_new(r, 256 * KIB);
    struct ans_segment s_big = {big.dva, 256 * KIB};
    CHECK(t_read(r, 0, 0, 64, &s_big, 1) == ANS_ERR_INVALID);

    /* PRP rules */
    struct ans_segment odd = {b.dva + 2, 4096};
    CHECK(t_read(r, 0, 0, 1, &odd, 1) == ANS_ERR_PRP);
    struct ans_segment mid_start[] = {{b.dva, 4096}, {b.dva + 0x1800, 4096}};
    CHECK(t_read(r, 0, 0, 2, mid_start, 2) == ANS_ERR_PRP);
    struct ans_segment mid_end[] = {{b.dva, 0x800}, {b.dva + 0x2000, 0x1800}};
    CHECK(t_read(r, 0, 0, 1, mid_end, 2) == ANS_ERR_PRP);
    struct ans_segment empty[] = {{b.dva, 0}, {b.dva + 0x1000, 0x1000}};
    CHECK(t_read(r, 0, 0, 1, empty, 2) == ANS_ERR_PRP);

    CHECK(r->f.doorbells == bells);
    CHECK(r->core.nvme.inflight == 0);
    stop(r);
    rig_finish(r);
}

/* ------------------------------------------------------------------------------- */
/* The whitelist                                                                     */

static bool listed(enum ans_nvme_queue q, uint8_t opcode)
{
    if (q == ANS_NVME_IOQ)
        return opcode == 0x02;
    return opcode == 0x00 || opcode == 0x01 || opcode == 0x04 || opcode == 0x05 ||
           opcode == 0x06;
}

static bool submit_denied(struct rig *r, enum ans_nvme_queue q, uint16_t tag,
                          const struct ans_nvme_cmd *cmd)
{
    struct ans_nvme_queue_mem *qm = &r->core.nvme.q[q];
    uint8_t before_cmd[64], before_tcb[128];
    uint32_t bells = r->f.doorbells, lines = env_log_count(&r->env, "nvme: denied");

    memcpy(before_cmd, (uint8_t *)qm->cmds.va + 64 * tag, 64);
    memcpy(before_tcb, (uint8_t *)qm->tcbs.va + 128 * tag, 128);
    int ret = ans_nvme_submit(&r->core, q, tag, cmd);
    return ret == ANS_ERR_DENIED && r->f.doorbells == bells &&
           memcmp(before_cmd, (uint8_t *)qm->cmds.va + 64 * tag, 64) == 0 &&
           memcmp(before_tcb, (uint8_t *)qm->tcbs.va + 128 * tag, 128) == 0 &&
           env_log_count(&r->env, "nvme: denied") == lines + 1;
}

static void test_whitelist(void)
{
    struct rig *r = rig_new(3, false);
    struct ans_nvme_cmd cmd;
    struct buf b = buf_new(r, 4096);

    if (start(r) != ANS_OK) { /* the rest looks into the queues */
        FAIL("start");
        rig_finish(r);
        return;
    }

    /* every opcode of both queues: only the listed ones may get through the check */
    for (int q = 0; q < 2; q++) {
        for (int op = 0; op < 256; op++) {
            memset(&cmd, 0, sizeof(cmd));
            cmd.opcode = (uint8_t)op;
            cmd.nsid = 1;
            cmd.prp1 = b.dva;
            cmd.cdw10 = 1;
            if (listed((enum ans_nvme_queue)q, (uint8_t)op))
                continue;
            if (!submit_denied(r, (enum ans_nvme_queue)q, q == 0 ? 0 : 7, &cmd)) {
                FAIL("a command off the whitelist was not denied");
                printf("    queue %d opcode 0x%02x\n", q, op);
            }
        }
    }

    /* listed opcodes with the wrong queue number, namespace, CNS or flags */
    struct {
        enum ans_nvme_queue q;
        uint8_t opcode;
        uint8_t flags;
        uint32_t nsid;
        uint32_t cdw10;
        uint32_t cdw11;
    } bad[] = {
        {ANS_NVME_ADMINQ, 0x05, 0, 0, 2 | (63u << 16), 3},        /* create CQ 2 */
        {ANS_NVME_ADMINQ, 0x01, 0, 0, 1 | (63u << 16), 1 | (2u << 16)}, /* SQ on CQ 2 */
        {ANS_NVME_ADMINQ, 0x01, 0, 0, 0 | (63u << 16), 1 | (1u << 16)}, /* admin SQ */
        {ANS_NVME_ADMINQ, 0x06, 0, 0, 2, 0},                      /* identify CNS 2 */
        {ANS_NVME_ADMINQ, 0x06, 0, 2, 0, 0},                      /* namespace 2 */
        {ANS_NVME_ADMINQ, 0x06, 0, 0xffffffff, 0, 0},             /* all namespaces */
        {ANS_NVME_ADMINQ, 0x06, 0, 1, 1, 0},                      /* controller with nsid */
        {ANS_NVME_ADMINQ, 0x00, 0, 0, 0, 0},                      /* delete admin SQ */
        {ANS_NVME_ADMINQ, 0x04, 0, 0, 0, 0},                      /* delete admin CQ */
        {ANS_NVME_ADMINQ, 0x06, 1, 0, 1, 0},                      /* fused */
        {ANS_NVME_IOQ, 0x02, 0, 2, 0, 0},                         /* read namespace 2 */
        {ANS_NVME_IOQ, 0x02, 0, 0xffffffff, 0, 0},
        {ANS_NVME_IOQ, 0x02, 0x40, 1, 0, 0},                      /* SGL instead of PRP */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = bad[i].opcode;
        cmd.flags = bad[i].flags;
        cmd.nsid = bad[i].nsid;
        cmd.cdw10 = bad[i].cdw10;
        cmd.cdw11 = bad[i].cdw11;
        cmd.prp1 = b.dva;
        if (!submit_denied(r, bad[i].q, bad[i].q == ANS_NVME_ADMINQ ? 0 : 3, &cmd)) {
            FAIL("a listed opcode with wrong fields was not denied");
            printf("    case %u\n", i);
        }
    }

    /* and the listed ones with the right fields do pass the check */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x02;
    cmd.nsid = 1;
    CHECK(ans_nvme_allowed(ANS_NVME_IOQ, &cmd));
    cmd.opcode = 0x06;
    cmd.nsid = 0;
    cmd.cdw10 = 1;
    CHECK(ans_nvme_allowed(ANS_NVME_ADMINQ, &cmd));

    /* the controller is still fine and only ever saw listed commands */
    struct ans_segment s = {b.dva, 4096};
    struct done_rec d;
    memset(&d, 0, sizeof(d));
    CHECK(t_read(r, 7, 3, 1, &s, 1) == ANS_OK);
    poll(r, &d);
    CHECK(d.done[7] && data_ok(b.va, 3, 4096, 4096));
    stop(r);
    rig_finish(r);
}

/* ------------------------------------------------------------------------------- */
/* Timeouts and a coprocessor that does not answer                                  */

static void check_failed_start(struct rig *r, int want, const char *log)
{
    CHECK(start(r) == want);
    CHECK(log == NULL || env_logged(&r->env, log));
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(fake_live_env_blocks(&r->f) == 0);
    CHECK(fake_sart_entries_in_use(&r->f) == 2);
    CHECK(!r->f.needs_reset);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    /* not tried again in the same boot */
    uint32_t writes = r->f.reg_writes;
    CHECK(ans_start(&r->core, &r->env, &r->cfg) == ANS_ERR_STATE);
    CHECK(r->f.reg_writes == writes);
}

static void test_coprocessor_silent(void)
{
    struct rig *r = rig_new(3, false);
    r->f.silent = true;
    check_failed_start(r, ANS_ERR_TIMEOUT, "timeout while waiting for HELLO");
    CHECK(r->f.now_us >= 1000000 && r->f.now_us < 1100000);
    rig_finish(r);
}

static void test_coprocessor_silent_interrupts(void)
{
    struct rig *r = rig_new(3, true);
    r->f.silent = true;
    check_failed_start(r, ANS_ERR_TIMEOUT, "timeout while waiting for HELLO");
    rig_finish(r);
}

static void test_controller_never_ready(void)
{
    struct rig *r = rig_new(3, false);
    r->f.never_ready = true;
    check_failed_start(r, ANS_ERR_TIMEOUT, "timeout while waiting for CSTS.RDY to be set");
    CHECK(r->f.now_us >= 5000000 && r->f.now_us < 5200000);
    CHECK(r->f.cp == CP_OFF);
    rig_finish(r);
}

static void test_boot_status_never_ok(void)
{
    struct rig *r = rig_new(3, false);
    r->f.never_boot_status = true;
    check_failed_start(r, ANS_ERR_TIMEOUT, "ANS did not boot correctly");
    CHECK(r->f.ncmds == 0);
    rig_finish(r);
}

static void test_admin_command_hangs(void)
{
    struct rig *r = rig_new(3, false);
    r->f.hang_admin_opcode = 0x06;
    check_failed_start(r, ANS_ERR_TIMEOUT, "timeout while waiting for identify controller");
    rig_finish(r);
}

static void test_admin_command_fails(void)
{
    struct rig *r = rig_new(3, false);
    r->f.fail_admin_opcode = 0x01; /* create SQ */
    check_failed_start(r, ANS_ERR_DEVICE, "create sq command failed");
    /* the completion queue that was made is deleted again */
    CHECK(count_cmds(&r->f, 0, 0x04) == 1);
    CHECK(!r->f.iocq_made);
    rig_finish(r);
}

static void test_mdts_too_small(void)
{
    struct rig *r = rig_new(3, false);
    r->f.mdts = 5; /* 128 KiB */
    check_failed_start(r, ANS_ERR_DEVICE, "takes at most 131072 bytes");
    CHECK(count_cmds(&r->f, 0, 0x05) == 0);
    rig_finish(r);
}

static void test_sleep_never_acked(void)
{
    struct rig *r = rig_new(3, false);
    CHECK(start(r) == ANS_OK);
    r->f.no_sleep = true;
    stop(r);
    CHECK(env_logged(&r->env, "timeout while waiting for IOP sleep"));
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(!r->f.needs_reset);
    rig_finish(r);
}

/* A read never completes: Storport would reset the bus; the miniport halts the core. */
static void test_read_hangs_then_halt(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    r->f.hang_reads = true;
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    memset(&d, 0, sizeof(d));
    CHECK(t_read(r, 2, 0, 1, &s, 1) == ANS_OK);
    for (int i = 0; i < 100; i++)
        poll(r, &d);
    CHECK(d.count == 0);

    uint32_t bells = r->f.doorbells;
    t_halt(r);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.nvme_cc & 1) == 0);
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(!r->f.needs_reset);
    for (int i = 0; i < 10; i++)
        poll(r, &d);
    CHECK(d.count == 0);
    CHECK(t_read(r, 3, 0, 1, &s, 1) == ANS_ERR_STATE);
    CHECK(r->f.doorbells == bells);

    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK(fake_live_env_blocks(&r->f) == 0);
    CHECK(fake_sart_entries_in_use(&r->f) == 2);
    CHECK(ans_start(&r->core, &r->env, &r->cfg) == ANS_ERR_STATE); /* no second chance */
    rig_finish(r);
}

/* ScsiStopAdapter arrives while the core waits for the coprocessor during start. */
static void test_halt_during_start(void)
{
    struct rig *r = rig_new(3, true);
    r->env.halt_at_wait = 3;
    CHECK(start(r) == ANS_ERR_STATE);
    CHECK(env_logged(&r->env, "the core was halted"));
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(fake_live_env_blocks(&r->f) == 0);
    rig_finish(r);
}

/* The halt before the RTKit handshake is through: RUN and the reset only, no NVMe. */
static void test_halt_before_handshake(void)
{
    struct rig *r = rig_new(3, false);
    r->env.halt_at_wait = 1; /* while waiting for HELLO */
    CHECK(start(r) == ANS_ERR_STATE);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(r->f.resets == 2); /* before the start, and in the halt */
    CHECK(!r->f.needs_reset);
    CHECK(fake_live_env_blocks(&r->f) == 0);
    rig_finish(r); /* an NVMe access would be a violation */
}

/* Halting a core that is off, ready, or failed already. */
static void test_halt_states(void)
{
    struct rig *r = rig_new(3, false);
    uint32_t writes = r->f.reg_writes;
    t_halt(r); /* off: nothing */
    CHECK(r->f.reg_writes == writes);
    CHECK(start(r) == ANS_OK);
    t_halt(r);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.nvme_cc & 1) == 0 && (r->f.cpu_control & 0x10) == 0);
    CHECK(r->f.resets == 2);
    t_halt(r); /* again: RUN is clear already, no second reset */
    CHECK(r->f.resets == 2);
    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    rig_finish(r);
}

/*
 * An environment whose ans_env_wait() always returns 0: every wait is taken as 1 us,
 * so every wait loop still ends.
 */
static void test_wait_returns_zero(void)
{
    static const char *const what[] = {"silent", "boot status", "identify hangs", "never ready", "no sleep"};
    for (int i = 0; i < 5; i++) {
        struct rig *r = rig_new(3, false);
        r->env.wait_returns_zero = true;
        if (i == 0)
            r->f.silent = true;
        if (i == 1)
            r->f.never_boot_status = true;
        if (i == 2)
            r->f.hang_admin_opcode = 0x06;
        if (i == 3)
            r->f.never_ready = true;
        int ret = start(r);
        if (i < 4) {
            CHECK(ret == ANS_ERR_TIMEOUT);
            CHECK(ans_get_state(&r->core) == ANS_FAILED);
        } else {
            CHECK(ret == ANS_OK);
            r->f.no_sleep = true;
            stop(r);
            CHECK(ans_get_state(&r->core) == ANS_FAILED);
        }
        CHECK((r->f.cpu_control & 0x10) == 0);
        /* each wait counted as 1 us: at least a full timeout of waits */
        CHECK(r->env.waits >= 1000000);
        if (g_test_failed)
            printf("    case %s\n", what[i]);
        rig_finish(r);
    }
}

/* The coprocessor asks for a buffer while the core is running: it cannot allocate. */
static void test_buffer_request_while_running(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    r->f.no_ioreport_buffer = true;
    CHECK(start(r) == ANS_OK);
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < 3; i++)
        poll(r, &d);
    r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg0 = (1ull << 52) | (1ull << 44);
    r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg1 = 4;
    r->f.i2a_n++;
    poll(r, &d);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK(ans_get_stats(&r->core)->msgs_refused == 1);
    CHECK(env_logged(&r->env, "a buffer while running"));
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    CHECK(t_read(r, 0, 0, 1, &s, 1) == ANS_ERR_STATE);
    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.cpu_control & 0x10) == 0);
    rig_finish(r);
}

/* A second buffer for an endpoint that has one: refused the same way. */
static void test_second_buffer_request(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < 3; i++)
        poll(r, &d);
    r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg0 = (1ull << 52) | (4ull << 44);
    r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg1 = 2;
    r->f.i2a_n++;
    poll(r, &d);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK(ans_get_stats(&r->core)->msgs_refused == 1);
    stop(r);
    rig_finish(r);
}

/* ScsiStopAdapter comes while the start allocates memory, before anything is touched. */
static void test_halt_during_allocation(void)
{
    struct rig *r = rig_new(3, false);
    r->env.halt_at_alloc = 2;
    CHECK(start(r) == ANS_ERR_STATE);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK(r->f.run_sets == 0); /* the coprocessor was never started */
    CHECK(r->f.resets == 0);
    CHECK(fake_live_env_blocks(&r->f) == 0);
    rig_finish(r);
}

/* Interrupts while the stop gives the memory back: the queues are out of the record first. */
static void test_interrupts_while_freeing(void)
{
    struct rig *r = rig_new(3, true);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    /* wrap the completion queue once, so a zeroed entry would look like a new one */
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    for (int i = 0; i < 64; i++) {
        memset(&d, 0, sizeof(d));
        CHECK(t_read(r, 0, (uint64_t)i, 1, &s, 1) == ANS_OK);
        poll(r, &d);
        CHECK(d.done[0]);
    }
    memset(&d, 0, sizeof(d));
    r->env.done = on_done;
    r->env.done_ctx = &d;
    stop(r);
    CHECK(d.count == 0);
    CHECK(ans_get_state(&r->core) == ANS_OFF);
    CHECK(ans_get_stats(&r->core)->stray_completions == 0);
    rig_finish(r);
}

/* An admin command that never completes: no further admin command goes out. */
static void test_admin_command_lost_at_stop(void)
{
    struct rig *r = rig_new(3, false);
    CHECK(start(r) == ANS_OK);
    r->f.hang_admin_opcode = 0x00; /* delete SQ */
    stop(r);
    CHECK(count_cmds(&r->f, 0, 0x00) == 1);
    CHECK(count_cmds(&r->f, 0, 0x04) == 0); /* delete CQ not sent */
    CHECK(env_logged(&r->env, "an earlier one never completed"));
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(!r->f.needs_reset);
    rig_finish(r);
}

/* After a crash, completions are taken off the queue (the line drops) but not reported. */
static void test_completions_after_crash(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    r->f.crash_now = true;
    memset(&d, 0, sizeof(d));
    poll(r, &d);
    CHECK(ans_get_state(&r->core) == ANS_CRASHED);
    fake_post_cqe(&r->f, 1, 5, 0); /* a completion that comes in after the crash */
    CHECK(t_isr(r, on_done, &d));
    CHECK(d.count == 0);
    CHECK(r->core.nvme.q[ANS_NVME_IOQ].cq_head == 1);
    CHECK(!t_isr(r, on_done, &d));
    stop(r);
    rig_finish(r);
}

/* The power state register reads as all ones: no reset, nothing written back. */
static void test_pmgr_all_ones(void)
{
    struct rig *r = rig_new(3, false);
    r->f.pmgr_all_ones = true;
    CHECK(start(r) == ANS_ERR_DEVICE);
    CHECK(r->f.reg_writes == 0);
    CHECK(env_logged(&r->env, "will not reset ANS"));
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    rig_finish(r);
}

/* ------------------------------------------------------------------------------- */
/* The coprocessor's side                                                            */

static void test_bad_rtkit_version(void)
{
    struct rig *r = rig_new(3, false);
    r->f.bad_version = true;
    check_failed_start(r, ANS_ERR_DEVICE, "must overlap versions [13,14]");
    rig_finish(r);
}

static void test_buffer_at_own_address(void)
{
    struct rig *r = rig_new(3, false);
    r->f.buffer_at_own_addr = true;
    check_failed_start(r, ANS_ERR_DEVICE, "at its own address");
    rig_finish(r);
}

static void test_crash_at_boot(void)
{
    struct rig *r = rig_new(3, false);
    r->f.crash_at_boot = true;
    CHECK(start(r) == ANS_ERR_CRASHED);
    CHECK(ans_get_state(&r->core) == ANS_CRASHED);
    CHECK(env_logged(&r->env, "IOP crashed!"));
    CHECK(env_logged(&r->env, "Message 7: fake crash test"));
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(fake_live_env_blocks(&r->f) == 0);
    CHECK(!r->f.needs_reset);
    CHECK(ans_start(&r->core, &r->env, &r->cfg) == ANS_ERR_STATE);
    rig_finish(r);
}

static void test_crash_while_running(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    r->f.crash_now = true;
    memset(&d, 0, sizeof(d));
    poll(r, &d);
    CHECK(ans_get_state(&r->core) == ANS_CRASHED);
    CHECK(env_logged(&r->env, "Message 7: fake crash test"));
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    uint32_t bells = r->f.doorbells;
    CHECK(t_read(r, 0, 0, 1, &s, 1) == ANS_ERR_CRASHED);
    CHECK(r->f.doorbells == bells);
    uint32_t ncmds = r->f.ncmds;
    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_CRASHED);
    CHECK(r->f.ncmds == ncmds); /* no commands to a crashed coprocessor */
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(fake_live_env_blocks(&r->f) == 0);
    CHECK(r->f.resets == 1u + 1u);
    rig_finish(r);
}

/* Syslog and ioreport messages are acknowledged; the controller's own endpoint is logged. */
static void test_messages_acknowledged(void)
{
    struct rig *r = rig_new(3, false);
    r->f.extra_messages = true;
    CHECK(start(r) == ANS_OK);
    CHECK(r->f.acks_expected == 2 && r->f.acks_seen == 2);
    CHECK(ans_get_stats(&r->core)->msgs_ignored == 1); /* endpoint 0x20 */
    CHECK(ans_get_stats(&r->core)->msgs_refused == 0);
    CHECK(env_logged(&r->env, "non-system endpoint 0x20"));
    stop(r);
    rig_finish(r);
}

/* The host-to-coprocessor FIFO holds one message: the rest wait in the send ring. */
static void test_mailbox_full(void)
{
    struct rig *r = rig_new(3, false);
    r->f.a2i_capacity = 1;
    CHECK(start(r) == ANS_OK);
    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_OFF);
    rig_finish(r);
}

static void test_receive_ring_overflow(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < 5; i++)
        poll(r, &d);
    CHECK(r->f.i2a_n == 0 && r->core.asc.rx_head == r->core.asc.rx_tail);
    /* 64 syslog messages fill the ring; 10 more are dropped */
    for (int i = 0; i < 64; i++) {
        r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg0 = (5ull << 52) | (uint64_t)i;
        r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg1 = 2;
        r->f.i2a_n++;
    }
    t_isr(r, on_done, &d);
    for (int i = 0; i < 10; i++) {
        r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg0 = (5ull << 52) | 100;
        r->f.i2a[(r->f.i2a_head + r->f.i2a_n) % FAKE_FIFO].msg1 = 2;
        r->f.i2a_n++;
    }
    CHECK(t_isr(r, on_done, &d));
    CHECK(r->f.i2a_n == 0);
    CHECK(ans_get_stats(&r->core)->msgs_lost == 10);
    CHECK(env_logged(&r->env, "receive ring full"));
    /* a lost message may have needed an answer: the core gives up */
    t_mbox(r);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    CHECK(t_read(r, 0, 0, 1, &s, 1) == ANS_ERR_STATE);
    stop(r);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK((r->f.cpu_control & 0x10) == 0);
    CHECK(!r->f.needs_reset);
    rig_finish(r);
}

/* ------------------------------------------------------------------------------- */
/* Hand-over, configuration, power manager, registers                               */

static void test_running_not_taken_over(void)
{
    struct rig *r = rig_new(3, false);
    r->f.cpu_control = 0x10; /* left running by whoever had it before */
    r->f.cp = CP_DEAD;
    CHECK(t_probe(r) == ANS_ERR_RUNNING);
    CHECK(start(r) == ANS_ERR_RUNNING);
    CHECK(r->f.reg_writes == 0);
    CHECK(ans_get_state(&r->core) == ANS_OFF);
    CHECK(env_logged(&r->env, "not taking it over"));
    r->f.cpu_control = 0; /* so rig_finish's check of RUN does not apply */
    rig_finish(r);
}

static void test_probe_discards_stale_messages(void)
{
    struct rig *r = rig_new(3, false);
    for (int i = 0; i < 3; i++) {
        r->f.i2a[i].msg0 = (0xbull << 52) | 0x10;
        r->f.i2a[i].msg1 = 0;
    }
    r->f.i2a_n = 3;
    CHECK(t_probe(r) == ANS_OK);
    CHECK(r->f.i2a_n == 0);
    CHECK(r->f.reg_writes == 0);
    CHECK(env_logged(&r->env, "discarded 3 stale mailbox messages"));
    CHECK(start(r) == ANS_OK);
    stop(r);
    rig_finish(r);
}

/* Without ans_probe(), ans_start() itself throws away what the previous owner left. */
static void test_start_discards_stale_messages(void)
{
    struct rig *r = rig_new(3, false);
    r->f.i2a[0].msg0 = (7ull << 52) | 0x1; /* an old "IOP asleep" */
    r->f.i2a[0].msg1 = 0;
    r->f.i2a_n = 1;
    CHECK(start(r) == ANS_OK);
    CHECK(env_logged(&r->env, "discarded 1 stale mailbox messages"));
    stop(r);
    rig_finish(r);
}

static void test_bad_configuration(void)
{
    struct rig *r = rig_new(3, false);
    struct ans_config good = r->cfg;
    struct ans_config bad[6];
    for (int i = 0; i < 6; i++)
        bad[i] = good;
    bad[0].io_depth = 0;
    bad[1].io_depth = ANS_MAX_IO_DEPTH + 1;
    bad[2].sart_version = 1;
    bad[3].sart_version = 5;
    bad[4].max_transfer = 0;
    bad[5].max_transfer = ANS_MAX_TRANSFER + 4096;
    for (int i = 0; i < 6; i++)
        CHECK(ans_start(&r->core, &r->env, &bad[i]) == ANS_ERR_INVALID);
    CHECK(r->f.reg_writes == 0);
    CHECK(ans_get_state(&r->core) == ANS_OFF);
    rig_finish(r);
}

static void test_pmgr_not_active(void)
{
    struct rig *r = rig_new(3, false);
    r->f.pmgr_inactive = true;
    /* the reset before the start is refused: the start fails before RUN is set */
    CHECK(start(r) == ANS_ERR_DEVICE);
    CHECK(r->f.reg_writes == 0);
    CHECK(ans_get_state(&r->core) == ANS_FAILED);
    CHECK(env_logged(&r->env, "will not reset ANS"));
    CHECK(r->f.resets == 0);
    rig_finish(r);
}

static void test_forbidden_register(void)
{
    struct rig *r = rig_new(3, false);
    CHECK(start(r) == ANS_OK);
    CHECK(ans_read32(&r->core, ANS_RGN_NVME, 0x24008) == 0xffffffffu);
    ans_write32(&r->core, ANS_RGN_NVME, 0x24008, 1);
    CHECK(ans_read64(&r->core, ANS_RGN_NVME, 0x24008) == ~0ull);
    ans_write64(&r->core, ANS_RGN_NVME, 0x24004, 1); /* overlaps it */
    CHECK(env_log_count(&r->env, "forbidden register") == 4);
    stop(r);
    rig_finish(r); /* the fake would have counted a violation */
}

static void test_tcb_invalidation_failure_logged(void)
{
    struct rig *r = rig_new(3, false);
    struct done_rec d;
    CHECK(start(r) == ANS_OK);
    r->f.tcb_stat = 1;
    struct buf b = buf_new(r, 4096);
    struct ans_segment s = {b.dva, 4096};
    memset(&d, 0, sizeof(d));
    CHECK(t_read(r, 4, 4, 1, &s, 1) == ANS_OK);
    poll(r, &d);
    CHECK(d.done[4] && d.status[4] == 0 && data_ok(b.va, 4, 4096, 4096));
    CHECK(env_logged(&r->env, "NVMMU invalidation for tag 4 failed"));
    CHECK(r->f.last_inval == 4);
    CHECK(ans_get_stats(&r->core)->nvmmu_inval_failed == 1);
    stop(r);
    rig_finish(r);
}

/* ------------------------------------------------------------------------------- */

struct test {
    const char *name;
    void (*fn)(void);
};

static const struct test tests[] = {
    {"start_stop_polling", test_start_stop_polling},
    {"start_stop_interrupts", test_start_stop_interrupts},
    {"sart_versions", test_sart_versions},
    {"without_secure_bar", test_without_secure_bar},
    {"restart_same_boot", test_restart_same_boot},
    {"previous_owner_did_not_reset", test_previous_owner_did_not_reset},
    {"reads_polling", test_reads_polling},
    {"reads_interrupts", test_reads_interrupts},
    {"reads_in_flight", test_reads_in_flight},
    {"read_high_lba", test_read_high_lba},
    {"read_error_status", test_read_error_status},
    {"read_refused", test_read_refused},
    {"whitelist", test_whitelist},
    {"coprocessor_silent", test_coprocessor_silent},
    {"coprocessor_silent_interrupts", test_coprocessor_silent_interrupts},
    {"controller_never_ready", test_controller_never_ready},
    {"boot_status_never_ok", test_boot_status_never_ok},
    {"admin_command_hangs", test_admin_command_hangs},
    {"admin_command_fails", test_admin_command_fails},
    {"mdts_too_small", test_mdts_too_small},
    {"sleep_never_acked", test_sleep_never_acked},
    {"read_hangs_then_halt", test_read_hangs_then_halt},
    {"halt_during_start", test_halt_during_start},
    {"halt_before_handshake", test_halt_before_handshake},
    {"halt_states", test_halt_states},
    {"halt_during_allocation", test_halt_during_allocation},
    {"interrupts_while_freeing", test_interrupts_while_freeing},
    {"admin_command_lost_at_stop", test_admin_command_lost_at_stop},
    {"completions_after_crash", test_completions_after_crash},
    {"pmgr_all_ones", test_pmgr_all_ones},
    {"wait_returns_zero", test_wait_returns_zero},
    {"buffer_request_while_running", test_buffer_request_while_running},
    {"second_buffer_request", test_second_buffer_request},
    {"bad_rtkit_version", test_bad_rtkit_version},
    {"buffer_at_own_address", test_buffer_at_own_address},
    {"crash_at_boot", test_crash_at_boot},
    {"crash_while_running", test_crash_while_running},
    {"messages_acknowledged", test_messages_acknowledged},
    {"mailbox_full", test_mailbox_full},
    {"receive_ring_overflow", test_receive_ring_overflow},
    {"running_not_taken_over", test_running_not_taken_over},
    {"probe_discards_stale_messages", test_probe_discards_stale_messages},
    {"start_discards_stale_messages", test_start_discards_stale_messages},
    {"bad_configuration", test_bad_configuration},
    {"pmgr_not_active", test_pmgr_not_active},
    {"forbidden_register", test_forbidden_register},
    {"tcb_invalidation_failure_logged", test_tcb_invalidation_failure_logged},
};

int main(int argc, char **argv)
{
    const char *filter = NULL;
    int run = 0, failed = 0;

    setvbuf(stdout, NULL, _IONBF, 0); /* nothing lost if a test crashes */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else
            filter = argv[i];
    }

    printf("model test of the disk controller core\n");
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (filter && !strstr(tests[i].name, filter))
            continue;
        g_test_failed = false;
        tests[i].fn();
        run++;
        if (g_test_failed)
            failed++;
        printf("  %-34s %s\n", tests[i].name, g_test_failed ? "FAIL" : "ok");
    }
    printf("%d tests, %d failed, %d checks, %d failed checks\n", run, failed, g_checks,
           g_failures);
    return (failed || run == 0) ? 1 : 0;
}
