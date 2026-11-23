/* SPDX-License-Identifier: MIT */
/*
 * The core: everything that talks to the internal disk controller of an Apple silicon
 * Mac (ANS): the coprocessor and its mailbox, the SART, the RTKit protocol, the NVMe
 * queues and the reset through the power manager. It reaches the outside world only
 * through ans_env.h, so the same code runs in the Windows miniport, the m1n1 test
 * guest, the model test on the host and, later, the firmware. SiliconNT
 * findings/0071 conclusions 16-24 say why it is built this way.
 *
 * The core never writes to the disk: the only NVMe commands it can send are the ones
 * on the whitelist in nvme.c (create and delete the I/O queues, identify, read).
 *
 * Use:
 *   - The caller allocates a struct ans_core, fills it with zeros once, and keeps it
 *     for as long as the device exists. Its contents are the core's own.
 *   - Only one call is inside the core at a time (the environment holds a lock).
 *   - ans_probe() (optional) checks that the controller was handed over stopped.
 *   - ans_start() brings the controller up; ans_get_info() then says what disk it has.
 *     ans_get_stats() counts what went wrong along the way.
 *   - ans_read() hands a read to the controller; the read completes in ans_isr().
 *   - ans_isr() is called from the interrupt handler, or in a loop by an environment
 *     that polls; ans_mbox_service() afterwards, outside the interrupt handler.
 *   - ans_stop() brings the controller down again for the next owner.
 *   - ans_halt() stops the controller's DMA at once, without waiting; ans_stop() then
 *     gives back the memory.
 */

#ifndef ANS_CORE_H
#define ANS_CORE_H

#include "ans_env.h"
#include "asc.h"
#include "nvme.h"
#include "rtkit.h"
#include "sart.h"

/* The interface only grows; environments check this when they need something new. */
#define ANS_CORE_API_VERSION 1

#define ANS_MAX_IO_DEPTH 63               /* read tags 0..62 */
#define ANS_MAX_TRANSFER (512u * 4096u)   /* one PRP list page, not chained */

enum ans_state {
    ANS_OFF = 0,  /* stopped and handed back cleanly (or never started) */
    ANS_STARTING, /* inside ans_start() */
    ANS_READY,    /* reads can be sent */
    ANS_STOPPING, /* inside ans_stop() */
    ANS_FAILED,   /* stopped after an error, or not handed back cleanly; no more reads */
    ANS_CRASHED,  /* the coprocessor crashed; only ans_stop() still does anything */
};

/* Results: 0 for success, a negative value for an error. */
enum ans_result {
    ANS_OK = 0,
    ANS_ERR_STATE = -1,   /* not allowed in the current state */
    ANS_ERR_INVALID = -2, /* bad argument or configuration */
    ANS_ERR_RUNNING = -3, /* the coprocessor was already running: not taken over */
    ANS_ERR_TIMEOUT = -4, /* the coprocessor or the controller did not answer in time */
    ANS_ERR_NOMEM = -5,   /* the environment could not allocate device memory */
    ANS_ERR_DEVICE = -6,  /* the coprocessor or the controller answered wrong */
    ANS_ERR_DENIED = -7,  /* the NVMe command is not on the whitelist */
    ANS_ERR_PRP = -8,     /* the read's memory cannot be described by PRPs */
    ANS_ERR_BUSY = -9,    /* the tag is in flight already */
    ANS_ERR_CRASHED = -10 /* the coprocessor crashed */
};

/* What the environment tells the core. findings/0071 conclusions 19, 32. */
struct ans_config {
    bool ioq_registers;    /* nvme-secure-bar: write 0x1200 and 0x1208 after the I/O queues are made */
    uint32_t sart_version; /* 0, 2, 3 or 4 */
    uint32_t io_depth;     /* read tags in flight at once, 1..ANS_MAX_IO_DEPTH */
    uint32_t max_transfer; /* the largest read in bytes, at most ANS_MAX_TRANSFER;
                              checked against the controller's MDTS at start */
};

/* What ans_start() found out. The strings are ASCII, padded with spaces, ended by NUL. */
struct ans_info {
    uint64_t blocks;     /* namespace 1: number of blocks */
    uint32_t block_size; /* bytes per block, 512..4096 */
    uint32_t mdts_bytes; /* largest transfer the controller takes; 0 = no limit */
    char serial[21];
    char model[41];
    char firmware[9];
};

/*
 * Things that went wrong without stopping the call that met them. Counted from the
 * first ans_start() on, never reset; all should stay 0.
 */
struct ans_stats {
    uint32_t msgs_ignored;       /* coprocessor messages of a kind the core does not know:
                                    logged and not answered, as m1n1 does */
    uint32_t msgs_refused;       /* coprocessor messages that needed an answer the core could
                                    not give (a buffer asked for while running, a second
                                    buffer, one at the coprocessor's own address): the core
                                    goes to ANS_FAILED */
    uint32_t msgs_lost;          /* messages dropped because the receive ring was full, or not
                                    sent because the send ring was full: ANS_FAILED */
    uint32_t nvmmu_inval_failed; /* NVMMU_TCB_STAT not 0 after a completion */
    uint32_t stray_completions;  /* completions for a tag that was not in flight */
};

/* A physically contiguous piece of the memory a read goes to, in device addresses. */
struct ans_segment {
    uint64_t addr;
    uint32_t len;
};

/*
 * Called from ans_isr() for every read that completed. status is 0 for success,
 * otherwise the NVMe status field of the completion (bits 0-7 code, 8-10 type).
 */
typedef void ans_done_fn(void *ctx, uint16_t tag, int status);

/* How far the hardware is up; the steps of ans_start() in order. */
enum ans_hw {
    ANS_HW_NONE = 0,
    ANS_HW_MEMORY,  /* device memory allocated */
    ANS_HW_CPU_RUN, /* the coprocessor's RUN bit may be set */
    ANS_HW_RTKIT,   /* the coprocessor is running and talking */
    ANS_HW_NVME_EN, /* the admin queue is set up, CC.EN may be set */
    ANS_HW_IOCQ,    /* the I/O completion queue exists */
    ANS_HW_IOSQ,    /* the I/O submission queue exists */
};

struct ans_core {
    enum ans_state state;
    enum ans_hw hw;
    uint32_t halts; /* ans_halt() calls; a wait that sees it change gives up */
    struct ans_env *env;
    struct ans_config config;
    struct ans_info info;
    struct ans_stats stats;
    struct ans_asc asc;
    struct ans_sart sart;
    struct ans_rtkit rtkit;
    struct ans_nvme nvme;
};

/*
 * ans_start() and ans_stop() wait, and are the only calls that allocate or free device
 * memory or call ans_env_wait(). The others never wait, allocate or free, and may be
 * called with the environment's lock held at any IRQL: ans_probe(), ans_halt(),
 * ans_read(), ans_isr(), ans_mbox_service() and the three ans_get_*().
 *
 * ans_start() fails without touching anything when the RUN bit is set already (the
 * controller is not taken over) or the configuration is wrong; the state stays
 * ANS_OFF. Once it has begun, a failure brings the controller down again and leaves
 * ANS_FAILED (or ANS_CRASHED): it is not tried again.
 *
 * ans_stop() and ans_halt() never call done for reads still in flight; the caller
 * completes those itself (Storport has none in flight when it stops the adapter).
 *
 * After ans_mbox_service() the caller looks at ans_get_state(): ANS_CRASHED or
 * ANS_FAILED means the core has stopped the controller on the spot (as ans_halt()
 * does) and no read in flight will complete; the caller completes them as failed.
 * After a crash the core never touches the NVMe registers again.
 */
int ans_probe(struct ans_core *core, struct ans_env *env);
int ans_start(struct ans_core *core, struct ans_env *env, const struct ans_config *config);
void ans_stop(struct ans_core *core);
void ans_halt(struct ans_core *core);
int ans_read(struct ans_core *core, uint16_t tag, uint64_t lba, uint32_t blocks,
             const struct ans_segment *segs, uint32_t nsegs);
bool ans_isr(struct ans_core *core, ans_done_fn *done, void *ctx);
void ans_mbox_service(struct ans_core *core);

enum ans_state ans_get_state(const struct ans_core *core);
const struct ans_info *ans_get_info(const struct ans_core *core);
const struct ans_stats *ans_get_stats(const struct ans_core *core);

#endif
