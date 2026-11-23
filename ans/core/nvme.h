/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/nvme.h; see LICENSE.m1n1.
 * Changed for SiliconNT: the queue state lives in struct ans_core, and every command
 * goes through one function that checks it against a whitelist. See SiliconNT
 * findings/0071 conclusions 19-22 and findings/0072.
 */

#ifndef ANS_NVME_H
#define ANS_NVME_H

#include "ans_env.h"

struct ans_core;

/* The two queues. Their opcodes are numbered separately. */
enum ans_nvme_queue {
    ANS_NVME_ADMINQ = 0,
    ANS_NVME_IOQ = 1,
    ANS_NVME_QUEUES
};

/* A command as the core fills it in, before it is written into a queue slot. */
struct ans_nvme_cmd {
    uint8_t opcode;
    uint8_t flags;
    uint32_t nsid;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct ans_nvme_queue_mem {
    struct ans_dma tcbs; /* NVMMU command table, one 128-byte entry per tag */
    struct ans_dma cmds; /* submission queue, slot = tag (linear submission) */
    struct ans_dma cqes; /* completion queue */
    uint8_t cq_head;
    uint8_t cq_phase;
};

/* The one admin command the core may have outstanding (tag 0). */
enum ans_admin_wait {
    ANS_ADMIN_IDLE = 0,
    ANS_ADMIN_WAITING,
    ANS_ADMIN_DONE,
    ANS_ADMIN_LOST /* one never completed: no more admin commands until the next start */
};

struct ans_nvme {
    struct ans_nvme_queue_mem q[ANS_NVME_QUEUES];
    struct ans_dma prp;      /* one 4 KiB PRP list page per read tag */
    struct ans_dma identify; /* one page for identify data */
    uint64_t inflight;       /* read tags handed to the controller and not completed */
    enum ans_admin_wait admin;
    uint32_t admin_status;
};

bool ans_nvme_allowed(enum ans_nvme_queue queue, const struct ans_nvme_cmd *cmd);
int ans_nvme_submit(struct ans_core *core, enum ans_nvme_queue queue, uint16_t tag,
                    const struct ans_nvme_cmd *cmd);

#endif
