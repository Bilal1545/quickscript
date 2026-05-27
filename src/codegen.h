/*
 * codegen.h — AST → C source emission.
 *
 * Mirrors src/codegen.js. Output is intended to be compiled with gcc against
 * runtime.c — we rely on GCC statement-expression extensions ({ ... }).
 *
 * The codegen produces a single C source string. Errors stop generation
 * and are reported via Codegen.err.
 */

#ifndef QSC_CODEGEN_H
#define QSC_CODEGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "ast.h"
#include "buffer.h"
#include "map.h"

typedef struct {
    char message[256];
    uint32_t line;
    uint32_t col;
    bool present;
} CodegenError;

typedef enum { CTX_FOR, CTX_WHILE, CTX_DOWHILE, CTX_SWITCH } CtxKind;

typedef struct {
    CtxKind kind;
    const char *break_label;     /* for CTX_SWITCH */
    const char *continue_label;  /* for CTX_FOR */
} CtxFrame;

typedef struct {
    Arena *arena;
    const char *source;
    size_t source_len;
    const char *filename;

    /* counters */
    int temp_counter;
    int lambda_counter;
    int label_counter;

    /* output buffers */
    Buffer lambdas;     /* lambda + class method bodies, joined */
    Buffer fns;         /* top-level function declarations */
    Buffer main_body;   /* statements emitted into main() */

    /* sets — insertion-ordered for determinism */
    Map known_functions;
    Map globals;

    /* loop/switch context stack */
    CtxFrame *ctx_stack;
    size_t ctx_len, ctx_cap;

    CodegenError err;
} Codegen;

/* Emit the full C source for `program`. Returns a malloc'd string on success
 * (caller frees) or NULL with err populated on failure. */
char *codegen_generate(AstNode *program, const char *source, size_t source_len,
                       const char *filename, Arena *arena, CodegenError *out_err);

#endif /* QSC_CODEGEN_H */
