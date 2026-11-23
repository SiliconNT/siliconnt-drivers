/* SPDX-License-Identifier: MIT */
/*
 * A fake disk controller for the model test: the coprocessor and its mailbox, the
 * SART, the NVMe controller with its NVMMU, the power state register, and the memory
 * the controller reaches by DMA. It follows what m1n1 and Linux show the real one
 * does, as far as the core depends on it, and it checks every access the core makes.
 * Anything the core does that the real controller would not take, or that the project
 * forbids, is counted as a violation.
 */

#ifndef FAKE_ANS_H
#define FAKE_ANS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Region sizes as the ACPI node gives them on the M4 (findings/0062 conclusion 1). */
#define FAKE_NVME_SIZE 0x60000u
#define FAKE_ASC_SIZE  0x88000u
#define FAKE_SART_SIZE 0xc000u
#define FAKE_PMGR_SIZE 4u

#define FAKE_MAX_BLOCKS 512
#define FAKE_MAX_CMDS   4096
#define FAKE_FIFO       64

struct fake_msg {
    uint64_t msg0;
    uint32_t msg1;
};

/* Memory the controller can reach, at made-up physical addresses above 2^40. */
struct fake_block {
    uint64_t dva;
    size_t size;
    void *va;
    bool live;
    bool from_env; /* allocated through ans_env_dma_alloc() */
};

/* What the coprocessor is doing. */
enum fake_cp {
    CP_OFF = 0,      /* RUN clear */
    CP_DEAD,         /* RUN set but it will never answer (needs a reset, or told not to) */
    CP_WAIT_WAKE,    /* RUN set, waiting for the wakeup message */
    CP_HELLO_SENT,
    CP_EPMAP_SENT,   /* first endpoint map message sent */
    CP_EPMAP_SENT2,  /* second (last) endpoint map message sent */
    CP_STARTING_EPS, /* map done, endpoints being started */
    CP_WAIT_BUFFERS, /* buffer requests sent */
    CP_WAIT_AP_ON,   /* IOP power "on" reported */
    CP_RUNNING,
    CP_QUIESCED,
    CP_ASLEEP,
    CP_CRASHED,
};

struct fake_cmd_record {
    int queue; /* 0 admin, 1 I/O */
    uint8_t opcode;
    uint32_t nsid;
    uint32_t cdw10;
};

struct fake {
    /* --- set by the test before the core starts --- */
    uint32_t sart_version;   /* register layout of the SART */
    bool secure_bar;         /* expects 0x1200/0x1208 after the I/O queues (M4) */
    uint8_t mdts;            /* identify controller byte 77 */
    uint64_t nsze;           /* namespace 1 size in blocks */
    uint32_t lbads;          /* log2 of the block size */
    bool needs_reset;        /* RUN was cleared without a reset since: no HELLO */
    bool silent;             /* the coprocessor never answers */
    bool bad_version;        /* HELLO offers versions the core does not speak */
    bool never_ready;        /* CSTS.RDY never comes */
    bool never_boot_status;  /* 0x1300 never shows the OK value */
    int hang_admin_opcode;   /* admin opcode that never completes, -1 none */
    int fail_admin_opcode;   /* admin opcode that completes with an error, -1 none */
    bool hang_reads;         /* reads never complete */
    int fail_read_status;    /* status for reads, 0 success */
    bool no_sleep;           /* never acknowledges going to sleep */
    bool crash_at_boot;      /* crashes right after its buffers are given */
    bool buffer_at_own_addr; /* asks for a buffer at an address of its own */
    uint32_t tcb_stat;       /* value of NVMMU_TCB_STAT */
    uint32_t a2i_capacity;   /* how many messages the host-to-coprocessor FIFO holds */
    bool pmgr_inactive;      /* the power state register does not say "active" */
    bool pmgr_all_ones;      /* the power state register reads as all ones */
    bool extra_messages;     /* sends syslog and ioreport messages that must be acked */
    bool no_ioreport_buffer; /* does not ask for the ioreport buffer while booting */

    /* --- registers --- */
    uint32_t cpu_control;
    struct fake_msg a2i[FAKE_FIFO];
    uint32_t a2i_n;
    uint64_t a2i_send0;
    bool a2i_send0_written;
    struct fake_msg i2a[FAKE_FIFO];
    uint32_t i2a_head, i2a_n;
    bool i2a_recv0_read;
    uint32_t nvme_cc, nvme_csts, nvme_aqa;
    uint64_t nvme_asq, nvme_acq;
    uint32_t linear_sq_ctrl, max_pend, nvmmu_num;
    uint64_t nvmmu_asq_base, nvmmu_iosq_base;
    uint64_t ioq_cmds, ioq_cqes;
    uint32_t sart[FAKE_SART_SIZE / 4];
    uint32_t sart_at_start[FAKE_SART_SIZE / 4];
    uint32_t pmgr;

    /* --- coprocessor --- */
    enum fake_cp cp;
    uint32_t eps_started;
    uint32_t buffers_answered;
    uint64_t crashlog_dva;
    bool crash_now; /* set by a test: crash on the next tick */
    uint32_t acks_expected, acks_seen;

    /* --- NVMe --- */
    bool iocq_made, iosq_made;
    uint64_t iocq_dva, iosq_dva;
    uint32_t cq_tail[2];
    uint32_t cq_phase[2];
    uint32_t cq_head[2];
    int pending[2][64];   /* tags rung and not yet executed, in order; -1 empty */
    uint32_t npending[2];
    uint64_t busy[2];     /* tags handed to the controller and not completed */
    uint32_t last_inval;
    bool irq;             /* a completion was posted since the test last looked */

    /* --- power manager --- */
    int reset_step;
    uint64_t reset_t0;
    uint32_t resets;

    /* --- memory --- */
    struct fake_block blocks[FAKE_MAX_BLOCKS];
    uint32_t nblocks;
    uint64_t next_dva;

    /* --- clock and records --- */
    uint64_t now_us;
    uint32_t violations;
    char first_violation[512];
    struct fake_cmd_record cmds[FAKE_MAX_CMDS];
    uint32_t ncmds;
    uint32_t doorbells;
    uint32_t reg_writes;
    uint32_t run_sets;       /* times the RUN bit went from 0 to 1 */
};

void fake_init(struct fake *f, uint32_t sart_version);
void fake_violation(struct fake *f, const char *fmt, ...);

uint64_t fake_reg_read(struct fake *f, int rgn, uint32_t off, unsigned width);
void fake_reg_write(struct fake *f, int rgn, uint32_t off, unsigned width, uint64_t val);

void fake_tick(struct fake *f);
void fake_post_cqe(struct fake *f, int q, uint32_t tag, uint32_t status);
void fake_advance(struct fake *f, uint32_t usec);

bool fake_mem_alloc(struct fake *f, size_t size, bool from_env, void **va, uint64_t *dva);
void fake_mem_free(struct fake *f, uint64_t dva);
void *fake_mem_at(struct fake *f, uint64_t dva, size_t len);
uint32_t fake_live_env_blocks(const struct fake *f);

bool fake_sart_covers(struct fake *f, uint64_t dva, uint64_t size);
uint32_t fake_sart_entries_in_use(struct fake *f);
bool fake_sart_unchanged_iboot(struct fake *f);

bool fake_whitelisted(int queue, uint8_t opcode, uint32_t nsid, uint32_t cdw10, uint32_t cdw11,
                      uint8_t flags);
uint8_t fake_disk_byte(uint64_t lba, uint32_t block_size, uint32_t i);

#endif
