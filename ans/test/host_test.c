/* SPDX-License-Identifier: MIT */
/*
 * Host test of the miniport's pure parts: the SCSI layer (scsi.c), the _DSD parser
 * (dsd.c) and the log's formatter (format.c). Built and run by ../../test.ps1.
 *
 * The SCSI part holds the read-only promise to account: every one of the 256 opcodes
 * is executed, and only READ(10) and READ(16) may ever come out as a read; every
 * command that writes is refused with DATA PROTECT.
 *
 *   host_test            run everything
 *   host_test <name>     run the tests whose name contains <name>
 */

#include "../dsd.h"
#include "../format.h"
#include "../scsi.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                              \
    } while (0)

/* ------------------------------------------------------------------------------- */
/* SCSI                                                                              */

/* The disk of the M4: 122,138,133 blocks of 4096 bytes (500.3 GB). */
static const struct ans_scsi_disk g_disk = {
    122138133ull, 4096, "APPLE SSD AP0512Z                       ", "2000.1.0",
    "FAKE0123456789ABCDEF",
};

static void exec(const uint8_t *cdb, uint32_t len, uint8_t *buf, uint32_t buf_len,
                 struct ans_scsi_reply *r)
{
    memset(r, 0xa5, sizeof(*r));
    ans_scsi_execute(&g_disk, cdb, len, buf, buf_len, r);
}

static bool is_write_opcode(uint8_t op)
{
    /* findings/0071 conclusion 15, and nine more that write (scsi.c) */
    static const uint8_t list[] = {0x0a, 0x2a, 0xaa, 0x8a, 0x2e, 0xae, 0x8e, 0x41, 0x93, 0x42,
                                   0x04, 0x48, 0x3b, 0x3f, 0x07, 0xb5, 0x15, 0x55, 0x5f,
                                   0x89, 0x8b, 0x50, 0x51, 0x53, 0x83, 0x9a, 0x9c, 0x9f};
    for (size_t i = 0; i < sizeof(list); i++)
        if (list[i] == op)
            return true;
    return false;
}

static void test_every_opcode(void)
{
    uint8_t buf[512];

    for (int op = 0; op < 256; op++) {
        uint8_t cdb[16];
        struct ans_scsi_reply r;

        memset(cdb, 0, sizeof(cdb));
        cdb[0] = (uint8_t)op;
        /* READ(10): LBA 0, one block; READ(16): LBA 0x100, one block */
        cdb[8] = 1;
        cdb[13] = 1;
        exec(cdb, 16, buf, 4096, &r);

        if (op == 0x28) {
            CHECK(r.action == ANS_SCSI_READ && r.lba == 0 && r.blocks == 1);
            continue;
        }
        if (op == 0x88) {
            CHECK(r.action == ANS_SCSI_READ && r.lba == 0x100 && r.blocks == 1);
            continue;
        }
        if (r.action != ANS_SCSI_COMPLETE)
            printf("    opcode 0x%02x came out as a read\n", op);
        CHECK(r.action == ANS_SCSI_COMPLETE);
        if (is_write_opcode((uint8_t)op)) {
            CHECK(r.write_refused);
            CHECK(r.status == ANS_SCSI_CHECK_CONDITION && r.sense_key == ANS_SENSE_DATA_PROTECT &&
                  r.asc == 0x24 && r.length == 0);
        } else {
            CHECK(!r.write_refused);
        }
    }
}

static void test_refused_write_targets(void)
{
    struct ans_scsi_reply r;
    uint8_t w10[10] = {0x2a, 0, 0x01, 0x02, 0x03, 0x04, 0, 0x00, 0x08, 0};
    uint8_t w16[16] = {0x8a, 0, 0, 0, 0, 0x01, 0, 0, 0, 0x05, 0, 0, 0x01, 0x00, 0, 0};
    uint8_t w6[6] = {0x0a, 0x01, 0x02, 0x03, 0, 0};
    uint8_t unmap[10] = {0x42, 0, 0, 0, 0, 0, 0, 0, 24, 0};

    exec(w10, 10, NULL, 0, &r);
    CHECK(r.write_refused && r.lba_known && r.lba == 0x01020304u && r.blocks == 8);
    exec(w16, 16, NULL, 0, &r);
    CHECK(r.write_refused && r.lba_known && r.lba == 0x0100000005ull && r.blocks == 256);
    exec(w6, 6, NULL, 0, &r);
    CHECK(r.write_refused && r.lba_known && r.lba == 0x010203u && r.blocks == 256);
    exec(unmap, 10, NULL, 0, &r);
    CHECK(r.write_refused && !r.lba_known && r.sense_key == ANS_SENSE_DATA_PROTECT);
    /* refused even when the CDB is too short to say where */
    exec(w10, 3, NULL, 0, &r);
    CHECK(r.write_refused && !r.lba_known);
}

static void test_reads(void)
{
    struct ans_scsi_reply r;
    uint8_t r10[10] = {0x28, 0, 0, 0, 0, 0x10, 0, 0, 0x02, 0};
    uint8_t r16[16] = {0x88, 0, 0, 0, 0, 0, 0x07, 0x47, 0xae, 0x13, 0, 0, 0, 0x02, 0, 0};

    exec(r10, 10, NULL, 8192, &r);
    CHECK(r.action == ANS_SCSI_READ && r.lba == 16 && r.blocks == 2);
    /* the last two blocks: 122138131 and 122138132 */
    exec(r16, 16, NULL, 8192, &r);
    CHECK(r.action == ANS_SCSI_READ && r.lba == 122138131ull && r.blocks == 2);
    /* one past the end */
    r16[9] = 0x14;
    exec(r16, 16, NULL, 8192, &r);
    CHECK(r.action == ANS_SCSI_COMPLETE && r.sense_key == ANS_SENSE_ILLEGAL_REQUEST &&
          r.asc == 0x21);
    /* the transfer length must be what the blocks are */
    exec(r10, 10, NULL, 4096, &r);
    CHECK(r.action == ANS_SCSI_COMPLETE && r.sense_key == ANS_SENSE_ILLEGAL_REQUEST);
    /* zero blocks: nothing to do, GOOD */
    r10[8] = 0;
    exec(r10, 10, NULL, 0, &r);
    CHECK(r.action == ANS_SCSI_COMPLETE && r.status == ANS_SCSI_GOOD && r.length == 0);
    /* READ(6) and READ(12) are not used by classpnp and not taken */
    uint8_t r6[6] = {0x08, 0, 0, 1, 1, 0};
    exec(r6, 6, NULL, 4096, &r);
    CHECK(r.action == ANS_SCSI_COMPLETE && r.asc == 0x20);
}

static void test_inquiry(void)
{
    struct ans_scsi_reply r;
    uint8_t buf[256];
    uint8_t std[6] = {0x12, 0, 0, 0, 128, 0};
    uint8_t vpd0[6] = {0x12, 1, 0x00, 0, 255, 0};
    uint8_t vpd80[6] = {0x12, 1, 0x80, 0, 255, 0};
    uint8_t vpd83[6] = {0x12, 1, 0x83, 0, 255, 0};

    memset(buf, 0xee, sizeof(buf));
    exec(std, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 36);
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 6 && buf[3] == 2 && buf[4] == 31 &&
          buf[7] == 2);
    CHECK(memcmp(buf + 8, "NVMe    ", 8) == 0);
    CHECK(memcmp(buf + 16, "APPLE SSD AP0512", 16) == 0);
    CHECK(memcmp(buf + 32, "2000", 4) == 0);
    CHECK(buf[36] == 0xee);

    /* allocation length cuts it short, still GOOD */
    std[4] = 5;
    exec(std, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 5);

    exec(vpd0, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 6 &&
          memcmp(buf, "\x00\x00\x00\x02\x00\x80", 6) == 0);
    exec(vpd80, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 24 && buf[1] == 0x80 && buf[3] == 20 &&
          memcmp(buf + 4, "FAKE0123456789ABCDEF", 20) == 0);
    exec(vpd83, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_CHECK_CONDITION && r.sense_key == ANS_SENSE_ILLEGAL_REQUEST &&
          r.asc == 0x24);
    /* page code without EVPD */
    uint8_t bad[6] = {0x12, 0, 0x80, 0, 255, 0};
    exec(bad, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_CHECK_CONDITION && r.asc == 0x24);
    /* a too small buffer is never overrun */
    memset(buf, 0xee, sizeof(buf));
    std[4] = 128;
    exec(std, 6, buf, 10, &r);
    CHECK(r.length == 10 && buf[10] == 0xee);
}

static void test_capacity(void)
{
    struct ans_scsi_reply r;
    uint8_t buf[64];
    uint8_t rc10[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t rc16[16] = {0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};

    exec(rc10, 10, buf, 8, &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 8);
    CHECK(memcmp(buf, "\x07\x47\xae\x14\x00\x00\x10\x00", 8) == 0); /* 122138132, 4096 */
    memset(buf, 0xee, sizeof(buf));
    exec(rc16, 16, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 32);
    CHECK(memcmp(buf, "\x00\x00\x00\x00\x07\x47\xae\x14\x00\x00\x10\x00", 12) == 0);
    CHECK(buf[12] == 0 && buf[13] == 0 && buf[14] == 0 && buf[15] == 0 && buf[31] == 0);
    /* another service action of 9Eh */
    rc16[1] = 0x11;
    exec(rc16, 16, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_CHECK_CONDITION && r.asc == 0x24);
}

static void test_mode_sense(void)
{
    struct ans_scsi_reply r;
    uint8_t buf[64];
    uint8_t ms6[6] = {0x1a, 0, 0x3f, 0, 0xff, 0};
    uint8_t ms10[10] = {0x5a, 0, 0x08, 0, 0, 0, 0, 0, 0xff, 0};
    uint8_t current[6] = {0x1a, 0, 0x7f, 0, 0xff, 0}; /* PC 01b: changeable values */
    uint8_t other[6] = {0x1a, 0, 0x01, 0, 0xff, 0};

    exec(ms6, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 24);
    CHECK(buf[0] == 23 && buf[1] == 0 && buf[2] == 0x80 && buf[3] == 0); /* write protected */
    CHECK(buf[4] == 0x08 && buf[5] == 0x12 && buf[6] == 0);              /* caching, WCE 0 */
    exec(ms10, 10, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 28);
    CHECK(buf[0] == 0 && buf[1] == 26 && buf[3] == 0x80 && buf[6] == 0 && buf[7] == 0);
    CHECK(buf[8] == 0x08 && buf[9] == 0x12);
    /* the 4-byte header only, as disk.sys asks for it */
    ms6[4] = 4;
    exec(ms6, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 4 && buf[2] == 0x80);
    exec(current, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_CHECK_CONDITION && r.asc == 0x24);
    exec(other, 6, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_CHECK_CONDITION && r.asc == 0x24);
}

static void test_report_luns_and_no_ops(void)
{
    struct ans_scsi_reply r;
    uint8_t buf[64];
    uint8_t rl[12] = {0xa0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0};
    static const uint8_t no_ops[][10] = {
        {0x00}, {0x1b, 0, 0, 0, 1}, {0x35}, {0x2f}, {0x91}, {0x8f},
    };

    memset(buf, 0xee, sizeof(buf));
    exec(rl, 12, buf, sizeof(buf), &r);
    CHECK(r.status == ANS_SCSI_GOOD && r.length == 16);
    CHECK(memcmp(buf, "\x00\x00\x00\x08\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16) == 0);
    for (size_t i = 0; i < sizeof(no_ops) / sizeof(no_ops[0]); i++) {
        uint8_t cdb[16];
        memset(cdb, 0, sizeof(cdb));
        memcpy(cdb, no_ops[i], sizeof(no_ops[i]));
        exec(cdb, 16, NULL, 0, &r);
        CHECK(r.status == ANS_SCSI_GOOD && r.action == ANS_SCSI_COMPLETE && r.length == 0);
    }
}

static void test_sense(void)
{
    struct ans_scsi_reply r;
    uint8_t sense[32];
    uint8_t w10[10] = {0x2a};

    exec(w10, 10, NULL, 0, &r);
    memset(sense, 0xee, sizeof(sense));
    CHECK(ans_scsi_sense(&r, sense, sizeof(sense)) == 18);
    CHECK(sense[0] == 0x70 && sense[2] == 0x07 && sense[7] == 10 && sense[12] == 0x24 &&
          sense[13] == 0 && sense[18] == 0xee);
    CHECK(ans_scsi_sense(&r, sense, 8) == 8);
}

/* ------------------------------------------------------------------------------- */
/* _DSD                                                                              */

struct acpi_buf {
    uint8_t b[512];
    uint32_t n;
};

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* Appends an argument; returns where its data starts (for packages, filled after). */
static uint32_t arg(struct acpi_buf *a, uint16_t type, const void *data, uint16_t len)
{
    uint32_t at = a->n;
    uint32_t size = 4 + (len > 4 ? len : 4u);

    put16(a->b + at, type);
    put16(a->b + at + 2, len);
    memset(a->b + at + 4, 0, size - 4);
    if (data)
        memcpy(a->b + at + 4, data, len);
    a->n += size;
    return at;
}

static uint32_t begin_package(struct acpi_buf *a)
{
    uint32_t at = a->n;
    a->n += 4;
    return at;
}

static void end_package(struct acpi_buf *a, uint32_t at)
{
    uint32_t len = a->n - at - 4;
    put16(a->b + at, 3);
    put16(a->b + at + 2, len);
}

static void prop(struct acpi_buf *a, const char *key, uint32_t value, uint16_t int_len)
{
    uint32_t p = begin_package(a);
    uint8_t v[8] = {0};

    put32(v, value);
    arg(a, 1, key, (uint16_t)(strlen(key) + 1));
    arg(a, 0, v, int_len);
    end_package(a, p);
}

static const uint8_t uuid[16] = {0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
                                 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01};

/* _DSD as the firmware writes it for the M4, as ACPI hands it back. */
static void build_m4_dsd(struct acpi_buf *a, bool secure_bar, uint32_t sart)
{
    uint32_t props;

    memset(a, 0, sizeof(*a));
    a->n = 12;
    arg(a, 2, uuid, 16);
    props = begin_package(a);
    if (secure_bar)
        prop(a, "nvme-secure-bar", 1, 4);
    prop(a, "sart-version", sart, 4);
    end_package(a, props);
    put32(a->b, 0x426f6541);
    put32(a->b + 4, a->n);
    put32(a->b + 8, 2);
}

static void test_dsd(void)
{
    struct acpi_buf a;
    struct ans_dsd d;

    build_m4_dsd(&a, true, 3);
    CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_OK);
    CHECK(d.has_sart_version && d.sart_version == 3 && d.has_secure_bar && d.secure_bar == 1);

    build_m4_dsd(&a, false, 2);
    CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_OK);
    CHECK(d.has_sart_version && d.sart_version == 2 && !d.has_secure_bar);

    /* every length short of the whole must fail cleanly, never read past it */
    build_m4_dsd(&a, true, 3);
    for (uint32_t n = 0; n < a.n; n++) {
        uint8_t copy[512];
        memcpy(copy, a.b, n);
        put32(copy + 4, n); /* the buffer claims to be this long */
        if (n >= 8)
            CHECK(ans_dsd_parse(copy, n, &d) != ANS_DSD_OK || !d.has_sart_version);
    }
    /* a Length beyond what was returned */
    CHECK(ans_dsd_parse(a.b, a.n - 1, &d) == ANS_DSD_BAD_BUFFER);

    /* wrong signature */
    build_m4_dsd(&a, true, 3);
    a.b[0] ^= 1;
    CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_BAD_BUFFER);

    /* another UUID only */
    build_m4_dsd(&a, true, 3);
    a.b[12 + 4] ^= 0xff;
    CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_NO_UUID);

    /* 8-byte integers, as ACPI 2.0 integers may come */
    {
        uint32_t props;
        memset(&a, 0, sizeof(a));
        a.n = 12;
        arg(&a, 2, uuid, 16);
        props = begin_package(&a);
        prop(&a, "sart-version", 4, 8);
        prop(&a, "something-else", 7, 4);
        end_package(&a, props);
        put32(a.b, 0x426f6541);
        put32(a.b + 4, a.n);
        put32(a.b + 8, 2);
        CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_OK && d.sart_version == 4 &&
              !d.has_secure_bar);
    }

    /* the whole package as one argument */
    {
        uint32_t whole, props;
        memset(&a, 0, sizeof(a));
        a.n = 12;
        whole = begin_package(&a);
        arg(&a, 2, uuid, 16);
        props = begin_package(&a);
        prop(&a, "sart-version", 3, 4);
        end_package(&a, props);
        end_package(&a, whole);
        put32(a.b, 0x426f6541);
        put32(a.b + 4, a.n);
        put32(a.b + 8, 1);
        CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_OK && d.sart_version == 3);
    }

    /* a value that is a string, not an integer */
    {
        uint32_t props, p;
        memset(&a, 0, sizeof(a));
        a.n = 12;
        arg(&a, 2, uuid, 16);
        props = begin_package(&a);
        p = begin_package(&a);
        arg(&a, 1, "sart-version", 13);
        arg(&a, 1, "3", 2);
        end_package(&a, p);
        end_package(&a, props);
        put32(a.b, 0x426f6541);
        put32(a.b + 4, a.n);
        put32(a.b + 8, 2);
        CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_BAD_PROPERTY);
    }

    /* a key that only starts the same */
    {
        uint32_t props;
        memset(&a, 0, sizeof(a));
        a.n = 12;
        arg(&a, 2, uuid, 16);
        props = begin_package(&a);
        prop(&a, "sart-versionX", 3, 4);
        end_package(&a, props);
        put32(a.b, 0x426f6541);
        put32(a.b + 4, a.n);
        put32(a.b + 8, 2);
        CHECK(ans_dsd_parse(a.b, a.n, &d) == ANS_DSD_OK && !d.has_sart_version);
    }
}

/* ------------------------------------------------------------------------------- */
/* Formatter                                                                         */

static void test_format(void)
{
    char buf[128];
    size_t n;

    n = ans_format(buf, sizeof(buf), "%s %d %u %x %02x %08x %llu %llx %016llx", "ab", -5, 7u,
                   0xbeefu, 3u, 0x1234u, 18446744073709551615ull, 0x123456789abcdefull,
                   0x42ull);
    CHECK(strcmp(buf, "ab -5 7 beef 03 00001234 18446744073709551615 123456789abcdef "
                      "0000000000000042") == 0);
    CHECK(n == strlen(buf));
    ans_format(buf, sizeof(buf), "[%5llu.%06llu] %s", 12ull, 345ull, "x");
    CHECK(strcmp(buf, "[   12.000345] x") == 0);
    ans_format(buf, sizeof(buf), "%s|%-4s|%c|%%|%X", (const char *)NULL, "a", 'z', 0xabu);
    CHECK(strcmp(buf, "(null)|a   |z|%|AB") == 0);
    /* cut off, always terminated */
    n = ans_format(buf, 6, "%s", "abcdefgh");
    CHECK(n == 5 && strcmp(buf, "abcde") == 0);
    n = ans_format(buf, 1, "%d", 123);
    CHECK(n == 0 && buf[0] == '\0');
    /* a lone % at the end, and an unknown conversion */
    ans_format(buf, sizeof(buf), "a%q b%");
    CHECK(strcmp(buf, "a%q b") == 0);
}

/* ------------------------------------------------------------------------------- */

struct test {
    const char *name;
    void (*fn)(void);
};

static const struct test g_tests[] = {
    {"scsi_every_opcode", test_every_opcode},
    {"scsi_refused_write_targets", test_refused_write_targets},
    {"scsi_reads", test_reads},
    {"scsi_inquiry", test_inquiry},
    {"scsi_capacity", test_capacity},
    {"scsi_mode_sense", test_mode_sense},
    {"scsi_report_luns_and_no_ops", test_report_luns_and_no_ops},
    {"scsi_sense", test_sense},
    {"dsd", test_dsd},
    {"format", test_format},
};

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;
    int ran = 0;

    for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
        int before = g_failures;

        if (filter && !strstr(g_tests[i].name, filter))
            continue;
        g_tests[i].fn();
        ran++;
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", g_tests[i].name);
    }
    printf("host test: %d tests, %d checks, %d failed\n", ran, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
