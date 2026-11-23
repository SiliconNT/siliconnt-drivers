/* SPDX-License-Identifier: MIT */
/*
 * Helpers shared by the core's own files. Environments do not include this.
 *
 * Written so that MSVC (Windows kernel, ARM64), m1n1's GCC and the host's compiler
 * all take it: no GNU extensions, no C library, no multi-character constants.
 */

#ifndef ANS_INTERNAL_H
#define ANS_INTERNAL_H

#include "ans_core.h"
#include "pmgr.h"

#if defined(_MSC_VER)
#define ANS_INLINE __inline
#else
#define ANS_INLINE inline
#endif

/* m1n1's bit helpers, without GNU extensions. */
#define BIT(n)        (1ull << (n))
#define GENMASK(h, l) ((~0ull << (l)) & (~0ull >> (63 - (h))))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static ANS_INLINE unsigned ans_mask_shift(uint64_t mask)
{
    unsigned n = 0;

    while (mask && !(mask & 1)) {
        mask >>= 1;
        n++;
    }
    return n;
}

#define FIELD_GET(mask, v)  (((uint64_t)(v) & (mask)) >> ans_mask_shift(mask))
#define FIELD_PREP(mask, v) (((uint64_t)(v) << ans_mask_shift(mask)) & (mask))

#define ans_log(core, ...) ans_env_log((core)->env, __VA_ARGS__)

/*
 * The registers the core must never touch. 0x24008 of the NVMe range: the controller
 * firmware macOS 15 and later installs no longer has it, and an access raises an
 * SError (findings/0057 conclusion 6).
 */
static ANS_INLINE bool ans_reg_forbidden(enum ans_region rgn, uint32_t off, uint32_t size)
{
    return rgn == ANS_RGN_NVME && off < 0x2400c && off + size > 0x24008;
}

static ANS_INLINE uint32_t ans_read32(struct ans_core *core, enum ans_region rgn, uint32_t off)
{
    if (ans_reg_forbidden(rgn, off, 4)) {
        ans_log(core, "core: refused read of forbidden register %u:%x", rgn, off);
        return 0xffffffffu;
    }
    return ans_env_read32(core->env, rgn, off);
}

static ANS_INLINE void ans_write32(struct ans_core *core, enum ans_region rgn, uint32_t off,
                                   uint32_t val)
{
    if (ans_reg_forbidden(rgn, off, 4)) {
        ans_log(core, "core: refused write of forbidden register %u:%x", rgn, off);
        return;
    }
    ans_env_write32(core->env, rgn, off, val);
}

static ANS_INLINE uint64_t ans_read64(struct ans_core *core, enum ans_region rgn, uint32_t off)
{
    if (ans_reg_forbidden(rgn, off, 8)) {
        ans_log(core, "core: refused read of forbidden register %u:%x", rgn, off);
        return ~0ull;
    }
    return ans_env_read64(core->env, rgn, off);
}

static ANS_INLINE void ans_write64(struct ans_core *core, enum ans_region rgn, uint32_t off,
                                   uint64_t val)
{
    if (ans_reg_forbidden(rgn, off, 8)) {
        ans_log(core, "core: refused write of forbidden register %u:%x", rgn, off);
        return;
    }
    ans_env_write64(core->env, rgn, off, val);
}

/* m1n1's set32(), clear32(), mask32(), write64_lo_hi(). */
static ANS_INLINE void ans_set32(struct ans_core *core, enum ans_region rgn, uint32_t off,
                                 uint32_t set)
{
    ans_write32(core, rgn, off, ans_read32(core, rgn, off) | set);
}

static ANS_INLINE void ans_clear32(struct ans_core *core, enum ans_region rgn, uint32_t off,
                                   uint32_t clear)
{
    ans_write32(core, rgn, off, ans_read32(core, rgn, off) & ~clear);
}

static ANS_INLINE void ans_mask32(struct ans_core *core, enum ans_region rgn, uint32_t off,
                                  uint32_t clear, uint32_t set)
{
    ans_write32(core, rgn, off, (ans_read32(core, rgn, off) & ~clear) | set);
}

static ANS_INLINE void ans_write64_lo_hi(struct ans_core *core, enum ans_region rgn,
                                         uint32_t off, uint64_t val)
{
    ans_write32(core, rgn, off, (uint32_t)val);
    ans_write32(core, rgn, off + 4, (uint32_t)(val >> 32));
}

/*
 * Device memory: only aligned 32-bit and 64-bit accesses, through volatile pointers so
 * the compiler cannot merge them into memcpy or memset (findings/0071 conclusion 6).
 */
static ANS_INLINE bool ans_dma_bad(struct ans_core *core, const struct ans_dma *dma, size_t off,
                                   size_t size)
{
    if (dma->va && (off & (size - 1)) == 0 && off + size <= dma->size)
        return false;
    ans_log(core, "core: bad device memory access at 0x%llx size %u",
            (unsigned long long)off, (unsigned)size);
    return true;
}

static ANS_INLINE uint32_t ans_dma_read32(struct ans_core *core, const struct ans_dma *dma,
                                          size_t off)
{
    if (ans_dma_bad(core, dma, off, 4))
        return 0;
    return *(volatile uint32_t *)((char *)dma->va + off);
}

static ANS_INLINE void ans_dma_write32(struct ans_core *core, const struct ans_dma *dma,
                                       size_t off, uint32_t val)
{
    if (ans_dma_bad(core, dma, off, 4))
        return;
    *(volatile uint32_t *)((char *)dma->va + off) = val;
}

static ANS_INLINE uint64_t ans_dma_read64(struct ans_core *core, const struct ans_dma *dma,
                                          size_t off)
{
    if (ans_dma_bad(core, dma, off, 8))
        return 0;
    return *(volatile uint64_t *)((char *)dma->va + off);
}

static ANS_INLINE void ans_dma_write64(struct ans_core *core, const struct ans_dma *dma,
                                       size_t off, uint64_t val)
{
    if (ans_dma_bad(core, dma, off, 8))
        return;
    *(volatile uint64_t *)((char *)dma->va + off) = val;
}

/* One byte of device memory, read with an aligned 32-bit read (little endian). */
static ANS_INLINE uint8_t ans_dma_read8(struct ans_core *core, const struct ans_dma *dma,
                                        size_t off)
{
    uint32_t w = ans_dma_read32(core, dma, off & ~(size_t)3);
    return (uint8_t)(w >> (8 * (off & 3)));
}

#endif
