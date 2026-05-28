#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- token buffer ----------------------------------------------- */

static void tb_push(TokenBuffer *tb, const Token *t) {
    if (tb->count == tb->cap) {
        size_t nc = tb->cap ? tb->cap * 2 : 64;
        Token *p = (Token *)realloc(tb->tokens, nc * sizeof(Token));
        if (!p) { fprintf(stderr, "qsc: parser out of memory\n"); abort(); }
        tb->tokens = p;
        tb->cap = nc;
    }
    tb->tokens[tb->count++] = *t;
}

/* ---------- error/cursor helpers --------------------------------------- */

static const Token *peek_tok(Parser *p) { return &p->toks.tokens[p->cur]; }
static const Token *peek_at(Parser *p, size_t off) {
    size_t idx = p->cur + off;
    if (idx >= p->toks.count) idx = p->toks.count - 1;
    return &p->toks.tokens[idx];
}
static TokenKind peek_kind(Parser *p) { return peek_tok(p)->kind; }
static const Token *advance_tok(Parser *p) {
    const Token *t = peek_tok(p);
    if (p->cur < p->toks.count - 1) p->cur++;
    return t;
}

static void set_error_at(Parser *p, const Token *t, const char *fmt, ...) {
    if (p->err.present) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err.message, sizeof p->err.message, fmt, ap);
    va_end(ap);
    p->err.line = t->line;
    p->err.col = t->col;
    p->err.present = true;
}

#define ERROR_AT(p, t, ...) do { set_error_at((p), (t), __VA_ARGS__); return NULL; } while (0)
#define ERROR_HERE(p, ...) do { set_error_at((p), peek_tok(p), __VA_ARGS__); return NULL; } while (0)

static void set_error_at_node(Parser *p, AstNode *n, const char *fmt, ...) {
    if (p->err.present) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err.message, sizeof p->err.message, fmt, ap);
    va_end(ap);
    p->err.line = n->line;
    p->err.col = n->col;
    p->err.present = true;
}
#define ERROR_NODE(p, n, ...) do { set_error_at_node((p), (n), __VA_ARGS__); return NULL; } while (0)

static bool match_kind(Parser *p, TokenKind k) {
    if (peek_kind(p) == k) { advance_tok(p); return true; }
    return false;
}
static const Token *expect(Parser *p, TokenKind k, const char *what) {
    if (peek_kind(p) != k) {
        set_error_at(p, peek_tok(p), "expected %s, got '%s'", what, tk_name(peek_kind(p)));
        return NULL;
    }
    return advance_tok(p);
}

/* Automatic semicolon insertion: consume an explicit `;` if present,
 * otherwise accept ASI before `}`, EOF, or a line break. */
static bool eat_semi(Parser *p) {
    if (match_kind(p, TK_SEMI)) return true;
    TokenKind k = peek_kind(p);
    if (k == TK_RBRACE || k == TK_EOF) return true;
    if (peek_tok(p)->had_line_break_before) return true;
    set_error_at(p, peek_tok(p), "expected ';'");
    return false;
}

/* ---------- node construction helpers ---------------------------------- */

static AstNode *make_node(Parser *p, AstKind k, const Token *start) {
    AstNode *n = ast_new(p->arena, k);
    n->start = start->start;
    n->line = start->line;
    n->col = start->col;
    n->end = start->end;
    n->filename = p->filename;
    return n;
}

static AstNode *make_ident_from_token(Parser *p, const Token *t) {
    AstNode *n = make_node(p, AST_IDENTIFIER, t);
    n->ident.name = arena_strndup(p->arena, p->src + t->start, t->end - t->start);
    n->end = t->end;
    return n;
}

/* Consume a binding-name token (identifier or contextual keyword). */
static AstNode *parse_binding_ident(Parser *p) {
    const Token *t = peek_tok(p);
    /* Accept TK_IDENT and contextual keywords commonly used as names. */
    if (t->kind == TK_IDENT || t->kind == TK_KW_ASYNC || t->kind == TK_KW_AWAIT ||
        t->kind == TK_KW_YIELD || t->kind == TK_KW_LET) {
        advance_tok(p);
        return make_ident_from_token(p, t);
    }
    ERROR_HERE(p, "expected identifier, got '%s'", tk_name(t->kind));
}

/* Consume an identifier-or-keyword as a property/key name. */
static AstNode *parse_ident_name(Parser *p) {
    const Token *t = peek_tok(p);
    if (t->kind == TK_IDENT || tk_is_keyword(t->kind)) {
        advance_tok(p);
        AstNode *n = make_node(p, AST_IDENTIFIER, t);
        n->ident.name = arena_strndup(p->arena, p->src + t->start, t->end - t->start);
        n->end = t->end;
        return n;
    }
    ERROR_HERE(p, "expected property name");
}

/* ---------- forward declarations --------------------------------------- */

static AstNode *parse_statement(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_expression(Parser *p);
static AstNode *parse_assign(Parser *p);
static AstNode *parse_unary(Parser *p);
static AstNode *parse_lhs_expr(Parser *p);   /* left-hand-side (call/member chains) */
static AstNode *parse_primary(Parser *p);
static AstNode *parse_var_decl(Parser *p, VarKind kind, bool no_semi);
static AstNode *parse_binding_pattern(Parser *p);
static AstNode *parse_function(Parser *p, bool is_decl);
static AstNode *parse_class(Parser *p, bool is_decl);
static AstNode *to_pattern(Parser *p, AstNode *expr);
static AstNode *parse_arrow_body(Parser *p, NodeList params, const Token *start);

/* ---------- expression: primary ---------------------------------------- */

static AstNode *parse_array_expr(Parser *p) {
    const Token *start = expect(p, TK_LBRACKET, "'['");
    if (!start) return NULL;
    AstNode *n = make_node(p, AST_ARRAY_EXPR, start);
    while (peek_kind(p) != TK_RBRACKET) {
        if (peek_kind(p) == TK_COMMA) {
            /* hole */
            nl_push(p->arena, &n->array.elements, NULL);
            advance_tok(p);
            continue;
        }
        AstNode *elt = NULL;
        if (peek_kind(p) == TK_ELLIPSIS) {
            const Token *st = advance_tok(p);
            AstNode *arg = parse_assign(p);
            if (!arg) return NULL;
            elt = make_node(p, AST_SPREAD_ELEMENT, st);
            elt->spread_or_rest.arg = arg;
            elt->end = arg->end;
        } else {
            elt = parse_assign(p);
            if (!elt) return NULL;
        }
        nl_push(p->arena, &n->array.elements, elt);
        if (peek_kind(p) != TK_RBRACKET) {
            if (!expect(p, TK_COMMA, "','")) return NULL;
        }
    }
    const Token *end = expect(p, TK_RBRACKET, "']'");
    if (!end) return NULL;
    n->end = end->end;
    return n;
}

static AstNode *parse_property(Parser *p) {
    const Token *start = peek_tok(p);

    /* spread inside object literal */
    if (start->kind == TK_ELLIPSIS) {
        advance_tok(p);
        AstNode *arg = parse_assign(p);
        if (!arg) return NULL;
        AstNode *s = make_node(p, AST_SPREAD_ELEMENT, start);
        s->spread_or_rest.arg = arg;
        s->end = arg->end;
        return s;
    }

    bool computed = false;
    AstNode *key = NULL;
    if (start->kind == TK_LBRACKET) {
        advance_tok(p);
        computed = true;
        key = parse_assign(p);
        if (!key) return NULL;
        if (!expect(p, TK_RBRACKET, "']'")) return NULL;
    } else if (start->kind == TK_STRING) {
        advance_tok(p);
        key = make_node(p, AST_LITERAL, start);
        key->literal.kind = LIT_STRING;
        key->literal.v.str.value = start->str_val;
        key->literal.v.str.len = start->str_len;
        key->end = start->end;
    } else if (start->kind == TK_NUMBER) {
        advance_tok(p);
        key = make_node(p, AST_LITERAL, start);
        key->literal.kind = LIT_NUMBER;
        key->literal.v.number = start->number_val;
        key->end = start->end;
    } else {
        key = parse_ident_name(p);
        if (!key) return NULL;
    }

    AstNode *prop = make_node(p, AST_PROPERTY, start);
    prop->property.key = key;
    prop->property.computed = computed;

    if (peek_kind(p) == TK_COLON) {
        advance_tok(p);
        AstNode *value = parse_assign(p);
        if (!value) return NULL;
        prop->property.value = value;
        prop->property.shorthand = false;
        prop->end = value->end;
        return prop;
    }
    if (peek_kind(p) == TK_LPAREN) {
        /* method shorthand: key (params) { body } */
        const Token *fstart = peek_tok(p);
        AstNode *fn = make_node(p, AST_FN_EXPR, fstart);
        fn->func.is_arrow = false;
        /* params */
        advance_tok(p); /* ( */
        while (peek_kind(p) != TK_RPAREN) {
            AstNode *param = parse_binding_pattern(p);
            if (!param) return NULL;
            nl_push(p->arena, &fn->func.params, param);
            if (peek_kind(p) != TK_RPAREN) {
                if (!expect(p, TK_COMMA, "','")) return NULL;
            }
        }
        if (!expect(p, TK_RPAREN, "')'")) return NULL;
        AstNode *body = parse_block(p);
        if (!body) return NULL;
        fn->func.body = body;
        fn->end = body->end;
        prop->property.value = fn;
        prop->property.is_method = true;
        prop->property.method_kind = MK_METHOD;
        prop->end = fn->end;
        return prop;
    }
    /* shorthand: { x } */
    if (!computed && key->kind == AST_IDENTIFIER) {
        AstNode *val = make_node(p, AST_IDENTIFIER, start);
        val->ident.name = key->ident.name;
        val->end = key->end;
        prop->property.value = val;
        prop->property.shorthand = true;
        prop->end = key->end;
        return prop;
    }
    ERROR_HERE(p, "expected ':' in object property");
}

static AstNode *parse_object_expr(Parser *p) {
    const Token *start = expect(p, TK_LBRACE, "'{'");
    if (!start) return NULL;
    AstNode *n = make_node(p, AST_OBJECT_EXPR, start);
    while (peek_kind(p) != TK_RBRACE) {
        AstNode *prop = parse_property(p);
        if (!prop) return NULL;
        nl_push(p->arena, &n->object.properties, prop);
        if (peek_kind(p) != TK_RBRACE) {
            if (!expect(p, TK_COMMA, "','")) return NULL;
        }
    }
    const Token *end = expect(p, TK_RBRACE, "'}'");
    if (!end) return NULL;
    n->end = end->end;
    return n;
}

static AstNode *parse_template_literal(Parser *p) {
    const Token *first = peek_tok(p);
    if (first->kind != TK_TEMPLATE_HEAD && first->kind != TK_TEMPLATE_TAIL) {
        ERROR_HERE(p, "expected template literal");
    }
    AstNode *n = make_node(p, AST_TEMPLATE_LITERAL, first);

    while (1) {
        const Token *t = advance_tok(p);
        AstNode *q = make_node(p, AST_TEMPLATE_ELEMENT, t);
        q->template_element.cooked = t->str_val;
        q->template_element.cooked_len = t->str_len;
        q->template_element.tail = (t->kind == TK_TEMPLATE_TAIL);
        q->end = t->end;
        nl_push(p->arena, &n->template_.quasis, q);
        if (t->kind == TK_TEMPLATE_TAIL) {
            n->end = t->end;
            break;
        }
        /* head or middle: parse expression, then expect middle/tail */
        AstNode *expr = parse_expression(p);
        if (!expr) return NULL;
        nl_push(p->arena, &n->template_.expressions, expr);
        if (peek_kind(p) != TK_TEMPLATE_MIDDLE && peek_kind(p) != TK_TEMPLATE_TAIL) {
            ERROR_HERE(p, "expected template continuation");
        }
    }
    return n;
}

/* Parse parenthesized expression or arrow function parameters.
 * After the closing `)`, we peek for `=>`. If found, convert each element
 * to a binding pattern and parse the arrow body. Otherwise wrap as paren expr. */
static AstNode *parse_paren_or_arrow(Parser *p) {
    const Token *start = expect(p, TK_LPAREN, "'('");
    if (!start) return NULL;

    /* special case: () => ... */
    if (peek_kind(p) == TK_RPAREN) {
        advance_tok(p);
        if (peek_kind(p) != TK_ARROW) ERROR_HERE(p, "expected '=>' after empty parens");
        advance_tok(p);
        NodeList params = {0};
        return parse_arrow_body(p, params, start);
    }

    /* Special case: (...rest) => ... */
    if (peek_kind(p) == TK_ELLIPSIS) {
        const Token *rs = advance_tok(p);
        AstNode *rarg = parse_binding_pattern(p);
        if (!rarg) return NULL;
        if (!expect(p, TK_RPAREN, "')'")) return NULL;
        if (peek_kind(p) != TK_ARROW) ERROR_HERE(p, "expected '=>'");
        advance_tok(p);
        AstNode *rest = make_node(p, AST_REST_ELEMENT, rs);
        rest->spread_or_rest.arg = rarg;
        rest->end = rarg->end;
        NodeList params = {0};
        nl_push(p->arena, &params, rest);
        return parse_arrow_body(p, params, start);
    }

    /* Parse a sequence of assign-expressions (the parens body). */
    NodeList items = {0};
    do {
        AstNode *e = parse_assign(p);
        if (!e) return NULL;
        nl_push(p->arena, &items, e);
    } while (match_kind(p, TK_COMMA) && peek_kind(p) != TK_RPAREN);

    /* Trailing ...rest is allowed in arrow param lists. */
    AstNode *trailing_rest = NULL;
    if (peek_kind(p) == TK_ELLIPSIS) {
        const Token *rs = advance_tok(p);
        AstNode *rarg = parse_binding_pattern(p);
        if (!rarg) return NULL;
        trailing_rest = make_node(p, AST_REST_ELEMENT, rs);
        trailing_rest->spread_or_rest.arg = rarg;
        trailing_rest->end = rarg->end;
    }

    if (!expect(p, TK_RPAREN, "')'")) return NULL;

    if (peek_kind(p) == TK_ARROW) {
        advance_tok(p);
        NodeList params = {0};
        for (size_t i = 0; i < items.len; ++i) {
            AstNode *param = to_pattern(p, items.items[i]);
            if (!param) return NULL;
            nl_push(p->arena, &params, param);
        }
        if (trailing_rest) nl_push(p->arena, &params, trailing_rest);
        return parse_arrow_body(p, params, start);
    }

    if (trailing_rest) ERROR_HERE(p, "rest element only valid in arrow params");
    if (items.len == 1) {
        /* Just a parenthesized expression — return the inner directly. */
        return items.items[0];
    }
    /* (a, b, c) — sequence expression */
    AstNode *seq = make_node(p, AST_SEQUENCE_EXPR, start);
    seq->sequence.expressions = items;
    seq->end = items.items[items.len - 1]->end;
    return seq;
}

static AstNode *parse_arrow_body(Parser *p, NodeList params, const Token *start) {
    AstNode *fn = make_node(p, AST_ARROW_FN_EXPR, start);
    fn->func.is_arrow = true;
    fn->func.params = params;
    if (peek_kind(p) == TK_LBRACE) {
        AstNode *body = parse_block(p);
        if (!body) return NULL;
        fn->func.body = body;
        fn->func.body_is_expr = false;
        fn->end = body->end;
    } else {
        AstNode *body = parse_assign(p);
        if (!body) return NULL;
        fn->func.body = body;
        fn->func.body_is_expr = true;
        fn->end = body->end;
    }
    return fn;
}

static AstNode *parse_primary(Parser *p) {
    const Token *t = peek_tok(p);
    switch (t->kind) {
        case TK_KW_NULL: {
            advance_tok(p);
            AstNode *n = make_node(p, AST_LITERAL, t);
            n->literal.kind = LIT_NULL;
            n->end = t->end;
            return n;
        }
        case TK_KW_TRUE: case TK_KW_FALSE: {
            advance_tok(p);
            AstNode *n = make_node(p, AST_LITERAL, t);
            n->literal.kind = LIT_BOOL;
            n->literal.v.boolean = (t->kind == TK_KW_TRUE);
            n->end = t->end;
            return n;
        }
        case TK_NUMBER: {
            advance_tok(p);
            AstNode *n = make_node(p, AST_LITERAL, t);
            n->literal.kind = LIT_NUMBER;
            n->literal.v.number = t->number_val;
            n->end = t->end;
            return n;
        }
        case TK_STRING: {
            advance_tok(p);
            AstNode *n = make_node(p, AST_LITERAL, t);
            n->literal.kind = LIT_STRING;
            n->literal.v.str.value = t->str_val;
            n->literal.v.str.len = t->str_len;
            n->end = t->end;
            return n;
        }
        case TK_REGEX: {
            advance_tok(p);
            AstNode *n = make_node(p, AST_LITERAL, t);
            n->literal.kind = LIT_REGEX;
            n->literal.v.regex.pattern = t->str_val;
            n->literal.v.regex.flags = t->flags_val;
            n->end = t->end;
            return n;
        }
        case TK_KW_THIS: {
            advance_tok(p);
            AstNode *n = make_node(p, AST_THIS_EXPR, t);
            n->end = t->end;
            return n;
        }
        case TK_LBRACKET: return parse_array_expr(p);
        case TK_LBRACE:   return parse_object_expr(p);
        case TK_LPAREN:   return parse_paren_or_arrow(p);
        case TK_TEMPLATE_HEAD:
        case TK_TEMPLATE_TAIL: return parse_template_literal(p);
        case TK_KW_FUNCTION: return parse_function(p, false);
        case TK_KW_CLASS:    return parse_class(p, false);
        case TK_KW_NEW: {
            advance_tok(p);
            /* new.target — meta property; we don't fully model it but handle the syntax */
            if (peek_kind(p) == TK_DOT) {
                advance_tok(p);
                AstNode *meta = make_node(p, AST_IDENTIFIER, t);
                meta->ident.name = "new";
                AstNode *prop = parse_ident_name(p);
                if (!prop) return NULL;
                AstNode *mp = make_node(p, AST_META_PROPERTY, t);
                mp->meta_property.meta = meta;
                mp->meta_property.property = prop;
                mp->end = prop->end;
                return mp;
            }
            AstNode *callee = parse_lhs_expr(p);
            if (!callee) return NULL;
            AstNode *ne = make_node(p, AST_NEW_EXPR, t);
            ne->call.callee = callee;
            if (peek_kind(p) == TK_LPAREN) {
                advance_tok(p);
                while (peek_kind(p) != TK_RPAREN) {
                    AstNode *arg;
                    if (peek_kind(p) == TK_ELLIPSIS) {
                        const Token *st = advance_tok(p);
                        AstNode *a = parse_assign(p);
                        if (!a) return NULL;
                        arg = make_node(p, AST_SPREAD_ELEMENT, st);
                        arg->spread_or_rest.arg = a;
                        arg->end = a->end;
                    } else {
                        arg = parse_assign(p);
                        if (!arg) return NULL;
                    }
                    nl_push(p->arena, &ne->call.args, arg);
                    if (peek_kind(p) != TK_RPAREN) {
                        if (!expect(p, TK_COMMA, "','")) return NULL;
                    }
                }
                const Token *rp = expect(p, TK_RPAREN, "')'");
                if (!rp) return NULL;
                ne->end = rp->end;
            } else {
                ne->end = callee->end;
            }
            return ne;
        }
        case TK_KW_SUPER: {
            advance_tok(p);
            /* Treat super as an identifier-like placeholder; codegen handles it specially elsewhere. */
            AstNode *n = make_node(p, AST_IDENTIFIER, t);
            n->ident.name = "super";
            n->end = t->end;
            return n;
        }
        case TK_KW_IMPORT: {
            /* import.meta — not supported but parse it */
            advance_tok(p);
            if (peek_kind(p) == TK_DOT) {
                advance_tok(p);
                AstNode *meta = make_node(p, AST_IDENTIFIER, t);
                meta->ident.name = "import";
                AstNode *prop = parse_ident_name(p);
                if (!prop) return NULL;
                AstNode *mp = make_node(p, AST_META_PROPERTY, t);
                mp->meta_property.meta = meta;
                mp->meta_property.property = prop;
                mp->end = prop->end;
                return mp;
            }
            ERROR_AT(p, t, "import expression not supported");
        }
        case TK_IDENT:
        case TK_KW_ASYNC:
        case TK_KW_AWAIT:
        case TK_KW_YIELD:
        case TK_KW_LET: {
            /* identifier (possibly start of arrow: `x => ...`) */
            AstNode *id = make_ident_from_token(p, t);
            advance_tok(p);
            if (peek_kind(p) == TK_ARROW) {
                advance_tok(p);
                NodeList params = {0};
                nl_push(p->arena, &params, id);
                return parse_arrow_body(p, params, t);
            }
            return id;
        }
        default:
            ERROR_HERE(p, "unexpected token '%s'", tk_name(t->kind));
    }
}

/* ---------- left-hand-side (member / call / optional chain) ----------- */

static const Token *parse_call_args(Parser *p, NodeList *out) {
    if (!expect(p, TK_LPAREN, "'('")) return NULL;
    while (peek_kind(p) != TK_RPAREN) {
        AstNode *arg;
        if (peek_kind(p) == TK_ELLIPSIS) {
            const Token *st = advance_tok(p);
            AstNode *a = parse_assign(p);
            if (!a) return NULL;
            arg = make_node(p, AST_SPREAD_ELEMENT, st);
            arg->spread_or_rest.arg = a;
            arg->end = a->end;
        } else {
            arg = parse_assign(p);
            if (!arg) return NULL;
        }
        nl_push(p->arena, out, arg);
        if (peek_kind(p) != TK_RPAREN) {
            if (!expect(p, TK_COMMA, "','")) return NULL;
        }
    }
    return expect(p, TK_RPAREN, "')'");
}

/* Returns true if any optional (?.) link appeared in the chain so far. */
static AstNode *parse_lhs_expr(Parser *p) {
    AstNode *expr = parse_primary(p);
    if (!expr) return NULL;
    bool has_optional = false;
    while (1) {
        TokenKind k = peek_kind(p);
        if (k == TK_DOT) {
            advance_tok(p);
            AstNode *prop = parse_ident_name(p);
            if (!prop) return NULL;
            AstNode *m = make_node(p, AST_MEMBER_EXPR, peek_tok(p));
            m->start = expr->start;
            m->member.object = expr;
            m->member.property = prop;
            m->member.computed = false;
            m->member.optional = false;
            m->end = prop->end;
            expr = m;
        } else if (k == TK_LBRACKET) {
            advance_tok(p);
            AstNode *key = parse_expression(p);
            if (!key) return NULL;
            const Token *rb = expect(p, TK_RBRACKET, "']'");
            if (!rb) return NULL;
            AstNode *m = make_node(p, AST_MEMBER_EXPR, peek_tok(p));
            m->start = expr->start;
            m->member.object = expr;
            m->member.property = key;
            m->member.computed = true;
            m->end = rb->end;
            expr = m;
        } else if (k == TK_LPAREN) {
            NodeList args = {0};
            const Token *rp = parse_call_args(p, &args);
            if (!rp) return NULL;
            AstNode *c = make_node(p, AST_CALL_EXPR, peek_tok(p));
            c->start = expr->start;
            c->call.callee = expr;
            c->call.args = args;
            c->end = rp->end;
            expr = c;
        } else if (k == TK_QUESTION_DOT) {
            has_optional = true;
            advance_tok(p);
            if (peek_kind(p) == TK_LPAREN) {
                NodeList args = {0};
                const Token *rp = parse_call_args(p, &args);
                if (!rp) return NULL;
                AstNode *c = make_node(p, AST_CALL_EXPR, peek_tok(p));
                c->start = expr->start;
                c->call.callee = expr;
                c->call.args = args;
                c->call.optional = true;
                c->end = rp->end;
                expr = c;
            } else if (peek_kind(p) == TK_LBRACKET) {
                advance_tok(p);
                AstNode *key = parse_expression(p);
                if (!key) return NULL;
                const Token *rb = expect(p, TK_RBRACKET, "']'");
                if (!rb) return NULL;
                AstNode *m = make_node(p, AST_MEMBER_EXPR, peek_tok(p));
                m->start = expr->start;
                m->member.object = expr;
                m->member.property = key;
                m->member.computed = true;
                m->member.optional = true;
                m->end = rb->end;
                expr = m;
            } else {
                AstNode *prop = parse_ident_name(p);
                if (!prop) return NULL;
                AstNode *m = make_node(p, AST_MEMBER_EXPR, peek_tok(p));
                m->start = expr->start;
                m->member.object = expr;
                m->member.property = prop;
                m->member.computed = false;
                m->member.optional = true;
                m->end = prop->end;
                expr = m;
            }
        } else if ((k == TK_TEMPLATE_HEAD || k == TK_TEMPLATE_TAIL) &&
                   !peek_tok(p)->from_substitution) {
            /* tagged template — only when the backtick is fresh (not a `}` continuation) */
            AstNode *quasi = parse_template_literal(p);
            if (!quasi) return NULL;
            AstNode *tt = make_node(p, AST_TAGGED_TEMPLATE_EXPR, peek_tok(p));
            tt->start = expr->start;
            tt->tagged_template.tag = expr;
            tt->tagged_template.quasi = quasi;
            tt->end = quasi->end;
            expr = tt;
        } else {
            break;
        }
    }
    if (has_optional) {
        AstNode *ch = make_node(p, AST_CHAIN_EXPR, peek_tok(p));
        ch->start = expr->start;
        ch->chain.expr = expr;
        ch->end = expr->end;
        return ch;
    }
    return expr;
}

/* ---------- unary / update --------------------------------------------- */

static const char *unary_op_name(TokenKind k) {
    switch (k) {
        case TK_BANG:   return "!";
        case TK_MINUS:  return "-";
        case TK_PLUS:   return "+";
        case TK_TILDE:  return "~";
        case TK_KW_TYPEOF: return "typeof";
        case TK_KW_VOID:   return "void";
        case TK_KW_DELETE: return "delete";
        default: return NULL;
    }
}

static AstNode *parse_unary(Parser *p) {
    const Token *t = peek_tok(p);
    const char *op = unary_op_name(t->kind);
    if (op) {
        advance_tok(p);
        AstNode *arg = parse_unary(p);
        if (!arg) return NULL;
        AstNode *n = make_node(p, AST_UNARY_EXPR, t);
        n->unary.op = op;
        n->unary.arg = arg;
        n->unary.prefix = true;
        n->end = arg->end;
        return n;
    }
    if (t->kind == TK_PLUS_PLUS || t->kind == TK_MINUS_MINUS) {
        advance_tok(p);
        AstNode *arg = parse_unary(p);
        if (!arg) return NULL;
        AstNode *n = make_node(p, AST_UPDATE_EXPR, t);
        n->update.op = (t->kind == TK_PLUS_PLUS) ? "++" : "--";
        n->update.arg = arg;
        n->update.prefix = true;
        n->end = arg->end;
        return n;
    }
    if (t->kind == TK_KW_AWAIT && p->allow_await) {
        advance_tok(p);
        AstNode *arg = parse_unary(p);
        if (!arg) return NULL;
        AstNode *n = make_node(p, AST_AWAIT_EXPR, t);
        n->yield_await.arg = arg;
        n->end = arg->end;
        return n;
    }

    AstNode *expr = parse_lhs_expr(p);
    if (!expr) return NULL;
    /* postfix ++ / -- */
    if (!peek_tok(p)->had_line_break_before &&
        (peek_kind(p) == TK_PLUS_PLUS || peek_kind(p) == TK_MINUS_MINUS)) {
        const Token *op_t = advance_tok(p);
        AstNode *n = make_node(p, AST_UPDATE_EXPR, op_t);
        n->start = expr->start;
        n->update.op = (op_t->kind == TK_PLUS_PLUS) ? "++" : "--";
        n->update.arg = expr;
        n->update.prefix = false;
        n->end = op_t->end;
        return n;
    }
    return expr;
}

/* ---------- binary ops with precedence climbing ------------------------ */

typedef struct {
    int prec;            /* precedence; 0 means not a binary op */
    bool is_logical;     /* true for &&, ||, ?? */
    const char *name;
    bool right_assoc;    /* true for ** */
} BinInfo;

static BinInfo binop_info(TokenKind k, bool no_in) {
    switch (k) {
        case TK_PIPE_PIPE: return (BinInfo){3, true, "||", false};
        case TK_AMP_AMP:   return (BinInfo){4, true, "&&", false};
        case TK_NULLISH:   return (BinInfo){3, true, "??", false};
        case TK_PIPE:      return (BinInfo){5, false, "|", false};
        case TK_CARET:     return (BinInfo){6, false, "^", false};
        case TK_AMP:       return (BinInfo){7, false, "&", false};
        case TK_EQ_EQ:     return (BinInfo){8, false, "==", false};
        case TK_BANG_EQ:   return (BinInfo){8, false, "!=", false};
        case TK_EQ_EQ_EQ:  return (BinInfo){8, false, "===", false};
        case TK_BANG_EQ_EQ:return (BinInfo){8, false, "!==", false};
        case TK_LT:        return (BinInfo){9, false, "<", false};
        case TK_GT:        return (BinInfo){9, false, ">", false};
        case TK_LE:        return (BinInfo){9, false, "<=", false};
        case TK_GE:        return (BinInfo){9, false, ">=", false};
        case TK_KW_IN:     return no_in ? (BinInfo){0,0,0,0} : (BinInfo){9, false, "in", false};
        case TK_KW_INSTANCEOF: return (BinInfo){9, false, "instanceof", false};
        case TK_LSHIFT:    return (BinInfo){10, false, "<<", false};
        case TK_RSHIFT:    return (BinInfo){10, false, ">>", false};
        case TK_URSHIFT:   return (BinInfo){10, false, ">>>", false};
        case TK_PLUS:      return (BinInfo){11, false, "+", false};
        case TK_MINUS:     return (BinInfo){11, false, "-", false};
        case TK_STAR:      return (BinInfo){12, false, "*", false};
        case TK_SLASH:     return (BinInfo){12, false, "/", false};
        case TK_PERCENT:   return (BinInfo){12, false, "%", false};
        case TK_STAR_STAR: return (BinInfo){13, false, "**", true};
        default: return (BinInfo){0, false, NULL, false};
    }
}

static AstNode *parse_binary(Parser *p, int min_prec) {
    AstNode *left = parse_unary(p);
    if (!left) return NULL;
    while (1) {
        BinInfo bi = binop_info(peek_kind(p), p->no_in);
        if (!bi.prec || bi.prec < min_prec) break;
        TokenKind op_tok = peek_kind(p);
        advance_tok(p);
        int next_prec = bi.right_assoc ? bi.prec : bi.prec + 1;
        AstNode *right = parse_binary(p, next_prec);
        if (!right) return NULL;
        AstNode *bn = make_node(p, bi.is_logical ? AST_LOGICAL_EXPR : AST_BINARY_EXPR, peek_tok(p));
        bn->start = left->start;
        bn->binary.op = bi.name;
        bn->binary.left = left;
        bn->binary.right = right;
        bn->end = right->end;
        left = bn;
        (void)op_tok;
    }
    return left;
}

/* ---------- conditional / assignment ----------------------------------- */

static const char *assign_op_name(TokenKind k) {
    switch (k) {
        case TK_ASSIGN: return "=";
        case TK_PLUS_EQ: return "+=";
        case TK_MINUS_EQ: return "-=";
        case TK_STAR_EQ: return "*=";
        case TK_SLASH_EQ: return "/=";
        case TK_PERCENT_EQ: return "%=";
        case TK_STAR_STAR_EQ: return "**=";
        case TK_AMP_EQ: return "&=";
        case TK_PIPE_EQ: return "|=";
        case TK_CARET_EQ: return "^=";
        case TK_LSHIFT_EQ: return "<<=";
        case TK_RSHIFT_EQ: return ">>=";
        case TK_URSHIFT_EQ: return ">>>=";
        case TK_AMP_AMP_EQ: return "&&=";
        case TK_PIPE_PIPE_EQ: return "||=";
        case TK_NULLISH_EQ: return "?\?=";
        default: return NULL;
    }
}

static AstNode *parse_assign(Parser *p) {
    AstNode *left = parse_binary(p, 1);
    if (!left) return NULL;

    /* conditional ?: */
    if (peek_kind(p) == TK_QUESTION) {
        advance_tok(p);
        bool save = p->no_in;
        p->no_in = false;
        AstNode *cons = parse_assign(p);
        p->no_in = save;
        if (!cons) return NULL;
        if (!expect(p, TK_COLON, "':'")) return NULL;
        AstNode *alt = parse_assign(p);
        if (!alt) return NULL;
        AstNode *c = make_node(p, AST_CONDITIONAL_EXPR, peek_tok(p));
        c->start = left->start;
        c->conditional.test = left;
        c->conditional.consequent = cons;
        c->conditional.alternate = alt;
        c->end = alt->end;
        left = c;
    }

    const char *op = assign_op_name(peek_kind(p));
    if (op) {
        advance_tok(p);
        AstNode *target = (strcmp(op, "=") == 0) ? to_pattern(p, left) : left;
        if (!target) return NULL;
        AstNode *right = parse_assign(p);
        if (!right) return NULL;
        AstNode *a = make_node(p, AST_ASSIGN_EXPR, peek_tok(p));
        a->start = target->start;
        a->assign.op = op;
        a->assign.left = target;
        a->assign.right = right;
        a->end = right->end;
        return a;
    }
    return left;
}

static AstNode *parse_expression(Parser *p) {
    AstNode *first = parse_assign(p);
    if (!first) return NULL;
    if (peek_kind(p) != TK_COMMA) return first;
    AstNode *seq = make_node(p, AST_SEQUENCE_EXPR, peek_tok(p));
    seq->start = first->start;
    nl_push(p->arena, &seq->sequence.expressions, first);
    while (match_kind(p, TK_COMMA)) {
        AstNode *e = parse_assign(p);
        if (!e) return NULL;
        nl_push(p->arena, &seq->sequence.expressions, e);
        seq->end = e->end;
    }
    return seq;
}

/* ---------- pattern reinterpretation ----------------------------------- */

/* Convert an expression to a binding pattern (for arrow params, destructuring
 * assignment). On failure returns NULL with error set. */
static AstNode *to_pattern(Parser *p, AstNode *expr) {
    if (!expr) return NULL;
    switch (expr->kind) {
        case AST_IDENTIFIER:
        case AST_ARRAY_PATTERN:
        case AST_OBJECT_PATTERN:
        case AST_ASSIGN_PATTERN:
        case AST_REST_ELEMENT:
        case AST_MEMBER_EXPR:
            return expr;
        case AST_ARRAY_EXPR: {
            AstNode *patt = ast_new(p->arena, AST_ARRAY_PATTERN);
            patt->start = expr->start; patt->end = expr->end;
            patt->line = expr->line; patt->col = expr->col;
            for (size_t i = 0; i < expr->array.elements.len; ++i) {
                AstNode *e = expr->array.elements.items[i];
                if (!e) {
                    nl_push(p->arena, &patt->array_pattern.elements, NULL);
                } else if (e->kind == AST_SPREAD_ELEMENT) {
                    AstNode *r = ast_new(p->arena, AST_REST_ELEMENT);
                    r->start = e->start; r->end = e->end;
                    r->line = e->line; r->col = e->col;
                    AstNode *inner = to_pattern(p, e->spread_or_rest.arg);
                    if (!inner) return NULL;
                    r->spread_or_rest.arg = inner;
                    nl_push(p->arena, &patt->array_pattern.elements, r);
                } else {
                    AstNode *q = to_pattern(p, e);
                    if (!q) return NULL;
                    nl_push(p->arena, &patt->array_pattern.elements, q);
                }
            }
            return patt;
        }
        case AST_OBJECT_EXPR: {
            AstNode *patt = ast_new(p->arena, AST_OBJECT_PATTERN);
            patt->start = expr->start; patt->end = expr->end;
            patt->line = expr->line; patt->col = expr->col;
            for (size_t i = 0; i < expr->object.properties.len; ++i) {
                AstNode *prop = expr->object.properties.items[i];
                if (prop->kind == AST_SPREAD_ELEMENT) {
                    AstNode *r = ast_new(p->arena, AST_REST_ELEMENT);
                    r->start = prop->start; r->end = prop->end;
                    r->line = prop->line; r->col = prop->col;
                    AstNode *inner = to_pattern(p, prop->spread_or_rest.arg);
                    if (!inner) return NULL;
                    r->spread_or_rest.arg = inner;
                    nl_push(p->arena, &patt->object_pattern.properties, r);
                } else if (prop->kind == AST_PROPERTY) {
                    AstNode *q = ast_new(p->arena, AST_PROPERTY);
                    *q = *prop;
                    q->property.value = to_pattern(p, prop->property.value);
                    if (!q->property.value) return NULL;
                    nl_push(p->arena, &patt->object_pattern.properties, q);
                } else {
                    ERROR_NODE(p, expr, "invalid destructuring property");
                }
            }
            return patt;
        }
        case AST_ASSIGN_EXPR: {
            if (strcmp(expr->assign.op, "=") != 0) {
                ERROR_NODE(p, expr, "invalid destructuring default");
            }
            AstNode *ap = ast_new(p->arena, AST_ASSIGN_PATTERN);
            ap->start = expr->start; ap->end = expr->end;
            ap->line = expr->line; ap->col = expr->col;
            ap->assign_pattern.left = to_pattern(p, expr->assign.left);
            if (!ap->assign_pattern.left) return NULL;
            ap->assign_pattern.right = expr->assign.right;
            return ap;
        }
        case AST_PAREN_EXPR:
            return to_pattern(p, expr->paren.expr);
        default:
            ERROR_NODE(p, expr, "invalid assignment target");
    }
}

/* ---------- binding patterns (for var decls, params) ------------------ */

static AstNode *parse_binding_pattern(Parser *p) {
    const Token *t = peek_tok(p);
    if (t->kind == TK_LBRACKET) {
        advance_tok(p);
        AstNode *n = make_node(p, AST_ARRAY_PATTERN, t);
        while (peek_kind(p) != TK_RBRACKET) {
            if (peek_kind(p) == TK_COMMA) {
                nl_push(p->arena, &n->array_pattern.elements, NULL);
                advance_tok(p);
                continue;
            }
            if (peek_kind(p) == TK_ELLIPSIS) {
                const Token *st = advance_tok(p);
                AstNode *arg = parse_binding_pattern(p);
                if (!arg) return NULL;
                AstNode *r = make_node(p, AST_REST_ELEMENT, st);
                r->spread_or_rest.arg = arg;
                r->end = arg->end;
                nl_push(p->arena, &n->array_pattern.elements, r);
            } else {
                AstNode *elt = parse_binding_pattern(p);
                if (!elt) return NULL;
                if (match_kind(p, TK_ASSIGN)) {
                    AstNode *def = parse_assign(p);
                    if (!def) return NULL;
                    AstNode *ap = make_node(p, AST_ASSIGN_PATTERN, peek_tok(p));
                    ap->start = elt->start;
                    ap->assign_pattern.left = elt;
                    ap->assign_pattern.right = def;
                    ap->end = def->end;
                    elt = ap;
                }
                nl_push(p->arena, &n->array_pattern.elements, elt);
            }
            if (peek_kind(p) != TK_RBRACKET) {
                if (!expect(p, TK_COMMA, "','")) return NULL;
            }
        }
        const Token *rb = expect(p, TK_RBRACKET, "']'");
        if (!rb) return NULL;
        n->end = rb->end;
        return n;
    }
    if (t->kind == TK_LBRACE) {
        advance_tok(p);
        AstNode *n = make_node(p, AST_OBJECT_PATTERN, t);
        while (peek_kind(p) != TK_RBRACE) {
            if (peek_kind(p) == TK_ELLIPSIS) {
                const Token *st = advance_tok(p);
                AstNode *arg = parse_binding_pattern(p);
                if (!arg) return NULL;
                AstNode *r = make_node(p, AST_REST_ELEMENT, st);
                r->spread_or_rest.arg = arg;
                r->end = arg->end;
                nl_push(p->arena, &n->object_pattern.properties, r);
            } else {
                const Token *kstart = peek_tok(p);
                bool computed = false;
                AstNode *key = NULL;
                if (peek_kind(p) == TK_LBRACKET) {
                    advance_tok(p);
                    computed = true;
                    key = parse_assign(p);
                    if (!key) return NULL;
                    if (!expect(p, TK_RBRACKET, "']'")) return NULL;
                } else {
                    key = parse_ident_name(p);
                    if (!key) return NULL;
                }
                AstNode *prop = make_node(p, AST_PROPERTY, kstart);
                prop->property.key = key;
                prop->property.computed = computed;
                AstNode *value;
                if (match_kind(p, TK_COLON)) {
                    value = parse_binding_pattern(p);
                    if (!value) return NULL;
                    prop->property.shorthand = false;
                } else {
                    /* shorthand */
                    AstNode *id = make_node(p, AST_IDENTIFIER, kstart);
                    id->ident.name = key->ident.name;
                    id->end = key->end;
                    value = id;
                    prop->property.shorthand = true;
                }
                if (match_kind(p, TK_ASSIGN)) {
                    AstNode *def = parse_assign(p);
                    if (!def) return NULL;
                    AstNode *ap = make_node(p, AST_ASSIGN_PATTERN, kstart);
                    ap->start = value->start;
                    ap->assign_pattern.left = value;
                    ap->assign_pattern.right = def;
                    ap->end = def->end;
                    value = ap;
                }
                prop->property.value = value;
                prop->end = value->end;
                nl_push(p->arena, &n->object_pattern.properties, prop);
            }
            if (peek_kind(p) != TK_RBRACE) {
                if (!expect(p, TK_COMMA, "','")) return NULL;
            }
        }
        const Token *rb = expect(p, TK_RBRACE, "'}'");
        if (!rb) return NULL;
        n->end = rb->end;
        return n;
    }
    return parse_binding_ident(p);
}

/* ---------- statements ------------------------------------------------- */

static AstNode *parse_block(Parser *p) {
    const Token *start = expect(p, TK_LBRACE, "'{'");
    if (!start) return NULL;
    AstNode *n = make_node(p, AST_BLOCK_STMT, start);
    while (peek_kind(p) != TK_RBRACE && peek_kind(p) != TK_EOF) {
        AstNode *s = parse_statement(p);
        if (!s) return NULL;
        nl_push(p->arena, &n->block.body, s);
    }
    const Token *end = expect(p, TK_RBRACE, "'}'");
    if (!end) return NULL;
    n->end = end->end;
    return n;
}

static AstNode *parse_var_decl(Parser *p, VarKind kind, bool no_semi) {
    const Token *start = advance_tok(p); /* let/const/var */
    AstNode *n = make_node(p, AST_VAR_DECL, start);
    n->var_decl.kind = kind;
    do {
        AstNode *id = parse_binding_pattern(p);
        if (!id) return NULL;
        AstNode *init = NULL;
        if (match_kind(p, TK_ASSIGN)) {
            init = parse_assign(p);
            if (!init) return NULL;
        }
        AstNode *d = make_node(p, AST_VAR_DECLARATOR, start);
        d->start = id->start;
        d->var_declarator.id = id;
        d->var_declarator.init = init;
        d->end = init ? init->end : id->end;
        nl_push(p->arena, &n->var_decl.declarations, d);
    } while (match_kind(p, TK_COMMA));
    if (!no_semi) {
        if (!eat_semi(p)) return NULL;
    }
    n->end = n->var_decl.declarations.items[n->var_decl.declarations.len - 1]->end;
    return n;
}

static AstNode *parse_if(Parser *p) {
    const Token *start = advance_tok(p); /* if */
    if (!expect(p, TK_LPAREN, "'('")) return NULL;
    AstNode *test = parse_expression(p);
    if (!test) return NULL;
    if (!expect(p, TK_RPAREN, "')'")) return NULL;
    AstNode *cons = parse_statement(p);
    if (!cons) return NULL;
    AstNode *alt = NULL;
    if (match_kind(p, TK_KW_ELSE)) {
        alt = parse_statement(p);
        if (!alt) return NULL;
    }
    AstNode *n = make_node(p, AST_IF_STMT, start);
    n->if_.test = test;
    n->if_.consequent = cons;
    n->if_.alternate = alt;
    n->end = alt ? alt->end : cons->end;
    return n;
}

static AstNode *parse_while(Parser *p) {
    const Token *start = advance_tok(p);
    if (!expect(p, TK_LPAREN, "'('")) return NULL;
    AstNode *test = parse_expression(p);
    if (!test) return NULL;
    if (!expect(p, TK_RPAREN, "')'")) return NULL;
    AstNode *body = parse_statement(p);
    if (!body) return NULL;
    AstNode *n = make_node(p, AST_WHILE_STMT, start);
    n->while_.test = test;
    n->while_.body = body;
    n->end = body->end;
    return n;
}

static AstNode *parse_do_while(Parser *p) {
    const Token *start = advance_tok(p);
    AstNode *body = parse_statement(p);
    if (!body) return NULL;
    if (!expect(p, TK_KW_WHILE, "'while'")) return NULL;
    if (!expect(p, TK_LPAREN, "'('")) return NULL;
    AstNode *test = parse_expression(p);
    if (!test) return NULL;
    const Token *rp = expect(p, TK_RPAREN, "')'");
    if (!rp) return NULL;
    match_kind(p, TK_SEMI); /* optional */
    AstNode *n = make_node(p, AST_DO_WHILE_STMT, start);
    n->while_.test = test;
    n->while_.body = body;
    n->end = rp->end;
    return n;
}

static AstNode *parse_for(Parser *p) {
    const Token *start = advance_tok(p);
    if (!expect(p, TK_LPAREN, "'('")) return NULL;

    AstNode *init = NULL;
    /* init clause: empty / var decl / expression */
    bool save_no_in = p->no_in;
    p->no_in = true;
    if (peek_kind(p) == TK_SEMI) {
        /* empty init */
    } else if (peek_kind(p) == TK_KW_LET || peek_kind(p) == TK_KW_CONST || peek_kind(p) == TK_KW_VAR) {
        VarKind vk = (peek_kind(p) == TK_KW_LET) ? VK_LET :
                     (peek_kind(p) == TK_KW_CONST) ? VK_CONST : VK_VAR;
        init = parse_var_decl(p, vk, true /* no_semi */);
        if (!init) return NULL;
    } else {
        init = parse_expression(p);
        if (!init) return NULL;
    }
    p->no_in = save_no_in;

    /* for-in / for-of: detect by 'in'/'of' after init (when init is a single binding) */
    if (peek_kind(p) == TK_KW_IN || (peek_kind(p) == TK_IDENT &&
                                     peek_tok(p)->end - peek_tok(p)->start == 2 &&
                                     memcmp(p->src + peek_tok(p)->start, "of", 2) == 0)) {
        bool is_in = (peek_kind(p) == TK_KW_IN);
        advance_tok(p);
        AstNode *left = init;
        /* If init is a VariableDeclaration with one declarator and no init, keep as VarDecl.
         * If init is an expression, convert to pattern. */
        if (left->kind != AST_VAR_DECL) {
            left = to_pattern(p, left);
            if (!left) return NULL;
        }
        AstNode *right = parse_expression(p);
        if (!right) return NULL;
        if (!expect(p, TK_RPAREN, "')'")) return NULL;
        AstNode *body = parse_statement(p);
        if (!body) return NULL;
        AstNode *n = make_node(p, is_in ? AST_FOR_IN_STMT : AST_FOR_OF_STMT, start);
        n->for_each.left = left;
        n->for_each.right = right;
        n->for_each.body = body;
        n->end = body->end;
        return n;
    }

    if (!expect(p, TK_SEMI, "';'")) return NULL;
    AstNode *test = NULL;
    if (peek_kind(p) != TK_SEMI) {
        test = parse_expression(p);
        if (!test) return NULL;
    }
    if (!expect(p, TK_SEMI, "';'")) return NULL;
    AstNode *update = NULL;
    if (peek_kind(p) != TK_RPAREN) {
        update = parse_expression(p);
        if (!update) return NULL;
    }
    if (!expect(p, TK_RPAREN, "')'")) return NULL;
    AstNode *body = parse_statement(p);
    if (!body) return NULL;
    AstNode *n = make_node(p, AST_FOR_STMT, start);
    n->for_.init = init;
    n->for_.test = test;
    n->for_.update = update;
    n->for_.body = body;
    n->end = body->end;
    return n;
}

static AstNode *parse_switch(Parser *p) {
    const Token *start = advance_tok(p);
    if (!expect(p, TK_LPAREN, "'('")) return NULL;
    AstNode *disc = parse_expression(p);
    if (!disc) return NULL;
    if (!expect(p, TK_RPAREN, "')'")) return NULL;
    if (!expect(p, TK_LBRACE, "'{'")) return NULL;
    AstNode *n = make_node(p, AST_SWITCH_STMT, start);
    n->switch_.discriminant = disc;
    while (peek_kind(p) != TK_RBRACE) {
        const Token *cstart = peek_tok(p);
        AstNode *test = NULL;
        if (cstart->kind == TK_KW_CASE) {
            advance_tok(p);
            test = parse_expression(p);
            if (!test) return NULL;
        } else if (cstart->kind == TK_KW_DEFAULT) {
            advance_tok(p);
        } else {
            ERROR_HERE(p, "expected case or default");
        }
        if (!expect(p, TK_COLON, "':'")) return NULL;
        AstNode *cn = make_node(p, AST_SWITCH_CASE, cstart);
        cn->switch_case.test = test;
        while (peek_kind(p) != TK_KW_CASE && peek_kind(p) != TK_KW_DEFAULT && peek_kind(p) != TK_RBRACE) {
            AstNode *s = parse_statement(p);
            if (!s) return NULL;
            nl_push(p->arena, &cn->switch_case.consequent, s);
        }
        cn->end = cn->switch_case.consequent.len ? cn->switch_case.consequent.items[cn->switch_case.consequent.len - 1]->end : cstart->end;
        nl_push(p->arena, &n->switch_.cases, cn);
    }
    const Token *rb = expect(p, TK_RBRACE, "'}'");
    if (!rb) return NULL;
    n->end = rb->end;
    return n;
}

static AstNode *parse_try(Parser *p) {
    const Token *start = advance_tok(p);
    AstNode *block = parse_block(p);
    if (!block) return NULL;
    AstNode *handler = NULL, *finalizer = NULL;
    if (match_kind(p, TK_KW_CATCH)) {
        AstNode *param = NULL;
        if (match_kind(p, TK_LPAREN)) {
            param = parse_binding_pattern(p);
            if (!param) return NULL;
            if (!expect(p, TK_RPAREN, "')'")) return NULL;
        }
        AstNode *body = parse_block(p);
        if (!body) return NULL;
        handler = make_node(p, AST_CATCH_CLAUSE, peek_tok(p));
        handler->catch_.param = param;
        handler->catch_.body = body;
        handler->end = body->end;
    }
    if (match_kind(p, TK_KW_FINALLY)) {
        finalizer = parse_block(p);
        if (!finalizer) return NULL;
    }
    if (!handler && !finalizer) ERROR_HERE(p, "expected 'catch' or 'finally'");
    AstNode *n = make_node(p, AST_TRY_STMT, start);
    n->try_.block = block;
    n->try_.handler = handler;
    n->try_.finalizer = finalizer;
    n->end = finalizer ? finalizer->end : (handler ? handler->end : block->end);
    return n;
}

static AstNode *parse_return(Parser *p) {
    const Token *start = advance_tok(p);
    AstNode *n = make_node(p, AST_RETURN_STMT, start);
    n->end = start->end;
    /* No expression if semicolon, EOF, } or line break (ASI). */
    if (peek_kind(p) != TK_SEMI && peek_kind(p) != TK_RBRACE && peek_kind(p) != TK_EOF &&
        !peek_tok(p)->had_line_break_before) {
        AstNode *arg = parse_expression(p);
        if (!arg) return NULL;
        n->unary_stmt.arg = arg;
        n->end = arg->end;
    }
    if (!eat_semi(p)) return NULL;
    return n;
}

static AstNode *parse_throw(Parser *p) {
    const Token *start = advance_tok(p);
    if (peek_tok(p)->had_line_break_before) ERROR_HERE(p, "illegal newline after throw");
    AstNode *arg = parse_expression(p);
    if (!arg) return NULL;
    if (!eat_semi(p)) return NULL;
    AstNode *n = make_node(p, AST_THROW_STMT, start);
    n->unary_stmt.arg = arg;
    n->end = arg->end;
    return n;
}

static AstNode *parse_break_or_continue(Parser *p, AstKind k) {
    const Token *start = advance_tok(p);
    AstNode *label = NULL;
    if (peek_kind(p) == TK_IDENT && !peek_tok(p)->had_line_break_before) {
        label = parse_binding_ident(p);
        if (!label) return NULL;
    }
    if (!eat_semi(p)) return NULL;
    AstNode *n = make_node(p, k, start);
    n->break_continue.label = label;
    n->end = label ? label->end : start->end;
    return n;
}

/* function declaration / expression. is_decl=true requires identifier. */
static AstNode *parse_function(Parser *p, bool is_decl) {
    const Token *start = advance_tok(p); /* function */
    bool is_generator = match_kind(p, TK_STAR);
    AstNode *id = NULL;
    if (peek_kind(p) == TK_IDENT || tk_is_keyword(peek_kind(p))) {
        if (is_decl || peek_kind(p) == TK_IDENT) {
            id = parse_binding_ident(p);
            if (!id) return NULL;
        }
    } else if (is_decl) {
        ERROR_HERE(p, "expected function name");
    }
    if (!expect(p, TK_LPAREN, "'('")) return NULL;
    AstNode *fn = make_node(p, is_decl ? AST_FN_DECL : AST_FN_EXPR, start);
    fn->func.id = id;
    fn->func.is_generator = is_generator;
    while (peek_kind(p) != TK_RPAREN) {
        AstNode *param;
        if (peek_kind(p) == TK_ELLIPSIS) {
            const Token *rs = advance_tok(p);
            AstNode *arg = parse_binding_pattern(p);
            if (!arg) return NULL;
            param = make_node(p, AST_REST_ELEMENT, rs);
            param->spread_or_rest.arg = arg;
            param->end = arg->end;
        } else {
            param = parse_binding_pattern(p);
            if (!param) return NULL;
            if (match_kind(p, TK_ASSIGN)) {
                AstNode *def = parse_assign(p);
                if (!def) return NULL;
                AstNode *ap = make_node(p, AST_ASSIGN_PATTERN, peek_tok(p));
                ap->start = param->start;
                ap->assign_pattern.left = param;
                ap->assign_pattern.right = def;
                ap->end = def->end;
                param = ap;
            }
        }
        nl_push(p->arena, &fn->func.params, param);
        if (peek_kind(p) != TK_RPAREN) {
            if (!expect(p, TK_COMMA, "','")) return NULL;
        }
    }
    if (!expect(p, TK_RPAREN, "')'")) return NULL;
    AstNode *body = parse_block(p);
    if (!body) return NULL;
    fn->func.body = body;
    fn->end = body->end;
    return fn;
}

static AstNode *parse_class(Parser *p, bool is_decl) {
    const Token *start = advance_tok(p);
    AstNode *id = NULL;
    if (peek_kind(p) == TK_IDENT) {
        id = parse_binding_ident(p);
        if (!id) return NULL;
    } else if (is_decl) {
        ERROR_HERE(p, "expected class name");
    }
    AstNode *super_class = NULL;
    if (match_kind(p, TK_KW_EXTENDS)) {
        super_class = parse_lhs_expr(p);
        if (!super_class) return NULL;
    }
    if (!expect(p, TK_LBRACE, "'{'")) return NULL;
    AstNode *body = make_node(p, AST_CLASS_BODY, peek_tok(p));
    while (peek_kind(p) != TK_RBRACE) {
        if (match_kind(p, TK_SEMI)) continue;
        bool is_static = false;
        if (peek_kind(p) == TK_IDENT) {
            const Token *t = peek_tok(p);
            if (t->end - t->start == 6 && memcmp(p->src + t->start, "static", 6) == 0) {
                /* If next token is also an identifier/`(`/`[`, treat as static modifier. */
                const Token *next = peek_at(p, 1);
                if (next->kind == TK_IDENT || tk_is_keyword(next->kind) ||
                    next->kind == TK_LBRACKET || next->kind == TK_STRING || next->kind == TK_NUMBER) {
                    advance_tok(p);
                    is_static = true;
                }
            }
        }
        const Token *mstart = peek_tok(p);
        bool computed = false;
        AstNode *key;
        if (peek_kind(p) == TK_LBRACKET) {
            advance_tok(p);
            computed = true;
            key = parse_assign(p);
            if (!key) return NULL;
            if (!expect(p, TK_RBRACKET, "']'")) return NULL;
        } else if (peek_kind(p) == TK_STRING) {
            const Token *kt = advance_tok(p);
            key = make_node(p, AST_LITERAL, kt);
            key->literal.kind = LIT_STRING;
            key->literal.v.str.value = kt->str_val;
            key->literal.v.str.len = kt->str_len;
            key->end = kt->end;
        } else if (peek_kind(p) == TK_NUMBER) {
            const Token *kt = advance_tok(p);
            key = make_node(p, AST_LITERAL, kt);
            key->literal.kind = LIT_NUMBER;
            key->literal.v.number = kt->number_val;
            key->end = kt->end;
        } else {
            key = parse_ident_name(p);
            if (!key) return NULL;
        }
        /* Method body */
        if (!expect(p, TK_LPAREN, "'('")) return NULL;
        AstNode *fn = make_node(p, AST_FN_EXPR, mstart);
        while (peek_kind(p) != TK_RPAREN) {
            AstNode *param;
            if (peek_kind(p) == TK_ELLIPSIS) {
                const Token *rs = advance_tok(p);
                AstNode *arg = parse_binding_pattern(p);
                if (!arg) return NULL;
                param = make_node(p, AST_REST_ELEMENT, rs);
                param->spread_or_rest.arg = arg;
                param->end = arg->end;
            } else {
                param = parse_binding_pattern(p);
                if (!param) return NULL;
                if (match_kind(p, TK_ASSIGN)) {
                    AstNode *def = parse_assign(p);
                    if (!def) return NULL;
                    AstNode *ap = make_node(p, AST_ASSIGN_PATTERN, peek_tok(p));
                    ap->start = param->start;
                    ap->assign_pattern.left = param;
                    ap->assign_pattern.right = def;
                    ap->end = def->end;
                    param = ap;
                }
            }
            nl_push(p->arena, &fn->func.params, param);
            if (peek_kind(p) != TK_RPAREN) {
                if (!expect(p, TK_COMMA, "','")) return NULL;
            }
        }
        if (!expect(p, TK_RPAREN, "')'")) return NULL;
        AstNode *body_block = parse_block(p);
        if (!body_block) return NULL;
        fn->func.body = body_block;
        fn->end = body_block->end;

        AstNode *md = make_node(p, AST_METHOD_DEF, mstart);
        md->method_def.key = key;
        md->method_def.value = fn;
        md->method_def.computed = computed;
        md->method_def.is_static = is_static;
        md->method_def.kind = MK_METHOD;
        if (!computed && key->kind == AST_IDENTIFIER && strcmp(key->ident.name, "constructor") == 0) {
            md->method_def.kind = MK_CONSTRUCTOR;
        }
        md->end = body_block->end;
        nl_push(p->arena, &body->class_body.body, md);
    }
    const Token *end = expect(p, TK_RBRACE, "'}'");
    if (!end) return NULL;
    body->end = end->end;
    AstNode *n = make_node(p, AST_CLASS_DECL, start);
    n->class_.id = id;
    n->class_.super_class = super_class;
    n->class_.body = body;
    n->end = end->end;
    return n;
}

/* ---------- import / export ------------------------------------------- */

static AstNode *parse_import_decl(Parser *p) {
    const Token *start = advance_tok(p); /* import */
    AstNode *n = make_node(p, AST_IMPORT_DECL, start);

    if (peek_kind(p) == TK_STRING) {
        /* import "side-effect"; */
        const Token *src_t = advance_tok(p);
        n->import_decl.source = arena_strndup(p->arena, src_t->str_val, src_t->str_len);
        if (!eat_semi(p)) return NULL;
        n->end = src_t->end;
        return n;
    }

    /* default specifier */
    if (peek_kind(p) == TK_IDENT) {
        const Token *t = peek_tok(p);
        AstNode *local = parse_binding_ident(p);
        if (!local) return NULL;
        AstNode *spec = make_node(p, AST_IMPORT_DEFAULT_SPECIFIER, t);
        spec->import_specifier.local = local;
        spec->end = local->end;
        nl_push(p->arena, &n->import_decl.specifiers, spec);
        if (peek_kind(p) == TK_COMMA) advance_tok(p);
    }
    /* namespace */
    if (peek_kind(p) == TK_STAR) {
        const Token *t = advance_tok(p);
        if (!expect(p, TK_IDENT, "'as'")) return NULL; /* should be 'as' but we treat as ident */
        AstNode *local = parse_binding_ident(p);
        if (!local) return NULL;
        AstNode *spec = make_node(p, AST_IMPORT_NAMESPACE_SPECIFIER, t);
        spec->import_specifier.local = local;
        spec->end = local->end;
        nl_push(p->arena, &n->import_decl.specifiers, spec);
    } else if (peek_kind(p) == TK_LBRACE) {
        advance_tok(p);
        while (peek_kind(p) != TK_RBRACE) {
            const Token *t = peek_tok(p);
            AstNode *imported = parse_ident_name(p);
            if (!imported) return NULL;
            AstNode *local = imported;
            if (peek_kind(p) == TK_IDENT) {
                const Token *as_t = peek_tok(p);
                if (as_t->end - as_t->start == 2 && memcmp(p->src + as_t->start, "as", 2) == 0) {
                    advance_tok(p);
                    local = parse_binding_ident(p);
                    if (!local) return NULL;
                }
            }
            AstNode *spec = make_node(p, AST_IMPORT_SPECIFIER, t);
            spec->import_specifier.imported = imported;
            spec->import_specifier.local = local;
            spec->end = local->end;
            nl_push(p->arena, &n->import_decl.specifiers, spec);
            if (peek_kind(p) != TK_RBRACE) {
                if (!expect(p, TK_COMMA, "','")) return NULL;
            }
        }
        if (!expect(p, TK_RBRACE, "'}'")) return NULL;
    }
    /* from "..." */
    if (peek_kind(p) == TK_IDENT) {
        const Token *from = peek_tok(p);
        if (from->end - from->start == 4 && memcmp(p->src + from->start, "from", 4) == 0) {
            advance_tok(p);
        } else {
            ERROR_HERE(p, "expected 'from'");
        }
    } else {
        ERROR_HERE(p, "expected 'from'");
    }
    const Token *src_t = expect(p, TK_STRING, "module source string");
    if (!src_t) return NULL;
    n->import_decl.source = arena_strndup(p->arena, src_t->str_val, src_t->str_len);
    if (!eat_semi(p)) return NULL;
    n->end = src_t->end;
    return n;
}

static AstNode *parse_export_decl(Parser *p) {
    const Token *start = advance_tok(p); /* export */
    if (match_kind(p, TK_KW_DEFAULT)) {
        AstNode *decl;
        if (peek_kind(p) == TK_KW_FUNCTION) decl = parse_function(p, true);
        else if (peek_kind(p) == TK_KW_CLASS) decl = parse_class(p, true);
        else { decl = parse_assign(p); if (decl && !eat_semi(p)) return NULL; }
        if (!decl) return NULL;
        AstNode *n = make_node(p, AST_EXPORT_DEFAULT_DECL, start);
        n->export_default.declaration = decl;
        n->end = decl->end;
        return n;
    }
    if (peek_kind(p) == TK_STAR) {
        advance_tok(p);
        if (peek_kind(p) != TK_IDENT) ERROR_HERE(p, "expected 'from'");
        advance_tok(p); /* 'from' */
        const Token *src_t = expect(p, TK_STRING, "module source string");
        if (!src_t) return NULL;
        if (!eat_semi(p)) return NULL;
        AstNode *n = make_node(p, AST_EXPORT_ALL_DECL, start);
        n->export_all.source = arena_strndup(p->arena, src_t->str_val, src_t->str_len);
        n->end = src_t->end;
        return n;
    }
    if (peek_kind(p) == TK_LBRACE) {
        AstNode *n = make_node(p, AST_EXPORT_NAMED_DECL, start);
        advance_tok(p);
        while (peek_kind(p) != TK_RBRACE) {
            const Token *t = peek_tok(p);
            AstNode *local = parse_ident_name(p);
            if (!local) return NULL;
            AstNode *exported = local;
            if (peek_kind(p) == TK_IDENT) {
                const Token *as_t = peek_tok(p);
                if (as_t->end - as_t->start == 2 && memcmp(p->src + as_t->start, "as", 2) == 0) {
                    advance_tok(p);
                    exported = parse_ident_name(p);
                    if (!exported) return NULL;
                }
            }
            AstNode *spec = make_node(p, AST_EXPORT_SPECIFIER, t);
            spec->export_specifier.local = local;
            spec->export_specifier.exported = exported;
            spec->end = exported->end;
            nl_push(p->arena, &n->export_named.specifiers, spec);
            if (peek_kind(p) != TK_RBRACE) {
                if (!expect(p, TK_COMMA, "','")) return NULL;
            }
        }
        if (!expect(p, TK_RBRACE, "'}'")) return NULL;
        if (peek_kind(p) == TK_IDENT) {
            const Token *from = peek_tok(p);
            if (from->end - from->start == 4 && memcmp(p->src + from->start, "from", 4) == 0) {
                advance_tok(p);
                const Token *src_t = expect(p, TK_STRING, "module source string");
                if (!src_t) return NULL;
                n->export_named.source = arena_strndup(p->arena, src_t->str_val, src_t->str_len);
                n->end = src_t->end;
            }
        }
        if (!eat_semi(p)) return NULL;
        return n;
    }
    AstNode *decl;
    if (peek_kind(p) == TK_KW_FUNCTION) decl = parse_function(p, true);
    else if (peek_kind(p) == TK_KW_CLASS) decl = parse_class(p, true);
    else if (peek_kind(p) == TK_KW_LET) decl = parse_var_decl(p, VK_LET, false);
    else if (peek_kind(p) == TK_KW_CONST) decl = parse_var_decl(p, VK_CONST, false);
    else if (peek_kind(p) == TK_KW_VAR) decl = parse_var_decl(p, VK_VAR, false);
    else ERROR_HERE(p, "expected export declaration");
    if (!decl) return NULL;
    AstNode *n = make_node(p, AST_EXPORT_NAMED_DECL, start);
    n->export_named.declaration = decl;
    n->end = decl->end;
    return n;
}

static AstNode *parse_statement(Parser *p) {
    TokenKind k = peek_kind(p);
    switch (k) {
        case TK_LBRACE: return parse_block(p);
        case TK_KW_LET:    return parse_var_decl(p, VK_LET, false);
        case TK_KW_CONST:  return parse_var_decl(p, VK_CONST, false);
        case TK_KW_VAR:    return parse_var_decl(p, VK_VAR, false);
        case TK_KW_IF:     return parse_if(p);
        case TK_KW_WHILE:  return parse_while(p);
        case TK_KW_DO:     return parse_do_while(p);
        case TK_KW_FOR:    return parse_for(p);
        case TK_KW_SWITCH: return parse_switch(p);
        case TK_KW_TRY:    return parse_try(p);
        case TK_KW_RETURN: return parse_return(p);
        case TK_KW_THROW:  return parse_throw(p);
        case TK_KW_BREAK:  return parse_break_or_continue(p, AST_BREAK_STMT);
        case TK_KW_CONTINUE: return parse_break_or_continue(p, AST_CONTINUE_STMT);
        case TK_KW_FUNCTION: return parse_function(p, true);
        case TK_KW_CLASS:    return parse_class(p, true);
        case TK_KW_IMPORT:   return parse_import_decl(p);
        case TK_KW_EXPORT:   return parse_export_decl(p);
        case TK_SEMI: {
            const Token *t = advance_tok(p);
            AstNode *n = make_node(p, AST_EMPTY_STMT, t);
            n->end = t->end;
            return n;
        }
        case TK_KW_DEBUGGER: {
            const Token *t = advance_tok(p);
            if (!eat_semi(p)) return NULL;
            AstNode *n = make_node(p, AST_DEBUGGER_STMT, t);
            n->end = t->end;
            return n;
        }
        case TK_C_BLOCK: {
            const Token *t = advance_tok(p);
            AstNode *n = make_node(p, AST_C_BLOCK, t);
            n->c_block.code = t->str_val;
            n->c_block.len = t->str_len;
            n->end = t->end;
            return n;
        }
        default: {
            /* labeled statement: IDENT ':' stmt */
            if (k == TK_IDENT && peek_at(p, 1)->kind == TK_COLON) {
                const Token *label_t = peek_tok(p);
                AstNode *label = parse_binding_ident(p);
                if (!label) return NULL;
                advance_tok(p); /* ':' */
                AstNode *body = parse_statement(p);
                if (!body) return NULL;
                AstNode *n = make_node(p, AST_LABELED_STMT, label_t);
                n->labeled.label = label;
                n->labeled.body = body;
                n->end = body->end;
                return n;
            }
            const Token *t = peek_tok(p);
            AstNode *expr = parse_expression(p);
            if (!expr) return NULL;
            if (!eat_semi(p)) return NULL;
            AstNode *n = make_node(p, AST_EXPR_STMT, t);
            n->expr_stmt.expr = expr;
            n->end = expr->end;
            return n;
        }
    }
}

/* ---------- top-level -------------------------------------------------- */

static bool tokenize_all(Parser *p) {
    Token tok;
    while (lex_next(&p->lexer, &tok)) {
        tb_push(&p->toks, &tok);
        if (tok.kind == TK_EOF) return true;
    }
    /* lexer failed */
    set_error_at(p, &(Token){.line = p->lexer.err.line, .col = p->lexer.err.col},
                 "%s", p->lexer.err.message);
    return false;
}

AstNode *parse_source(const char *src, size_t src_len, const char *filename,
                      Arena *arena, ParseError *out_err) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.src = src;
    p.src_len = src_len;
    p.filename = filename;
    p.arena = arena;
    lex_init(&p.lexer, src, src_len, filename, arena);

    if (!tokenize_all(&p)) goto fail;
    if (p.toks.count == 0) {
        if (out_err) { memset(out_err, 0, sizeof *out_err); strcpy(out_err->message, "empty input"); out_err->present = true; }
        free(p.toks.tokens);
        lex_free(&p.lexer);
        return NULL;
    }

    AstNode *prog = make_node(&p, AST_PROGRAM, &p.toks.tokens[0]);
    while (peek_kind(&p) != TK_EOF) {
        AstNode *s = parse_statement(&p);
        if (!s) goto fail;
        nl_push(p.arena, &prog->block.body, s);
    }
    prog->end = p.toks.tokens[p.toks.count - 1].end;

    free(p.toks.tokens);
    lex_free(&p.lexer);
    return prog;

fail:
    if (out_err) *out_err = p.err;
    free(p.toks.tokens);
    lex_free(&p.lexer);
    return NULL;
}
