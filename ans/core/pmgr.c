/* SPDX-License-Identifier: MIT */
/*
 * The reset steps of m1n1 v1.9.9 (809541515659) src/pmgr.c (pmgr_reset_device()) and
 * the bits of src/pmgr.h they use; see LICENSE.m1n1. Only the reset is taken: the
 * register comes from the environment (the ACPI node's fourth memory range), not from
 * a device-table lookup. See SiliconNT findings/0071 conclusion 31 and findings/0072.
 */

#include "ans_internal.h"

#define PMGR_RESET        BIT(31)
#define PMGR_DEV_DISABLE  BIT(10)
#define PMGR_PS_ACTUAL    GENMASK(7, 4)

#define PMGR_PS_ACTIVE 0xf

/* The power state register is the whole PMGR region. */
#define PMGR_PS_REG 0

/*
 * Resets the controller through its power state register without powering it down:
 * the core never writes the target state (bits 0-3). Refuses when the controller is
 * not in the active state, as m1n1 does ("will not reset disabled device").
 */
int ans_pmgr_reset(struct ans_core *core)
{
    uint32_t reg = ans_read32(core, ANS_RGN_PMGR, PMGR_PS_REG);

    /* all ones is what a refused or dead access reads: writing it back would set the target */
    if (reg == 0xffffffffu || FIELD_GET(PMGR_PS_ACTUAL, reg) != PMGR_PS_ACTIVE) {
        ans_log(core, "pmgr: will not reset ANS, power state register %08x is not active", reg);
        return ANS_ERR_DEVICE;
    }

    ans_log(core, "pmgr: resetting ANS");

    ans_set32(core, ANS_RGN_PMGR, PMGR_PS_REG, PMGR_DEV_DISABLE);
    ans_set32(core, ANS_RGN_PMGR, PMGR_PS_REG, PMGR_RESET);
    ans_env_udelay(core->env, 10);
    ans_clear32(core, ANS_RGN_PMGR, PMGR_PS_REG, PMGR_RESET);
    ans_clear32(core, ANS_RGN_PMGR, PMGR_PS_REG, PMGR_DEV_DISABLE);

    return ANS_OK;
}
