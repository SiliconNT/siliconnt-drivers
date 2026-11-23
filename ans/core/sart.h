/* SPDX-License-Identifier: MIT */
/*
 * Taken from m1n1 v1.9.9 (809541515659) src/sart.h; see LICENSE.m1n1.
 * Changed for SiliconNT: the version comes from the configuration, not the device
 * tree; only entries the core added are ever cleared. See SiliconNT findings/0072.
 */

#ifndef ANS_SART_H
#define ANS_SART_H

#include "ans_env.h"

struct ans_core;

#define SART_MAX_ENTRIES 16

struct ans_sart {
    uint32_t version;         /* 0, 2, 3 or 4 */
    uint32_t flags_allow;     /* flags of an entry the core adds */
    uint16_t protected_mask;  /* entries that were in use before the core started */
    uint16_t added_mask;      /* entries the core added and has not removed */
};

int ans_sart_init(struct ans_core *core, uint32_t version);
bool ans_sart_add(struct ans_core *core, uint64_t paddr, uint64_t size);
bool ans_sart_remove(struct ans_core *core, uint64_t paddr, uint64_t size);
void ans_sart_remove_all(struct ans_core *core);

#endif
