/* SPDX-License-Identifier: MIT */
/*
 * The reset steps of m1n1 v1.9.9 (809541515659) src/pmgr.c and the bits of
 * src/pmgr.h they use; see LICENSE.m1n1. Only the reset is taken: the register comes
 * from the environment (the ACPI node's fourth memory range), not from a device-table
 * lookup. See SiliconNT findings/0071 conclusion 31 and findings/0072.
 */

#ifndef ANS_PMGR_H
#define ANS_PMGR_H

struct ans_core;

int ans_pmgr_reset(struct ans_core *core);

#endif
