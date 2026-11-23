/* SPDX-License-Identifier: MIT */
/* Reads the ANS0 node's _DSD; see dsd.h. */

#include "dsd.h"

#define ACPI_OUTPUT_SIGNATURE 0x426f6541u /* 'BoeA', ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1 */
#define ACPI_OUTPUT_HEADER    12u
#define ACPI_ARG_HEADER       4u

#define ACPI_ARG_INTEGER 0
#define ACPI_ARG_STRING  1
#define ACPI_ARG_BUFFER  2
#define ACPI_ARG_PACKAGE 3

/* Device Properties UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301, as ToUUID lays it out. */
static const uint8_t device_properties_uuid[16] = {
    0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
    0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01,
};

/* One argument, as found in the buffer. */
struct arg {
    uint16_t type;
    uint16_t len;
    const uint8_t *data;
};

/* A run of consecutive arguments. */
struct args {
    const uint8_t *p;
    const uint8_t *end;
};

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | (rd16(p + 2) << 16);
}

/* Takes the next argument off the run; false when it would run past the end. */
static bool next_arg(struct args *a, struct arg *out)
{
    size_t room = (size_t)(a->end - a->p);
    uint32_t len, size;

    if (room < ACPI_ARG_HEADER)
        return false;
    out->type = (uint16_t)rd16(a->p);
    len = rd16(a->p + 2);
    out->len = (uint16_t)len;
    size = ACPI_ARG_HEADER + (len > 4 ? len : 4);
    if (size > room)
        return false;
    out->data = a->p + ACPI_ARG_HEADER;
    a->p += size;
    return true;
}

static bool get_integer(const struct arg *a, uint64_t *v)
{
    if (a->type != ACPI_ARG_INTEGER || (a->len != 4 && a->len != 8))
        return false;
    *v = rd32(a->data);
    if (a->len == 8)
        *v |= (uint64_t)rd32(a->data + 4) << 32;
    return true;
}

static bool string_is(const struct arg *a, const char *s)
{
    uint32_t i = 0;

    if (a->type != ACPI_ARG_STRING)
        return false;
    for (; s[i]; i++) {
        if (i >= a->len || a->data[i] != (uint8_t)s[i])
            return false;
    }
    /* the rest is the terminating NUL, if anything */
    return i == a->len || (i + 1 == a->len && a->data[i] == 0);
}

/* The property package: each element a package of (string key, integer value). */
static enum ans_dsd_result parse_properties(const struct arg *props, struct ans_dsd *dsd)
{
    struct args list;
    struct arg prop;

    list.p = props->data;
    list.end = props->data + props->len;
    while (list.p < list.end) {
        struct args pair;
        struct arg key, value;
        uint64_t v;

        if (!next_arg(&list, &prop))
            return ANS_DSD_BAD_BUFFER;
        if (prop.type != ACPI_ARG_PACKAGE)
            return ANS_DSD_BAD_PROPERTY;
        pair.p = prop.data;
        pair.end = prop.data + prop.len;
        if (!next_arg(&pair, &key) || !next_arg(&pair, &value) || pair.p != pair.end)
            return ANS_DSD_BAD_PROPERTY;
        if (key.type != ACPI_ARG_STRING)
            return ANS_DSD_BAD_PROPERTY;
        if (string_is(&key, "sart-version")) {
            if (!get_integer(&value, &v))
                return ANS_DSD_BAD_PROPERTY;
            dsd->has_sart_version = true;
            dsd->sart_version = v;
        } else if (string_is(&key, "nvme-secure-bar")) {
            if (!get_integer(&value, &v))
                return ANS_DSD_BAD_PROPERTY;
            dsd->has_secure_bar = true;
            dsd->secure_bar = v;
        }
        /* other keys: not ours to know, skipped */
    }
    return ANS_DSD_OK;
}

static bool is_properties_uuid(const struct arg *a)
{
    if (a->type != ACPI_ARG_BUFFER || a->len != 16)
        return false;
    for (int i = 0; i < 16; i++) {
        if (a->data[i] != device_properties_uuid[i])
            return false;
    }
    return true;
}

enum ans_dsd_result ans_dsd_parse(const void *buf, uint32_t len, struct ans_dsd *dsd)
{
    const uint8_t *b = buf;
    struct args top;
    struct arg uuid, props;
    uint32_t total, count;

    dsd->has_sart_version = false;
    dsd->sart_version = 0;
    dsd->has_secure_bar = false;
    dsd->secure_bar = 0;

    if (!b || len < ACPI_OUTPUT_HEADER || rd32(b) != ACPI_OUTPUT_SIGNATURE)
        return ANS_DSD_BAD_BUFFER;
    total = rd32(b + 4);
    count = rd32(b + 8);
    if (total < ACPI_OUTPUT_HEADER || total > len)
        return ANS_DSD_BAD_BUFFER;
    top.p = b + ACPI_OUTPUT_HEADER;
    top.end = b + total;

    /*
     * _DSD's elements (UUID, package, UUID, package ...) come as the top-level
     * arguments; should ACPI hand back the whole package as one argument instead,
     * look inside it.
     */
    if (count == 1) {
        struct args run = top;
        struct arg whole;

        if (next_arg(&run, &whole) && whole.type == ACPI_ARG_PACKAGE) {
            top.p = whole.data;
            top.end = whole.data + whole.len;
        }
    }
    while (top.p < top.end) {
        if (!next_arg(&top, &uuid) || !next_arg(&top, &props))
            return ANS_DSD_BAD_BUFFER;
        if (is_properties_uuid(&uuid) && props.type == ACPI_ARG_PACKAGE)
            return parse_properties(&props, dsd);
    }
    return ANS_DSD_NO_UUID;
}
