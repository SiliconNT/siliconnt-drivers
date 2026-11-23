/* SPDX-License-Identifier: MIT */
/*
 * What the core needs from the environment it runs in.
 *
 * The core (this directory) drives the disk controller and touches nothing else. Each
 * environment it is built into implements every function below in one C file: the
 * Windows miniport, the test guest in m1n1, the model test on the host (test/), and
 * later the firmware. Calls arrive one at a time: the environment makes sure that only
 * one thread is inside the core at any moment (SiliconNT findings/0071 conclusion 5).
 */
#ifndef ANS_ENV_H
#define ANS_ENV_H

#include <stdbool.h>
#include <stddef.h>
#if defined(_MSC_VER) && defined(_KERNEL_MODE)
/*
 * The WDK's kernel headers come with no <stdint.h>, and MSVC's own pulls in
 * <vcruntime.h>, which does not go with the kernel's <vadefs.h>.
 */
typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#else
#include <stdint.h>
#endif

/* Defined by the environment. The core only stores the pointer and hands it back. */
struct ans_env;

/*
 * The memory ranges of the controller, in the order of the ACPI node's _CRS
 * (findings/0062 conclusion 2, findings/0071 conclusion 31).
 */
enum ans_region {
    ANS_RGN_NVME = 0, /* NVMe and NVMMU registers */
    ANS_RGN_ASC = 1,  /* coprocessor control at +0, mailbox at +0x8000 */
    ANS_RGN_SART = 2, /* the SART in front of the coprocessor */
    ANS_RGN_PMGR = 3, /* the controller's one 32-bit power state register, 4 bytes */
    ANS_RGN_COUNT
};

/*
 * Register access. The offset counts from the start of the region and is aligned to
 * the access size. An access that does not fit inside the region is refused: the
 * environment logs it, does not touch the hardware, and a read returns all ones.
 * A 64-bit access is one single 64-bit access, not two 32-bit ones.
 */
uint32_t ans_env_read32(struct ans_env *env, enum ans_region rgn, uint32_t offset);
void ans_env_write32(struct ans_env *env, enum ans_region rgn, uint32_t offset, uint32_t value);
uint64_t ans_env_read64(struct ans_env *env, enum ans_region rgn, uint32_t offset);
void ans_env_write64(struct ans_env *env, enum ans_region rgn, uint32_t offset, uint64_t value);

/*
 * Memory the controller reads or writes.
 *
 * ans_env_dma_alloc() gives a block that is physically contiguous, starts at a device
 * address aligned to 16 KiB, is filled with zeros, and is coherent between the CPU and
 * the device without cache maintenance (not cached). The CPU address is at least
 * 8-byte aligned. The core only makes aligned 32-bit and 64-bit accesses to it
 * (findings/0071 conclusion 6). The device address is the address the controller
 * uses; the core never derives one address from the other.
 *
 * The core calls these two only from inside ans_start() and ans_stop(), at points
 * where its own state is consistent, so an environment may give up its lock around
 * them. It never calls them from ans_isr(), ans_mbox_service(), ans_read() or
 * ans_halt().
 */
struct ans_dma {
    void *va;       /* CPU address */
    uint64_t dva;   /* device address */
    size_t size;    /* bytes, as asked for */
    void *env_priv; /* the environment's own bookkeeping; the core does not touch it */
};

bool ans_env_dma_alloc(struct ans_env *env, size_t size, struct ans_dma *dma);
void ans_env_dma_free(struct ans_env *env, struct ans_dma *dma);

/* Spins for at least usec microseconds and gives nothing up (the 10 us of the reset). */
void ans_env_udelay(struct ans_env *env, uint32_t usec);

/*
 * Pauses while the core waits for the coprocessor or the controller, for about usec
 * microseconds. Called only from inside ans_start() and ans_stop(), at points where
 * the core's state is consistent: the environment may give up its lock, let its
 * interrupt handler and DPC run (they call ans_isr() and ans_mbox_service()), and take
 * the lock again before returning.
 *
 * Returns the microseconds the environment counts as gone by, at least 1. The core
 * takes that from its timeout and never reads a clock itself. Returning usec is always
 * right; an environment that wakes early on an interrupt and can tell how long it
 * really waited may return less.
 */
uint32_t ans_env_wait(struct ans_env *env, uint32_t usec);

/*
 * Barriers between memory the device shares and the device's registers (m1n1's
 * dma_wmb() and dma_rmb()).
 * ans_env_wmb(): writes to device memory before it are visible to the device before
 *                any register write after it.
 * ans_env_rmb(): reads of device memory after it see what the device wrote before
 *                the reads (of registers or device memory) before it.
 */
void ans_env_wmb(struct ans_env *env);
void ans_env_rmb(struct ans_env *env);

/*
 * One line of log, without the trailing newline. The core only uses these
 * conversions: %s, %d, %u, %x, %02x, %08x, %llu, %llx, %016llx; 64-bit values are
 * always passed as unsigned long long.
 */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void ans_env_log(struct ans_env *env, const char *fmt, ...);

#endif
