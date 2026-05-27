/*
 * parser.h — recursive-descent / Pratt parser for ESTree-style JS subset.
 *
 * The parser consumes a pre-lexed Token array (built once by parse_source)
 * and emits AstNode trees rooted at a Program node.
 *
 * Error model: a parser error stops parsing and populates p->err.
 * The parser does not attempt error recovery.
 */

#ifndef QSC_PARSER_H
#define QSC_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "ast.h"
#include "lexer.h"

typedef struct {
    Token *tokens;
    size_t count;
    size_t cap;
} TokenBuffer;

typedef struct {
    char message[256];
    uint32_t line;
    uint32_t col;
    bool present;
} ParseError;

typedef struct {
    /* input */
    const char *src;
    size_t src_len;
    const char *filename;

    /* arena & lexer (kept alive for the parse duration) */
    Arena *arena;
    Lexer lexer;

    /* token buffer */
    TokenBuffer toks;
    size_t cur;

    /* state flags affecting expression parsing */
    bool no_in;          /* when true, parseExpression treats `in` as terminator */
    bool allow_yield;    /* true inside generator functions */
    bool allow_await;    /* true inside async functions */

    /* error */
    ParseError err;
} Parser;

/* Top-level: lex + parse `src` into a Program node.
 * On success returns the Program AstNode (allocated from `arena`).
 * On failure returns NULL and fills *out_err. */
AstNode *parse_source(const char *src, size_t src_len, const char *filename,
                      Arena *arena, ParseError *out_err);

#endif /* QSC_PARSER_H */
