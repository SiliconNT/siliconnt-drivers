/* SPDX-License-Identifier: MIT */
/*
 * ans: Storport miniport for the internal disk controller of Apple silicon Macs (ANS).
 * Read-only.
 *
 * The device is ACPI\SLNT0001, the ANS0 node the SiliconNT firmware builds from the
 * Apple device tree: four memory ranges (NVMe, coprocessor and mailbox, SART, the
 * controller's power state register), two level-triggered interrupts (controller,
 * mailbox "message received") and a _DSD with "sart-version" and, on some machines,
 * "nvme-secure-bar" (SiliconNT findings/0062 conclusions 1, 2).
 *
 * How it is built, and why, is SiliconNT findings/0071; what this file adds, and the
 * log it keeps, findings/0073. In short:
 *   - Everything that touches the controller is the core (core/, ans_core.h). This file
 *     never touches a register or a queue itself.
 *   - HwFindAdapter maps the ranges, reads _DSD, and checks the controller was handed
 *     over stopped (ans_probe()). The coprocessor is started in the passive
 *     initialization routine, after the interrupts are connected (ans_start()).
 *   - The disk is read-only: Windows is told it is write protected, every command that
 *     writes is refused without touching the controller, and so are the pass-through
 *     requests. A read goes to the core with the physical addresses Storport gives.
 *   - Shutdown and removal bring the controller down in a work item (ans_stop());
 *     ScsiStopAdapter and HwResetBus stop it at once (ans_halt()).
 *   - Crash dump and hibernation (the "dump_" copy of the driver) are not supported:
 *     loaded that way, it refuses the adapter.
 */

#include "adapter.h"
#include "dsd.h"

#define STR2(x) #x
#define STR(x) STR2(x)
#ifdef ANS_BUILD_ID
#define BUILD STR(ANS_BUILD_ID)
#else
#define BUILD "(no build id)"
#endif

/* _DSD as a method name for StorPortInvokeAcpiMethod: the bytes '_', 'D', 'S', 'D'. */
#define ACPI_METHOD_DSD ((ULONG)('_' | ('D' << 8) | ('S' << 16) | ('D' << 24)))

/* STORAGE_BUS_TYPE BusTypeNvme (ntddstor.h, which storport.h does not include). */
#define ANS_BUS_TYPE_NVME 0x11

/* Sense data for what the miniport fails itself. */
#define SENSE_MEDIUM_ERROR   0x03
#define SENSE_HARDWARE_ERROR 0x04
#define ASC_UNRECOVERED_READ 0x11
#define ASC_INTERNAL_FAILURE 0x44

/* How many lines of a kind are logged before only the counters go on (log.h). */
#define LOG_CAP_UNKNOWN  64
#define LOG_CAP_READS    64
#define LOG_CAP_EARLY    16
#define LOG_CAP_PASS     16

sp_DRIVER_INITIALIZE DriverEntry;
HW_FIND_ADAPTER AnsFindAdapter;
HW_INITIALIZE AnsInitialize;
HW_PASSIVE_INITIALIZE_ROUTINE AnsPassiveInitialize;
HW_STARTIO AnsStartIo;
HW_INTERRUPT AnsInterrupt;
HW_MESSAGE_SIGNALED_INTERRUPT_ROUTINE AnsMessageInterrupt;
HW_DPC_ROUTINE AnsDpc;
HW_RESET_BUS AnsResetBus;
HW_ADAPTER_CONTROL AnsAdapterControl;
HW_WORKITEM AnsStopWork;

static long g_logged_unknown, g_logged_reads, g_logged_early, g_logged_pass;

/* True the first `cap` times for a counter of logged lines. */
static bool log_quota(long *counter, long cap)
{
    return InterlockedIncrement(counter) <= cap;
}

static const char *state_name(enum ans_state s)
{
    switch (s) {
        case ANS_OFF:
            return "off";
        case ANS_STARTING:
            return "starting";
        case ANS_READY:
            return "ready";
        case ANS_STOPPING:
            return "stopping";
        case ANS_FAILED:
            return "failed";
        case ANS_CRASHED:
            return "crashed";
    }
    return "?";
}

static void set_phase(PANS_ADAPTER a, enum ans_phase phase)
{
    a->phase = phase;
    ans_log_set(ANS_LC_PHASE, (long)phase);
}

/*
 * Mirrors the core's state and its counters into the log's counter block, and logs a
 * line whenever one of the five counters moved. Under the interrupt lock.
 */
static void copy_stats(PANS_ADAPTER a)
{
    const struct ans_stats *s = ans_get_stats(&a->core);
    static const enum ans_log_counter where[5] = {
        ANS_LC_STATS_MSGS_IGNORED, ANS_LC_STATS_MSGS_REFUSED, ANS_LC_STATS_MSGS_LOST,
        ANS_LC_STATS_NVMMU_INVAL_FAILED, ANS_LC_STATS_STRAY_COMPLETIONS,
    };
    uint32_t now[5];
    bool moved = false;

    now[0] = s->msgs_ignored;
    now[1] = s->msgs_refused;
    now[2] = s->msgs_lost;
    now[3] = s->nvmmu_inval_failed;
    now[4] = s->stray_completions;
    for (int i = 0; i < 5; i++) {
        if ((uint32_t)ans_log_get(where[i]) != now[i]) {
            moved = true;
            ans_log_set(where[i], (long)now[i]);
        }
    }
    ans_log_set(ANS_LC_CORE_STATE, (long)ans_get_state(&a->core));
    if (moved)
        ans_log("stats: msgs_ignored %u msgs_refused %u msgs_lost %u nvmmu_inval_failed %u "
                "stray_completions %u",
                now[0], now[1], now[2], now[3], now[4]);
}

/* --------------------------------------------------------------------------------- */
/* Completing requests                                                                 */

static void complete(PANS_ADAPTER a, PSCSI_REQUEST_BLOCK srb, UCHAR status)
{
    srb->SrbStatus = status;
    StorPortNotification(RequestComplete, a, srb);
}

/* CHECK CONDITION with fixed-format sense, when the request has room for it. */
static void complete_check(PANS_ADAPTER a, PSCSI_REQUEST_BLOCK srb,
                           const struct ans_scsi_reply *reply)
{
    UCHAR status = SRB_STATUS_ERROR;

    srb->ScsiStatus = ANS_SCSI_CHECK_CONDITION;
    srb->DataTransferLength = 0;
    if (srb->SenseInfoBuffer && srb->SenseInfoBufferLength &&
        !(srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        srb->SenseInfoBufferLength = (UCHAR)ans_scsi_sense(reply, (uint8_t *)srb->SenseInfoBuffer,
                                                           srb->SenseInfoBufferLength);
        status |= SRB_STATUS_AUTOSENSE_VALID;
    }
    complete(a, srb, status);
}

static void complete_sense(PANS_ADAPTER a, PSCSI_REQUEST_BLOCK srb, uint8_t key, uint8_t asc)
{
    struct ans_scsi_reply reply;

    ans_scsi_check(&reply, key, asc, 0);
    complete_check(a, srb, &reply);
}

/*
 * Completes every read the controller had, after it was stopped without finishing
 * them (ans_halt(), a crash): the controller does no more DMA, so the memory can go
 * back. Under the interrupt lock.
 */
static void fail_in_flight(PANS_ADAPTER a, const char *why)
{
    ULONG n = 0;

    for (ULONG tag = 0; tag < ANS_IO_DEPTH; tag++) {
        PSCSI_REQUEST_BLOCK srb = a->tags[tag];

        if (!srb)
            continue;
        a->tags[tag] = NULL;
        srb->DataTransferLength = 0;
        complete(a, srb, SRB_STATUS_BUS_RESET);
        n++;
    }
    if (n) {
        ans_log_add(ANS_LC_READS_ABORTED, (long)n);
        ans_log("reads: %u in flight completed as failed (%s)", n, why);
    }
}

/* Stops the controller now, from wherever (lock held): ScsiStopAdapter, HwResetBus. */
static void halt(PANS_ADAPTER a, const char *why)
{
    enum ans_state s = ans_get_state(&a->core);

    if (s != ANS_OFF) {
        ans_log("halt: %s, controller %s: stopping it now", why, state_name(s));
        ans_halt(&a->core);
        set_phase(a, ANS_PHASE_STOPPED);
    }
    fail_in_flight(a, why);
    copy_stats(a);
}

/* --------------------------------------------------------------------------------- */
/* Reads                                                                               */

static void add_bytes_read(ULONG n)
{
    ULONGLONG total = ((ULONGLONG)(ULONG)ans_log_get(ANS_LC_BYTES_READ_HI) << 32) |
                      (ULONG)ans_log_get(ANS_LC_BYTES_READ_LO);

    total += n;
    ans_log_set(ANS_LC_BYTES_READ_LO, (long)(ULONG)total);
    ans_log_set(ANS_LC_BYTES_READ_HI, (long)(ULONG)(total >> 32));
}

/* Called by ans_isr() for each read the controller finished; interrupt lock held. */
static void read_done(void *ctx, uint16_t tag, int status)
{
    PANS_ADAPTER a = ctx;
    PSCSI_REQUEST_BLOCK srb = tag < ANS_IO_DEPTH ? a->tags[tag] : NULL;

    if (!srb) {
        ans_log("reads: completion for tag %u, which has no request", tag);
        return;
    }
    a->tags[tag] = NULL;
    if (status == 0) {
        ans_log_add(ANS_LC_READS_OK, 1);
        add_bytes_read(srb->DataTransferLength);
        srb->ScsiStatus = ANS_SCSI_GOOD;
        complete(a, srb, SRB_STATUS_SUCCESS);
        return;
    }
    ans_log_add(ANS_LC_READS_FAILED, 1);
    if (log_quota(&g_logged_reads, LOG_CAP_READS))
        ans_log("reads: tag %u failed, NVMe status 0x%x", tag, status);
    complete_sense(a, srb, SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ);
}

#define SUBMIT_NO_TAG 1 /* besides the core's own results */

struct submit {
    PSCSI_REQUEST_BLOCK srb;
    uint64_t lba;
    uint32_t blocks;
    uint32_t nsegs;
    int result;
};

/* Under the interrupt lock (StorPortSynchronizeAccess): take a tag and send the read. */
static BOOLEAN submit_read(PVOID dev, PVOID context)
{
    PANS_ADAPTER a = dev;
    struct submit *s = context;
    ULONG tag;

    for (tag = 0; tag < ANS_IO_DEPTH; tag++) {
        if (!a->tags[tag])
            break;
    }
    if (tag == ANS_IO_DEPTH) {
        s->result = SUBMIT_NO_TAG;
        return TRUE;
    }
    a->tags[tag] = s->srb;
    s->result = ans_read(&a->core, (uint16_t)tag, s->lba, s->blocks, a->segs, s->nsegs);
    if (s->result != ANS_OK)
        a->tags[tag] = NULL;
    /* on success the request may already be completed (another processor): not touched */
    return TRUE;
}

/*
 * A read: the memory is what Storport's scatter/gather list says, physical addresses
 * as they are; the core checks them against the PRP rules (findings/0071 c. 7, 20).
 */
static void send_read(PANS_ADAPTER a, PSCSI_REQUEST_BLOCK srb, const struct ans_scsi_reply *reply)
{
    PSTOR_SCATTER_GATHER_LIST sg = StorPortGetScatterGatherList(a, srb);
    struct submit s;

    if (!sg || sg->NumberOfElements == 0 || sg->NumberOfElements > ANS_MAX_SEGMENTS) {
        ans_log_add(ANS_LC_READS_REFUSED, 1);
        if (log_quota(&g_logged_reads, LOG_CAP_READS))
            ans_log("reads: %u blocks at %llu: scatter/gather list with %u elements, not sent",
                    reply->blocks, (unsigned long long)reply->lba, sg ? sg->NumberOfElements : 0);
        complete_sense(a, srb, SENSE_HARDWARE_ERROR, ASC_INTERNAL_FAILURE);
        return;
    }
    for (ULONG i = 0; i < sg->NumberOfElements; i++) {
        a->segs[i].addr = (uint64_t)sg->List[i].PhysicalAddress.QuadPart;
        a->segs[i].len = sg->List[i].Length;
    }

    s.srb = srb;
    s.lba = reply->lba;
    s.blocks = reply->blocks;
    s.nsegs = sg->NumberOfElements;
    s.result = ANS_ERR_STATE;
    StorPortSynchronizeAccess(a, submit_read, &s);

    if (s.result == ANS_OK) {
        ans_log_add(ANS_LC_READS_SENT, 1);
        return;
    }
    if (s.result == SUBMIT_NO_TAG) {
        complete(a, srb, SRB_STATUS_BUSY); /* Storport sends it again later */
        return;
    }
    ans_log_add(ANS_LC_READS_REFUSED, 1);
    if (log_quota(&g_logged_reads, LOG_CAP_READS))
        ans_log("reads: %u blocks at %llu not sent (%d), controller %s", reply->blocks,
                (unsigned long long)reply->lba, s.result,
                state_name(ans_get_state(&a->core)));
    complete_sense(a, srb, SENSE_HARDWARE_ERROR, ASC_INTERNAL_FAILURE);
}

/* --------------------------------------------------------------------------------- */
/* SCSI commands                                                                       */

static void log_refused_write(const struct ans_scsi_reply *reply)
{
    long n = ans_log_add(ANS_LC_WRITES_REFUSED, 1);

    if (n > ANS_LOG_WRITE_LINES)
        return;
    if (reply->lba_known)
        ans_log("scsi: refused write #%d: opcode 0x%02x lba %llu blocks %u", n, reply->opcode,
                (unsigned long long)reply->lba, reply->blocks);
    else
        ans_log("scsi: refused write #%d: opcode 0x%02x", n, reply->opcode);
}

static void execute_scsi(PANS_ADAPTER a, PSCSI_REQUEST_BLOCK srb)
{
    struct ans_scsi_reply reply;
    /*
     * Data goes into the request's memory only when the request says it flows that
     * way: a pass-through can send READ with its buffer marked as data out, and
     * memory locked for reading only must not be written, by the CPU or the disk.
     */
    bool data_in = (srb->SrbFlags & (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT)) == SRB_FLAGS_DATA_IN;

    /* one disk, at (0, 0, 0); before the start, after a stop, or when it failed, none */
    if (srb->PathId || srb->TargetId || srb->Lun || !a->disk_present) {
        complete(a, srb, SRB_STATUS_NO_DEVICE);
        return;
    }

    ans_scsi_execute(&a->disk, srb->Cdb, srb->CdbLength,
                     data_in ? (uint8_t *)srb->DataBuffer : NULL, srb->DataTransferLength,
                     &reply);

    if (reply.write_refused)
        log_refused_write(&reply);
    if (reply.action == ANS_SCSI_READ) {
        if (!data_in) {
            if (log_quota(&g_logged_unknown, LOG_CAP_UNKNOWN))
                ans_log("scsi: read whose data direction is not in (flags 0x%x), refused",
                        srb->SrbFlags);
            complete_sense(a, srb, ANS_SENSE_ILLEGAL_REQUEST, 0x24);
            return;
        }
        send_read(a, srb, &reply);
        return;
    }
    if (reply.status == ANS_SCSI_GOOD) {
        srb->ScsiStatus = ANS_SCSI_GOOD;
        srb->DataTransferLength = reply.length;
        complete(a, srb, SRB_STATUS_SUCCESS);
        return;
    }
    if (!reply.write_refused && log_quota(&g_logged_unknown, LOG_CAP_UNKNOWN))
        ans_log("scsi: opcode 0x%02x answered with sense %x/%02x", reply.opcode,
                reply.sense_key, reply.asc);
    complete_check(a, srb, &reply);
}

/* --------------------------------------------------------------------------------- */
/* Bringing the controller down for shutdown, hibernation, stop and removal            */

/*
 * Work item, PASSIVE_LEVEL: ans_stop() waits for the coprocessor to go to sleep, then
 * the request that asked for it is completed (findings/0071 conclusions 25, 27).
 */
VOID AnsStopWork(PVOID dev, PVOID context, PVOID worker)
{
    PANS_ADAPTER a = dev;
    PSCSI_REQUEST_BLOCK srb = context;
    enum ans_state s;

    UNREFERENCED_PARAMETER(worker);
    ans_env_enter(&a->env);
    s = ans_get_state(&a->core);
    if (s != ANS_OFF) {
        ans_log("stop: bringing the controller down, it was %s", state_name(s));
        ans_stop(&a->core);
        fail_in_flight(a, "stop");
    }
    copy_stats(a);
    s = ans_get_state(&a->core);
    a->disk_present = false;
    ans_env_leave(&a->env);

    ans_log("stop: controller %s", state_name(s));
    set_phase(a, ANS_PHASE_STOPPED);
    complete(a, srb, SRB_STATUS_SUCCESS);
}

/* For SRB_FUNCTION_POWER (D3) and SRB_FUNCTION_PNP (stop, remove) of the adapter. */
static void stop_for(PANS_ADAPTER a, PSCSI_REQUEST_BLOCK srb, const char *why)
{
    enum ans_state s = ans_get_state(&a->core); /* only the work item changes it from READY */
    ULONG status;

    if (s == ANS_OFF) {
        ans_log("stop: %s, controller already off", why);
        complete(a, srb, SRB_STATUS_SUCCESS);
        return;
    }
    ans_log("stop: %s", why);
    status = a->worker ? StorPortQueueWorkItem(a, AnsStopWork, a->worker, srb)
                       : STOR_STATUS_UNSUCCESSFUL;
    if (status != STOR_STATUS_SUCCESS) {
        /* ScsiStopAdapter, which comes next, stops the controller instead */
        ans_log("stop: cannot queue the work item (0x%x); ScsiStopAdapter will halt it", status);
        complete(a, srb, SRB_STATUS_SUCCESS);
    }
}

/* --------------------------------------------------------------------------------- */
/* Storport entry points                                                               */

BOOLEAN AnsStartIo(PVOID dev, PSCSI_REQUEST_BLOCK srb)
{
    PANS_ADAPTER a = dev;

    switch (srb->Function) {
        case SRB_FUNCTION_EXECUTE_SCSI:
            execute_scsi(a, srb);
            break;

        case SRB_FUNCTION_SHUTDOWN: /* the controller goes down on the power request */
        case SRB_FUNCTION_FLUSH:    /* nothing is ever written */
            complete(a, srb, SRB_STATUS_SUCCESS);
            break;

        case SRB_FUNCTION_POWER: {
            PSCSI_POWER_REQUEST_BLOCK p = (PSCSI_POWER_REQUEST_BLOCK)srb;

            if ((p->SrbPowerFlags & SRB_POWER_FLAGS_ADAPTER_REQUEST) &&
                p->DevicePowerState == StorPowerDeviceD3)
                stop_for(a, srb, "adapter power D3");
            else
                complete(a, srb, SRB_STATUS_SUCCESS);
            break;
        }

        case SRB_FUNCTION_PNP: {
            PSCSI_PNP_REQUEST_BLOCK p = (PSCSI_PNP_REQUEST_BLOCK)srb;

            if ((p->SrbPnPFlags & SRB_PNP_FLAGS_ADAPTER_REQUEST) &&
                (p->PnPAction == StorStopDevice || p->PnPAction == StorRemoveDevice ||
                 p->PnPAction == StorSurpriseRemoval))
                stop_for(a, srb, p->PnPAction == StorStopDevice ? "adapter stop"
                                 : p->PnPAction == StorRemoveDevice ? "adapter removal"
                                                                    : "adapter surprise removal");
            else
                complete(a, srb, SRB_STATUS_INVALID_REQUEST);
            break;
        }

        case SRB_FUNCTION_IO_CONTROL:       /* miniport IOCTLs, NVMe pass-through among them */
        case SRB_FUNCTION_PROTOCOL_COMMAND: /* NVMe pass-through */
            ans_log_add(ANS_LC_PASSTHROUGH_REFUSED, 1);
            if (log_quota(&g_logged_pass, LOG_CAP_PASS))
                ans_log("scsi: refused pass-through request, function 0x%02x", srb->Function);
            complete(a, srb, SRB_STATUS_INVALID_REQUEST);
            break;

        default:
            /* resets and aborts go up to HwResetBus (findings/0071 conclusion 28) */
            complete(a, srb, SRB_STATUS_INVALID_REQUEST);
            break;
    }
    return TRUE;
}

/*
 * Both lines come here, under one lock, and the routine cannot tell which fired
 * (findings/0071 conclusions 2, 3): the core looks at the mailbox and both completion
 * queues every time. What the core took tells which source had work (log.h).
 */
BOOLEAN AnsInterrupt(PVOID dev)
{
    PANS_ADAPTER a = dev;
    LONG mbox = a->env.mbox_reads;
    LONG done = a->env.completions;
    BOOLEAN handled = ans_isr(&a->core, read_done, a) ? TRUE : FALSE;
    LONG dm = a->env.mbox_reads - mbox;
    LONG dc = a->env.completions - done;
    bool early = a->phase == ANS_PHASE_FOUND;

    ans_log_add(ANS_LC_ISR_CALLS, 1);
    ans_log_add(ANS_LC_MBOX_MSGS, dm);
    ans_log_add(ANS_LC_COMPLETIONS, dc);
    if (dm)
        ans_log_add(ANS_LC_ISR_MBOX, 1);
    if (dc)
        ans_log_add(ANS_LC_ISR_CQ, 1);
    if (!dm && !dc)
        ans_log_add(ANS_LC_ISR_NONE, 1);
    if (early) {
        /* connected, not started: R38 saw one interrupt left over from before */
        ans_log_add(ANS_LC_EARLY_CALLS, 1);
        if (dm)
            ans_log_add(ANS_LC_EARLY_MBOX, 1);
        else
            ans_log_add(ANS_LC_EARLY_NONE, 1);
        if (log_quota(&g_logged_early, LOG_CAP_EARLY))
            ans_log("isr: interrupt before the start, %d mailbox messages thrown away", dm);
    }
    copy_stats(a);

    /*
     * The DPC answers the coprocessor and wakes ans_start() or ans_stop() when they
     * wait: needed when a message came, or anything came while the core is starting or
     * stopping. Read completions alone, while the controller is ready, need none.
     * Storport lets interrupts through only under the lock HwInitialize runs under, so
     * the DPC is set up by then; dpc_ready costs nothing should that ever change.
     */
    if (handled && a->dpc_ready && (dm || ans_get_state(&a->core) != ANS_READY))
        StorPortIssueDpc(a, &a->dpc, NULL, NULL);
    return handled;
}

/* Only called for real message-signaled interrupts, which the device has none of. */
BOOLEAN AnsMessageInterrupt(PVOID dev, ULONG message)
{
    UNREFERENCED_PARAMETER(message);
    return AnsInterrupt(dev);
}

/* Under the interrupt lock: the coprocessor's messages, after every interrupt. */
static BOOLEAN dpc_synchronized(PVOID dev, PVOID context)
{
    PANS_ADAPTER a = dev;
    enum ans_state before = ans_get_state(&a->core);
    enum ans_state after;

    UNREFERENCED_PARAMETER(context);
    ans_mbox_service(&a->core);
    after = ans_get_state(&a->core);
    if (before == ANS_READY && after != ANS_READY) {
        /* the core stopped the controller on the spot (crash, failed conversation) */
        ans_log("dpc: controller went from ready to %s", state_name(after));
        set_phase(a, ANS_PHASE_STOPPED);
        fail_in_flight(a, after == ANS_CRASHED ? "coprocessor crashed" : "coprocessor failed");
    }
    copy_stats(a);
    return TRUE;
}

VOID AnsDpc(PSTOR_DPC dpc, PVOID dev, PVOID arg1, PVOID arg2)
{
    PANS_ADAPTER a = dev;

    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(arg1);
    UNREFERENCED_PARAMETER(arg2);
    ans_log_add(ANS_LC_DPC_CALLS, 1);
    StorPortSynchronizeAccess(a, dpc_synchronized, NULL);
    /* wakes ans_start() or ans_stop() if one is waiting (env_storport.c) */
    StorPortSetEvent(a, &a->env.wake);
}

static BOOLEAN reset_synchronized(PVOID dev, PVOID context)
{
    halt((PANS_ADAPTER)dev, (const char *)context);
    return TRUE;
}

/*
 * After a timeout, once unit and target resets failed. The controller cannot abort
 * single commands; it is stopped, the reads in flight fail, and nothing is sent any
 * more (findings/0071 conclusion 28: no recovery in this version). Storport calls
 * this with its StartIo lock held, not the interrupt lock (RaidAdapterResetBus,
 * findings/0073), so the lock is taken here.
 */
BOOLEAN AnsResetBus(PVOID dev, ULONG path)
{
    ans_log("reset bus %u", path);
    StorPortSynchronizeAccess(dev, reset_synchronized, "reset bus");
    return TRUE;
}

SCSI_ADAPTER_CONTROL_STATUS
AnsAdapterControl(PVOID dev, SCSI_ADAPTER_CONTROL_TYPE type, PVOID parameters)
{
    PANS_ADAPTER a = dev;

    switch (type) {
        case ScsiQuerySupportedControlTypes: {
            PSCSI_SUPPORTED_CONTROL_TYPE_LIST list = parameters;

            /* no ScsiRestartAdapter, no ScsiAdapterPower (findings/0071 conclusion 26) */
            if (list->MaxControlType > ScsiQuerySupportedControlTypes)
                list->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
            if (list->MaxControlType > ScsiStopAdapter)
                list->SupportedTypeList[ScsiStopAdapter] = TRUE;
            return ScsiAdapterControlSuccess;
        }
        case ScsiStopAdapter:
            /* DIRQL, interrupt lock held. Normally the controller is off by now. */
            halt(a, "ScsiStopAdapter");
            a->disk_present = false; /* also if Storport starts the adapter again (c. 26) */
            return ScsiAdapterControlSuccess;
        default:
            return ScsiAdapterControlUnsuccessful;
    }
}

/*
 * PASSIVE_LEVEL, after the interrupts are connected and enabled: the start
 * (findings/0071 conclusion 4). Returning FALSE fails the device's start; Storport
 * does not try again, and neither does the core (conclusion 30).
 */
BOOLEAN AnsPassiveInitialize(PVOID dev)
{
    PANS_ADAPTER a = dev;
    const struct ans_info *info;
    enum ans_state s;
    int r;

    set_phase(a, ANS_PHASE_STARTING);
    ans_log("start: bringing the controller up (sart-version %u, nvme-secure-bar %s)",
            a->config.sart_version, a->config.ioq_registers ? "yes" : "no");

    ans_env_enter(&a->env);
    r = ans_start(&a->core, &a->env, &a->config);
    s = ans_get_state(&a->core);
    copy_stats(a);
    ans_env_leave(&a->env);
    ans_log_set(ANS_LC_START_RESULT, r);

    if (r != ANS_OK) {
        ans_log("start: failed (%d), controller %s: the device does not start", r, state_name(s));
        set_phase(a, ANS_PHASE_START_FAILED);
        return FALSE;
    }

    info = ans_get_info(&a->core);
    a->disk.blocks = info->blocks;
    a->disk.block_size = info->block_size;
    a->disk.model = info->model;
    a->disk.firmware = info->firmware;
    a->disk.serial = info->serial;
    a->disk_present = true;
    ans_log_set(ANS_LC_BLOCKS_LO, (long)(ULONG)info->blocks);
    ans_log_set(ANS_LC_BLOCKS_HI, (long)(ULONG)(info->blocks >> 32));
    ans_log_set(ANS_LC_BLOCK_SIZE, (long)info->block_size);
    ans_log_set(ANS_LC_MDTS_BYTES, (long)info->mdts_bytes);
    ans_log("start: disk of %llu blocks of %u bytes (%llu MB), read-only",
            (unsigned long long)info->blocks, info->block_size,
            (unsigned long long)(info->blocks * info->block_size / 1000000ull));
    set_phase(a, ANS_PHASE_UP);
    return TRUE;
}

/* DIRQL, interrupt lock held: nothing here touches the controller (conclusion 4). */
BOOLEAN AnsInitialize(PVOID dev)
{
    PANS_ADAPTER a = dev;

    if (!a->dpc_ready) { /* once: a second HwInitialize could find it queued */
        StorPortInitializeDpc(a, &a->dpc, AnsDpc);
        a->dpc_ready = true;
    }
    if (!a->found)
        return FALSE;
    if (a->phase != ANS_PHASE_FOUND) {
        /* called again (back to D0): the controller is not started again (c. 26) */
        ans_log("init: called again; no start");
        return TRUE;
    }
    if (!StorPortEnablePassiveInitialization(a, AnsPassiveInitialize)) {
        ans_log("init: StorPortEnablePassiveInitialization failed");
        return FALSE;
    }
    return TRUE;
}

/* "dump=" in the argument string, the other sign of a crash dump or hibernation load. */
static bool dump_argument(const char *s)
{
    for (; s && *s; s++) {
        if ((s[0] | 0x20) == 'd' && (s[1] | 0x20) == 'u' && (s[2] | 0x20) == 'm' &&
            (s[3] | 0x20) == 'p' && s[4] == '=')
            return true;
    }
    return false;
}

/* Reads _DSD (findings/0062 conclusion 2, findings/0071 conclusion 32). */
static ULONG read_dsd(PANS_ADAPTER a)
{
    struct ans_dsd dsd;
    ULONG got = 0;
    ULONG status;
    enum ans_dsd_result r;

    status = StorPortInvokeAcpiMethod(a, NULL, ACPI_METHOD_DSD, NULL, 0, a->acpi_out,
                                      sizeof(a->acpi_out), &got);
    if (status != STOR_STATUS_SUCCESS) {
        ans_log("find: evaluating _DSD failed (0x%x)", status);
        return SP_RETURN_BAD_CONFIG;
    }
    r = ans_dsd_parse(a->acpi_out, got < sizeof(a->acpi_out) ? got : sizeof(a->acpi_out), &dsd);
    if (r != ANS_DSD_OK) {
        ans_log("find: _DSD (%u bytes) not understood (%d)", got, r);
        return SP_RETURN_BAD_CONFIG;
    }
    if (!dsd.has_sart_version ||
        (dsd.sart_version != 0 && dsd.sart_version != 2 && dsd.sart_version != 3 &&
         dsd.sart_version != 4)) {
        ans_log("find: _DSD has no usable sart-version (%s %llu): not guessing one",
                dsd.has_sart_version ? "value" : "missing",
                (unsigned long long)dsd.sart_version);
        return SP_RETURN_BAD_CONFIG;
    }
    a->config.sart_version = (uint32_t)dsd.sart_version;
    a->config.ioq_registers = dsd.has_secure_bar && dsd.secure_bar != 0;
    ans_log("find: _DSD sart-version %u, nvme-secure-bar %s", a->config.sart_version,
            dsd.has_secure_bar ? (a->config.ioq_registers ? "1" : "0") : "absent");
    return SP_RETURN_FOUND;
}

/* The four memory ranges of _CRS, by position (findings/0062 conclusion 2). */
static ULONG map_ranges(PANS_ADAPTER a, PPORT_CONFIGURATION_INFORMATION config)
{
    if (config->NumberOfAccessRanges < ANS_RGN_COUNT) {
        ans_log("find: %u memory ranges, %u wanted", config->NumberOfAccessRanges, ANS_RGN_COUNT);
        return SP_RETURN_BAD_CONFIG;
    }
    for (ULONG i = 0; i < ANS_RGN_COUNT; i++) {
        PACCESS_RANGE range = &(*config->AccessRanges)[i];
        PVOID base;

        if (!range->RangeInMemory || range->RangeLength == 0) {
            ans_log("find: range %u is not memory", i);
            return SP_RETURN_BAD_CONFIG;
        }
        base = StorPortGetDeviceBase(a, config->AdapterInterfaceType, config->SystemIoBusNumber,
                                     range->RangeStart, range->RangeLength, FALSE);
        if (!base) {
            ans_log("find: range %u at %016llx cannot be mapped", i,
                    (unsigned long long)range->RangeStart.QuadPart);
            return SP_RETURN_ERROR;
        }
        a->env.base[i] = base;
        a->env.length[i] = range->RangeLength;
        ans_log("find: range %u: %016llx, 0x%x bytes", i,
                (unsigned long long)range->RangeStart.QuadPart, range->RangeLength);
    }
    return SP_RETURN_FOUND;
}

/* findings/0071 conclusion 8 */
static void fill_config(PPORT_CONFIGURATION_INFORMATION config)
{
    config->VirtualDevice = FALSE;
    config->Dma64BitAddresses = SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED;
    config->ScatterGather = TRUE;
    config->Master = TRUE;
    config->NeedPhysicalAddresses = TRUE;
    config->TaggedQueuing = TRUE;
    config->CachesData = FALSE;
    config->AlignmentMask = 3;
    config->NumberOfPhysicalBreaks = ANS_MAX_SEGMENTS;
    config->MaximumTransferLength = ANS_MAX_TRANSFER_BYTES;
    config->NumberOfBuses = 1;
    config->MaximumNumberOfTargets = 1;
    config->MaximumNumberOfLogicalUnits = 1;
    config->MaxNumberOfIO = ANS_IO_DEPTH;
    config->MaxIOsPerLun = ANS_IO_DEPTH;
    config->InitialLunQueueDepth = ANS_IO_DEPTH;
    config->SynchronizationModel = StorSynchronizeFullDuplex;
    config->InterruptSynchronizationMode = InterruptSynchronizeAll;
    config->HwMSInterruptRoutine = AnsMessageInterrupt;
}

static ULONG refuse(PANS_ADAPTER a, ULONG r)
{
    if (a->worker) {
        StorPortFreeWorker(a, a->worker);
        a->worker = NULL;
    }
    set_phase(a, ANS_PHASE_REFUSED);
    return r;
}

ULONG
AnsFindAdapter(PVOID dev, PVOID context, PVOID bus, PCHAR argument,
               PPORT_CONFIGURATION_INFORMATION config, PBOOLEAN again)
{
    PANS_ADAPTER a = dev;
    LARGE_INTEGER now;
    ULONG r;
    int pr;

    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(bus);
    UNREFERENCED_PARAMETER(again);

    /* The dump_ copy of the driver (crash dump, hibernation): not in this version. */
    if (config->DumpMode != 0 || dump_argument(argument)) {
        ans_log("find: ans build %s loaded for a crash dump or hibernation (mode %u): "
                "not supported, refused", BUILD, config->DumpMode);
        set_phase(a, ANS_PHASE_REFUSED);
        return SP_RETURN_NOT_FOUND;
    }

    ans_log_keep(dev);
    ans_log("find: ans build %s", BUILD);
    a->env.dev = dev;

    if (a->found) {
        /*
         * Again, after the adapter was stopped (back to D0, a stop for rebalancing):
         * the resources may have moved and the configuration must be given again, but
         * the controller is not brought up again; there is no disk (conclusion 26).
         */
        ans_log("find: called again; mapping again, the controller is not brought up again");
        a->disk_present = false;
        r = map_ranges(a, config);
        if (r != SP_RETURN_FOUND)
            return r;
        fill_config(config);
        return SP_RETURN_FOUND;
    }

    now.QuadPart = 0;
    StorPortQueryPerformanceCounter(dev, &a->env.freq, &now);

    r = map_ranges(a, config);
    if (r != SP_RETURN_FOUND)
        return refuse(a, r);
    r = read_dsd(a);
    if (r != SP_RETURN_FOUND)
        return refuse(a, r);

    if (StorPortInitializeEvent(dev, &a->env.wake, StorSynchronizationEvent, FALSE) !=
            STOR_STATUS_SUCCESS ||
        StorPortInitializeWorker(dev, &a->worker) != STOR_STATUS_SUCCESS) {
        ans_log("find: cannot set up the event or the work item");
        return refuse(a, SP_RETURN_ERROR);
    }

    /* Handed over stopped? Leaves both lines low before Storport connects them (c. 4). */
    pr = ans_probe(&a->core, &a->env);
    if (pr != ANS_OK) {
        ans_log("find: the controller was not handed over stopped (%d): not taken", pr);
        return refuse(a, SP_RETURN_ERROR);
    }

    a->config.io_depth = ANS_IO_DEPTH;
    a->config.max_transfer = ANS_MAX_TRANSFER_BYTES;
    fill_config(config);

    r = StorPortSetAdapterBusType(dev, ANS_BUS_TYPE_NVME);
    if (r != STOR_STATUS_SUCCESS)
        ans_log("find: StorPortSetAdapterBusType failed (0x%x)", r);

    a->found = true;
    set_phase(a, ANS_PHASE_FOUND);
    ans_log("find: done; Storport connects the interrupts next");
    return SP_RETURN_FOUND;
}

ULONG DriverEntry(PVOID driver, PVOID registry)
{
    HW_INITIALIZATION_DATA init;
    ULONG status;

    ans_log_init();
    ans_log_set(ANS_LC_PHASE, (long)ANS_PHASE_LOADED);

    RtlZeroMemory(&init, sizeof(init));
    init.HwInitializationDataSize = sizeof(init);

    /* The device is enumerated by ACPI, as the in-box storufs.sys registers. */
    init.AdapterInterfaceType = ACPIBus;

    /* The field is a PVOID because virtual miniports put another type of routine here. */
#pragma warning(suppress : 4152)
    init.HwFindAdapter = AnsFindAdapter;
    init.HwInitialize = AnsInitialize;
    init.HwStartIo = AnsStartIo;
    init.HwInterrupt = AnsInterrupt;
    init.HwResetBus = AnsResetBus;
    init.HwAdapterControl = AnsAdapterControl;

    init.DeviceExtensionSize = sizeof(ANS_ADAPTER);
    init.NumberOfAccessRanges = ANS_RGN_COUNT;
    /* INQUIRY and the like are answered by the CPU; reads and writes are never mapped */
    init.MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    init.NeedPhysicalAddresses = TRUE;
    init.TaggedQueuing = TRUE;
    init.AutoRequestSense = TRUE;
    init.MultipleRequestPerLu = TRUE;
    /* classic SCSI_REQUEST_BLOCK; FeatureSupport stays 0 (no DMA_ALLOCATION_NO_BOUNDARY) */

    status = StorPortInitialize(driver, registry, &init, NULL);
    ans_log("driver: ans build %s loaded, StorPortInitialize 0x%x", BUILD, status);
    return status;
}
