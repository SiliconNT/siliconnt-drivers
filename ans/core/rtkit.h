/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/rtkit.h; see LICENSE.m1n1.
 * Changed for SiliconNT: SART only (no DART, no SRAM), and boot and shutdown are
 * steps of a state machine driven by the messages, so the core can take the messages
 * from an interrupt handler. See SiliconNT findings/0072.
 */

#ifndef ANS_RTKIT_H
#define ANS_RTKIT_H

#include "ans_env.h"

struct ans_core;

/* Where the conversation with the coprocessor stands. */
enum rtkit_phase {
    RTKIT_OFF = 0,        /* not started, or stopped */
    RTKIT_WAIT_HELLO,     /* woken up, waiting for HELLO */
    RTKIT_WAIT_EPMAP,     /* HELLO answered, receiving the endpoint map */
    RTKIT_WAIT_POWER_ON,  /* endpoints started, waiting for the IOP to report "on" */
    RTKIT_RUNNING,        /* booted; AP power "on" sent */
    RTKIT_WAIT_QUIESCED,  /* AP power "quiesced" sent */
    RTKIT_WAIT_SLEEP,     /* IOP power "sleep" sent */
    RTKIT_ASLEEP,         /* IOP reported "sleep" */
    RTKIT_FAILED,         /* the coprocessor broke the protocol, or a reply could not be sent */
    RTKIT_CRASHED,        /* the coprocessor asked for a second crash log buffer */
};

/* The buffers the coprocessor asks for, one per system endpoint that asks. */
enum rtkit_buf {
    RTKIT_BUF_CRASHLOG = 0,
    RTKIT_BUF_SYSLOG,
    RTKIT_BUF_IOREPORT,
    RTKIT_BUF_COUNT
};

struct rtkit_buffer {
    struct ans_dma dma; /* dma.size is rounded up to 16 KiB, the size given to the SART */
    bool held;
};

struct ans_rtkit {
    enum rtkit_phase phase;
    uint32_t iop_power;
    uint32_t ap_power;
    uint32_t endpoints; /* system endpoints (0..31) in the endpoint map */
    struct rtkit_buffer buf[RTKIT_BUF_COUNT];
    uint32_t syslog_count, syslog_size;
};

void ans_rtkit_init(struct ans_core *core);
bool ans_rtkit_boot(struct ans_core *core);
bool ans_rtkit_sleep(struct ans_core *core);
void ans_rtkit_service(struct ans_core *core, bool can_alloc);
void ans_rtkit_free(struct ans_core *core);

#endif
