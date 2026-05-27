/*
 * errors.h — unified CompileError, formatted to match the JS compiler.
 *
 * Format:
 *   filename:LINE:COLUMN ErrorType: message
 *     LINE | source line
 *          | <spaces>^
 *
 * Column is 0-based internally and printed 1-based, mirroring the JS
 * `(this.column ?? 0) + 1` rule. The caret marker is positioned at the
 * 0-based column directly (`' '.repeat(column)`).
 */

#ifndef QSC_ERRORS_H
#define QSC_ERRORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    QSC_ERR_PARSE,
    QSC_ERR_CODEGEN,
    QSC_ERR_BUILD,
    QSC_ERR_RUNTIME,
} QscErrorType;

typedef struct {
    QscErrorType type;
    const char *message;        /* heap-allocated; freed with the error */
    const char *file;           /* borrowed, may be NULL */
    uint32_t line;              /* 1-based, 0 if unknown */
    uint32_t column;            /* 0-based */
    bool has_loc;
} QscError;

const char *qsc_error_type_name(QscErrorType t);

/* Print a formatted error to stderr. `source` may be NULL (then no
 * source-line context is printed). The function does not free anything. */
void qsc_error_print(const QscError *err, const char *source, size_t source_len);

#endif /* QSC_ERRORS_H */
