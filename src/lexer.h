/*
 * lexer.h — token stream over JS source.
 *
 * Interaction model: parser drives the lexer. Most tokens come from
 * lex_next(); template-literal continuations come from lex_template_part();
 * regex literals come from lex_regex_from_slash() because the lexer alone
 * cannot tell a regex from a division.
 *
 * Cooked string values for STRING / TEMPLATE_* tokens are allocated from the
 * arena passed to lex_init() and remain valid for the lifetime of that arena.
 */

#ifndef QSC_LEXER_H
#define QSC_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"

typedef enum {
    TK_EOF,

    /* literal-bearing */
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    TK_REGEX,
    TK_TEMPLATE_HEAD,     /* `head${ */
    TK_TEMPLATE_MIDDLE,   /* }middle${ */
    TK_TEMPLATE_TAIL,     /* }tail`     (also used for no-substitution backtick strings) */
    TK_C_BLOCK,           /* __c {{{ ...raw C... }}} — body in str_val */

    /* punctuators */
    TK_LBRACE, TK_RBRACE,
    TK_LPAREN, TK_RPAREN,
    TK_LBRACKET, TK_RBRACKET,
    TK_COMMA,
    TK_SEMI,
    TK_DOT,
    TK_ELLIPSIS,            /* ... */
    TK_QUESTION,
    TK_QUESTION_DOT,        /* ?. */
    TK_NULLISH,             /* ?? */
    TK_NULLISH_EQ,          /* ??= */
    TK_COLON,
    TK_ARROW,               /* => */

    TK_ASSIGN,
    TK_PLUS_EQ, TK_MINUS_EQ, TK_STAR_EQ, TK_SLASH_EQ, TK_PERCENT_EQ,
    TK_STAR_STAR_EQ,
    TK_AMP_EQ, TK_PIPE_EQ, TK_CARET_EQ,
    TK_LSHIFT_EQ, TK_RSHIFT_EQ, TK_URSHIFT_EQ,
    TK_AMP_AMP_EQ, TK_PIPE_PIPE_EQ,

    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_STAR_STAR,                 /* ** */

    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_BANG,
    TK_LSHIFT, TK_RSHIFT, TK_URSHIFT,

    TK_EQ_EQ, TK_BANG_EQ, TK_EQ_EQ_EQ, TK_BANG_EQ_EQ,
    TK_LT, TK_GT, TK_LE, TK_GE,

    TK_AMP_AMP, TK_PIPE_PIPE,
    TK_PLUS_PLUS, TK_MINUS_MINUS,

    /* keywords */
    TK_KW_BREAK, TK_KW_CASE, TK_KW_CATCH, TK_KW_CLASS, TK_KW_CONST,
    TK_KW_CONTINUE, TK_KW_DEBUGGER, TK_KW_DEFAULT, TK_KW_DELETE, TK_KW_DO,
    TK_KW_ELSE, TK_KW_EXPORT, TK_KW_EXTENDS, TK_KW_FALSE, TK_KW_FINALLY,
    TK_KW_FOR, TK_KW_FUNCTION, TK_KW_IF, TK_KW_IMPORT, TK_KW_IN,
    TK_KW_INSTANCEOF, TK_KW_LET, TK_KW_NEW, TK_KW_NULL, TK_KW_RETURN,
    TK_KW_SUPER, TK_KW_SWITCH, TK_KW_THIS, TK_KW_THROW, TK_KW_TRUE,
    TK_KW_TRY, TK_KW_TYPEOF, TK_KW_VAR, TK_KW_VOID, TK_KW_WHILE,
    TK_KW_WITH, TK_KW_YIELD,
    /* contextual but commonly used; parser still distinguishes via context: */
    TK_KW_ASYNC, TK_KW_AWAIT,

    TK_COUNT
} TokenKind;

typedef struct {
    TokenKind kind;
    uint32_t start;             /* byte offset into source */
    uint32_t end;
    uint32_t line;              /* 1-based */
    uint32_t col;               /* 0-based */
    bool had_line_break_before; /* required by ASI */

    /* literal payload */
    double number_val;          /* TK_NUMBER */
    const char *str_val;        /* TK_STRING, TK_TEMPLATE_*, TK_REGEX (pattern) — cooked, NUL-terminated */
    uint32_t str_len;
    const char *flags_val;      /* TK_REGEX flags (NUL-terminated, may be empty) */
    bool template_terminates;   /* TK_TEMPLATE_HEAD/MIDDLE/TAIL: true if this part closes the backtick */
    bool from_substitution;     /* TEMPLATE_*: true when continued from `}` (not a fresh backtick) */
} Token;

typedef struct LexError {
    char message[256];
    uint32_t line;
    uint32_t col;
    bool present;
} LexError;

typedef struct {
    /* input */
    const char *src;
    size_t src_len;
    const char *filename;

    /* cursor */
    size_t pos;
    uint32_t line;
    uint32_t col;

    /* arena for cooked string storage */
    Arena *arena;

    /* template handling: stack of brace depths at which a template substitution opened */
    int *tmpl_stack;
    size_t tmpl_stack_len;
    size_t tmpl_stack_cap;
    int brace_depth;            /* current { } nesting */

    /* error reporting (lexer errors fall here; parser may also write its own) */
    LexError err;
} Lexer;

void lex_init(Lexer *l, const char *src, size_t src_len, const char *filename, Arena *arena);
void lex_free(Lexer *l);

/* Returns true on success; on failure populates l->err and returns false.
 * On EOF, returns true with tok->kind == TK_EOF. */
bool lex_next(Lexer *l, Token *tok);

/* Lex a template part beginning at l->pos (which must point to a character
 * right after the opening backtick or the closing `}` of a substitution).
 * Produces TK_TEMPLATE_HEAD / TK_TEMPLATE_MIDDLE / TK_TEMPLATE_TAIL.
 * The caller must call this instead of lex_next() at those positions. */
bool lex_template_part(Lexer *l, Token *tok, bool after_substitution);

/* Re-lex the slash that lex_next() just produced as a regex literal.
 * Call this when the parser determines (from the prior token) that the slash
 * begins a regex. The caller must invoke this BEFORE consuming any further
 * tokens; it rewinds to the slash and re-scans. */
bool lex_regex_from_slash(Lexer *l, Token *tok);

/* Diagnostic helpers. */
const char *tk_name(TokenKind k);
bool tk_is_keyword(TokenKind k);

#endif /* QSC_LEXER_H */
