#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"

/* ------- char classes --------------------------------------------------- */

static inline bool is_id_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static inline bool is_id_cont(int c) {
    return is_id_start(c) || (c >= '0' && c <= '9');
}
static inline bool is_digit(int c) { return c >= '0' && c <= '9'; }
static inline bool is_hex(int c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/* ------- cursor --------------------------------------------------------- */

static inline int peek(const Lexer *l) {
    return l->pos < l->src_len ? (unsigned char)l->src[l->pos] : -1;
}
static inline int peek_n(const Lexer *l, size_t n) {
    return l->pos + n < l->src_len ? (unsigned char)l->src[l->pos + n] : -1;
}
static inline bool at_eof(const Lexer *l) { return l->pos >= l->src_len; }

static void advance(Lexer *l) {
    if (l->pos < l->src_len) {
        if (l->src[l->pos] == '\n') {
            l->line++;
            l->col = 0;
        } else {
            l->col++;
        }
        l->pos++;
    }
}

/* Consume `c` if at cursor; return whether consumed. */
static bool match(Lexer *l, int c) {
    if (peek(l) == c) { advance(l); return true; }
    return false;
}

/* ------- error helper --------------------------------------------------- */

static bool lex_error(Lexer *l, const char *msg) {
    if (!l->err.present) {
        snprintf(l->err.message, sizeof l->err.message, "%s", msg);
        l->err.line = l->line;
        l->err.col = l->col;
        l->err.present = true;
    }
    return false;
}

/* ------- whitespace + comments ----------------------------------------- */

/* Returns true if any line terminator was consumed during skip. */
static bool skip_ws_comments(Lexer *l) {
    bool line_break = false;
    while (!at_eof(l)) {
        int c = peek(l);
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r') {
            advance(l);
        } else if (c == '\n') {
            line_break = true;
            advance(l);
        } else if (c == '/' && peek_n(l, 1) == '/') {
            advance(l); advance(l);
            while (!at_eof(l) && peek(l) != '\n') advance(l);
        } else if (c == '/' && peek_n(l, 1) == '*') {
            advance(l); advance(l);
            while (!at_eof(l)) {
                if (peek(l) == '*' && peek_n(l, 1) == '/') {
                    advance(l); advance(l);
                    break;
                }
                if (peek(l) == '\n') line_break = true;
                advance(l);
            }
        } else {
            break;
        }
    }
    return line_break;
}

/* ------- keyword lookup ------------------------------------------------- */

typedef struct {
    const char *name;
    TokenKind kind;
} KwEntry;

/* Alphabetically sorted; used by bsearch. */
static const KwEntry KEYWORDS[] = {
    {"async",      TK_KW_ASYNC},
    {"await",      TK_KW_AWAIT},
    {"break",      TK_KW_BREAK},
    {"case",       TK_KW_CASE},
    {"catch",      TK_KW_CATCH},
    {"class",      TK_KW_CLASS},
    {"const",      TK_KW_CONST},
    {"continue",   TK_KW_CONTINUE},
    {"debugger",   TK_KW_DEBUGGER},
    {"default",    TK_KW_DEFAULT},
    {"delete",     TK_KW_DELETE},
    {"do",         TK_KW_DO},
    {"else",       TK_KW_ELSE},
    {"export",     TK_KW_EXPORT},
    {"extends",    TK_KW_EXTENDS},
    {"false",      TK_KW_FALSE},
    {"finally",    TK_KW_FINALLY},
    {"for",        TK_KW_FOR},
    {"function",   TK_KW_FUNCTION},
    {"if",         TK_KW_IF},
    {"import",     TK_KW_IMPORT},
    {"in",         TK_KW_IN},
    {"instanceof", TK_KW_INSTANCEOF},
    {"let",        TK_KW_LET},
    {"new",        TK_KW_NEW},
    {"null",       TK_KW_NULL},
    {"return",     TK_KW_RETURN},
    {"super",      TK_KW_SUPER},
    {"switch",     TK_KW_SWITCH},
    {"this",       TK_KW_THIS},
    {"throw",      TK_KW_THROW},
    {"true",       TK_KW_TRUE},
    {"try",        TK_KW_TRY},
    {"typeof",     TK_KW_TYPEOF},
    {"var",        TK_KW_VAR},
    {"void",       TK_KW_VOID},
    {"while",      TK_KW_WHILE},
    {"with",       TK_KW_WITH},
    {"yield",      TK_KW_YIELD},
};
#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static int kw_cmp(const void *a, const void *b) {
    const KwEntry *ka = (const KwEntry *)a;
    const KwEntry *kb = (const KwEntry *)b;
    return strcmp(ka->name, kb->name);
}

static TokenKind kw_lookup(const char *s, size_t n) {
    /* bsearch needs NUL-terminated key */
    char buf[32];
    if (n >= sizeof buf) return TK_IDENT;
    memcpy(buf, s, n);
    buf[n] = '\0';
    KwEntry key = {buf, TK_IDENT};
    KwEntry *hit = (KwEntry *)bsearch(&key, KEYWORDS, KEYWORD_COUNT, sizeof(KwEntry), kw_cmp);
    return hit ? hit->kind : TK_IDENT;
}

/* ------- identifier ----------------------------------------------------- */

static bool lex_ident(Lexer *l, Token *t) {
    size_t s = l->pos;
    while (!at_eof(l) && is_id_cont(peek(l))) advance(l);
    t->kind = kw_lookup(l->src + s, l->pos - s);
    t->start = (uint32_t)s;
    t->end = (uint32_t)l->pos;
    return true;
}

/* ------- number --------------------------------------------------------- */

static bool lex_number(Lexer *l, Token *t) {
    size_t s = l->pos;
    int c = peek(l);
    double val = 0;

    if (c == '0' && (peek_n(l, 1) == 'x' || peek_n(l, 1) == 'X')) {
        advance(l); advance(l);
        size_t start_digits = l->pos;
        while (!at_eof(l) && is_hex(peek(l))) {
            val = val * 16.0 + hex_val(peek(l));
            advance(l);
        }
        if (l->pos == start_digits) return lex_error(l, "expected hex digits");
    } else if (c == '0' && (peek_n(l, 1) == 'o' || peek_n(l, 1) == 'O')) {
        advance(l); advance(l);
        size_t start_digits = l->pos;
        while (!at_eof(l) && peek(l) >= '0' && peek(l) <= '7') {
            val = val * 8.0 + (peek(l) - '0');
            advance(l);
        }
        if (l->pos == start_digits) return lex_error(l, "expected octal digits");
    } else if (c == '0' && (peek_n(l, 1) == 'b' || peek_n(l, 1) == 'B')) {
        advance(l); advance(l);
        size_t start_digits = l->pos;
        while (!at_eof(l) && (peek(l) == '0' || peek(l) == '1')) {
            val = val * 2.0 + (peek(l) - '0');
            advance(l);
        }
        if (l->pos == start_digits) return lex_error(l, "expected binary digits");
    } else {
        /* decimal with optional fraction and exponent — defer to strtod for accuracy. */
        while (!at_eof(l) && is_digit(peek(l))) advance(l);
        if (peek(l) == '.') {
            advance(l);
            while (!at_eof(l) && is_digit(peek(l))) advance(l);
        }
        if (peek(l) == 'e' || peek(l) == 'E') {
            advance(l);
            if (peek(l) == '+' || peek(l) == '-') advance(l);
            if (!is_digit(peek(l))) return lex_error(l, "expected exponent digits");
            while (!at_eof(l) && is_digit(peek(l))) advance(l);
        }
        /* parse via strtod over the in-memory slice */
        size_t n = l->pos - s;
        char tmp[64];
        const char *p = l->src + s;
        if (n >= sizeof tmp) {
            char *heap = (char *)malloc(n + 1);
            if (!heap) return lex_error(l, "out of memory");
            memcpy(heap, p, n);
            heap[n] = '\0';
            val = strtod(heap, NULL);
            free(heap);
        } else {
            memcpy(tmp, p, n);
            tmp[n] = '\0';
            val = strtod(tmp, NULL);
        }
    }

    /* Reject identifier-character immediately after literal (e.g. `1abc`). */
    if (!at_eof(l) && is_id_start(peek(l))) return lex_error(l, "identifier directly after numeric literal");

    t->kind = TK_NUMBER;
    t->start = (uint32_t)s;
    t->end = (uint32_t)l->pos;
    t->number_val = val;
    return true;
}

/* ------- string --------------------------------------------------------- */

static bool read_hex_escape(Lexer *l, Buffer *out, int digits) {
    int v = 0;
    for (int i = 0; i < digits; ++i) {
        if (!is_hex(peek(l))) return lex_error(l, "bad hex escape");
        v = (v << 4) | hex_val(peek(l));
        advance(l);
    }
    /* UTF-8 encode v */
    if (v < 0x80) {
        buf_append_char(out, (char)v);
    } else if (v < 0x800) {
        buf_append_char(out, (char)(0xC0 | (v >> 6)));
        buf_append_char(out, (char)(0x80 | (v & 0x3F)));
    } else if (v < 0x10000) {
        buf_append_char(out, (char)(0xE0 | (v >> 12)));
        buf_append_char(out, (char)(0x80 | ((v >> 6) & 0x3F)));
        buf_append_char(out, (char)(0x80 | (v & 0x3F)));
    } else {
        buf_append_char(out, (char)(0xF0 | (v >> 18)));
        buf_append_char(out, (char)(0x80 | ((v >> 12) & 0x3F)));
        buf_append_char(out, (char)(0x80 | ((v >> 6) & 0x3F)));
        buf_append_char(out, (char)(0x80 | (v & 0x3F)));
    }
    return true;
}

static bool read_unicode_escape(Lexer *l, Buffer *out) {
    /* assumes the leading 'u' has been consumed */
    if (peek(l) == '{') {
        advance(l);
        int v = 0;
        int n = 0;
        while (is_hex(peek(l))) {
            v = (v << 4) | hex_val(peek(l));
            advance(l);
            n++;
            if (n > 6) return lex_error(l, "unicode escape out of range");
        }
        if (n == 0) return lex_error(l, "empty unicode escape");
        if (peek(l) != '}') return lex_error(l, "expected '}' in unicode escape");
        advance(l);
        if (v > 0x10FFFF) return lex_error(l, "unicode escape out of range");
        /* utf8 encode */
        if (v < 0x80) {
            buf_append_char(out, (char)v);
        } else if (v < 0x800) {
            buf_append_char(out, (char)(0xC0 | (v >> 6)));
            buf_append_char(out, (char)(0x80 | (v & 0x3F)));
        } else if (v < 0x10000) {
            buf_append_char(out, (char)(0xE0 | (v >> 12)));
            buf_append_char(out, (char)(0x80 | ((v >> 6) & 0x3F)));
            buf_append_char(out, (char)(0x80 | (v & 0x3F)));
        } else {
            buf_append_char(out, (char)(0xF0 | (v >> 18)));
            buf_append_char(out, (char)(0x80 | ((v >> 12) & 0x3F)));
            buf_append_char(out, (char)(0x80 | ((v >> 6) & 0x3F)));
            buf_append_char(out, (char)(0x80 | (v & 0x3F)));
        }
        return true;
    }
    return read_hex_escape(l, out, 4);
}

/* Process a backslash escape into `out`. The backslash has been consumed. */
static bool read_escape(Lexer *l, Buffer *out, bool in_template) {
    int c = peek(l);
    advance(l);
    switch (c) {
        case 'n': buf_append_char(out, '\n'); return true;
        case 't': buf_append_char(out, '\t'); return true;
        case 'r': buf_append_char(out, '\r'); return true;
        case 'b': buf_append_char(out, '\b'); return true;
        case 'f': buf_append_char(out, '\f'); return true;
        case 'v': buf_append_char(out, '\v'); return true;
        case '0':
            /* \0 not followed by a digit is null */
            if (!is_digit(peek(l))) { buf_append_char(out, '\0'); return true; }
            return lex_error(l, "octal escapes not supported");
        case '\'': buf_append_char(out, '\''); return true;
        case '"':  buf_append_char(out, '"');  return true;
        case '\\': buf_append_char(out, '\\'); return true;
        case '`':  buf_append_char(out, '`');  return true;
        case '$':  buf_append_char(out, '$');  return true;
        case '/':  buf_append_char(out, '/');  return true;
        case '\n': /* line continuation — produces nothing */ return true;
        case 'x':  return read_hex_escape(l, out, 2);
        case 'u':  return read_unicode_escape(l, out);
        default:
            (void)in_template;
            /* unknown escape: emit the char verbatim (ES spec behavior). */
            buf_append_char(out, (char)c);
            return true;
    }
}

static bool lex_string(Lexer *l, Token *t) {
    int quote = peek(l);
    size_t s = l->pos;
    advance(l); /* consume opening quote */
    Buffer cooked; buf_init(&cooked);
    while (!at_eof(l)) {
        int c = peek(l);
        if (c == quote) {
            advance(l);
            t->kind = TK_STRING;
            t->start = (uint32_t)s;
            t->end = (uint32_t)l->pos;
            t->str_len = (uint32_t)cooked.len;
            t->str_val = arena_strndup(l->arena, cooked.data ? cooked.data : "", cooked.len);
            buf_free(&cooked);
            return true;
        }
        if (c == '\n') { buf_free(&cooked); return lex_error(l, "unterminated string literal"); }
        if (c == '\\') {
            advance(l);
            if (!read_escape(l, &cooked, false)) { buf_free(&cooked); return false; }
        } else {
            buf_append_char(&cooked, (char)c);
            advance(l);
        }
    }
    buf_free(&cooked);
    return lex_error(l, "unterminated string literal");
}

/* ------- template literal ---------------------------------------------- */

static bool push_tmpl(Lexer *l, int brace_at) {
    if (l->tmpl_stack_len == l->tmpl_stack_cap) {
        size_t nc = l->tmpl_stack_cap ? l->tmpl_stack_cap * 2 : 4;
        int *p = (int *)realloc(l->tmpl_stack, nc * sizeof(int));
        if (!p) return lex_error(l, "out of memory");
        l->tmpl_stack = p;
        l->tmpl_stack_cap = nc;
    }
    l->tmpl_stack[l->tmpl_stack_len++] = brace_at;
    return true;
}
static int pop_tmpl(Lexer *l) {
    if (l->tmpl_stack_len == 0) return -1;
    return l->tmpl_stack[--l->tmpl_stack_len];
}
static int top_tmpl(const Lexer *l) {
    return l->tmpl_stack_len ? l->tmpl_stack[l->tmpl_stack_len - 1] : -1;
}

bool lex_template_part(Lexer *l, Token *tok, bool after_substitution) {
    size_t s = l->pos;
    uint32_t start_line = l->line;
    uint32_t start_col = l->col;
    Token zero = {0};
    *tok = zero;
    tok->line = start_line;
    tok->col = start_col;
    tok->from_substitution = after_substitution;
    Buffer cooked; buf_init(&cooked);

    /* The opening backtick (or the closing } of a substitution) has been consumed by the
     * caller. We read until '${' (HEAD/MIDDLE) or '`' (TAIL / no-substitution). */
    while (!at_eof(l)) {
        int c = peek(l);
        if (c == '`') {
            advance(l);
            tok->kind = after_substitution ? TK_TEMPLATE_TAIL : TK_TEMPLATE_TAIL;
            tok->template_terminates = true;
            tok->str_val = arena_strndup(l->arena, cooked.data ? cooked.data : "", cooked.len);
            tok->str_len = (uint32_t)cooked.len;
            tok->start = (uint32_t)s;
            tok->end = (uint32_t)l->pos;
            buf_free(&cooked);
            return true;
        }
        if (c == '$' && peek_n(l, 1) == '{') {
            advance(l); advance(l);
            tok->kind = after_substitution ? TK_TEMPLATE_MIDDLE : TK_TEMPLATE_HEAD;
            tok->template_terminates = false;
            tok->str_val = arena_strndup(l->arena, cooked.data ? cooked.data : "", cooked.len);
            tok->str_len = (uint32_t)cooked.len;
            tok->start = (uint32_t)s;
            tok->end = (uint32_t)l->pos;
            buf_free(&cooked);
            /* enter expression mode: bump brace_depth and record the level. */
            l->brace_depth++;
            return push_tmpl(l, l->brace_depth);
        }
        if (c == '\\') {
            advance(l);
            if (!read_escape(l, &cooked, true)) { buf_free(&cooked); return false; }
        } else {
            if (c == '\n') {
                /* preserve as-is in cooked output */
            }
            buf_append_char(&cooked, (char)c);
            advance(l);
        }
    }
    buf_free(&cooked);
    return lex_error(l, "unterminated template literal");
}

/* ------- regex ---------------------------------------------------------- */

bool lex_regex_from_slash(Lexer *l, Token *tok) {
    /* The caller has already consumed the slash. We must rewind to it. */
    if (l->pos == 0 || l->src[l->pos - 1] != '/') return lex_error(l, "regex re-lex must follow a '/'");
    l->pos--;
    /* Best-effort column rewind. */
    if (l->col > 0) l->col--;

    size_t s = l->pos;
    advance(l); /* consume opening / */
    Buffer pattern; buf_init(&pattern);
    bool in_class = false;
    while (!at_eof(l)) {
        int c = peek(l);
        if (c == '\n') { buf_free(&pattern); return lex_error(l, "unterminated regex"); }
        if (c == '\\') {
            buf_append_char(&pattern, (char)c);
            advance(l);
            if (!at_eof(l)) {
                buf_append_char(&pattern, (char)peek(l));
                advance(l);
            }
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        else if (c == '/' && !in_class) { advance(l); break; }
        buf_append_char(&pattern, (char)c);
        advance(l);
    }
    Buffer flags; buf_init(&flags);
    while (!at_eof(l) && is_id_cont(peek(l))) {
        buf_append_char(&flags, (char)peek(l));
        advance(l);
    }
    tok->kind = TK_REGEX;
    tok->start = (uint32_t)s;
    tok->end = (uint32_t)l->pos;
    tok->str_val = arena_strndup(l->arena, pattern.data ? pattern.data : "", pattern.len);
    tok->str_len = (uint32_t)pattern.len;
    tok->flags_val = arena_strndup(l->arena, flags.data ? flags.data : "", flags.len);
    buf_free(&pattern);
    buf_free(&flags);
    return true;
}

/* ------- top-level lex_next -------------------------------------------- */

void lex_init(Lexer *l, const char *src, size_t src_len, const char *filename, Arena *arena) {
    memset(l, 0, sizeof *l);
    l->src = src;
    l->src_len = src_len;
    l->filename = filename;
    l->arena = arena;
    l->line = 1;
    l->col = 0;
}

void lex_free(Lexer *l) {
    free(l->tmpl_stack);
    l->tmpl_stack = NULL;
    l->tmpl_stack_len = l->tmpl_stack_cap = 0;
}

static void emit_simple(Token *t, TokenKind k, uint32_t s, uint32_t e) {
    t->kind = k;
    t->start = s;
    t->end = e;
}

bool lex_next(Lexer *l, Token *tok) {
    Token zero = {0};
    *tok = zero;
    bool line_break = skip_ws_comments(l);
    tok->had_line_break_before = line_break;
    tok->line = l->line;
    tok->col = l->col;
    if (at_eof(l)) {
        tok->kind = TK_EOF;
        tok->start = tok->end = (uint32_t)l->pos;
        return true;
    }

    size_t s = l->pos;
    int c = peek(l);

    /* identifier / keyword */
    if (is_id_start(c)) {
        return lex_ident(l, tok);
    }

    /* number */
    if (is_digit(c) || (c == '.' && is_digit(peek_n(l, 1)))) {
        return lex_number(l, tok);
    }

    /* string */
    if (c == '"' || c == '\'') {
        return lex_string(l, tok);
    }

    /* template literal (no-substitution form has same TAIL kind) */
    if (c == '`') {
        advance(l); /* consume opening backtick */
        return lex_template_part(l, tok, false);
    }

    /* punctuators */
    advance(l);
    switch (c) {
        case '{':
            l->brace_depth++;
            emit_simple(tok, TK_LBRACE, (uint32_t)s, (uint32_t)l->pos);
            return true;
        case '}':
            if (top_tmpl(l) == l->brace_depth) {
                /* This `}` closes a template substitution. Re-enter template body. */
                pop_tmpl(l);
                l->brace_depth--;
                return lex_template_part(l, tok, true);
            }
            l->brace_depth--;
            emit_simple(tok, TK_RBRACE, (uint32_t)s, (uint32_t)l->pos);
            return true;
        case '(': emit_simple(tok, TK_LPAREN,   (uint32_t)s, (uint32_t)l->pos); return true;
        case ')': emit_simple(tok, TK_RPAREN,   (uint32_t)s, (uint32_t)l->pos); return true;
        case '[': emit_simple(tok, TK_LBRACKET, (uint32_t)s, (uint32_t)l->pos); return true;
        case ']': emit_simple(tok, TK_RBRACKET, (uint32_t)s, (uint32_t)l->pos); return true;
        case ',': emit_simple(tok, TK_COMMA,    (uint32_t)s, (uint32_t)l->pos); return true;
        case ';': emit_simple(tok, TK_SEMI,     (uint32_t)s, (uint32_t)l->pos); return true;
        case ':': emit_simple(tok, TK_COLON,    (uint32_t)s, (uint32_t)l->pos); return true;
        case '~': emit_simple(tok, TK_TILDE,    (uint32_t)s, (uint32_t)l->pos); return true;

        case '.':
            if (peek(l) == '.' && peek_n(l, 1) == '.') { advance(l); advance(l); emit_simple(tok, TK_ELLIPSIS, (uint32_t)s, (uint32_t)l->pos); }
            else emit_simple(tok, TK_DOT, (uint32_t)s, (uint32_t)l->pos);
            return true;

        case '?':
            if (match(l, '.')) emit_simple(tok, TK_QUESTION_DOT, (uint32_t)s, (uint32_t)l->pos);
            else if (peek(l) == '?') {
                advance(l);
                if (match(l, '=')) emit_simple(tok, TK_NULLISH_EQ, (uint32_t)s, (uint32_t)l->pos);
                else emit_simple(tok, TK_NULLISH, (uint32_t)s, (uint32_t)l->pos);
            }
            else emit_simple(tok, TK_QUESTION, (uint32_t)s, (uint32_t)l->pos);
            return true;

        case '+':
            if (match(l, '+'))      emit_simple(tok, TK_PLUS_PLUS, (uint32_t)s, (uint32_t)l->pos);
            else if (match(l, '=')) emit_simple(tok, TK_PLUS_EQ,   (uint32_t)s, (uint32_t)l->pos);
            else                    emit_simple(tok, TK_PLUS,      (uint32_t)s, (uint32_t)l->pos);
            return true;

        case '-':
            if (match(l, '-'))      emit_simple(tok, TK_MINUS_MINUS, (uint32_t)s, (uint32_t)l->pos);
            else if (match(l, '=')) emit_simple(tok, TK_MINUS_EQ,    (uint32_t)s, (uint32_t)l->pos);
            else                    emit_simple(tok, TK_MINUS,       (uint32_t)s, (uint32_t)l->pos);
            return true;

        case '*':
            if (peek(l) == '*') {
                advance(l);
                if (match(l, '=')) emit_simple(tok, TK_STAR_STAR_EQ, (uint32_t)s, (uint32_t)l->pos);
                else emit_simple(tok, TK_STAR_STAR, (uint32_t)s, (uint32_t)l->pos);
            } else if (match(l, '=')) {
                emit_simple(tok, TK_STAR_EQ, (uint32_t)s, (uint32_t)l->pos);
            } else {
                emit_simple(tok, TK_STAR, (uint32_t)s, (uint32_t)l->pos);
            }
            return true;

        case '/':
            if (match(l, '=')) emit_simple(tok, TK_SLASH_EQ, (uint32_t)s, (uint32_t)l->pos);
            else emit_simple(tok, TK_SLASH, (uint32_t)s, (uint32_t)l->pos);
            return true;

        case '%':
            if (match(l, '=')) emit_simple(tok, TK_PERCENT_EQ, (uint32_t)s, (uint32_t)l->pos);
            else emit_simple(tok, TK_PERCENT, (uint32_t)s, (uint32_t)l->pos);
            return true;

        case '=':
            if (peek(l) == '=') {
                advance(l);
                if (match(l, '=')) emit_simple(tok, TK_EQ_EQ_EQ, (uint32_t)s, (uint32_t)l->pos);
                else emit_simple(tok, TK_EQ_EQ, (uint32_t)s, (uint32_t)l->pos);
            } else if (match(l, '>')) {
                emit_simple(tok, TK_ARROW, (uint32_t)s, (uint32_t)l->pos);
            } else {
                emit_simple(tok, TK_ASSIGN, (uint32_t)s, (uint32_t)l->pos);
            }
            return true;

        case '!':
            if (peek(l) == '=') {
                advance(l);
                if (match(l, '=')) emit_simple(tok, TK_BANG_EQ_EQ, (uint32_t)s, (uint32_t)l->pos);
                else emit_simple(tok, TK_BANG_EQ, (uint32_t)s, (uint32_t)l->pos);
            } else {
                emit_simple(tok, TK_BANG, (uint32_t)s, (uint32_t)l->pos);
            }
            return true;

        case '<':
            if (peek(l) == '<') {
                advance(l);
                if (match(l, '=')) emit_simple(tok, TK_LSHIFT_EQ, (uint32_t)s, (uint32_t)l->pos);
                else emit_simple(tok, TK_LSHIFT, (uint32_t)s, (uint32_t)l->pos);
            } else if (match(l, '=')) {
                emit_simple(tok, TK_LE, (uint32_t)s, (uint32_t)l->pos);
            } else {
                emit_simple(tok, TK_LT, (uint32_t)s, (uint32_t)l->pos);
            }
            return true;

        case '>':
            if (peek(l) == '>') {
                advance(l);
                if (peek(l) == '>') {
                    advance(l);
                    if (match(l, '=')) emit_simple(tok, TK_URSHIFT_EQ, (uint32_t)s, (uint32_t)l->pos);
                    else emit_simple(tok, TK_URSHIFT, (uint32_t)s, (uint32_t)l->pos);
                } else if (match(l, '=')) {
                    emit_simple(tok, TK_RSHIFT_EQ, (uint32_t)s, (uint32_t)l->pos);
                } else {
                    emit_simple(tok, TK_RSHIFT, (uint32_t)s, (uint32_t)l->pos);
                }
            } else if (match(l, '=')) {
                emit_simple(tok, TK_GE, (uint32_t)s, (uint32_t)l->pos);
            } else {
                emit_simple(tok, TK_GT, (uint32_t)s, (uint32_t)l->pos);
            }
            return true;

        case '&':
            if (peek(l) == '&') {
                advance(l);
                if (match(l, '=')) emit_simple(tok, TK_AMP_AMP_EQ, (uint32_t)s, (uint32_t)l->pos);
                else emit_simple(tok, TK_AMP_AMP, (uint32_t)s, (uint32_t)l->pos);
            } else if (match(l, '=')) {
                emit_simple(tok, TK_AMP_EQ, (uint32_t)s, (uint32_t)l->pos);
            } else {
                emit_simple(tok, TK_AMP, (uint32_t)s, (uint32_t)l->pos);
            }
            return true;

        case '|':
            if (peek(l) == '|') {
                advance(l);
                if (match(l, '=')) emit_simple(tok, TK_PIPE_PIPE_EQ, (uint32_t)s, (uint32_t)l->pos);
                else emit_simple(tok, TK_PIPE_PIPE, (uint32_t)s, (uint32_t)l->pos);
            } else if (match(l, '=')) {
                emit_simple(tok, TK_PIPE_EQ, (uint32_t)s, (uint32_t)l->pos);
            } else {
                emit_simple(tok, TK_PIPE, (uint32_t)s, (uint32_t)l->pos);
            }
            return true;

        case '^':
            if (match(l, '=')) emit_simple(tok, TK_CARET_EQ, (uint32_t)s, (uint32_t)l->pos);
            else emit_simple(tok, TK_CARET, (uint32_t)s, (uint32_t)l->pos);
            return true;
    }

    char msg[64];
    snprintf(msg, sizeof msg, "unexpected character '%c' (0x%02x)", c, (unsigned)c);
    return lex_error(l, msg);
}

/* ------- diagnostics ---------------------------------------------------- */

bool tk_is_keyword(TokenKind k) { return k >= TK_KW_BREAK && k <= TK_KW_AWAIT; }

const char *tk_name(TokenKind k) {
    static const char *NAMES[TK_COUNT] = {0};
    if (!NAMES[TK_EOF]) {
        NAMES[TK_EOF] = "EOF";
        NAMES[TK_IDENT] = "IDENT";
        NAMES[TK_NUMBER] = "NUMBER";
        NAMES[TK_STRING] = "STRING";
        NAMES[TK_REGEX] = "REGEX";
        NAMES[TK_TEMPLATE_HEAD] = "TEMPLATE_HEAD";
        NAMES[TK_TEMPLATE_MIDDLE] = "TEMPLATE_MIDDLE";
        NAMES[TK_TEMPLATE_TAIL] = "TEMPLATE_TAIL";
        NAMES[TK_LBRACE] = "{"; NAMES[TK_RBRACE] = "}";
        NAMES[TK_LPAREN] = "("; NAMES[TK_RPAREN] = ")";
        NAMES[TK_LBRACKET] = "["; NAMES[TK_RBRACKET] = "]";
        NAMES[TK_COMMA] = ","; NAMES[TK_SEMI] = ";";
        NAMES[TK_DOT] = "."; NAMES[TK_ELLIPSIS] = "...";
        NAMES[TK_QUESTION] = "?"; NAMES[TK_QUESTION_DOT] = "?.";
        NAMES[TK_NULLISH] = "?\?"; NAMES[TK_NULLISH_EQ] = "?\?=";
        NAMES[TK_COLON] = ":"; NAMES[TK_ARROW] = "=>";
        NAMES[TK_ASSIGN] = "=";
        NAMES[TK_PLUS_EQ] = "+="; NAMES[TK_MINUS_EQ] = "-=";
        NAMES[TK_STAR_EQ] = "*="; NAMES[TK_SLASH_EQ] = "/=";
        NAMES[TK_PERCENT_EQ] = "%="; NAMES[TK_STAR_STAR_EQ] = "**=";
        NAMES[TK_AMP_EQ] = "&="; NAMES[TK_PIPE_EQ] = "|=";
        NAMES[TK_CARET_EQ] = "^=";
        NAMES[TK_LSHIFT_EQ] = "<<="; NAMES[TK_RSHIFT_EQ] = ">>=";
        NAMES[TK_URSHIFT_EQ] = ">>>=";
        NAMES[TK_AMP_AMP_EQ] = "&&="; NAMES[TK_PIPE_PIPE_EQ] = "||=";
        NAMES[TK_PLUS] = "+"; NAMES[TK_MINUS] = "-";
        NAMES[TK_STAR] = "*"; NAMES[TK_SLASH] = "/"; NAMES[TK_PERCENT] = "%";
        NAMES[TK_STAR_STAR] = "**";
        NAMES[TK_AMP] = "&"; NAMES[TK_PIPE] = "|"; NAMES[TK_CARET] = "^";
        NAMES[TK_TILDE] = "~"; NAMES[TK_BANG] = "!";
        NAMES[TK_LSHIFT] = "<<"; NAMES[TK_RSHIFT] = ">>"; NAMES[TK_URSHIFT] = ">>>";
        NAMES[TK_EQ_EQ] = "=="; NAMES[TK_BANG_EQ] = "!=";
        NAMES[TK_EQ_EQ_EQ] = "==="; NAMES[TK_BANG_EQ_EQ] = "!==";
        NAMES[TK_LT] = "<"; NAMES[TK_GT] = ">";
        NAMES[TK_LE] = "<="; NAMES[TK_GE] = ">=";
        NAMES[TK_AMP_AMP] = "&&"; NAMES[TK_PIPE_PIPE] = "||";
        NAMES[TK_PLUS_PLUS] = "++"; NAMES[TK_MINUS_MINUS] = "--";
        for (size_t i = 0; i < KEYWORD_COUNT; ++i) {
            NAMES[KEYWORDS[i].kind] = KEYWORDS[i].name;
        }
    }
    if (k < 0 || k >= TK_COUNT) return "?";
    return NAMES[k] ? NAMES[k] : "?";
}
