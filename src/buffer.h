/*
 * buffer.h — growable byte buffer for codegen output.
 *
 * Uses plain malloc/realloc (not arena) because buffers may persist across
 * compile sub-tasks and benefit from explicit free.
 */

#ifndef QSC_BUFFER_H
#define QSC_BUFFER_H

#include <stdarg.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

void buf_init(Buffer *b);
void buf_free(Buffer *b);
void buf_reset(Buffer *b);

void buf_reserve(Buffer *b, size_t extra);
void buf_append(Buffer *b, const char *s, size_t n);
void buf_append_str(Buffer *b, const char *s);
void buf_append_char(Buffer *b, char c);
void buf_appendf(Buffer *b, const char *fmt, ...);
void buf_vappendf(Buffer *b, const char *fmt, va_list ap);

/* Append a JS string literal as a C string literal, escaping as needed.
 * Output is wrapped in surrounding double quotes. */
void buf_append_c_string(Buffer *b, const char *s, size_t n);

/* Detach the buffer's memory and return a NUL-terminated string. Caller
 * must free() the returned pointer. The buffer is left empty and reusable. */
char *buf_take(Buffer *b);

#endif /* QSC_BUFFER_H */
