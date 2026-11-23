/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/sart.c; see LICENSE.m1n1.
 * Changed for SiliconNT: the version comes from the configuration, not the device
 * tree; registers through the environment; only entries the core added are ever
 * cleared. See SiliconNT findings/0072.
 */

#include "ans_internal.h"

/* SARTv0 registers */
#define APPLE_SART0_CONFIG(idx)       (0x00 + 4 * (idx))
#define APPLE_SART0_CONFIG_FLAGS      GENMASK(28, 24)
#define APPLE_SART0_CONFIG_SIZE       GENMASK(18, 0)
#define APPLE_SART0_CONFIG_SIZE_SHIFT 12
#define APPLE_SART0_CONFIG_SIZE_MAX   GENMASK(18, 0)

#define APPLE_SART0_PADDR(idx)  (0x40 + 4 * (idx))
#define APPLE_SART0_PADDR_SHIFT 12

#define APPLE_SART0_FLAGS_ALLOW 0xf

/* SARTv2 registers */
#define APPLE_SART2_CONFIG(idx)       (0x00 + 4 * (idx))
#define APPLE_SART2_CONFIG_FLAGS      GENMASK(31, 24)
#define APPLE_SART2_CONFIG_SIZE       GENMASK(23, 0)
#define APPLE_SART2_CONFIG_SIZE_SHIFT 12
#define APPLE_SART2_CONFIG_SIZE_MAX   GENMASK(23, 0)

#define APPLE_SART2_PADDR(idx)  (0x40 + 4 * (idx))
#define APPLE_SART2_PADDR_SHIFT 12

#define APPLE_SART2_FLAGS_ALLOW 0xff

/* SARTv3 registers */
#define APPLE_SART3_CONFIG(idx) (0x00 + 4 * (idx))

#define APPLE_SART3_PADDR(idx)  (0x40 + 4 * (idx))
#define APPLE_SART3_PADDR_SHIFT 12

#define APPLE_SART3_SIZE(idx)  (0x80 + 4 * (idx))
#define APPLE_SART3_SIZE_SHIFT 12
#define APPLE_SART3_SIZE_MAX   GENMASK(29, 0)

#define APPLE_SART3_FLAGS_ALLOW 0xff

/* SARTv4 registers */
#define APPLE_SART4_CONFIG(idx) (0x00 + 4 * (idx))

#define APPLE_SART4_PADDR(idx)  (0x60 + 4 * (idx))
#define APPLE_SART4_PADDR_SHIFT 12

#define APPLE_SART4_SIZE(idx)  (0xc0 + 4 * (idx))
#define APPLE_SART4_SIZE_SHIFT 12
#define APPLE_SART4_SIZE_MAX   GENMASK(29, 0)

#define APPLE_SART4_FLAGS_ALLOW 0xff

/* All four versions keep addresses and sizes in 4 KiB units. */
#define SART_UNIT 0x1000ull

static uint32_t sart_rd(struct ans_core *core, uint32_t off)
{
    return ans_read32(core, ANS_RGN_SART, off);
}

static void sart_wr(struct ans_core *core, uint32_t off, uint32_t val)
{
    ans_write32(core, ANS_RGN_SART, off, val);
}

static void sart_get_entry(struct ans_core *core, int index, uint8_t *flags, uint64_t *paddr,
                           uint64_t *size)
{
    uint32_t cfg;

    switch (core->sart.version) {
        case 0:
            cfg = sart_rd(core, APPLE_SART0_CONFIG(index));
            *flags = (uint8_t)FIELD_GET(APPLE_SART0_CONFIG_FLAGS, cfg);
            *size = FIELD_GET(APPLE_SART0_CONFIG_SIZE, cfg) << APPLE_SART0_CONFIG_SIZE_SHIFT;
            *paddr = (uint64_t)sart_rd(core, APPLE_SART0_PADDR(index)) << APPLE_SART0_PADDR_SHIFT;
            break;
        case 2:
            cfg = sart_rd(core, APPLE_SART2_CONFIG(index));
            *flags = (uint8_t)FIELD_GET(APPLE_SART2_CONFIG_FLAGS, cfg);
            *size = FIELD_GET(APPLE_SART2_CONFIG_SIZE, cfg) << APPLE_SART2_CONFIG_SIZE_SHIFT;
            *paddr = (uint64_t)sart_rd(core, APPLE_SART2_PADDR(index)) << APPLE_SART2_PADDR_SHIFT;
            break;
        case 3:
            *flags = (uint8_t)sart_rd(core, APPLE_SART3_CONFIG(index));
            *size = (uint64_t)sart_rd(core, APPLE_SART3_SIZE(index)) << APPLE_SART3_SIZE_SHIFT;
            *paddr = (uint64_t)sart_rd(core, APPLE_SART3_PADDR(index)) << APPLE_SART3_PADDR_SHIFT;
            break;
        default: /* 4; ans_sart_init() lets no other value through */
            *flags = (uint8_t)sart_rd(core, APPLE_SART4_CONFIG(index));
            *size = (uint64_t)sart_rd(core, APPLE_SART4_SIZE(index)) << APPLE_SART4_SIZE_SHIFT;
            *paddr = (uint64_t)sart_rd(core, APPLE_SART4_PADDR(index)) << APPLE_SART4_PADDR_SHIFT;
            break;
    }
}

static bool sart_set_entry(struct ans_core *core, int index, uint8_t flags, uint64_t paddr,
                           uint64_t size)
{
    if (size & (SART_UNIT - 1))
        return false;
    if (paddr & (SART_UNIT - 1))
        return false;

    size >>= 12;
    paddr >>= 12;

    /* The address register holds the address in 4 KiB units in 32 bits. */
    if (paddr > 0xffffffffull)
        return false;

    /* the registers of this entry and what goes into them; versions 0 and 2 have no size register */
    uint32_t cfg_off, paddr_off, size_off = 0, cfg, size_val = 0;

    switch (core->sart.version) {
        case 0:
            if (size > APPLE_SART0_CONFIG_SIZE_MAX)
                return false;
            cfg_off = APPLE_SART0_CONFIG(index);
            paddr_off = APPLE_SART0_PADDR(index);
            cfg = (uint32_t)(FIELD_PREP(APPLE_SART0_CONFIG_FLAGS, flags) |
                             FIELD_PREP(APPLE_SART0_CONFIG_SIZE, size));
            break;
        case 2:
            if (size > APPLE_SART2_CONFIG_SIZE_MAX)
                return false;
            cfg_off = APPLE_SART2_CONFIG(index);
            paddr_off = APPLE_SART2_PADDR(index);
            cfg = (uint32_t)(FIELD_PREP(APPLE_SART2_CONFIG_FLAGS, flags) |
                             FIELD_PREP(APPLE_SART2_CONFIG_SIZE, size));
            break;
        case 3:
            if (size > APPLE_SART3_SIZE_MAX)
                return false;
            cfg_off = APPLE_SART3_CONFIG(index);
            paddr_off = APPLE_SART3_PADDR(index);
            size_off = APPLE_SART3_SIZE(index);
            cfg = flags;
            size_val = (uint32_t)size;
            break;
        default:
            if (size > APPLE_SART4_SIZE_MAX)
                return false;
            cfg_off = APPLE_SART4_CONFIG(index);
            paddr_off = APPLE_SART4_PADDR(index);
            size_off = APPLE_SART4_SIZE(index);
            cfg = flags;
            size_val = (uint32_t)size;
            break;
    }

    /*
     * Adding: address and size first, the flags that turn the entry on last (m1n1's
     * order). Removing: the flags off first, so the entry never lets the coprocessor
     * through to address 0 on the way.
     */
    if (flags) {
        sart_wr(core, paddr_off, (uint32_t)paddr);
        if (size_off)
            sart_wr(core, size_off, size_val);
        sart_wr(core, cfg_off, cfg);
    } else {
        sart_wr(core, cfg_off, cfg);
        if (size_off)
            sart_wr(core, size_off, size_val);
        sart_wr(core, paddr_off, (uint32_t)paddr);
    }
    return true;
}

/*
 * Notes which entries are in use already (iBoot's); the core never writes those.
 * The version is the "sart-version" the firmware put into the ACPI node
 * (findings/0071 conclusion 32).
 */
int ans_sart_init(struct ans_core *core, uint32_t version)
{
    struct ans_sart *sart = &core->sart;

    switch (version) {
        case 0:
            sart->flags_allow = APPLE_SART0_FLAGS_ALLOW;
            break;
        case 2:
            sart->flags_allow = APPLE_SART2_FLAGS_ALLOW;
            break;
        case 3:
            sart->flags_allow = APPLE_SART3_FLAGS_ALLOW;
            break;
        case 4:
            sart->flags_allow = APPLE_SART4_FLAGS_ALLOW;
            break;
        default:
            ans_log(core, "sart: unknown version %u", version);
            return ANS_ERR_INVALID;
    }
    sart->version = version;
    sart->protected_mask = 0;
    sart->added_mask = 0;

    for (int i = 0; i < SART_MAX_ENTRIES; ++i) {
        uint8_t flags;
        uint64_t paddr, sz;

        sart_get_entry(core, i, &flags, &paddr, &sz);
        if (flags)
            sart->protected_mask |= (uint16_t)(1u << i);
    }

    ans_log(core, "sart: SARTv%u, entries in use before start 0x%04x", version,
            sart->protected_mask);
    return ANS_OK;
}

/* Lets the coprocessor reach [paddr, paddr + size). */
bool ans_sart_add(struct ans_core *core, uint64_t paddr, uint64_t size)
{
    struct ans_sart *sart = &core->sart;

    for (int i = 0; i < SART_MAX_ENTRIES; ++i) {
        uint8_t e_flags;
        uint64_t e_paddr, e_sz;

        if ((sart->protected_mask | sart->added_mask) & (1u << i))
            continue;

        sart_get_entry(core, i, &e_flags, &e_paddr, &e_sz);
        if (e_flags)
            continue;

        if (!sart_set_entry(core, i, (uint8_t)sart->flags_allow, paddr, size)) {
            ans_log(core, "sart: cannot describe %016llx size 0x%llx",
                    (unsigned long long)paddr, (unsigned long long)size);
            return false;
        }
        sart->added_mask |= (uint16_t)(1u << i);
        return true;
    }

    ans_log(core, "sart: no more free entries");
    return false;
}

/* Takes back an entry the core added. */
bool ans_sart_remove(struct ans_core *core, uint64_t paddr, uint64_t size)
{
    struct ans_sart *sart = &core->sart;

    for (int i = 0; i < SART_MAX_ENTRIES; ++i) {
        uint8_t e_flags;
        uint64_t e_paddr, e_sz;

        if (!(sart->added_mask & (1u << i)))
            continue;

        sart_get_entry(core, i, &e_flags, &e_paddr, &e_sz);
        if (e_paddr != paddr || e_sz != size)
            continue;

        sart_set_entry(core, i, 0, 0, 0);
        sart->added_mask &= (uint16_t)~(1u << i);
        return true;
    }

    ans_log(core, "sart: could not find entry %016llx size 0x%llx to be removed",
            (unsigned long long)paddr, (unsigned long long)size);
    return false;
}

/* Takes back every entry the core added and still holds. */
void ans_sart_remove_all(struct ans_core *core)
{
    struct ans_sart *sart = &core->sart;

    for (int i = 0; i < SART_MAX_ENTRIES; ++i) {
        if (!(sart->added_mask & (1u << i)))
            continue;
        ans_log(core, "sart: removing leftover entry %d", i);
        sart_set_entry(core, i, 0, 0, 0);
    }
    sart->added_mask = 0;
}
