#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_INITIAL 64

static void grow_to(Buffer *b, size_t need) {
    if (b->cap >= need) return;
    size_t cap = b->cap ? b->cap : BUF_INITIAL;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(b->data, cap);
    if (!p) {
        fprintf(stderr, "qsc: buffer out of memory\n");
        abort();
    }
    b->data = p;
    b->cap = cap;
}

void buf_init(Buffer *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buffer *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void buf_reset(Buffer *b) {
    b->len = 0;
}

void buf_reserve(Buffer *b, size_t extra) {
    /* +1 for NUL terminator. */
    grow_to(b, b->len + extra + 1);
}

void buf_append(Buffer *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(Buffer *b, const char *s) {
    buf_append(b, s, strlen(s));
}

void buf_append_char(Buffer *b, char c) {
    buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void buf_vappendf(Buffer *b, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) return;
    buf_reserve(b, (size_t)needed);
    vsnprintf(b->data + b->len, (size_t)needed + 1, fmt, ap);
    b->len += (size_t)needed;
}

void buf_appendf(Buffer *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_vappendf(b, fmt, ap);
    va_end(ap);
}

void buf_append_c_string(Buffer *b, const char *s, size_t n) {
    buf_append_char(b, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\\': buf_append(b, "\\\\", 2); break;
            case '"':  buf_append(b, "\\\"", 2); break;
            case '\n': buf_append(b, "\\n",  2); break;
            case '\r': buf_append(b, "\\r",  2); break;
            case '\t': buf_append(b, "\\t",  2); break;
            case '\f': buf_append(b, "\\f",  2); break;
            case '\v': buf_append(b, "\\v",  2); break;
            case '\b': buf_append(b, "\\b",  2); break;
            case '\0': buf_append(b, "\\0",  2); break;
            default:
                if (c < 0x20) buf_appendf(b, "\\x%02x", c);
                else buf_append_char(b, (char)c);
                break;
        }
    }
    buf_append_char(b, '"');
}

char *buf_take(Buffer *b) {
    if (!b->data) {
        char *empty = (char *)malloc(1);
        if (!empty) abort();
        empty[0] = '\0';
        return empty;
    }
    char *p = b->data;
    if (b->len >= b->cap) {
        /* shouldn't happen since we always keep one byte for NUL, but defensively grow */
        grow_to(b, b->len + 1);
        p = b->data;
    }
    p[b->len] = '\0';
    b->data = NULL;
    b->len = b->cap = 0;
    return p;
}
