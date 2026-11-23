/* SPDX-License-Identifier: MIT */
/*
 * Reads the two properties of the ANS0 node's _DSD from what ACPI returns for it.
 *
 * The firmware writes them (SiliconNT findings/0062 conclusions 1, 2):
 *   _DSD = Package { ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
 *                    Package { Package (2) { "nvme-secure-bar", 1 },   only on some machines
 *                              Package (2) { "sart-version", 3 } } }   always
 *
 * The miniport evaluates _DSD with StorPortInvokeAcpiMethod; the output buffer has
 * the layout of ACPI_EVAL_OUTPUT_BUFFER_V1 in the WDK's acpiioct.h, rewritten here so
 * the parser builds without kernel headers and runs in the host test:
 *
 *   output:   u32 Signature 'BoeA' (0x426f6541), u32 Length (bytes, whole buffer),
 *             u32 Count, then Count arguments
 *   argument: u16 Type, u16 DataLength, then max(4, DataLength) bytes of data;
 *             the next argument follows straight after
 *   Type:     0 integer (DataLength 4 or 8, little endian), 1 string (with its NUL),
 *             2 buffer, 3 package (the data is the elements, one argument each)
 *
 * Pure C, no Storport: built into the miniport and the host test (test/host_test.c).
 */
#ifndef ANS_DSD_H
#define ANS_DSD_H

#include "core/ans_env.h"

struct ans_dsd {
    bool has_sart_version;
    uint64_t sart_version;
    bool has_secure_bar;
    uint64_t secure_bar;
};

enum ans_dsd_result {
    ANS_DSD_OK = 0,
    ANS_DSD_BAD_BUFFER,  /* not an ACPI output buffer, or an argument runs past its end */
    ANS_DSD_NO_UUID,     /* no package for the Device Properties UUID */
    ANS_DSD_BAD_PROPERTY /* a property is not a (string, integer) pair */
};

/*
 * Parses len bytes at buf. Fills dsd with what it found; keys it does not know are
 * skipped. Never reads past buf + len.
 */
enum ans_dsd_result ans_dsd_parse(const void *buf, uint32_t len, struct ans_dsd *dsd);

#endif
