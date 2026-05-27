#include "errors.h"

#include <stdio.h>
#include <string.h>

const char *qsc_error_type_name(QscErrorType t) {
    switch (t) {
        case QSC_ERR_PARSE:   return "ParseError";
        case QSC_ERR_CODEGEN: return "CodegenError";
        case QSC_ERR_BUILD:   return "BuildError";
        case QSC_ERR_RUNTIME: return "RuntimeError";
    }
    return "Error";
}

/* Look up the 1-based line `line` in `source` (length `n`). Returns a pointer
 * to the start and writes the line length (excluding terminator). */
static const char *find_line(const char *source, size_t n, uint32_t line,
                             size_t *out_len) {
    if (!source || line == 0) { *out_len = 0; return NULL; }
    uint32_t cur = 1;
    const char *p = source;
    const char *end = source + n;
    while (cur < line && p < end) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) { *out_len = 0; return NULL; }
    const char *line_end = p;
    while (line_end < end && *line_end != '\n') line_end++;
    *out_len = (size_t)(line_end - p);
    return p;
}

void qsc_error_print(const QscError *err, const char *source, size_t source_len) {
    const char *type = qsc_error_type_name(err->type);

    /* "file:line:col " prefix only when we have location info */
    if (err->file && err->has_loc) {
        fprintf(stderr, "%s:%u:%u ", err->file, err->line, err->column + 1);
    } else if (err->file) {
        fprintf(stderr, "%s ", err->file);
    } else if (err->has_loc) {
        fprintf(stderr, "%u:%u ", err->line, err->column + 1);
    }
    fprintf(stderr, "%s: %s", type, err->message ? err->message : "");

    if (err->has_loc && source) {
        size_t line_len = 0;
        const char *line = find_line(source, source_len, err->line, &line_len);
        if (line) {
            /* line-number gutter width */
            char num_buf[16];
            int num_w = snprintf(num_buf, sizeof num_buf, "%u", err->line);
            fprintf(stderr, "\n  %s | %.*s", num_buf, (int)line_len, line);
            fprintf(stderr, "\n  %*s | ", num_w, "");
            for (uint32_t i = 0; i < err->column && i < line_len; ++i) fputc(' ', stderr);
            fputc('^', stderr);
        }
    }
    fputc('\n', stderr);
}
