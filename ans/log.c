/* SPDX-License-Identifier: MIT */
/* The miniport's log; the layout, and where it lives, are in log.h. */

#include <miniport.h>
#include <storport.h>

#include "format.h"
#include "log.h"

struct page_head {
    char magic[16];
    uint32_t version;
    uint32_t index;
    uint32_t pages;
    uint32_t text_per_page;
    unsigned long long instance;
    unsigned long long reserved;
};

struct counter_page {
    struct page_head head;
    volatile long long written;
    uint32_t counters;
    uint32_t reserved;
    volatile long counter[ANS_LOG_COUNTERS];
};

struct text_page {
    struct page_head head;
    char text[ANS_LOG_PAGE_TEXT];
};

/* Each page aligned to a page, so that each is one physical page. */
struct ans_log {
    __declspec(align(4096)) struct counter_page c;
    __declspec(align(4096)) struct text_page t[ANS_LOG_TEXT_PAGES];
};

C_ASSERT(sizeof(struct page_head) == ANS_LOG_HEAD);
C_ASSERT(sizeof(struct text_page) == ANS_LOG_PAGE);
C_ASSERT(FIELD_OFFSET(struct counter_page, written) == 48);
C_ASSERT(FIELD_OFFSET(struct counter_page, counters) == 56);
C_ASSERT(FIELD_OFFSET(struct counter_page, counter) == 64);
C_ASSERT(sizeof(struct counter_page) <= ANS_LOG_PAGE);
C_ASSERT(FIELD_OFFSET(struct ans_log, t) == ANS_LOG_PAGE);
C_ASSERT(sizeof(struct ans_log) == ANS_LOG_PAGE * (1 + ANS_LOG_TEXT_PAGES));
C_ASSERT(sizeof(long) == 4);

/* The log until ans_log_keep() moves it. */
static struct ans_log g_image_log;

/* The log of the driver, wherever it is now. From the debugger: dx *ans!g_ans_log. */
struct ans_log *volatile g_ans_log = &g_image_log;

/*
 * "SiliconNTansLog1" with every byte xor 0x5a. Unscrambled only into the live log, so
 * the plain marker is in memory only where the live log is.
 */
static const unsigned char scrambled_magic[16] = {
    'S' ^ 0x5a, 'i' ^ 0x5a, 'l' ^ 0x5a, 'i' ^ 0x5a, 'c' ^ 0x5a, 'o' ^ 0x5a, 'n' ^ 0x5a, 'N' ^ 0x5a,
    'T' ^ 0x5a, 'a' ^ 0x5a, 'n' ^ 0x5a, 's' ^ 0x5a, 'L' ^ 0x5a, 'o' ^ 0x5a, 'g' ^ 0x5a, '1' ^ 0x5a,
};

/* The generic timer: the same clock on every processor, readable at any IRQL. */
#define CNTVCT_EL0 ARM64_SYSREG(3, 3, 14, 0, 2)
#define CNTFRQ_EL0 ARM64_SYSREG(3, 3, 14, 0, 0)

static ULONGLONG g_clock_start;
static ULONGLONG g_clock_freq;

#define LINE_MAX 240

static void init_head(struct page_head *h, uint32_t index, unsigned long long instance)
{
    h->version = ANS_LOG_VERSION;
    h->index = index;
    h->pages = 1 + ANS_LOG_TEXT_PAGES;
    h->text_per_page = ANS_LOG_PAGE_TEXT;
    h->instance = instance;
    h->reserved = 0;
    /* the marker last: a page that has it is complete */
    for (int i = 0; i < 16; i++)
        h->magic[i] = (char)(scrambled_magic[i] ^ 0x5a);
}

void ans_log_init(void)
{
    struct ans_log *log = g_ans_log;

    g_clock_start = (ULONGLONG)_ReadStatusReg(CNTVCT_EL0);
    g_clock_freq = (ULONGLONG)_ReadStatusReg(CNTFRQ_EL0) & 0xffffffffull;

    /*
     * Tells this load from older logs memory may still hold: the timer's count mixed
     * with where the image was put (KASLR).
     */
    unsigned long long instance = g_clock_start ^ ((unsigned long long)(ULONG_PTR)log << 16);

    log->c.counters = ANS_LOG_COUNTERS;
    init_head(&log->c.head, 0, instance);
    for (uint32_t i = 0; i < ANS_LOG_TEXT_PAGES; i++)
        init_head(&log->t[i].head, i + 1, instance);
}

void ans_log_keep(void *dev)
{
    PHYSICAL_ADDRESS low, high, boundary, pa;
    PVOID va = NULL;
    ULONG status;

    if (g_ans_log != &g_image_log)
        return;
    low.QuadPart = 0;
    high.QuadPart = -1;
    boundary.QuadPart = 0;
    pa.QuadPart = 0;
    status = StorPortAllocateDmaMemory(dev, sizeof(struct ans_log), low, high, boundary, MmCached,
                                       MM_ANY_NODE_OK, &va, &pa);
    if (status != STOR_STATUS_SUCCESS || !va || ((ULONG_PTR)va & (ANS_LOG_PAGE - 1))) {
        ans_log("log: no memory of its own (0x%x); the log stays in the driver image and goes "
                "with it if the driver is unloaded", status);
        return;
    }

    /* copy page by page with plain stores (no memcpy: that would be an ntoskrnl import) */
    volatile ULONGLONG *to = (volatile ULONGLONG *)va;
    const volatile ULONGLONG *from = (const volatile ULONGLONG *)&g_image_log;
    for (SIZE_T i = 0; i < sizeof(struct ans_log) / 8; i++)
        to[i] = from[i];
    g_ans_log = (struct ans_log *)va;

    /* only the kept pages keep the marker */
    g_image_log.c.head.magic[0] = 0;
    for (uint32_t i = 0; i < ANS_LOG_TEXT_PAGES; i++)
        g_image_log.t[i].head.magic[0] = 0;

    ans_log("log: kept at physical %016llx, %u bytes; it outlives the driver",
            (unsigned long long)pa.QuadPart, (unsigned)sizeof(struct ans_log));
}

static ULONGLONG clock_us(void)
{
    ULONGLONG ticks = (ULONGLONG)_ReadStatusReg(CNTVCT_EL0) - g_clock_start;

    if (g_clock_freq == 0)
        return 0;
    return ticks / g_clock_freq * 1000000ull + ticks % g_clock_freq * 1000000ull / g_clock_freq;
}

void ans_vlog(const char *fmt, va_list ap)
{
    struct ans_log *log = g_ans_log;
    char line[LINE_MAX];
    ULONGLONG us = clock_us();
    size_t head, n;

    head = ans_format(line, sizeof(line), "[%5llu.%06llu] ", us / 1000000ull, us % 1000000ull);
    n = head + ans_vformat(line + head, sizeof(line) - head - 1, fmt, ap);
    line[n++] = '\n';

    /* reserve the bytes, then copy: writers on other processors take the next ones */
    ULONGLONG at = (ULONGLONG)InterlockedExchangeAdd64(&log->c.written, (LONG64)n);
    for (size_t i = 0; i < n; i++) {
        ULONGLONG q = at + i;
        log->t[(q / ANS_LOG_PAGE_TEXT) % ANS_LOG_TEXT_PAGES].text[q % ANS_LOG_PAGE_TEXT] = line[i];
    }
    InterlockedIncrement(&log->c.counter[ANS_LC_LINES]);

    line[n - 1] = '\0';
    StorPortDebugPrint(0, "ans: %s\n", line + head);
}

void ans_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ans_vlog(fmt, ap);
    va_end(ap);
}

long ans_log_add(enum ans_log_counter c, long n)
{
    if ((unsigned)c < ANS_LOG_COUNTERS)
        return InterlockedExchangeAdd(&g_ans_log->c.counter[c], n) + n;
    return 0;
}

void ans_log_set(enum ans_log_counter c, long v)
{
    if ((unsigned)c < ANS_LOG_COUNTERS)
        InterlockedExchange(&g_ans_log->c.counter[c], v);
}

long ans_log_get(enum ans_log_counter c)
{
    if ((unsigned)c < ANS_LOG_COUNTERS)
        return g_ans_log->c.counter[c];
    return 0;
}
