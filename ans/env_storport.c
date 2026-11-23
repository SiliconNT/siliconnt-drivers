/* SPDX-License-Identifier: MIT */
/*
 * The core's environment on Windows: ans_env.h, implemented with Storport.
 * The locking rules are in adapter.h; the reasons in SiliconNT findings/0071
 * conclusions 5, 6, 17, 18 and findings/0073.
 */

#include "adapter.h"
#include "format.h"

/*
 * Two registers the environment watches, so the interrupt handler can tell what an
 * interrupt brought (log.h, ANS_LC_ISR_MBOX and _CQ): the core reads the mailbox's
 * receive register once per message (asc.c, ASC_MBOX_I2A_RECV0) and writes the
 * NVMMU's invalidate register once per completion (nvme.c, NVMMU_TCB_INVAL).
 */
#define WATCH_MBOX_RECV0   0x8830u
#define WATCH_NVMMU_INVAL  0x28118u

/* Memory for the device: aligned to 16 KiB by allocating this much more (conclusion 18). */
#define DMA_ALIGN 0x4000u
#define DMA_EXTRA (DMA_ALIGN - PAGE_SIZE)

/* --------------------------------------------------------------------------------- */
/* Registers                                                                           */

/* An access must fit inside the region, aligned to its size (ans_env.h). */
static bool reg_ok(struct ans_env *env, enum ans_region rgn, uint32_t off, uint32_t width)
{
    if ((unsigned)rgn < ANS_RGN_COUNT && env->base[rgn] && (off & (width - 1)) == 0 &&
        off <= env->length[rgn] && width <= env->length[rgn] - off)
        return true;
    ans_log("env: refused %u-byte access to region %u offset 0x%x", width, (unsigned)rgn, off);
    return false;
}

uint32_t ans_env_read32(struct ans_env *env, enum ans_region rgn, uint32_t offset)
{
    if (!reg_ok(env, rgn, offset, 4))
        return 0xffffffffu;
    return StorPortReadRegisterUlong(env->dev, (PULONG)(env->base[rgn] + offset));
}

void ans_env_write32(struct ans_env *env, enum ans_region rgn, uint32_t offset, uint32_t value)
{
    if (!reg_ok(env, rgn, offset, 4))
        return;
    if (rgn == ANS_RGN_NVME && offset == WATCH_NVMMU_INVAL)
        InterlockedIncrement(&env->completions);
    StorPortWriteRegisterUlong(env->dev, (PULONG)(env->base[rgn] + offset), value);
}

uint64_t ans_env_read64(struct ans_env *env, enum ans_region rgn, uint32_t offset)
{
    if (!reg_ok(env, rgn, offset, 8))
        return ~0ull;
    if (rgn == ANS_RGN_ASC && offset == WATCH_MBOX_RECV0)
        InterlockedIncrement(&env->mbox_reads);
    return StorPortReadRegisterUlong64(env->dev, (PULONG64)(env->base[rgn] + offset));
}

void ans_env_write64(struct ans_env *env, enum ans_region rgn, uint32_t offset, uint64_t value)
{
    if (!reg_ok(env, rgn, offset, 8))
        return;
    StorPortWriteRegisterUlong64(env->dev, (PULONG64)(env->base[rgn] + offset), value);
}

/* --------------------------------------------------------------------------------- */
/* The lock                                                                            */

void ans_env_enter(struct ans_env *env)
{
    StorPortAcquireSpinLock(env->dev, InterruptLock, NULL, &env->lock);
    env->passive = true;
}

void ans_env_leave(struct ans_env *env)
{
    env->passive = false;
    StorPortReleaseSpinLock(env->dev, &env->lock);
}

/*
 * Gives the lock up for a moment (only for a passive caller, see adapter.h). Meanwhile
 * the interrupt handler and the DPC may enter the core: passive is false for them.
 */
static void lock_give_up(struct ans_env *env)
{
    env->passive = false;
    StorPortReleaseSpinLock(env->dev, &env->lock);
}

static void lock_take_back(struct ans_env *env)
{
    StorPortAcquireSpinLock(env->dev, InterruptLock, NULL, &env->lock);
    env->passive = true;
}

/* --------------------------------------------------------------------------------- */
/* Memory for the device                                                               */

/*
 * StorPortAllocateDmaMemory, not cached (findings/0071 conclusion 6): physically
 * contiguous, and its physical address is the device address (no IOMMU is described to
 * Windows). It only aligns to a page, so the block is 12 KiB larger and the aligned
 * part is handed out; env_priv keeps the address the allocation really starts at.
 * Not zeroed by Storport: zeroed here with aligned 64-bit writes (no memset on memory
 * that may be Device memory, conclusion 6).
 */
bool ans_env_dma_alloc(struct ans_env *env, size_t size, struct ans_dma *dma)
{
    PHYSICAL_ADDRESS low, high, boundary, pa;
    PVOID va = NULL;
    SIZE_T whole;
    ULONG status;

    dma->va = NULL;
    dma->dva = 0;
    dma->size = 0;
    dma->env_priv = NULL;
    if (!env->passive) {
        ans_log("env: device memory asked for outside start or stop, refused");
        return false;
    }
    if (size == 0 || size > 16u * 1024u * 1024u)
        return false;
    whole = ((size + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1)) + DMA_EXTRA;

    low.QuadPart = 0;
    high.QuadPart = -1;
    boundary.QuadPart = 0;
    pa.QuadPart = 0;
    lock_give_up(env);
    status = StorPortAllocateDmaMemory(env->dev, whole, low, high, boundary, MmNonCached,
                                       MM_ANY_NODE_OK, &va, &pa);
    ULONGLONG skip = ((ULONGLONG)pa.QuadPart + DMA_ALIGN - 1) / DMA_ALIGN * DMA_ALIGN -
                     (ULONGLONG)pa.QuadPart;
    if (status == STOR_STATUS_SUCCESS && va) {
        /* zeroed before the lock is taken back: uncached writes are slow */
        volatile ULONGLONG *p = (volatile ULONGLONG *)((PUCHAR)va + skip);
        for (size_t i = 0; i < (size + 7) / 8; i++)
            p[i] = 0;
    }
    lock_take_back(env);
    if (status != STOR_STATUS_SUCCESS || !va) {
        ans_log("env: StorPortAllocateDmaMemory of 0x%llx bytes failed (0x%x)",
                (unsigned long long)whole, status);
        return false;
    }

    dma->va = (PUCHAR)va + skip;
    dma->dva = (ULONGLONG)pa.QuadPart + skip;
    dma->size = size;
    dma->env_priv = va;
    return true;
}

void ans_env_dma_free(struct ans_env *env, struct ans_dma *dma)
{
    PHYSICAL_ADDRESS pa;
    PVOID va = dma->env_priv;
    SIZE_T whole;

    if (!va)
        return;
    whole = ((dma->size + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1)) + DMA_EXTRA;
    pa.QuadPart = (LONGLONG)(dma->dva - (ULONGLONG)((PUCHAR)dma->va - (PUCHAR)va));
    dma->va = NULL;
    dma->env_priv = NULL;
    if (!env->passive) {
        /* the core never does this (ans_env.h); keep the memory rather than free at DIRQL */
        ans_log("env: device memory freed outside start or stop, kept");
        return;
    }
    lock_give_up(env);
    StorPortFreeDmaMemory(env->dev, va, whole, MmNonCached, pa);
    lock_take_back(env);
}

/* --------------------------------------------------------------------------------- */
/* Waiting, barriers, log                                                              */

void ans_env_udelay(struct ans_env *env, uint32_t usec)
{
    UNREFERENCED_PARAMETER(env);
    StorPortStallExecution(usec);
}

static ULONGLONG now_ticks(struct ans_env *env)
{
    LARGE_INTEGER t;

    t.QuadPart = 0;
    StorPortQueryPerformanceCounter(env->dev, NULL, &t);
    return (ULONGLONG)t.QuadPart;
}

/*
 * Gives the lock up and sleeps until the DPC says an interrupt came (it sets the
 * event every time) or usec ran out, whichever is first. The kernel's timers do not
 * go below its clock tick, so this can sleep much longer than asked: what it returns
 * is the time that really went by, from the performance counter.
 */
uint32_t ans_env_wait(struct ans_env *env, uint32_t usec)
{
    LARGE_INTEGER timeout;
    ULONGLONG t0, t1, us;

    if (usec == 0)
        usec = 1;
    if (!env->passive) {
        /* the core waits only inside ans_start() and ans_stop(); this would be a bug */
        ans_log("env: wait outside start or stop, spinning %u us", usec);
        StorPortStallExecution(usec);
        return usec;
    }

    t0 = now_ticks(env);
    lock_give_up(env);
    timeout.QuadPart = -10 * (LONGLONG)usec;
    StorPortWaitForSingleObject(env->dev, &env->wake, FALSE, &timeout);
    lock_take_back(env);
    t1 = now_ticks(env);

    if (env->freq.QuadPart <= 0 || t1 <= t0)
        return usec;
    us = (t1 - t0) * 1000000ull / (ULONGLONG)env->freq.QuadPart;
    if (us == 0)
        return 1;
    return us > 0xffffffffull ? 0xffffffffu : (uint32_t)us;
}

/* m1n1's dma_wmb() and dma_rmb(): dmb oshst, dmb oshld. */
void ans_env_wmb(struct ans_env *env)
{
    UNREFERENCED_PARAMETER(env);
    __dmb(_ARM64_BARRIER_OSHST);
}

void ans_env_rmb(struct ans_env *env)
{
    UNREFERENCED_PARAMETER(env);
    __dmb(_ARM64_BARRIER_OSHLD);
}

void ans_env_log(struct ans_env *env, const char *fmt, ...)
{
    va_list ap;

    UNREFERENCED_PARAMETER(env);
    va_start(ap, fmt);
    ans_vlog(fmt, ap);
    va_end(ap);
}
