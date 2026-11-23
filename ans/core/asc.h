/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/asc.h; see LICENSE.m1n1.
 * Changed for SiliconNT: no device tree, registers through the environment, receive
 * and send rings. The differences and why are in SiliconNT findings/0072.
 */

#ifndef ANS_ASC_H
#define ANS_ASC_H

#include "ans_env.h"

struct ans_core;

struct asc_message {
    uint64_t msg0;
    uint32_t msg1;
};

/* Sizes of the rings; powers of two. */
#define ASC_RX_RING 64
#define ASC_TX_RING 16

/*
 * The mailbox, as seen by the core. Messages from the coprocessor are moved from the
 * hardware FIFO into rx by ans_asc_drain() (in the interrupt handler) and handled
 * later. Messages to the coprocessor that find the hardware FIFO full wait in tx.
 * head is where the next message is taken, tail where the next one is put; both only
 * grow, and the index is the value modulo the ring size.
 */
struct ans_asc {
    struct asc_message rx[ASC_RX_RING];
    uint32_t rx_head, rx_tail;
    bool lost; /* a message was dropped (rx full) or not sent (tx full) since start */
    struct asc_message tx[ASC_TX_RING];
    uint32_t tx_head, tx_tail;
};

void ans_asc_init(struct ans_core *core);

void ans_asc_cpu_start(struct ans_core *core);
void ans_asc_cpu_stop(struct ans_core *core);
bool ans_asc_cpu_running(struct ans_core *core);

uint32_t ans_asc_drain(struct ans_core *core);
uint32_t ans_asc_discard(struct ans_core *core);
bool ans_asc_recv(struct ans_core *core, struct asc_message *msg);

bool ans_asc_send(struct ans_core *core, const struct asc_message *msg);
void ans_asc_flush(struct ans_core *core);

#endif
