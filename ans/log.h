/* SPDX-License-Identifier: MIT */
/*
 * The miniport's log: a block of counters and a ring of text lines in nonpaged
 * memory, so it can be read without a debugger, from a copy of the machine's
 * physical memory taken after the guest is stopped (SiliconNT findings/0073), or with
 * the debugger (dx *ans!g_ans_log).
 *
 * Where it lives: first in a static buffer in the driver image; from the first
 * HwFindAdapter on in memory of its own (StorPortAllocateDmaMemory), copied over,
 * which is never given back. When the device does not start, Windows unloads the
 * driver and its image's pages go back to the system and get overwritten: the log
 * must outlive the driver, because a start that fails is what it is most needed for.
 * Should that allocation fail, the log stays in the image.
 *
 * The pages of the image lie anywhere in physical memory, so the log is made of whole
 * 4 KiB pages, each aligned to 4 KiB (each one is one physical page) and each starting
 * with the same small header: a reader finds every page by its marker and puts the
 * text back together by page number, without the page tables. After the copy only
 * the kept pages carry the marker.
 *
 * Layout, version 1. Little endian. Page header (every page, ANS_LOG_HEAD bytes):
 *
 *    0  magic[16]   "SiliconNTansLog1" (16 ASCII bytes, no NUL). The driver file holds
 *                   it only scrambled, so a search of memory finds the live log only.
 *   16  u32 version    1
 *   20  u32 index      0 for the counter page, 1..ANS_LOG_TEXT_PAGES for text
 *   24  u32 pages      1 + ANS_LOG_TEXT_PAGES
 *   28  u32 text_per_page  ANS_LOG_PAGE_TEXT
 *   32  u64 instance   the same on every page of one load of the driver, different
 *                      the next time (memory may still hold an older log)
 *   40  u64 reserved
 *
 * Counter page (index 0), after the header:
 *   48  u64 written    text bytes ever written. Text byte number q is in text page
 *                      1 + (q / text_per_page) % (pages - 1), at offset
 *                      ANS_LOG_HEAD + q % text_per_page. Once written exceeds
 *                      (pages - 1) * text_per_page, the oldest text is overwritten.
 *   56  u32 counters   number of counters that follow
 *   60  u32 reserved
 *   64  u32 counter[ANS_LOG_COUNTERS], in the order of enum ans_log_counter
 *
 * Each line: "[sssss.uuuuuu] text\n", seconds and microseconds since the driver was
 * loaded (the generic timer, CNTVCT_EL0 / CNTFRQ_EL0). Plain ASCII. Lines never hold
 * the disk's serial number or anything read from the disk.
 *
 * Lines are also sent to an attached kernel debugger (StorPortDebugPrint, error
 * level), prefixed "ans: ".
 */
#ifndef ANS_LOG_H
#define ANS_LOG_H

#include <stdarg.h>

#include "core/ans_env.h"

#define ANS_LOG_VERSION    1
#define ANS_LOG_PAGE       4096u
#define ANS_LOG_HEAD       48u
#define ANS_LOG_PAGE_TEXT  (ANS_LOG_PAGE - ANS_LOG_HEAD)
#define ANS_LOG_TEXT_PAGES 64u /* about 253 KiB of text */

/*
 * The counters. Only ever added to; never reset. Interrupt counts: "mbox" calls took
 * at least one message out of the mailbox FIFO, "cq" calls took at least one
 * completion off a queue, "none" calls found neither (the handler cannot tell which
 * line fired: both go to the same routine, findings/0071 conclusion 2).
 */
enum ans_log_counter {
    ANS_LC_PHASE = 0,        /* enum ans_phase of the adapter (latest) */
    ANS_LC_CORE_STATE,       /* ans_get_state() (latest) */
    ANS_LC_START_RESULT,     /* what ans_start() returned (latest), as a signed value */
    ANS_LC_ISR_CALLS,        /* HwInterrupt calls */
    ANS_LC_ISR_MBOX,
    ANS_LC_ISR_CQ,
    ANS_LC_ISR_NONE,
    ANS_LC_EARLY_CALLS,      /* HwInterrupt calls after the interrupts were connected and
                                before the start began (passive initialization) */
    ANS_LC_EARLY_MBOX,
    ANS_LC_EARLY_NONE,
    ANS_LC_MBOX_MSGS,        /* messages taken out of the mailbox FIFO, all in all */
    ANS_LC_COMPLETIONS,      /* completions taken off the queues, all in all */
    ANS_LC_DPC_CALLS,
    ANS_LC_READS_SENT,       /* reads handed to the controller */
    ANS_LC_READS_OK,
    ANS_LC_READS_FAILED,     /* completed by the controller with an error status */
    ANS_LC_READS_REFUSED,    /* not sent: the core refused, or the controller is not up */
    ANS_LC_READS_ABORTED,    /* sent, then completed by the miniport when it stopped the
                                controller (halt, reset, crash) */
    ANS_LC_BYTES_READ_LO,    /* bytes read successfully, low and high halves */
    ANS_LC_BYTES_READ_HI,
    ANS_LC_WRITES_REFUSED,   /* commands that write, refused (each one also logged,
                                up to ANS_LOG_WRITE_LINES lines) */
    ANS_LC_PASSTHROUGH_REFUSED, /* SRB_FUNCTION_IO_CONTROL and _PROTOCOL_COMMAND */
    ANS_LC_STATS_MSGS_IGNORED,  /* ans_get_stats(), copied after each interrupt, DPC,
                                   start and stop */
    ANS_LC_STATS_MSGS_REFUSED,
    ANS_LC_STATS_MSGS_LOST,
    ANS_LC_STATS_NVMMU_INVAL_FAILED,
    ANS_LC_STATS_STRAY_COMPLETIONS,
    ANS_LC_BLOCKS_LO,        /* the disk, once started */
    ANS_LC_BLOCKS_HI,
    ANS_LC_BLOCK_SIZE,
    ANS_LC_MDTS_BYTES,
    ANS_LC_LINES,            /* lines written */
    ANS_LOG_COUNTERS
};

#define ANS_LOG_WRITE_LINES 256 /* refused writes logged one by one, then only counted */

/* Where the adapter stands, for ANS_LC_PHASE. */
enum ans_phase {
    ANS_PHASE_LOADED = 0,     /* DriverEntry ran */
    ANS_PHASE_FOUND,          /* HwFindAdapter succeeded; interrupts get connected next */
    ANS_PHASE_STARTING,       /* passive initialization: ans_start() */
    ANS_PHASE_UP,             /* the disk is there */
    ANS_PHASE_START_FAILED,   /* ans_start() failed: the device did not start */
    ANS_PHASE_STOPPED,        /* stopped for shutdown, removal, or halted */
    ANS_PHASE_REFUSED         /* HwFindAdapter refused (dump mode, bad resources ...) */
};

/* DriverEntry: writes the page headers; timestamps count from now. */
void ans_log_init(void);

/*
 * HwFindAdapter (PASSIVE_LEVEL, nothing else logging yet): moves the log into memory
 * that outlives the driver. dev is the device extension. Only the first call does it.
 */
void ans_log_keep(void *dev);

/* One line; the text without the newline. Any IRQL. */
void ans_log(const char *fmt, ...);
void ans_vlog(const char *fmt, va_list ap);

/* Counters, any IRQL. ans_log_add() returns the new value. */
long ans_log_add(enum ans_log_counter c, long n);
void ans_log_set(enum ans_log_counter c, long v);
long ans_log_get(enum ans_log_counter c);

#endif
