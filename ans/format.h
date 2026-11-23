/* SPDX-License-Identifier: MIT */
/*
 * A small snprintf for the miniport's log (log.c).
 *
 * The kernel's own formatting functions are ntoskrnl exports; the miniport takes
 * nothing from the kernel but storport.sys, so it carries its own. Pure C: it is
 * built into the miniport and into the host test (test/host_test.c).
 */
#ifndef ANS_FORMAT_H
#define ANS_FORMAT_H

#include <stdarg.h>

#include "core/ans_env.h"

/*
 * Formats into buf, which holds cap bytes, and always ends it with a NUL (cap > 0).
 * Returns the number of characters stored, without the NUL; output that does not fit
 * is cut off.
 *
 * Conversions: %s (NULL prints "(null)"), %c, %d, %u, %x, %X, %p and %%; the length
 * modifiers l, ll and z; the flags 0 and -; a field width. That covers what the core
 * uses (ans_env.h) and what the miniport uses.
 */
size_t ans_vformat(char *buf, size_t cap, const char *fmt, va_list ap);
size_t ans_format(char *buf, size_t cap, const char *fmt, ...);

#endif
