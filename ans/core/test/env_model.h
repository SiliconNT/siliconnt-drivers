/* SPDX-License-Identifier: MIT */
/* The environment of the model test: ans_env.h implemented on top of the fake controller. */

#ifndef ENV_MODEL_H
#define ENV_MODEL_H

#include "../ans_core.h"
#include "fake_ans.h"

#define ENV_LOG_LINES 4096
#define ENV_LOG_WIDTH 200

struct ans_env {
    struct fake *fake;
    struct ans_core *core;

    /*
     * Interrupt mode: while the core waits, run what the interrupt handler and the DPC
     * would run (ans_isr(), then ans_mbox_service()), as the Windows miniport does when
     * it gives up its lock. Off: a polling environment, like the test guest.
     */
    bool interrupts;
    ans_done_fn *done;
    void *done_ctx;

    /*
     * Call ans_halt() from inside the n-th wait, or the n-th allocation (0: never), like
     * ScsiStopAdapter would while the lock is given up.
     */
    uint32_t halt_at_wait;
    uint32_t halt_at_alloc;
    uint32_t allocs;

    /* A broken environment whose ans_env_wait() always returns 0. */
    bool wait_returns_zero;

    /*
     * Set while the test is inside a call that must not wait, allocate or free
     * (ans_halt(), ans_isr(), ans_mbox_service(), ans_read(), ans_probe()); any of those
     * then counts as a broken contract.
     */
    uint32_t no_wait;
    uint32_t contract_broken;

    uint32_t waits;
    uint32_t refused;   /* register accesses outside a region */
    uint32_t rmb, wmb;
    bool verbose;
    char log[ENV_LOG_LINES][ENV_LOG_WIDTH];
    uint32_t nlog;
};

void env_init(struct ans_env *env, struct fake *fake, struct ans_core *core);
bool env_logged(const struct ans_env *env, const char *needle);
uint32_t env_log_count(const struct ans_env *env, const char *needle);

#endif
