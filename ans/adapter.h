/* SPDX-License-Identifier: MIT */
/*
 * The miniport's per-adapter state, shared by miniport.c (the Storport entry points)
 * and env_storport.c (the core's environment on Windows).
 */
#ifndef ANS_ADAPTER_H
#define ANS_ADAPTER_H

#include <miniport.h> /* base types and INTERFACE_TYPE; storport.h does not include it */
#include <storport.h>

#include "core/ans_core.h"
#include "log.h"
#include "scsi.h"

#if NTDDI_VERSION < NTDDI_WIN10_VB
#error "the Storport event functions this driver waits with need NTDDI_WIN10_VB or later"
#endif

/* Reads in flight at once: tags 0..62 (findings/0071 conclusions 8, 21). */
#define ANS_IO_DEPTH ANS_MAX_IO_DEPTH
/* 63 pages: any buffer of this size, wherever it starts, spans at most 64 pages. */
#define ANS_MAX_TRANSFER_BYTES (63u * 4096u)
#define ANS_MAX_SEGMENTS 64

/*
 * The core's environment on Windows (ans_env.h). Every call into the core is made
 * with Storport's interrupt lock held (findings/0071 conclusion 5): HwInterrupt and
 * ScsiStopAdapter get it from Storport, HwStartIo, the DPC and HwResetBus through
 * StorPortSynchronizeAccess, and the two passive callers (passive initialization and
 * the stop work item) with ans_env_enter(). Only those two call ans_start() and
 * ans_stop(), the core's only calls that wait or allocate; there the environment gives
 * the lock up while it waits or allocates.
 */
struct ans_env {
    void *dev;                                /* the device extension */
    volatile UCHAR *base[ANS_RGN_COUNT];      /* mapped _CRS memory ranges, in order */
    ULONG length[ANS_RGN_COUNT];
    bool passive;                             /* the lock was taken by ans_env_enter() */
    STOR_LOCK_HANDLE lock;
    STOR_EVENT wake;                          /* set by the DPC after every interrupt */
    LARGE_INTEGER freq;                       /* performance counter frequency */
    volatile LONG mbox_reads;                 /* messages read out of the mailbox FIFO */
    volatile LONG completions;                /* completions the core took off a queue */
};

/* PASSIVE_LEVEL only: take and give back the interrupt lock around ans_start()/ans_stop(). */
void ans_env_enter(struct ans_env *env);
void ans_env_leave(struct ans_env *env);

typedef struct _ANS_ADAPTER {
    struct ans_env env;
    struct ans_core core;
    struct ans_config config;
    enum ans_phase phase;                /* where the adapter stands (log.h); also in the log */
    bool found;                          /* HwFindAdapter succeeded once */
    bool disk_present;                   /* started, and not stopped since: SCSI answers for a disk */
    struct ans_scsi_disk disk;
    STOR_DPC dpc;
    bool dpc_ready;                      /* dpc initialized (HwInitialize) */
    PVOID worker;                        /* the stop work item */
    PSCSI_REQUEST_BLOCK tags[ANS_IO_DEPTH]; /* reads in flight, by tag; under the interrupt lock */
    struct ans_segment segs[ANS_MAX_SEGMENTS]; /* the read being sent; under the StartIo lock */
    ULONG acpi_out[256];                 /* _DSD as ACPI returns it */
} ANS_ADAPTER, *PANS_ADAPTER;

#endif
