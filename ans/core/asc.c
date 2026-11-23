/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/asc.c; see LICENSE.m1n1.
 * Changed for SiliconNT: no device tree, registers through the environment, receive
 * and send rings. The differences and why are in SiliconNT findings/0072.
 */

#include "ans_internal.h"

#define ASC_CPU_CONTROL       0x44
#define ASC_CPU_CONTROL_START 0x10

#define ASC_MBOX_CONTROL_FULL  BIT(16)
#define ASC_MBOX_CONTROL_EMPTY BIT(17)

/* The mailbox registers, from the start of the ASC region (mailbox at +0x8000). */
#define ASC_MBOX              0x8000
#define ASC_MBOX_A2I_CONTROL  (ASC_MBOX + 0x110)
#define ASC_MBOX_A2I_SEND0    (ASC_MBOX + 0x800)
#define ASC_MBOX_A2I_SEND1    (ASC_MBOX + 0x808)

#define ASC_MBOX_I2A_CONTROL  (ASC_MBOX + 0x114)
#define ASC_MBOX_I2A_RECV0    (ASC_MBOX + 0x830)
#define ASC_MBOX_I2A_RECV1    (ASC_MBOX + 0x838)

void ans_asc_init(struct ans_core *core)
{
    struct ans_asc *asc = &core->asc;

    asc->rx_head = asc->rx_tail = 0;
    asc->lost = false;
    asc->tx_head = asc->tx_tail = 0;
}

void ans_asc_cpu_start(struct ans_core *core)
{
    ans_set32(core, ANS_RGN_ASC, ASC_CPU_CONTROL, ASC_CPU_CONTROL_START);
}

void ans_asc_cpu_stop(struct ans_core *core)
{
    ans_clear32(core, ANS_RGN_ASC, ASC_CPU_CONTROL, ASC_CPU_CONTROL_START);
}

bool ans_asc_cpu_running(struct ans_core *core)
{
    return (ans_read32(core, ANS_RGN_ASC, ASC_CPU_CONTROL) & ASC_CPU_CONTROL_START) != 0;
}

static bool asc_can_recv(struct ans_core *core)
{
    return !(ans_read32(core, ANS_RGN_ASC, ASC_MBOX_I2A_CONTROL) & ASC_MBOX_CONTROL_EMPTY);
}

static bool asc_can_send(struct ans_core *core)
{
    return !(ans_read32(core, ANS_RGN_ASC, ASC_MBOX_A2I_CONTROL) & ASC_MBOX_CONTROL_FULL);
}

/* Takes one message out of the hardware FIFO. */
static bool asc_fifo_recv(struct ans_core *core, struct asc_message *msg)
{
    if (!asc_can_recv(core))
        return false;

    msg->msg0 = ans_read64(core, ANS_RGN_ASC, ASC_MBOX_I2A_RECV0);
    msg->msg1 = (uint32_t)ans_read64(core, ANS_RGN_ASC, ASC_MBOX_I2A_RECV1);
    ans_env_rmb(core->env);
    return true;
}

/* At most this many messages per call, so a broken mailbox cannot keep us here. */
#define ASC_DRAIN_MAX 256

/*
 * Empties the hardware FIFO into the receive ring. The "message received" line stays
 * high while the FIFO is not empty and the mailbox has no interrupt mask, so the
 * interrupt handler must empty it every time (findings/0071 conclusion 3). When the
 * ring is full the message is dropped, counted and logged, and the conversation with
 * the coprocessor fails (rtkit.c). Returns how many were taken out.
 */
uint32_t ans_asc_drain(struct ans_core *core)
{
    struct ans_asc *asc = &core->asc;
    struct asc_message msg;
    uint32_t n = 0;

    while (n < ASC_DRAIN_MAX && asc_fifo_recv(core, &msg)) {
        n++;
        if (asc->rx_tail - asc->rx_head == ASC_RX_RING) {
            asc->lost = true;
            core->stats.msgs_lost++;
            ans_log(core, "asc: receive ring full, dropped message %016llx ep 0x%x",
                    (unsigned long long)msg.msg0, msg.msg1);
            continue;
        }
        asc->rx[asc->rx_tail % ASC_RX_RING] = msg;
        asc->rx_tail++;
    }
    return n;
}

/* Empties the hardware FIFO and throws the messages away (nobody is listening). */
uint32_t ans_asc_discard(struct ans_core *core)
{
    struct asc_message msg;
    uint32_t n = 0;

    while (n < ASC_DRAIN_MAX && asc_fifo_recv(core, &msg)) {
        ans_log(core, "asc: discarded message %016llx ep 0x%x", (unsigned long long)msg.msg0,
                msg.msg1);
        n++;
    }
    return n;
}

/* Takes the oldest message out of the receive ring. */
bool ans_asc_recv(struct ans_core *core, struct asc_message *msg)
{
    struct ans_asc *asc = &core->asc;

    if (asc->rx_head == asc->rx_tail)
        return false;
    *msg = asc->rx[asc->rx_head % ASC_RX_RING];
    asc->rx_head++;
    return true;
}

static void asc_fifo_send(struct ans_core *core, const struct asc_message *msg)
{
    ans_env_wmb(core->env);
    ans_write64(core, ANS_RGN_ASC, ASC_MBOX_A2I_SEND0, msg->msg0);
    ans_write64(core, ANS_RGN_ASC, ASC_MBOX_A2I_SEND1, msg->msg1);
}

/* Sends what waits in the send ring, as far as the hardware FIFO takes it. */
void ans_asc_flush(struct ans_core *core)
{
    struct ans_asc *asc = &core->asc;

    while (asc->tx_head != asc->tx_tail && asc_can_send(core)) {
        asc_fifo_send(core, &asc->tx[asc->tx_head % ASC_TX_RING]);
        asc->tx_head++;
    }
}

/*
 * Sends a message, in order after any that are still waiting. When the hardware FIFO
 * is full the message waits in the send ring and goes out on a later flush; nothing
 * here spins (m1n1 waited up to 200 ms). Fails only when the send ring is full too;
 * that message is counted as lost and the conversation fails (rtkit.c).
 */
bool ans_asc_send(struct ans_core *core, const struct asc_message *msg)
{
    struct ans_asc *asc = &core->asc;

    ans_asc_flush(core);
    if (asc->tx_head == asc->tx_tail && asc_can_send(core)) {
        asc_fifo_send(core, msg);
        return true;
    }

    if (asc->tx_tail - asc->tx_head == ASC_TX_RING) {
        asc->lost = true;
        core->stats.msgs_lost++;
        ans_log(core, "asc: mailbox full and send ring full, message %016llx ep 0x%x not sent",
                (unsigned long long)msg->msg0, msg->msg1);
        return false;
    }
    asc->tx[asc->tx_tail % ASC_TX_RING] = *msg;
    asc->tx_tail++;
    return true;
}
