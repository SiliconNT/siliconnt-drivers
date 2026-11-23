/* SPDX-License-Identifier: MIT */
/* The environment of the model test: ans_env.h implemented on top of the fake controller. */

#include "env_model.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t region_size[ANS_RGN_COUNT] = {
    FAKE_NVME_SIZE,
    FAKE_ASC_SIZE,
    FAKE_SART_SIZE,
    FAKE_PMGR_SIZE,
};

void env_init(struct ans_env *env, struct fake *fake, struct ans_core *core)
{
    memset(env, 0, sizeof(*env));
    env->fake = fake;
    env->core = core;
}

bool env_logged(const struct ans_env *env, const char *needle)
{
    return env_log_count(env, needle) != 0;
}

uint32_t env_log_count(const struct ans_env *env, const char *needle)
{
    uint32_t n = 0;
    uint32_t lines = env->nlog < ENV_LOG_LINES ? env->nlog : ENV_LOG_LINES;

    for (uint32_t i = 0; i < lines; i++)
        if (strstr(env->log[i], needle))
            n++;
    return n;
}

/* The environment's own check: an access must fit inside its region (ans_env.h). */
static bool env_access_ok(struct ans_env *env, enum ans_region rgn, uint32_t off, unsigned width)
{
    if ((unsigned)rgn < ANS_RGN_COUNT && (off % width) == 0 &&
        (uint64_t)off + width <= region_size[rgn])
        return true;
    env->refused++;
    ans_env_log(env, "env: refused %u-byte access to region %u offset 0x%x", width, rgn, off);
    return false;
}

uint32_t ans_env_read32(struct ans_env *env, enum ans_region rgn, uint32_t offset)
{
    if (!env_access_ok(env, rgn, offset, 4))
        return 0xffffffffu;
    return (uint32_t)fake_reg_read(env->fake, (int)rgn, offset, 4);
}

void ans_env_write32(struct ans_env *env, enum ans_region rgn, uint32_t offset, uint32_t value)
{
    if (!env_access_ok(env, rgn, offset, 4))
        return;
    fake_reg_write(env->fake, (int)rgn, offset, 4, value);
}

uint64_t ans_env_read64(struct ans_env *env, enum ans_region rgn, uint32_t offset)
{
    if (!env_access_ok(env, rgn, offset, 8))
        return ~0ull;
    return fake_reg_read(env->fake, (int)rgn, offset, 8);
}

void ans_env_write64(struct ans_env *env, enum ans_region rgn, uint32_t offset, uint64_t value)
{
    if (!env_access_ok(env, rgn, offset, 8))
        return;
    fake_reg_write(env->fake, (int)rgn, offset, 8, value);
}

static void env_no_wait_here(struct ans_env *env, const char *what)
{
    if (env->no_wait) {
        env->contract_broken++;
        ans_env_log(env, "env: CONTRACT BROKEN: %s inside a call that must not wait", what);
    }
}

/*
 * Where the core lets the environment give up its lock (allocating, freeing,
 * waiting), the interrupt handler and the DPC may run, and ScsiStopAdapter may come.
 */
static void env_lock_given_up(struct ans_env *env, bool waiting)
{
    env->no_wait++;
    if (env->interrupts) {
        ans_isr(env->core, env->done, env->done_ctx);
        ans_mbox_service(env->core);
    }
    if (waiting && env->halt_at_wait && env->waits == env->halt_at_wait) {
        env->halt_at_wait = 0;
        ans_halt(env->core); /* ScsiStopAdapter: DIRQL, lock held */
    }
    if (!waiting && env->halt_at_alloc && env->allocs == env->halt_at_alloc) {
        env->halt_at_alloc = 0;
        ans_halt(env->core);
    }
    env->no_wait--;
}

bool ans_env_dma_alloc(struct ans_env *env, size_t size, struct ans_dma *dma)
{
    void *va;
    uint64_t dva;

    env_no_wait_here(env, "ans_env_dma_alloc()");
    env->allocs++;
    env_lock_given_up(env, false);

    if (!fake_mem_alloc(env->fake, size, true, &va, &dva))
        return false;
    dma->va = va;
    dma->dva = dva;
    dma->size = size;
    dma->env_priv = NULL;
    return true;
}

void ans_env_dma_free(struct ans_env *env, struct ans_dma *dma)
{
    env_no_wait_here(env, "ans_env_dma_free()");
    env_lock_given_up(env, false);
    fake_mem_free(env->fake, dma->dva);
    env_lock_given_up(env, false);
}

void ans_env_udelay(struct ans_env *env, uint32_t usec)
{
    fake_advance(env->fake, usec);
}

uint32_t ans_env_wait(struct ans_env *env, uint32_t usec)
{
    env_no_wait_here(env, "ans_env_wait()");
    env->waits++;
    /* the longest timeout is 5 s, so a wait loop that runs past this never ends */
    if (env->waits > 20000000) {
        printf("    FAIL: a wait loop of the core does not end\n");
        exit(1);
    }
    fake_advance(env->fake, usec);
    fake_tick(env->fake);
    env_lock_given_up(env, true);
    return env->wait_returns_zero ? 0 : usec;
}

void ans_env_wmb(struct ans_env *env)
{
    env->wmb++;
}

void ans_env_rmb(struct ans_env *env)
{
    env->rmb++;
}

void ans_env_log(struct ans_env *env, const char *fmt, ...)
{
    char line[ENV_LOG_WIDTH];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (env->nlog < ENV_LOG_LINES)
        memcpy(env->log[env->nlog], line, sizeof(line));
    env->nlog++;
    if (env->verbose)
        printf("    | %s\n", line);
}
