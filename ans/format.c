/* SPDX-License-Identifier: MIT */
/* A small snprintf for the miniport's log; see format.h. */

#include "format.h"

struct out {
    char *buf;
    size_t cap;
    size_t n;
};

static void put(struct out *o, char c)
{
    if (o->n + 1 < o->cap)
        o->buf[o->n++] = c;
}

static void put_padded(struct out *o, const char *s, size_t len, unsigned width, bool left,
                       char pad)
{
    size_t fill = width > len ? width - len : 0;

    if (!left) {
        while (fill--)
            put(o, pad);
    }
    for (size_t i = 0; i < len; i++)
        put(o, s[i]);
    if (left) {
        while (fill--)
            put(o, ' ');
    }
}

/* Digits of v in base 10 or 16, into the end of tmp; returns where they start. */
static char *digits(char *end, unsigned long long v, unsigned base, bool upper)
{
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p = end;

    do {
        *--p = set[v % base];
        v /= base;
    } while (v);
    return p;
}

size_t ans_vformat(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct out o;

    o.buf = buf;
    o.cap = cap;
    o.n = 0;
    if (!buf || cap == 0)
        return 0;

    while (*fmt) {
        char tmp[24];
        char *end = tmp + sizeof(tmp);
        bool left = false;
        char pad = ' ';
        unsigned width = 0;
        int longs = 0; /* 0: int, 1: long, 2: long long, 3: size_t */

        if (*fmt != '%') {
            put(&o, *fmt++);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '0')
                pad = '0';
            else if (*fmt == '-')
                left = true;
            else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (unsigned)(*fmt++ - '0');
        if (*fmt == 'l') {
            fmt++;
            longs = 1;
            if (*fmt == 'l') {
                fmt++;
                longs = 2;
            }
        } else if (*fmt == 'z') {
            fmt++;
            longs = 3;
        }

        switch (*fmt) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                size_t len = 0;

                if (!s)
                    s = "(null)";
                while (s[len])
                    len++;
                put_padded(&o, s, len, width, left, ' ');
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                put_padded(&o, &c, 1, width, left, ' ');
                break;
            }
            case 'd': {
                long long v;
                unsigned long long m;
                char *p;

                if (longs == 2)
                    v = va_arg(ap, long long);
                else if (longs == 1)
                    v = va_arg(ap, long);
                else if (longs == 3)
                    v = (long long)va_arg(ap, size_t);
                else
                    v = va_arg(ap, int);
                m = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
                p = digits(end, m, 10, false);
                if (v < 0)
                    *--p = '-';
                put_padded(&o, p, (size_t)(end - p), width, left, pad);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                unsigned long long v;
                char *p;

                if (longs == 2)
                    v = va_arg(ap, unsigned long long);
                else if (longs == 1)
                    v = va_arg(ap, unsigned long);
                else if (longs == 3)
                    v = va_arg(ap, size_t);
                else
                    v = va_arg(ap, unsigned int);
                p = digits(end, v, *fmt == 'u' ? 10u : 16u, *fmt == 'X');
                put_padded(&o, p, (size_t)(end - p), width, left, pad);
                break;
            }
            case 'p': {
                unsigned long long v = (unsigned long long)(size_t)va_arg(ap, void *);
                char *p = digits(end, v, 16, false);

                put(&o, '0');
                put(&o, 'x');
                put_padded(&o, p, (size_t)(end - p), 16, false, '0');
                break;
            }
            case '%':
                put(&o, '%');
                break;
            case '\0':
                fmt--; /* a lone % at the end: stop at the NUL below */
                break;
            default:
                /* unknown conversion: show it as it is, so the mistake is visible */
                put(&o, '%');
                put(&o, *fmt);
                break;
        }
        fmt++;
    }
    o.buf[o.n] = '\0';
    return o.n;
}

size_t ans_format(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    size_t n;

    va_start(ap, fmt);
    n = ans_vformat(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
