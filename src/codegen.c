#include "codegen.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"

/* ---------- error / helpers ------------------------------------------- */

static void cg_error(Codegen *c, AstNode *n, const char *fmt, ...) {
    if (c->err.present) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err.message, sizeof c->err.message, fmt, ap);
    va_end(ap);
    c->err.line = n ? n->line : 0;
    c->err.col = n ? n->col : 0;
    c->err.present = true;
}

/* Arena-allocated safe_name: avoids the static buffer race in builtins.c. */
static const char *safe_name_a(Codegen *c, const char *name) {
    if (!is_c_keyword(name)) return name;
    size_t n = strlen(name);
    char *out = (char *)arena_alloc(c->arena, n + 5);
    memcpy(out, "_js_", 4);
    memcpy(out + 4, name, n);
    out[n + 4] = '\0';
    return out;
}

static void ctx_push(Codegen *c, CtxFrame f) {
    if (c->ctx_len == c->ctx_cap) {
        size_t nc = c->ctx_cap ? c->ctx_cap * 2 : 8;
        CtxFrame *p = (CtxFrame *)realloc(c->ctx_stack, nc * sizeof(CtxFrame));
        if (!p) abort();
        c->ctx_stack = p;
        c->ctx_cap = nc;
    }
    c->ctx_stack[c->ctx_len++] = f;
}
static void ctx_pop(Codegen *c) { if (c->ctx_len) c->ctx_len--; }

/* ---------- string escaping ------------------------------------------- */

/* Returns a malloc'd C-string literal body (no surrounding quotes) for inclusion
 * inside double-quoted C string literals. Caller frees. */
static char *escape_str_for_c(Codegen *c, const char *s, size_t n) {
    Buffer b; buf_init(&b);
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '\\': buf_append(&b, "\\\\", 2); break;
            case '"':  buf_append(&b, "\\\"", 2); break;
            case '\n': buf_append(&b, "\\n", 2); break;
            case '\r': buf_append(&b, "\\r", 2); break;
            case '\t': buf_append(&b, "\\t", 2); break;
            case '\0': buf_append(&b, "\\0", 2); break;
            default:
                if (ch < 0x20) buf_appendf(&b, "\\x%02x", ch);
                else buf_append_char(&b, (char)ch);
                break;
        }
    }
    char *out = buf_take(&b);
    /* copy to arena so callers don't have to free */
    char *aout = arena_strdup(c->arena, out);
    free(out);
    return aout;
}

/* ---------- forward decls --------------------------------------------- */

static char *gen_expr(Codegen *c, AstNode *n);
static void gen_stmt(Codegen *c, AstNode *n, Buffer *out);
static char *gen_assign_target(Codegen *c, AstNode *target, const char *value_code);
static char *gen_call(Codegen *c, AstNode *n);
static char *gen_call_target(Codegen *c, AstNode *node, const char *args_code, const char *count_code);
static char *gen_lambda(Codegen *c, AstNode *n);
static void gen_param_list(Codegen *c, NodeList *params, Buffer *out);
static void gen_destructure_param(Codegen *c, AstNode *pattern, int idx, Buffer *out);
static void gen_var_decl(Codegen *c, AstNode *decl, bool is_top_level, Buffer *out);
static void gen_nested_destructure(Codegen *c, AstNode *pattern, const char *src, bool is_top_level, Buffer *out);
static void gen_class_decl(Codegen *c, AstNode *n, Buffer *out);
static void gen_fn_decl(Codegen *c, AstNode *n, Buffer *out);
static void collect_function_names(Codegen *c, AstNode *prog);

/* For nested destructure inside expressions (catch param destructure too): */

/* sprintf into arena */
static char *afmt(Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *out = (char *)arena_alloc(a, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

/* Returns a name string suitable for the property name in object_get/set.
 * For Identifier, the identifier's name. For Literal, its string repr. */
static const char *prop_key_name(Codegen *c, AstNode *key) {
    if (!key) return "";
    if (key->kind == AST_IDENTIFIER) return key->ident.name;
    if (key->kind == AST_LITERAL) {
        if (key->literal.kind == LIT_STRING) return key->literal.v.str.value;
        if (key->literal.kind == LIT_NUMBER) {
            return afmt(c->arena, "%g", key->literal.v.number);
        }
    }
    return "";
}

/* ---------- expression codegen ---------------------------------------- */

static char *gen_literal(Codegen *c, AstNode *n) {
    switch (n->literal.kind) {
        case LIT_NULL: return arena_strdup(c->arena, "js_null()");
        case LIT_BOOL: return afmt(c->arena, "js_bool(%d)", n->literal.v.boolean ? 1 : 0);
        case LIT_STRING: {
            char *esc = escape_str_for_c(c, n->literal.v.str.value, n->literal.v.str.len);
            return afmt(c->arena, "js_string(\"%s\")", esc);
        }
        case LIT_NUMBER: {
            double v = n->literal.v.number;
            if (isnan(v)) return arena_strdup(c->arena, "js_number(NAN)");
            if (isinf(v)) return arena_strdup(c->arena, v < 0 ? "js_number(-INFINITY)" : "js_number(INFINITY)");
            /* preserve -0 */
            if (v == 0.0 && signbit(v)) return arena_strdup(c->arena, "js_number(-0.0)");
            return afmt(c->arena, "js_number(%.17g)", v);
        }
        case LIT_REGEX: {
            char *p = escape_str_for_c(c, n->literal.v.regex.pattern, strlen(n->literal.v.regex.pattern));
            char *f = escape_str_for_c(c, n->literal.v.regex.flags ? n->literal.v.regex.flags : "", strlen(n->literal.v.regex.flags ? n->literal.v.regex.flags : ""));
            return afmt(c->arena, "js_regex_new(\"%s\", \"%s\")", p, f);
        }
    }
    return arena_strdup(c->arena, "js_undefined()");
}

static char *args_expr(Codegen *c, NodeList *args, const char **out_count) {
    if (args->len == 0) {
        *out_count = "0";
        return arena_strdup(c->arena, "NULL");
    }
    Buffer b; buf_init(&b);
    buf_append_str(&b, "(JsValue*[]){ ");
    for (size_t i = 0; i < args->len; ++i) {
        if (i) buf_append_str(&b, ", ");
        char *e = gen_expr(c, args->items[i]);
        if (!e) { buf_free(&b); return NULL; }
        buf_append_str(&b, e);
    }
    buf_append_str(&b, " }");
    char *out = buf_take(&b);
    char *aout = arena_strdup(c->arena, out);
    free(out);
    *out_count = afmt(c->arena, "%zu", args->len);
    return aout;
}

static char *gen_array_expr(Codegen *c, AstNode *n) {
    if (n->array.elements.len == 0) return arena_strdup(c->arena, "js_array_new()");
    int id = c->temp_counter++;
    Buffer b; buf_init(&b);
    buf_appendf(&b, "({ JsValue* _arr_%d = js_array_new(); ", id);
    for (size_t i = 0; i < n->array.elements.len; ++i) {
        AstNode *el = n->array.elements.items[i];
        if (!el) {
            buf_appendf(&b, "js_array_push(_arr_%d, js_undefined()); ", id);
        } else if (el->kind == AST_SPREAD_ELEMENT) {
            int s = c->temp_counter++;
            char *arg = gen_expr(c, el->spread_or_rest.arg);
            if (!arg) { buf_free(&b); return NULL; }
            buf_appendf(&b, "{ JsValue* _spr_%d = %s; if (_spr_%d->type == JS_ARRAY) for (int _i = 0; _i < _spr_%d->array.length; _i++) js_array_push(_arr_%d, _spr_%d->array.items[_i]); else js_array_push(_arr_%d, _spr_%d); } ",
                       s, arg, s, s, id, s, id, s);
        } else {
            char *e = gen_expr(c, el);
            if (!e) { buf_free(&b); return NULL; }
            buf_appendf(&b, "js_array_push(_arr_%d, %s); ", id, e);
        }
    }
    buf_appendf(&b, "_arr_%d; })", id);
    char *out = buf_take(&b);
    char *r = arena_strdup(c->arena, out);
    free(out);
    return r;
}

static char *gen_object_expr(Codegen *c, AstNode *n) {
    if (n->object.properties.len == 0) return arena_strdup(c->arena, "js_object_new()");
    int id = c->temp_counter++;
    Buffer b; buf_init(&b);
    buf_appendf(&b, "({ JsValue* _obj_%d = js_object_new(); ", id);
    for (size_t i = 0; i < n->object.properties.len; ++i) {
        AstNode *p = n->object.properties.items[i];
        if (p->kind == AST_SPREAD_ELEMENT) {
            int s = c->temp_counter++;
            char *arg = gen_expr(c, p->spread_or_rest.arg);
            if (!arg) { buf_free(&b); return NULL; }
            buf_appendf(&b, "{ JsValue* _spr_%d = %s; if (_spr_%d->type == JS_OBJECT) for (int _i = 0; _i < _spr_%d->object.length; _i++) js_object_set(_obj_%d, _spr_%d->object.keys[_i], _spr_%d->object.values[_i]); } ",
                       s, arg, s, s, id, s, s);
            continue;
        }
        if (p->property.computed) {
            int k = c->temp_counter++;
            char *key = gen_expr(c, p->property.key);
            char *val = gen_expr(c, p->property.value);
            if (!key || !val) { buf_free(&b); return NULL; }
            buf_appendf(&b, "{ char* _k_%d = js_to_string(%s); js_object_set(_obj_%d, _k_%d, %s); free(_k_%d); } ",
                       k, key, id, k, val, k);
            continue;
        }
        const char *kname = prop_key_name(c, p->property.key);
        char *esc = escape_str_for_c(c, kname, strlen(kname));
        char *val = gen_expr(c, p->property.value);
        if (!val) { buf_free(&b); return NULL; }
        buf_appendf(&b, "js_object_set(_obj_%d, \"%s\", %s); ", id, esc, val);
    }
    buf_appendf(&b, "_obj_%d; })", id);
    char *out = buf_take(&b);
    char *r = arena_strdup(c->arena, out);
    free(out);
    return r;
}

static char *gen_template_literal(Codegen *c, AstNode *n) {
    NodeList *quasis = &n->template_.quasis;
    NodeList *exprs = &n->template_.expressions;
    if (quasis->len == 0) return arena_strdup(c->arena, "js_string(\"\")");
    AstNode *first = quasis->items[0];
    char *esc = escape_str_for_c(c, first->template_element.cooked, first->template_element.cooked_len);
    char *result = afmt(c->arena, "js_string(\"%s\")", esc);
    for (size_t i = 0; i < exprs->len; ++i) {
        char *e = gen_expr(c, exprs->items[i]);
        if (!e) return NULL;
        result = afmt(c->arena, "js_add(%s, %s)", result, e);
        AstNode *next = quasis->items[i + 1];
        if (next && next->template_element.cooked_len > 0) {
            char *nesc = escape_str_for_c(c, next->template_element.cooked, next->template_element.cooked_len);
            result = afmt(c->arena, "js_add(%s, js_string(\"%s\"))", result, nesc);
        }
    }
    return result;
}

static char *gen_member_expr(Codegen *c, AstNode *n) {
    char *obj = gen_expr(c, n->member.object);
    if (!obj) return NULL;
    if (n->member.computed) {
        char *key = gen_expr(c, n->member.property);
        if (!key) return NULL;
        if (n->member.optional) {
            return afmt(c->arena,
                "({ JsValue* _oc = %s; (_oc->type == JS_UNDEFINED || _oc->type == JS_NULL) ? js_undefined() : js_member_get(_oc, %s); })",
                obj, key);
        }
        return afmt(c->arena, "js_member_get(%s, %s)", obj, key);
    }
    const char *pname = n->member.property->ident.name;
    char *esc = escape_str_for_c(c, pname, strlen(pname));
    if (n->member.optional) {
        return afmt(c->arena,
            "({ JsValue* _oc = %s; (_oc->type == JS_UNDEFINED || _oc->type == JS_NULL) ? js_undefined() : js_object_get(_oc, \"%s\"); })",
            obj, esc);
    }
    return afmt(c->arena, "js_object_get(%s, \"%s\")", obj, esc);
}

static char *gen_unary(Codegen *c, AstNode *n) {
    char *arg = gen_expr(c, n->unary.arg);
    if (!arg) return NULL;
    const char *op = n->unary.op;
    if (strcmp(op, "!") == 0)        return afmt(c->arena, "js_not(%s)", arg);
    if (strcmp(op, "-") == 0)        return afmt(c->arena, "js_neg(%s)", arg);
    if (strcmp(op, "+") == 0)        return afmt(c->arena, "js_pos(%s)", arg);
    if (strcmp(op, "~") == 0)        return afmt(c->arena, "js_bitnot(%s)", arg);
    if (strcmp(op, "typeof") == 0)   return afmt(c->arena, "js_typeof(%s)", arg);
    if (strcmp(op, "void") == 0)     return afmt(c->arena, "(%s, js_undefined())", arg);
    if (strcmp(op, "delete") == 0) {
        AstNode *a = n->unary.arg;
        if (a->kind == AST_MEMBER_EXPR && !a->member.computed) {
            char *obj = gen_expr(c, a->member.object);
            if (!obj) return NULL;
            const char *pname = a->member.property->ident.name;
            return afmt(c->arena, "js_bool(js_delete_prop(%s, \"%s\"))", obj, pname);
        }
        return afmt(c->arena, "(%s, js_bool(1))", arg);
    }
    cg_error(c, n, "unknown unary op: %s", op);
    return NULL;
}

static char *gen_update(Codegen *c, AstNode *n) {
    char *arg = gen_expr(c, n->update.arg);
    if (!arg) return NULL;
    const char *delta = (strcmp(n->update.op, "++") == 0) ? "1" : "-1";
    AstNode *a = n->update.arg;
    if (a->kind == AST_IDENTIFIER) {
        if (n->update.prefix) {
            return afmt(c->arena, "(%s = js_number(js_to_number(%s) + %s))", arg, arg, delta);
        }
        return afmt(c->arena, "({ JsValue* _old = %s; %s = js_number(js_to_number(%s) + %s); _old; })",
                    arg, arg, arg, delta);
    }
    if (a->kind == AST_MEMBER_EXPR) {
        char *obj = gen_expr(c, a->member.object);
        if (!obj) return NULL;
        int t = c->temp_counter++;
        char *key;
        if (a->member.computed) {
            key = gen_expr(c, a->member.property);
            if (!key) return NULL;
        } else {
            const char *pname = a->member.property->ident.name;
            key = afmt(c->arena, "js_string(\"%s\")", pname);
        }
        if (n->update.prefix) {
            return afmt(c->arena,
                "({ JsValue* _upd_%d = js_number(js_to_number(js_member_get(%s, %s)) + %s); js_member_set(%s, %s, _upd_%d); _upd_%d; })",
                t, obj, key, delta, obj, key, t, t);
        }
        return afmt(c->arena,
            "({ JsValue* _upd_%d = js_member_get(%s, %s); js_member_set(%s, %s, js_number(js_to_number(_upd_%d) + %s)); _upd_%d; })",
            t, obj, key, obj, key, t, delta, t);
    }
    cg_error(c, n, "unsupported update target");
    return NULL;
}

static char *gen_assign_target(Codegen *c, AstNode *target, const char *value_code) {
    if (target->kind == AST_IDENTIFIER) {
        return afmt(c->arena, "(%s = %s)", safe_name_a(c, target->ident.name), value_code);
    }
    if (target->kind == AST_MEMBER_EXPR) {
        char *obj = gen_expr(c, target->member.object);
        if (!obj) return NULL;
        int t = c->temp_counter++;
        if (target->member.computed) {
            char *key = gen_expr(c, target->member.property);
            if (!key) return NULL;
            return afmt(c->arena,
                "({ JsValue* _asgn_%d = %s; js_member_set(%s, %s, _asgn_%d); _asgn_%d; })",
                t, value_code, obj, key, t, t);
        }
        const char *pname = target->member.property->ident.name;
        return afmt(c->arena,
            "({ JsValue* _asgn_%d = %s; js_object_set(%s, \"%s\", _asgn_%d); _asgn_%d; })",
            t, value_code, obj, pname, t, t);
    }
    cg_error(c, target, "unsupported assignment target: %s", ast_kind_name(target->kind));
    return NULL;
}

static char *gen_assign_expr(Codegen *c, AstNode *n) {
    char *right = gen_expr(c, n->assign.right);
    if (!right) return NULL;
    const char *op = n->assign.op;
    if (strcmp(op, "=") == 0) return gen_assign_target(c, n->assign.left, right);

    char *left = gen_expr(c, n->assign.left);
    if (!left) return NULL;
    if (strcmp(op, "||=") == 0)
        return gen_assign_target(c, n->assign.left, afmt(c->arena, "JS_OR(%s, %s)", left, right));
    if (strcmp(op, "&&=") == 0)
        return gen_assign_target(c, n->assign.left, afmt(c->arena, "JS_AND(%s, %s)", left, right));
    if (strcmp(op, "?\?=") == 0)
        return gen_assign_target(c, n->assign.left, afmt(c->arena, "JS_NULLISH(%s, %s)", left, right));

    const char *op_fn = compound_op(op);
    if (!op_fn) { cg_error(c, n, "unknown assignment op: %s", op); return NULL; }
    return gen_assign_target(c, n->assign.left, afmt(c->arena, "%s(%s, %s)", op_fn, left, right));
}

static char *gen_call_target(Codegen *c, AstNode *node, const char *args_code, const char *count_code) {
    AstNode *callee = node->call.callee;

    /* method call: obj.method(args) */
    if (callee->kind == AST_MEMBER_EXPR && !callee->member.computed) {
        char *obj = gen_expr(c, callee->member.object);
        if (!obj) return NULL;
        const char *method = callee->member.property->ident.name;
        if (callee->member.optional || node->call.optional) {
            return afmt(c->arena,
                "({ JsValue* _co = %s; (_co->type == JS_UNDEFINED || _co->type == JS_NULL) ? js_undefined() : js_method_call(_co, \"%s\", %s, %s); })",
                obj, method, args_code, count_code);
        }
        return afmt(c->arena, "js_method_call(%s, \"%s\", %s, %s)", obj, method, args_code, count_code);
    }

    /* computed method call: obj[expr](args) */
    if (callee->kind == AST_MEMBER_EXPR && callee->member.computed) {
        char *obj = gen_expr(c, callee->member.object);
        char *key = gen_expr(c, callee->member.property);
        if (!obj || !key) return NULL;
        int t = c->temp_counter++;
        return afmt(c->arena,
            "({ char* _cm_%d = js_to_string(%s); JsValue* _r = js_method_call(%s, _cm_%d, %s, %s); free(_cm_%d); _r; })",
            t, key, obj, t, args_code, count_code, t);
    }

    /* direct call: name(args) */
    if (callee->kind == AST_IDENTIFIER) {
        const char *name = callee->ident.name;
        const char *bfn = builtin_function(name);
        if (bfn) return afmt(c->arena, "%s(%s, %s)", bfn, args_code, count_code);
        const char *sn = safe_name_a(c, name);
        if (map_has(&c->known_functions, sn)) {
            return afmt(c->arena, "%s(%s, %s)", sn, args_code, count_code);
        }
        const char *gv = global_var(name);
        if (gv) return afmt(c->arena, "%s->function(%s, %s)", gv, args_code, count_code);
        return afmt(c->arena, "%s->function(%s, %s)", sn, args_code, count_code);
    }

    /* generic */
    char *cal = gen_expr(c, callee);
    if (!cal) return NULL;
    return afmt(c->arena, "(%s)->function(%s, %s)", cal, args_code, count_code);
}

static char *gen_call(Codegen *c, AstNode *n) {
    bool has_spread = false;
    for (size_t i = 0; i < n->call.args.len; ++i) {
        if (n->call.args.items[i]->kind == AST_SPREAD_ELEMENT) { has_spread = true; break; }
    }
    if (has_spread) {
        int id = c->temp_counter++;
        Buffer b; buf_init(&b);
        buf_appendf(&b, "({ JsValue* _sa_%d = js_array_new(); ", id);
        for (size_t i = 0; i < n->call.args.len; ++i) {
            AstNode *a = n->call.args.items[i];
            if (a->kind == AST_SPREAD_ELEMENT) {
                int s = c->temp_counter++;
                char *arg = gen_expr(c, a->spread_or_rest.arg);
                if (!arg) { buf_free(&b); return NULL; }
                buf_appendf(&b,
                    "{ JsValue* _ss_%d = %s; if (_ss_%d->type == JS_ARRAY) for (int _i = 0; _i < _ss_%d->array.length; _i++) js_array_push(_sa_%d, _ss_%d->array.items[_i]); else js_array_push(_sa_%d, _ss_%d); } ",
                    s, arg, s, s, id, s, id, s);
            } else {
                char *e = gen_expr(c, a);
                if (!e) { buf_free(&b); return NULL; }
                buf_appendf(&b, "js_array_push(_sa_%d, %s); ", id, e);
            }
        }
        char *args_code = afmt(c->arena, "_sa_%d->array.items", id);
        char *count_code = afmt(c->arena, "_sa_%d->array.length", id);
        char *call = gen_call_target(c, n, args_code, count_code);
        if (!call) { buf_free(&b); return NULL; }
        buf_appendf(&b, "%s; })", call);
        char *out = buf_take(&b);
        char *r = arena_strdup(c->arena, out);
        free(out);
        return r;
    }
    const char *count;
    char *args_code = args_expr(c, &n->call.args, &count);
    if (!args_code) return NULL;
    return gen_call_target(c, n, args_code, count);
}

static char *gen_lambda(Codegen *c, AstNode *n) {
    int idx = c->lambda_counter++;
    char *name = afmt(c->arena, "_lambda_%d", idx);

    Buffer params; buf_init(&params);
    gen_param_list(c, &n->func.params, &params);

    Buffer body; buf_init(&body);
    if (!n->func.body_is_expr && n->func.body && n->func.body->kind == AST_BLOCK_STMT) {
        for (size_t i = 0; i < n->func.body->block.body.len; ++i) {
            gen_stmt(c, n->func.body->block.body.items[i], &body);
        }
        buf_append_str(&body, "return js_undefined();\n");
    } else {
        char *e = gen_expr(c, n->func.body);
        if (!e) { buf_free(&params); buf_free(&body); return NULL; }
        buf_appendf(&body, "return %s;\n", e);
    }

    buf_appendf(&c->lambdas, "JsValue* %s(JsValue** _a, int _ac) {\n", name);
    buf_append_str(&c->lambdas, params.data ? params.data : "");
    buf_append_char(&c->lambdas, '\n');
    buf_append_str(&c->lambdas, body.data ? body.data : "");
    buf_append_str(&c->lambdas, "}\n");
    buf_free(&params);
    buf_free(&body);
    return afmt(c->arena, "js_function(%s)", name);
}

static char *gen_expr(Codegen *c, AstNode *n) {
    if (c->err.present) return NULL;
    if (!n) return arena_strdup(c->arena, "js_undefined()");
    switch (n->kind) {
        case AST_LITERAL: return gen_literal(c, n);
        case AST_IDENTIFIER: {
            const char *gv = global_var(n->ident.name);
            if (gv) return arena_strdup(c->arena, gv);
            const char *sn = safe_name_a(c, n->ident.name);
            if (map_has(&c->known_functions, sn)) return afmt(c->arena, "js_function(%s)", sn);
            return arena_strdup(c->arena, sn);
        }
        case AST_THIS_EXPR: return arena_strdup(c->arena, "_this");
        case AST_BINARY_EXPR: {
            const char *fn = binary_op(n->binary.op);
            if (!fn) { cg_error(c, n, "unknown binary op: %s", n->binary.op); return NULL; }
            char *l = gen_expr(c, n->binary.left);
            char *r = gen_expr(c, n->binary.right);
            if (!l || !r) return NULL;
            return afmt(c->arena, "%s(%s, %s)", fn, l, r);
        }
        case AST_LOGICAL_EXPR: {
            char *l = gen_expr(c, n->binary.left);
            char *r = gen_expr(c, n->binary.right);
            if (!l || !r) return NULL;
            if (strcmp(n->binary.op, "||") == 0) return afmt(c->arena, "JS_OR(%s, %s)", l, r);
            if (strcmp(n->binary.op, "&&") == 0) return afmt(c->arena, "JS_AND(%s, %s)", l, r);
            if (strcmp(n->binary.op, "?\?") == 0) return afmt(c->arena, "JS_NULLISH(%s, %s)", l, r);
            cg_error(c, n, "unknown logical op: %s", n->binary.op);
            return NULL;
        }
        case AST_UNARY_EXPR:  return gen_unary(c, n);
        case AST_UPDATE_EXPR: return gen_update(c, n);
        case AST_ASSIGN_EXPR: return gen_assign_expr(c, n);
        case AST_MEMBER_EXPR: return gen_member_expr(c, n);
        case AST_CALL_EXPR:   return gen_call(c, n);
        case AST_NEW_EXPR:    return gen_call(c, n);
        case AST_CONDITIONAL_EXPR: {
            char *t = gen_expr(c, n->conditional.test);
            char *cs = gen_expr(c, n->conditional.consequent);
            char *al = gen_expr(c, n->conditional.alternate);
            if (!t || !cs || !al) return NULL;
            return afmt(c->arena, "(js_truthy(%s) ? %s : %s)", t, cs, al);
        }
        case AST_ARRAY_EXPR:  return gen_array_expr(c, n);
        case AST_OBJECT_EXPR: return gen_object_expr(c, n);
        case AST_ARROW_FN_EXPR:
        case AST_FN_EXPR:     return gen_lambda(c, n);
        case AST_TEMPLATE_LITERAL: return gen_template_literal(c, n);
        case AST_TAGGED_TEMPLATE_EXPR: {
            /* Desugar tag`a${x}b` to tag(["a","b"], x). */
            AstNode *quasi = n->tagged_template.quasi;
            AstNode *call = ast_new(c->arena, AST_CALL_EXPR);
            call->line = n->line; call->col = n->col;
            call->start = n->start; call->end = n->end;
            call->call.callee = n->tagged_template.tag;
            AstNode *arr = ast_new(c->arena, AST_ARRAY_EXPR);
            arr->line = n->line; arr->col = n->col;
            for (size_t i = 0; i < quasi->template_.quasis.len; ++i) {
                AstNode *q = quasi->template_.quasis.items[i];
                AstNode *lit = ast_new(c->arena, AST_LITERAL);
                lit->literal.kind = LIT_STRING;
                lit->literal.v.str.value = q->template_element.cooked;
                lit->literal.v.str.len = q->template_element.cooked_len;
                lit->line = q->line; lit->col = q->col;
                nl_push(c->arena, &arr->array.elements, lit);
            }
            nl_push(c->arena, &call->call.args, arr);
            for (size_t i = 0; i < quasi->template_.expressions.len; ++i) {
                nl_push(c->arena, &call->call.args, quasi->template_.expressions.items[i]);
            }
            return gen_call(c, call);
        }
        case AST_SEQUENCE_EXPR: {
            Buffer b; buf_init(&b);
            buf_append_char(&b, '(');
            for (size_t i = 0; i < n->sequence.expressions.len; ++i) {
                if (i) buf_append_str(&b, ", ");
                char *e = gen_expr(c, n->sequence.expressions.items[i]);
                if (!e) { buf_free(&b); return NULL; }
                buf_append_str(&b, e);
            }
            buf_append_char(&b, ')');
            char *out = buf_take(&b);
            char *r = arena_strdup(c->arena, out);
            free(out);
            return r;
        }
        case AST_CHAIN_EXPR:  return gen_expr(c, n->chain.expr);
        case AST_SPREAD_ELEMENT: return gen_expr(c, n->spread_or_rest.arg);
        case AST_YIELD_EXPR:  return arena_strdup(c->arena, "js_undefined()");
        case AST_AWAIT_EXPR:  return gen_expr(c, n->yield_await.arg);
        case AST_META_PROPERTY: return arena_strdup(c->arena, "js_undefined()");
        case AST_PAREN_EXPR:  return gen_expr(c, n->paren.expr);
        default:
            cg_error(c, n, "unsupported expression: %s", ast_kind_name(n->kind));
            return NULL;
    }
}

/* ---------- parameter list -------------------------------------------- */

static void gen_param_list(Codegen *c, NodeList *params, Buffer *out) {
    for (size_t i = 0; i < params->len; ++i) {
        AstNode *p = params->items[i];
        if (i) buf_append_char(out, '\n');
        if (p->kind == AST_IDENTIFIER) {
            buf_appendf(out, "JsValue* %s = _ac > %zu ? _a[%zu] : js_undefined();",
                        safe_name_a(c, p->ident.name), i, i);
        } else if (p->kind == AST_ASSIGN_PATTERN) {
            const char *pname = (p->assign_pattern.left->kind == AST_IDENTIFIER)
                ? safe_name_a(c, p->assign_pattern.left->ident.name)
                : afmt(c->arena, "_p%zu", i);
            char *def = gen_expr(c, p->assign_pattern.right);
            buf_appendf(out, "JsValue* %s = (_ac > %zu && _a[%zu]->type != JS_UNDEFINED) ? _a[%zu] : %s;",
                        pname, i, i, i, def ? def : "js_undefined()");
        } else if (p->kind == AST_REST_ELEMENT) {
            const char *rname = safe_name_a(c, p->spread_or_rest.arg->ident.name);
            buf_appendf(out, "JsValue* %s = js_array_new(); for (int _ri = %zu; _ri < _ac; _ri++) js_array_push(%s, _a[_ri]);",
                        rname, i, rname);
        } else if (p->kind == AST_OBJECT_PATTERN || p->kind == AST_ARRAY_PATTERN) {
            gen_destructure_param(c, p, (int)i, out);
        } else {
            buf_appendf(out, "/* unsupported param: %s */", ast_kind_name(p->kind));
        }
    }
}

static void gen_destructure_param(Codegen *c, AstNode *pattern, int idx, Buffer *out) {
    int tmp = c->temp_counter++;
    buf_appendf(out, "JsValue* _dp_%d = _ac > %d ? _a[%d] : js_undefined();\n", tmp, idx, idx);
    if (pattern->kind == AST_OBJECT_PATTERN) {
        for (size_t i = 0; i < pattern->object_pattern.properties.len; ++i) {
            AstNode *prop = pattern->object_pattern.properties.items[i];
            if (prop->kind == AST_REST_ELEMENT) {
                buf_appendf(out, "JsValue* %s = js_object_new();\n", prop->spread_or_rest.arg->ident.name);
                continue;
            }
            const char *key = prop_key_name(c, prop->property.key);
            AstNode *val = prop->property.value;
            if (val->kind == AST_IDENTIFIER) {
                buf_appendf(out, "JsValue* %s = js_object_get(_dp_%d, \"%s\");\n", val->ident.name, tmp, key);
            } else if (val->kind == AST_ASSIGN_PATTERN) {
                char *def = gen_expr(c, val->assign_pattern.right);
                buf_appendf(out,
                    "JsValue* %s = ({ JsValue* _v = js_object_get(_dp_%d, \"%s\"); _v->type == JS_UNDEFINED ? %s : _v; });\n",
                    val->assign_pattern.left->ident.name, tmp, key, def ? def : "js_undefined()");
            }
        }
    } else if (pattern->kind == AST_ARRAY_PATTERN) {
        for (size_t i = 0; i < pattern->array_pattern.elements.len; ++i) {
            AstNode *el = pattern->array_pattern.elements.items[i];
            if (!el) continue;
            if (el->kind == AST_IDENTIFIER) {
                buf_appendf(out, "JsValue* %s = js_member_get(_dp_%d, js_number(%zu));\n",
                            safe_name_a(c, el->ident.name), tmp, i);
            } else if (el->kind == AST_REST_ELEMENT) {
                const char *rn = el->spread_or_rest.arg->ident.name;
                buf_appendf(out,
                    "JsValue* %s = js_array_new(); for (int _i = %zu; _i < (_dp_%d->type == JS_ARRAY ? _dp_%d->array.length : 0); _i++) js_array_push(%s, _dp_%d->array.items[_i]);\n",
                    rn, i, tmp, tmp, rn, tmp);
            } else if (el->kind == AST_ASSIGN_PATTERN) {
                char *def = gen_expr(c, el->assign_pattern.right);
                buf_appendf(out,
                    "JsValue* %s = ({ JsValue* _v = js_member_get(_dp_%d, js_number(%zu)); _v->type == JS_UNDEFINED ? %s : _v; });\n",
                    el->assign_pattern.left->ident.name, tmp, i, def ? def : "js_undefined()");
            }
        }
    }
}

/* ---------- variable declaration / destructure ----------------------- */

static void emit_binding_init(Codegen *c, const char *name, const char *value_expr, bool is_top_level, Buffer *out) {
    const char *sn = safe_name_a(c, name);
    if (is_top_level) {
        map_put(&c->globals, arena_strdup(c->arena, sn), (void *)1);
        buf_appendf(out, "%s = %s;\n", sn, value_expr);
    } else {
        buf_appendf(out, "JsValue* %s = %s;\n", sn, value_expr);
    }
}

static void gen_nested_destructure(Codegen *c, AstNode *pattern, const char *src, bool is_top_level, Buffer *out) {
    if (pattern->kind == AST_OBJECT_PATTERN) {
        for (size_t i = 0; i < pattern->object_pattern.properties.len; ++i) {
            AstNode *prop = pattern->object_pattern.properties.items[i];
            if (prop->kind == AST_REST_ELEMENT) {
                emit_binding_init(c, prop->spread_or_rest.arg->ident.name, "js_object_new()", is_top_level, out);
                continue;
            }
            const char *key = prop_key_name(c, prop->property.key);
            AstNode *val = prop->property.value;
            if (val->kind == AST_IDENTIFIER) {
                emit_binding_init(c, val->ident.name,
                    afmt(c->arena, "js_object_get(%s, \"%s\")", src, key), is_top_level, out);
            } else if (val->kind == AST_ASSIGN_PATTERN) {
                char *def = gen_expr(c, val->assign_pattern.right);
                emit_binding_init(c, val->assign_pattern.left->ident.name,
                    afmt(c->arena,
                        "({ JsValue* _v = js_object_get(%s, \"%s\"); _v->type == JS_UNDEFINED ? %s : _v; })",
                        src, key, def ? def : "js_undefined()"),
                    is_top_level, out);
            }
        }
    } else if (pattern->kind == AST_ARRAY_PATTERN) {
        for (size_t i = 0; i < pattern->array_pattern.elements.len; ++i) {
            AstNode *el = pattern->array_pattern.elements.items[i];
            if (!el) continue;
            if (el->kind == AST_IDENTIFIER) {
                emit_binding_init(c, el->ident.name,
                    afmt(c->arena, "js_member_get(%s, js_number(%zu))", src, i), is_top_level, out);
            } else if (el->kind == AST_ASSIGN_PATTERN) {
                char *def = gen_expr(c, el->assign_pattern.right);
                emit_binding_init(c, el->assign_pattern.left->ident.name,
                    afmt(c->arena,
                        "({ JsValue* _v = js_member_get(%s, js_number(%zu)); _v->type == JS_UNDEFINED ? %s : _v; })",
                        src, i, def ? def : "js_undefined()"),
                    is_top_level, out);
            }
        }
    }
}

static void gen_var_declarator(Codegen *c, AstNode *decl, bool is_top_level, Buffer *out) {
    AstNode *id = decl->var_declarator.id;
    AstNode *init = decl->var_declarator.init;

    if (id->kind == AST_IDENTIFIER) {
        const char *val = init ? gen_expr(c, init) : "js_undefined()";
        if (!val) return;
        emit_binding_init(c, id->ident.name, val, is_top_level, out);
        return;
    }
    if (id->kind == AST_ARRAY_PATTERN) {
        int tmp = c->temp_counter++;
        const char *init_code = init ? gen_expr(c, init) : "js_undefined()";
        if (!init_code) return;
        buf_appendf(out, "JsValue* _d_%d = %s;\n", tmp, init_code);
        for (size_t i = 0; i < id->array_pattern.elements.len; ++i) {
            AstNode *el = id->array_pattern.elements.items[i];
            if (!el) continue;
            if (el->kind == AST_IDENTIFIER) {
                emit_binding_init(c, el->ident.name,
                    afmt(c->arena, "js_member_get(_d_%d, js_number(%zu))", tmp, i), is_top_level, out);
            } else if (el->kind == AST_REST_ELEMENT) {
                const char *rn = el->spread_or_rest.arg->ident.name;
                emit_binding_init(c, rn, "js_array_new()", is_top_level, out);
                buf_appendf(out,
                    "for (int _i = %zu; _i < (_d_%d->type == JS_ARRAY ? _d_%d->array.length : 0); _i++) js_array_push(%s, _d_%d->array.items[_i]);\n",
                    i, tmp, tmp, safe_name_a(c, rn), tmp);
            } else if (el->kind == AST_ASSIGN_PATTERN) {
                char *def = gen_expr(c, el->assign_pattern.right);
                emit_binding_init(c, el->assign_pattern.left->ident.name,
                    afmt(c->arena,
                        "({ JsValue* _v = js_member_get(_d_%d, js_number(%zu)); _v->type == JS_UNDEFINED ? %s : _v; })",
                        tmp, i, def ? def : "js_undefined()"),
                    is_top_level, out);
            }
        }
        return;
    }
    if (id->kind == AST_OBJECT_PATTERN) {
        int tmp = c->temp_counter++;
        const char *init_code = init ? gen_expr(c, init) : "js_undefined()";
        if (!init_code) return;
        buf_appendf(out, "JsValue* _d_%d = %s;\n", tmp, init_code);
        for (size_t i = 0; i < id->object_pattern.properties.len; ++i) {
            AstNode *prop = id->object_pattern.properties.items[i];
            if (prop->kind == AST_REST_ELEMENT) {
                const char *rn = prop->spread_or_rest.arg->ident.name;
                emit_binding_init(c, rn, "js_object_new()", is_top_level, out);
                buf_appendf(out, "if (_d_%d->type == JS_OBJECT) { for (int _i = 0; _i < _d_%d->object.length; _i++) { int _skip = 0; ", tmp, tmp);
                for (size_t j = 0; j < id->object_pattern.properties.len; ++j) {
                    AstNode *other = id->object_pattern.properties.items[j];
                    if (other == prop) continue;
                    const char *k = prop_key_name(c, other->property.key);
                    buf_appendf(out, "if (strcmp(_d_%d->object.keys[_i], \"%s\") == 0) _skip = 1; ", tmp, k);
                }
                buf_appendf(out, "if (!_skip) js_object_set(%s, _d_%d->object.keys[_i], _d_%d->object.values[_i]); } }\n",
                            safe_name_a(c, rn), tmp, tmp);
                continue;
            }
            const char *key = prop_key_name(c, prop->property.key);
            AstNode *val = prop->property.value;
            if (val->kind == AST_IDENTIFIER) {
                emit_binding_init(c, val->ident.name,
                    afmt(c->arena, "js_object_get(_d_%d, \"%s\")", tmp, key), is_top_level, out);
            } else if (val->kind == AST_ASSIGN_PATTERN) {
                char *def = gen_expr(c, val->assign_pattern.right);
                emit_binding_init(c, val->assign_pattern.left->ident.name,
                    afmt(c->arena,
                        "({ JsValue* _v = js_object_get(_d_%d, \"%s\"); _v->type == JS_UNDEFINED ? %s : _v; })",
                        tmp, key, def ? def : "js_undefined()"),
                    is_top_level, out);
            } else if (val->kind == AST_OBJECT_PATTERN || val->kind == AST_ARRAY_PATTERN) {
                int nested = c->temp_counter++;
                buf_appendf(out, "JsValue* _d_%d = js_object_get(_d_%d, \"%s\");\n", nested, tmp, key);
                gen_nested_destructure(c, val, afmt(c->arena, "_d_%d", nested), is_top_level, out);
            }
        }
        return;
    }
    cg_error(c, decl, "unsupported variable pattern: %s", ast_kind_name(id->kind));
}

static void gen_var_decl(Codegen *c, AstNode *decl, bool is_top_level, Buffer *out) {
    for (size_t i = 0; i < decl->var_decl.declarations.len; ++i) {
        gen_var_declarator(c, decl->var_decl.declarations.items[i], is_top_level, out);
    }
}

/* ---------- statement codegen ---------------------------------------- */

static void gen_block(Codegen *c, AstNode *node, Buffer *out) {
    buf_append_str(out, "{\n");
    if (node->kind == AST_BLOCK_STMT) {
        for (size_t i = 0; i < node->block.body.len; ++i) {
            gen_stmt(c, node->block.body.items[i], out);
        }
    } else {
        gen_stmt(c, node, out);
    }
    buf_append_str(out, "}\n");
}

static void gen_block_inner(Codegen *c, AstNode *node, Buffer *out) {
    if (node->kind == AST_BLOCK_STMT) {
        for (size_t i = 0; i < node->block.body.len; ++i) {
            gen_stmt(c, node->block.body.items[i], out);
        }
    } else {
        gen_stmt(c, node, out);
    }
}

/* Emit a runtime-line update before the statement so uncaught throws can
 * surface a QS-source line. We always emit (cheap one-int assignment) — no
 * cross-buffer dedupe because gen_stmt is called across main_body / fns /
 * lambdas in non-linear order. Filename changes are emitted alongside. */
static void emit_loc_update(Codegen *c, AstNode *n, Buffer *out) {
    if (!n || n->line == 0) return;
    if (n->filename) {
        char *esc = escape_str_for_c(c, n->filename, strlen(n->filename));
        buf_appendf(out, "_qsc_cur_file = \"%s\"; _qsc_cur_line = %u;\n",
                    esc, n->line);
    } else {
        buf_appendf(out, "_qsc_cur_line = %u;\n", n->line);
    }
}

static void gen_stmt(Codegen *c, AstNode *n, Buffer *out) {
    if (c->err.present || !n) return;
    emit_loc_update(c, n, out);
    switch (n->kind) {
        case AST_BLOCK_STMT: gen_block(c, n, out); return;
        case AST_VAR_DECL:   gen_var_decl(c, n, false, out); return;
        case AST_EXPR_STMT: {
            char *e = gen_expr(c, n->expr_stmt.expr);
            if (!e) return;
            buf_appendf(out, "%s;\n", e);
            return;
        }
        case AST_IF_STMT: {
            char *t = gen_expr(c, n->if_.test);
            if (!t) return;
            buf_appendf(out, "if (js_truthy(%s)) ", t);
            gen_block(c, n->if_.consequent, out);
            if (n->if_.alternate) {
                if (n->if_.alternate->kind == AST_IF_STMT) {
                    buf_append_str(out, "else ");
                    gen_stmt(c, n->if_.alternate, out);
                } else {
                    buf_append_str(out, "else ");
                    gen_block(c, n->if_.alternate, out);
                }
            }
            return;
        }
        case AST_WHILE_STMT: {
            ctx_push(c, (CtxFrame){.kind = CTX_WHILE});
            char *t = gen_expr(c, n->while_.test);
            if (!t) { ctx_pop(c); return; }
            buf_appendf(out, "while (js_truthy(%s)) ", t);
            gen_block(c, n->while_.body, out);
            ctx_pop(c);
            return;
        }
        case AST_DO_WHILE_STMT: {
            ctx_push(c, (CtxFrame){.kind = CTX_DOWHILE});
            buf_append_str(out, "do ");
            gen_block(c, n->while_.body, out);
            char *t = gen_expr(c, n->while_.test);
            if (!t) { ctx_pop(c); return; }
            buf_appendf(out, "while (js_truthy(%s));\n", t);
            ctx_pop(c);
            return;
        }
        case AST_FOR_STMT: {
            int lbl = c->label_counter++;
            char *cont_label = afmt(c->arena, "_for_%d_cont", lbl);
            ctx_push(c, (CtxFrame){.kind = CTX_FOR, .continue_label = cont_label});
            buf_append_str(out, "{\n");
            if (n->for_.init) {
                if (n->for_.init->kind == AST_VAR_DECL) {
                    gen_var_decl(c, n->for_.init, false, out);
                } else {
                    char *e = gen_expr(c, n->for_.init);
                    if (e) buf_appendf(out, "%s;\n", e);
                }
            }
            const char *test = "1";
            char *tcode = NULL;
            if (n->for_.test) {
                tcode = gen_expr(c, n->for_.test);
                if (tcode) test = afmt(c->arena, "js_truthy(%s)", tcode);
            }
            buf_appendf(out, "while (%s) {\n", test);
            gen_block_inner(c, n->for_.body, out);
            buf_appendf(out, "%s:;\n", cont_label);
            if (n->for_.update) {
                char *u = gen_expr(c, n->for_.update);
                if (u) buf_appendf(out, "%s;\n", u);
            }
            buf_append_str(out, "}\n}\n");
            ctx_pop(c);
            return;
        }
        case AST_FOR_IN_STMT: {
            int it = c->temp_counter++;
            int ix = c->temp_counter++;
            int buf_id = c->temp_counter++;
            ctx_push(c, (CtxFrame){.kind = CTX_WHILE});
            AstNode *left = n->for_each.left;
            const char *var_name;
            bool declare;
            if (left->kind == AST_VAR_DECL) {
                var_name = safe_name_a(c, left->var_decl.declarations.items[0]->var_declarator.id->ident.name);
                declare = true;
            } else {
                var_name = safe_name_a(c, left->ident.name);
                declare = false;
            }
            char *r = gen_expr(c, n->for_each.right);
            if (!r) { ctx_pop(c); return; }
            buf_append_str(out, "{\n");
            buf_appendf(out, "JsValue* _fi_%d = %s;\n", it, r);
            buf_appendf(out, "if (_fi_%d->type == JS_OBJECT) {\nfor (int _fii_%d = 0; _fii_%d < _fi_%d->object.length; _fii_%d++) {\n",
                        it, ix, ix, it, ix);
            if (declare) buf_appendf(out, "JsValue* %s = js_string(_fi_%d->object.keys[_fii_%d]);\n", var_name, it, ix);
            else         buf_appendf(out, "%s = js_string(_fi_%d->object.keys[_fii_%d]);\n", var_name, it, ix);
            gen_block_inner(c, n->for_each.body, out);
            buf_append_str(out, "}\n}\n");
            buf_appendf(out, "if (_fi_%d->type == JS_ARRAY) {\nfor (int _fii_%d = 0; _fii_%d < _fi_%d->array.length; _fii_%d++) {\n",
                        it, ix, ix, it, ix);
            buf_appendf(out, "char _buf_%d[20]; snprintf(_buf_%d, 20, \"%%d\", _fii_%d);\n", buf_id, buf_id, ix);
            if (declare) buf_appendf(out, "JsValue* %s = js_string(_buf_%d);\n", var_name, buf_id);
            else         buf_appendf(out, "%s = js_string(_buf_%d);\n", var_name, buf_id);
            gen_block_inner(c, n->for_each.body, out);
            buf_append_str(out, "}\n}\n}\n");
            ctx_pop(c);
            return;
        }
        case AST_FOR_OF_STMT: {
            int it = c->temp_counter++;
            int ix = c->temp_counter++;
            int ch = c->temp_counter++;
            ctx_push(c, (CtxFrame){.kind = CTX_WHILE});
            AstNode *left = n->for_each.left;
            const char *var_name;
            bool declare = false;
            bool destruct = false;
            AstNode *dpat = NULL;
            if (left->kind == AST_VAR_DECL) {
                AstNode *decl_id = left->var_decl.declarations.items[0]->var_declarator.id;
                if (decl_id->kind == AST_IDENTIFIER) {
                    var_name = safe_name_a(c, decl_id->ident.name);
                    declare = true;
                } else {
                    destruct = true;
                    dpat = decl_id;
                    var_name = afmt(c->arena, "_fov_%d", c->temp_counter++);
                    declare = true;
                }
            } else {
                var_name = safe_name_a(c, left->ident.name);
            }
            char *r = gen_expr(c, n->for_each.right);
            if (!r) { ctx_pop(c); return; }
            buf_append_str(out, "{\n");
            buf_appendf(out, "JsValue* _fo_%d = %s;\n", it, r);
            buf_appendf(out, "if (_fo_%d->type == JS_ARRAY) {\nfor (int _foi_%d = 0; _foi_%d < _fo_%d->array.length; _foi_%d++) {\n",
                        it, ix, ix, it, ix);
            if (declare) buf_appendf(out, "JsValue* %s = _fo_%d->array.items[_foi_%d];\n", var_name, it, ix);
            else         buf_appendf(out, "%s = _fo_%d->array.items[_foi_%d];\n", var_name, it, ix);
            if (destruct) gen_nested_destructure(c, dpat, var_name, false, out);
            gen_block_inner(c, n->for_each.body, out);
            buf_append_str(out, "}\n}\n");
            buf_appendf(out, "else if (_fo_%d->type == JS_STRING) {\nfor (int _foi_%d = 0; _fo_%d->string[_foi_%d]; _foi_%d++) {\n",
                        it, ix, it, ix, ix);
            buf_appendf(out, "char _ch_%d[2] = { _fo_%d->string[_foi_%d], '\\0' };\n", ch, it, ix);
            if (declare) buf_appendf(out, "JsValue* %s = js_string(_ch_%d);\n", var_name, ch);
            else         buf_appendf(out, "%s = js_string(_ch_%d);\n", var_name, ch);
            gen_block_inner(c, n->for_each.body, out);
            buf_append_str(out, "}\n}\n}\n");
            ctx_pop(c);
            return;
        }
        case AST_SWITCH_STMT: {
            int lbl = c->label_counter++;
            char *break_label = afmt(c->arena, "_sw_%d_end", lbl);
            ctx_push(c, (CtxFrame){.kind = CTX_SWITCH, .break_label = break_label});
            char *disc = gen_expr(c, n->switch_.discriminant);
            if (!disc) { ctx_pop(c); return; }
            buf_append_str(out, "{\n");
            buf_appendf(out, "JsValue* _disc_%d = %s;\n", lbl, disc);
            buf_appendf(out, "int _matched_%d = 0;\n", lbl);
            char *default_code = NULL;
            for (size_t i = 0; i < n->switch_.cases.len; ++i) {
                AstNode *cs = n->switch_.cases.items[i];
                if (cs->switch_case.test) {
                    char *t = gen_expr(c, cs->switch_case.test);
                    if (!t) { ctx_pop(c); return; }
                    buf_appendf(out, "if (!_matched_%d && js_truthy(js_strict_eq(_disc_%d, %s))) _matched_%d = 1;\n",
                                lbl, lbl, t, lbl);
                    buf_appendf(out, "if (_matched_%d) {\n", lbl);
                    for (size_t j = 0; j < cs->switch_case.consequent.len; ++j) {
                        gen_stmt(c, cs->switch_case.consequent.items[j], out);
                    }
                    buf_append_str(out, "}\n");
                } else {
                    Buffer db; buf_init(&db);
                    for (size_t j = 0; j < cs->switch_case.consequent.len; ++j) {
                        gen_stmt(c, cs->switch_case.consequent.items[j], &db);
                    }
                    char *dout = buf_take(&db);
                    default_code = arena_strdup(c->arena, dout);
                    free(dout);
                    buf_free(&db);
                }
            }
            if (default_code) {
                buf_appendf(out, "if (!_matched_%d) {\n%s}\n", lbl, default_code);
            }
            buf_appendf(out, "%s:;\n}\n", break_label);
            ctx_pop(c);
            return;
        }
        case AST_BREAK_STMT: {
            for (ssize_t i = (ssize_t)c->ctx_len - 1; i >= 0; --i) {
                CtxFrame f = c->ctx_stack[i];
                if (f.kind == CTX_SWITCH) { buf_appendf(out, "goto %s;\n", f.break_label); return; }
                if (f.kind == CTX_FOR || f.kind == CTX_WHILE || f.kind == CTX_DOWHILE) {
                    buf_append_str(out, "break;\n"); return;
                }
            }
            buf_append_str(out, "break;\n");
            return;
        }
        case AST_CONTINUE_STMT: {
            for (ssize_t i = (ssize_t)c->ctx_len - 1; i >= 0; --i) {
                CtxFrame f = c->ctx_stack[i];
                if (f.kind == CTX_FOR) { buf_appendf(out, "goto %s;\n", f.continue_label); return; }
                if (f.kind == CTX_WHILE || f.kind == CTX_DOWHILE) {
                    buf_append_str(out, "continue;\n"); return;
                }
            }
            buf_append_str(out, "continue;\n");
            return;
        }
        case AST_RETURN_STMT: {
            if (!n->unary_stmt.arg) { buf_append_str(out, "return js_undefined();\n"); return; }
            char *a = gen_expr(c, n->unary_stmt.arg);
            if (!a) return;
            buf_appendf(out, "return %s;\n", a);
            return;
        }
        case AST_THROW_STMT: {
            char *a = gen_expr(c, n->unary_stmt.arg);
            if (!a) return;
            buf_appendf(out, "js_throw(%s);\n", a);
            return;
        }
        case AST_TRY_STMT: {
            int id = c->label_counter++;
            buf_append_str(out, "{\n");
            buf_appendf(out, "int _try_%d_caught = 0;\n", id);
            buf_appendf(out, "JsValue* _try_%d_exc = js_undefined();\n", id);
            buf_append_str(out, "js_try_depth++;\n");
            buf_append_str(out, "if (setjmp(js_try_stack[js_try_depth - 1]) == 0) {\n");
            gen_block_inner(c, n->try_.block, out);
            buf_append_str(out, "js_try_depth--;\n} else {\n");
            buf_appendf(out, "_try_%d_caught = 1;\n", id);
            buf_appendf(out, "_try_%d_exc = js_exception_stack[js_try_depth];\n", id);
            buf_append_str(out, "}\n");
            if (n->try_.handler) {
                AstNode *h = n->try_.handler;
                const char *pname;
                if (h->catch_.param && h->catch_.param->kind == AST_IDENTIFIER) {
                    pname = h->catch_.param->ident.name;
                } else {
                    pname = afmt(c->arena, "_catch_%d", c->temp_counter++);
                }
                buf_appendf(out, "if (_try_%d_caught) {\n", id);
                buf_appendf(out, "JsValue* %s = _try_%d_exc;\n", pname, id);
                if (h->catch_.param && h->catch_.param->kind != AST_IDENTIFIER) {
                    gen_nested_destructure(c, h->catch_.param, pname, false, out);
                }
                gen_block_inner(c, h->catch_.body, out);
                buf_append_str(out, "}\n");
            }
            if (n->try_.finalizer) gen_block_inner(c, n->try_.finalizer, out);
            buf_append_str(out, "}\n");
            return;
        }
        case AST_FN_DECL:    /* emitted at top level */ return;
        case AST_EMPTY_STMT: return;
        case AST_DEBUGGER_STMT: buf_append_str(out, "/* debugger */\n"); return;
        case AST_LABELED_STMT: {
            buf_appendf(out, "%s: ", n->labeled.label->ident.name);
            gen_stmt(c, n->labeled.body, out);
            return;
        }
        case AST_CLASS_DECL: gen_class_decl(c, n, out); return;
        case AST_C_BLOCK: {
            buf_append_str(out, "/* __c block */ {\n");
            if (n->c_block.code) buf_append(out, n->c_block.code, n->c_block.len);
            buf_append_str(out, "\n}\n");
            return;
        }
        default:
            cg_error(c, n, "unsupported statement: %s", ast_kind_name(n->kind));
            return;
    }
}

/* ---------- class declaration ---------------------------------------- */

static void gen_class_decl(Codegen *c, AstNode *n, Buffer *out) {
    (void)out;
    const char *class_name = n->class_.id ? n->class_.id->ident.name
                                          : afmt(c->arena, "_class_%d", c->temp_counter++);

    /* Find constructor + collect methods */
    AstNode *ctor = NULL;
    AstNode **methods = NULL;
    size_t methods_len = 0, methods_cap = 0;
    NodeList *body = &n->class_.body->class_body.body;
    for (size_t i = 0; i < body->len; ++i) {
        AstNode *m = body->items[i];
        if (m->kind != AST_METHOD_DEF) continue;
        if (m->method_def.kind == MK_CONSTRUCTOR) { ctor = m; continue; }
        if (m->method_def.is_static) continue;
        if (methods_len == methods_cap) {
            methods_cap = methods_cap ? methods_cap * 2 : 4;
            methods = (AstNode **)realloc(methods, methods_cap * sizeof(AstNode *));
        }
        methods[methods_len++] = m;
    }

    /* Emit method functions */
    for (size_t i = 0; i < methods_len; ++i) {
        AstNode *m = methods[i];
        const char *mname = prop_key_name(c, m->method_def.key);
        char *fnname = afmt(c->arena, "_method_%s_%s", class_name, mname);
        buf_appendf(&c->lambdas, "JsValue* %s(JsValue** _a, int _ac) {\n", fnname);
        AstNode *fn = m->method_def.value;
        for (size_t j = 0; j < fn->func.params.len; ++j) {
            AstNode *p = fn->func.params.items[j];
            if (p->kind == AST_IDENTIFIER) {
                buf_appendf(&c->lambdas, "JsValue* %s = _ac > %zu ? _a[%zu] : js_undefined();\n",
                            p->ident.name, j, j);
            }
        }
        for (size_t j = 0; j < fn->func.body->block.body.len; ++j) {
            gen_stmt(c, fn->func.body->block.body.items[j], &c->lambdas);
        }
        buf_append_str(&c->lambdas, "return js_undefined();\n}\n");
    }

    /* Emit constructor or factory */
    if (ctor) {
        char *ctor_name = afmt(c->arena, "_ctor_%s", class_name);
        NodeList *params = &ctor->method_def.value->func.params;
        buf_appendf(&c->lambdas, "JsValue* %s(JsValue** _a, int _ac) {\n", ctor_name);
        buf_append_str(&c->lambdas, "JsValue* _this = _ac > 0 ? _a[0] : js_object_new();\n");
        for (size_t j = 0; j < params->len; ++j) {
            AstNode *p = params->items[j];
            if (p->kind == AST_IDENTIFIER) {
                buf_appendf(&c->lambdas, "JsValue* %s = _ac > %zu ? _a[%zu] : js_undefined();\n",
                            p->ident.name, j + 1, j + 1);
            } else if (p->kind == AST_ASSIGN_PATTERN) {
                char *def = gen_expr(c, p->assign_pattern.right);
                buf_appendf(&c->lambdas,
                    "JsValue* %s = (_ac > %zu && _a[%zu]->type != JS_UNDEFINED) ? _a[%zu] : %s;\n",
                    p->assign_pattern.left->ident.name, j + 1, j + 1, j + 1,
                    def ? def : "js_undefined()");
            }
        }
        for (size_t j = 0; j < ctor->method_def.value->func.body->block.body.len; ++j) {
            gen_stmt(c, ctor->method_def.value->func.body->block.body.items[j], &c->lambdas);
        }
        buf_append_str(&c->lambdas, "return _this;\n}\n");

        /* Factory wrapper */
        buf_appendf(&c->lambdas, "JsValue* %s(JsValue** _a, int _ac) {\n", class_name);
        buf_append_str(&c->lambdas, "JsValue* _this = js_object_new();\n");
        for (size_t i = 0; i < methods_len; ++i) {
            const char *mname = prop_key_name(c, methods[i]->method_def.key);
            buf_appendf(&c->lambdas, "js_object_set(_this, \"%s\", js_function(_method_%s_%s));\n",
                        mname, class_name, mname);
        }
        Buffer args; buf_init(&args);
        buf_append_str(&args, "_this");
        for (size_t j = 0; j < params->len; ++j) {
            buf_appendf(&args, ", _ac > %zu ? _a[%zu] : js_undefined()", j, j);
        }
        char *args_s = buf_take(&args);
        buf_appendf(&c->lambdas, "JsValue* _ctor_args[] = { %s };\n", args_s);
        buf_appendf(&c->lambdas, "%s(_ctor_args, %zu);\n", ctor_name, params->len + 1);
        buf_append_str(&c->lambdas, "return _this;\n}\n");
        free(args_s);
    } else {
        buf_appendf(&c->lambdas, "JsValue* %s(JsValue** _a, int _ac) {\n", class_name);
        (void)0; /* _a/_ac unused — kept for signature uniformity */
        buf_append_str(&c->lambdas, "(void)_a; (void)_ac;\n");
        buf_append_str(&c->lambdas, "JsValue* _this = js_object_new();\n");
        for (size_t i = 0; i < methods_len; ++i) {
            const char *mname = prop_key_name(c, methods[i]->method_def.key);
            buf_appendf(&c->lambdas, "js_object_set(_this, \"%s\", js_function(_method_%s_%s));\n",
                        mname, class_name, mname);
        }
        buf_append_str(&c->lambdas, "return _this;\n}\n");
    }
    free(methods);

    /* Register as a known function */
    map_put(&c->known_functions, arena_strdup(c->arena, class_name), (void *)1);
}

/* ---------- function declaration ------------------------------------- */

static void gen_fn_decl(Codegen *c, AstNode *n, Buffer *out) {
    const char *fn_name = safe_name_a(c, n->func.id->ident.name);
    buf_appendf(out, "JsValue* %s(JsValue** _a, int _ac) {\n", fn_name);
    Buffer params; buf_init(&params);
    gen_param_list(c, &n->func.params, &params);
    if (params.data) buf_append_str(out, params.data);
    buf_append_char(out, '\n');
    buf_free(&params);
    for (size_t i = 0; i < n->func.body->block.body.len; ++i) {
        gen_stmt(c, n->func.body->block.body.items[i], out);
    }
    buf_append_str(out, "return js_undefined();\n}\n");
}

/* ---------- collect function names ----------------------------------- */

static void collect_function_names(Codegen *c, AstNode *prog) {
    for (size_t i = 0; i < prog->block.body.len; ++i) {
        AstNode *node = prog->block.body.items[i];
        if (node->kind == AST_FN_DECL && node->func.id) {
            map_put(&c->known_functions, arena_strdup(c->arena, safe_name_a(c, node->func.id->ident.name)), (void *)1);
        }
        if (node->kind == AST_CLASS_DECL && node->class_.id) {
            map_put(&c->known_functions, arena_strdup(c->arena, safe_name_a(c, node->class_.id->ident.name)), (void *)1);
        }
    }
}

/* ---------- C-header import wrappers --------------------------------- */

/* Each imported C function has a signature described by a compact DSL stored
 * in C_FN_SIG_TABLE. The parser turns "v(i,i,s)" into a CSig of (return_type,
 * arg_types[]); emit_c_wrapper walks that to generate marshaling glue.
 *
 * Type tokens (lowercase = primitive, UpperCamel = struct from C_STRUCT_TABLE):
 *   v   void                 i   int                u   unsigned
 *   l   long                 f   float              d   double
 *   b   bool (uses js_truthy) c   signed char       uc  unsigned char
 *   s   const char*          t   size_t             n   literal NULL ptr arg
 *
 * Unknown functions fall back to "d(d)" (single-double in/out) so most
 * single-arg math.h functions still work without an explicit entry.
 */

typedef enum {
    CT_VOID, CT_INT, CT_UINT, CT_LONG, CT_FLOAT, CT_DOUBLE,
    CT_BOOL, CT_CHAR, CT_UCHAR, CT_STR, CT_SIZET, CT_NULL,
    CT_STRUCT,
} CType;

typedef struct {
    CType kind;
    const char *struct_name; /* CT_STRUCT only — points into the sig string */
} CTypeRef;

typedef struct { const char *name; CTypeRef type; } CStructField;
typedef struct {
    const char *name;
    const CStructField *fields;
    int field_count;
} CStructDef;

#define MAX_C_ARGS 8
typedef struct {
    CTypeRef ret;
    CTypeRef args[MAX_C_ARGS];
    int argc;
    bool ok;
} CSig;

/* ---- struct registry ---- */

static const CStructField FIELDS_Color[]     = {
    {"r", {CT_UCHAR, NULL}}, {"g", {CT_UCHAR, NULL}},
    {"b", {CT_UCHAR, NULL}}, {"a", {CT_UCHAR, NULL}},
};
static const CStructField FIELDS_Vector2[]   = {
    {"x", {CT_FLOAT, NULL}}, {"y", {CT_FLOAT, NULL}},
};
static const CStructField FIELDS_Vector3[]   = {
    {"x", {CT_FLOAT, NULL}}, {"y", {CT_FLOAT, NULL}}, {"z", {CT_FLOAT, NULL}},
};
static const CStructField FIELDS_Vector4[]   = {
    {"x", {CT_FLOAT, NULL}}, {"y", {CT_FLOAT, NULL}},
    {"z", {CT_FLOAT, NULL}}, {"w", {CT_FLOAT, NULL}},
};
static const CStructField FIELDS_Rectangle[] = {
    {"x", {CT_FLOAT, NULL}}, {"y", {CT_FLOAT, NULL}},
    {"width", {CT_FLOAT, NULL}}, {"height", {CT_FLOAT, NULL}},
};

/* Alphabetically sorted for bsearch. */
static const CStructDef C_STRUCT_TABLE[] = {
    {"Color",     FIELDS_Color,     4},
    {"Rectangle", FIELDS_Rectangle, 4},
    {"Vector2",   FIELDS_Vector2,   2},
    {"Vector3",   FIELDS_Vector3,   3},
    {"Vector4",   FIELDS_Vector4,   4},
};
#define C_STRUCT_TABLE_LEN (sizeof C_STRUCT_TABLE / sizeof C_STRUCT_TABLE[0])

static int struct_cmp(const void *a, const void *b) {
    return strcmp(((const CStructDef *)a)->name, ((const CStructDef *)b)->name);
}
static const CStructDef *lookup_struct(const char *name) {
    if (!name) return NULL;
    CStructDef key = {name, NULL, 0};
    return (const CStructDef *)bsearch(&key, C_STRUCT_TABLE, C_STRUCT_TABLE_LEN,
                                        sizeof(CStructDef), struct_cmp);
}

/* ---- function signature table ---- */

typedef struct { const char *name; const char *sig; } CFnSig;

/* Keep alphabetically sorted for bsearch. */
static const CFnSig C_FN_SIG_TABLE[] = {
    /* raylib — core */
    {"BeginDrawing",        "v()"},
    {"ClearBackground",     "v(Color)"},
    {"CloseWindow",         "v()"},
    {"DrawCircle",          "v(i,i,f,Color)"},
    {"DrawCircleV",         "v(Vector2,f,Color)"},
    {"DrawFPS",             "v(i,i)"},
    {"DrawLine",            "v(i,i,i,i,Color)"},
    {"DrawPixel",           "v(i,i,Color)"},
    {"DrawRectangle",       "v(i,i,i,i,Color)"},
    {"DrawRectangleLines",  "v(i,i,i,i,Color)"},
    {"DrawRectangleRec",    "v(Rectangle,Color)"},
    {"DrawText",            "v(s,i,i,i,Color)"},
    {"EndDrawing",          "v()"},
    {"Fade",                "Color(Color,f)"},
    {"GetColor",            "Color(u)"},
    {"GetFPS",              "i()"},
    {"GetFrameTime",        "f()"},
    {"GetMousePosition",    "Vector2()"},
    {"GetMouseX",           "i()"},
    {"GetMouseY",           "i()"},
    {"GetRandomValue",      "i(i,i)"},
    {"GetScreenHeight",     "i()"},
    {"GetScreenWidth",      "i()"},
    {"GetTime",             "d()"},
    {"InitWindow",          "v(i,i,s)"},
    {"IsKeyDown",           "i(i)"},
    {"IsKeyPressed",        "i(i)"},
    {"IsMouseButtonDown",   "i(i)"},
    {"IsMouseButtonPressed","i(i)"},
    {"SetTargetFPS",        "v(i)"},
    {"SetWindowTitle",      "v(s)"},
    {"WindowShouldClose",   "i()"},

    /* libc — math, stdio, stdlib, string, time */
    {"abort",     "v()"},
    {"abs",       "i(i)"},
    {"acos",      "d(d)"},
    {"acosh",     "d(d)"},
    {"asin",      "d(d)"},
    {"asinh",     "d(d)"},
    {"atan",      "d(d)"},
    {"atan2",     "d(d,d)"},
    {"atanh",     "d(d)"},
    {"atof",      "d(s)"},
    {"atoi",      "i(s)"},
    {"cbrt",      "d(d)"},
    {"ceil",      "d(d)"},
    {"copysign",  "d(d,d)"},
    {"cos",       "d(d)"},
    {"cosh",      "d(d)"},
    {"exit",      "v(i)"},
    {"exp",       "d(d)"},
    {"exp2",      "d(d)"},
    {"expm1",     "d(d)"},
    {"fabs",      "d(d)"},
    {"fdim",      "d(d,d)"},
    {"floor",     "d(d)"},
    {"fmax",      "d(d,d)"},
    {"fmin",      "d(d,d)"},
    {"fmod",      "d(d,d)"},
    {"getchar",   "i()"},
    {"hypot",     "d(d,d)"},
    {"log",       "d(d)"},
    {"log10",     "d(d)"},
    {"log1p",     "d(d)"},
    {"log2",      "d(d)"},
    {"nearbyint", "d(d)"},
    {"pow",       "d(d,d)"},
    {"putchar",   "i(i)"},
    {"puts",      "i(s)"},
    {"rand",      "i()"},
    {"remainder", "d(d,d)"},
    {"rint",      "d(d)"},
    {"round",     "d(d)"},
    {"sin",       "d(d)"},
    {"sinh",      "d(d)"},
    {"sleep",     "u(u)"},
    {"sqrt",      "d(d)"},
    {"srand",     "v(u)"},
    {"strcmp",    "i(s,s)"},
    {"strlen",    "t(s)"},
    {"system",    "i(s)"},
    {"tan",       "d(d)"},
    {"tanh",      "d(d)"},
    {"time",      "l(n)"},
    {"trunc",     "d(d)"},
};
#define C_FN_SIG_TABLE_LEN (sizeof C_FN_SIG_TABLE / sizeof C_FN_SIG_TABLE[0])

static int c_fn_sig_cmp(const void *a, const void *b) {
    return strcmp(((const CFnSig *)a)->name, ((const CFnSig *)b)->name);
}
static const char *lookup_c_fn_sig(const char *imported) {
    if (!imported) return NULL;
    CFnSig key = {imported, NULL};
    const CFnSig *hit = (const CFnSig *)bsearch(&key, C_FN_SIG_TABLE, C_FN_SIG_TABLE_LEN,
                                                 sizeof(CFnSig), c_fn_sig_cmp);
    return hit ? hit->sig : NULL;
}

/* ---- C constant table ----
 * Constants (macros, enum values, const ints) are imported by value: the
 * compiler emits a one-shot builder that materializes the JsValue at
 * runtime init. Each entry pairs a name with its CTypeRef DSL token (e.g.
 * "Color" for a struct constant, "i" for an int, "d" for a double). */
typedef struct { const char *name; const char *type; } CConstEntry;

/* Alphabetically sorted (C-locale) for bsearch.
 * Note: uppercase letters < '_' < lowercase in ASCII, so e.g. MAGENTA and
 * MOUSE_BUTTON_* come *before* M_E, M_PI, etc. */
static const CConstEntry C_CONST_TABLE[] = {
    /* raylib colors */
    {"BEIGE",     "Color"},
    {"BLACK",     "Color"},
    {"BLANK",     "Color"},
    {"BLUE",      "Color"},
    {"BROWN",     "Color"},
    {"DARKBLUE",  "Color"},
    {"DARKBROWN", "Color"},
    {"DARKGRAY",  "Color"},
    {"DARKGREEN", "Color"},
    {"DARKPURPLE","Color"},
    {"GOLD",      "Color"},
    {"GRAY",      "Color"},
    {"GREEN",     "Color"},

    /* raylib keyboard keys (KeyboardKey enum) */
    {"KEY_A",            "i"},
    {"KEY_APOSTROPHE",   "i"},
    {"KEY_B",            "i"},
    {"KEY_BACKSLASH",    "i"},
    {"KEY_BACKSPACE",    "i"},
    {"KEY_C",            "i"},
    {"KEY_CAPS_LOCK",    "i"},
    {"KEY_COMMA",        "i"},
    {"KEY_D",            "i"},
    {"KEY_DELETE",       "i"},
    {"KEY_DOWN",         "i"},
    {"KEY_E",            "i"},
    {"KEY_END",          "i"},
    {"KEY_ENTER",        "i"},
    {"KEY_EQUAL",        "i"},
    {"KEY_ESCAPE",       "i"},
    {"KEY_F",            "i"},
    {"KEY_F1",           "i"},
    {"KEY_F10",          "i"},
    {"KEY_F11",          "i"},
    {"KEY_F12",          "i"},
    {"KEY_F2",           "i"},
    {"KEY_F3",           "i"},
    {"KEY_F4",           "i"},
    {"KEY_F5",           "i"},
    {"KEY_F6",           "i"},
    {"KEY_F7",           "i"},
    {"KEY_F8",           "i"},
    {"KEY_F9",           "i"},
    {"KEY_G",            "i"},
    {"KEY_GRAVE",        "i"},
    {"KEY_H",            "i"},
    {"KEY_HOME",         "i"},
    {"KEY_I",            "i"},
    {"KEY_INSERT",       "i"},
    {"KEY_J",            "i"},
    {"KEY_K",            "i"},
    {"KEY_L",            "i"},
    {"KEY_LEFT",         "i"},
    {"KEY_LEFT_ALT",     "i"},
    {"KEY_LEFT_BRACKET", "i"},
    {"KEY_LEFT_CONTROL", "i"},
    {"KEY_LEFT_SHIFT",   "i"},
    {"KEY_LEFT_SUPER",   "i"},
    {"KEY_M",            "i"},
    {"KEY_MINUS",        "i"},
    {"KEY_N",            "i"},
    {"KEY_NUM_LOCK",     "i"},
    {"KEY_O",            "i"},
    {"KEY_P",            "i"},
    {"KEY_PAGE_DOWN",    "i"},
    {"KEY_PAGE_UP",      "i"},
    {"KEY_PAUSE",        "i"},
    {"KEY_PERIOD",       "i"},
    {"KEY_PRINT_SCREEN", "i"},
    {"KEY_Q",            "i"},
    {"KEY_R",            "i"},
    {"KEY_RIGHT",        "i"},
    {"KEY_RIGHT_ALT",    "i"},
    {"KEY_RIGHT_BRACKET","i"},
    {"KEY_RIGHT_CONTROL","i"},
    {"KEY_RIGHT_SHIFT",  "i"},
    {"KEY_RIGHT_SUPER",  "i"},
    {"KEY_S",            "i"},
    {"KEY_SCROLL_LOCK",  "i"},
    {"KEY_SEMICOLON",    "i"},
    {"KEY_SLASH",        "i"},
    {"KEY_SPACE",        "i"},
    {"KEY_T",            "i"},
    {"KEY_TAB",          "i"},
    {"KEY_U",            "i"},
    {"KEY_UP",           "i"},
    {"KEY_V",            "i"},
    {"KEY_W",            "i"},
    {"KEY_X",            "i"},
    {"KEY_Y",            "i"},
    {"KEY_Z",            "i"},

    /* more colors */
    {"LIGHTGRAY", "Color"},
    {"LIME",      "Color"},
    {"MAGENTA",   "Color"},
    {"MAROON",    "Color"},

    /* raylib mouse buttons — fall here because uppercase letters sort
     * before '_' (MOUSE_… < M_…). */
    {"MOUSE_BUTTON_BACK",    "i"},
    {"MOUSE_BUTTON_EXTRA",   "i"},
    {"MOUSE_BUTTON_FORWARD", "i"},
    {"MOUSE_BUTTON_LEFT",    "i"},
    {"MOUSE_BUTTON_MIDDLE",  "i"},
    {"MOUSE_BUTTON_RIGHT",   "i"},
    {"MOUSE_BUTTON_SIDE",    "i"},

    /* libc math (math.h) constants */
    {"M_E",       "d"},
    {"M_LN10",    "d"},
    {"M_LN2",     "d"},
    {"M_LOG10E",  "d"},
    {"M_LOG2E",   "d"},
    {"M_PI",      "d"},
    {"M_PI_2",    "d"},
    {"M_PI_4",    "d"},
    {"M_SQRT1_2", "d"},
    {"M_SQRT2",   "d"},

    /* remaining colors */
    {"ORANGE",    "Color"},
    {"PINK",      "Color"},
    {"PURPLE",    "Color"},
    {"RAYWHITE",  "Color"},
    {"RED",       "Color"},
    {"SKYBLUE",   "Color"},
    {"VIOLET",    "Color"},
    {"WHITE",     "Color"},
    {"YELLOW",    "Color"},
};
#define C_CONST_TABLE_LEN (sizeof C_CONST_TABLE / sizeof C_CONST_TABLE[0])

static int c_const_cmp(const void *a, const void *b) {
    return strcmp(((const CConstEntry *)a)->name, ((const CConstEntry *)b)->name);
}
static const char *lookup_c_const(const char *imported) {
    if (!imported) return NULL;
    CConstEntry key = {imported, NULL};
    const CConstEntry *hit = (const CConstEntry *)bsearch(&key, C_CONST_TABLE, C_CONST_TABLE_LEN,
                                                           sizeof(CConstEntry), c_const_cmp);
    return hit ? hit->type : NULL;
}

/* ---- signature parser ---- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool is_name_start(char c) { return (c >= 'A' && c <= 'Z'); }
static bool is_name_cont(char c)  {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
}

static bool parse_ctyperef(Arena *arena, const char **pp, CTypeRef *out) {
    const char *p = skip_ws(*pp);
    if (!*p) return false;

    /* Struct: starts with uppercase. */
    if (is_name_start(*p)) {
        const char *start = p;
        while (is_name_cont(*p)) p++;
        if (p == start) return false;
        out->kind = CT_STRUCT;
        out->struct_name = arena_strndup(arena, start, p - start);
        *pp = p;
        return true;
    }

    /* Two-letter primitives first so we don't mis-parse "uc" as "u" then "c". */
    if (p[0] == 'u' && p[1] == 'c') { out->kind = CT_UCHAR; out->struct_name = NULL; *pp = p + 2; return true; }

    out->struct_name = NULL;
    switch (*p) {
        case 'v': out->kind = CT_VOID;   break;
        case 'i': out->kind = CT_INT;    break;
        case 'u': out->kind = CT_UINT;   break;
        case 'l': out->kind = CT_LONG;   break;
        case 'f': out->kind = CT_FLOAT;  break;
        case 'd': out->kind = CT_DOUBLE; break;
        case 'b': out->kind = CT_BOOL;   break;
        case 'c': out->kind = CT_CHAR;   break;
        case 's': out->kind = CT_STR;    break;
        case 't': out->kind = CT_SIZET;  break;
        case 'n': out->kind = CT_NULL;   break;
        default: return false;
    }
    *pp = p + 1;
    return true;
}

static bool parse_csig(Arena *arena, const char *sig, CSig *out) {
    memset(out, 0, sizeof *out);
    const char *p = sig;
    if (!parse_ctyperef(arena, &p, &out->ret)) return false;
    p = skip_ws(p);
    if (*p != '(') return false;
    p++;
    p = skip_ws(p);
    if (*p == ')') { out->ok = true; return true; }
    while (1) {
        if (out->argc >= MAX_C_ARGS) return false;
        if (!parse_ctyperef(arena, &p, &out->args[out->argc])) return false;
        out->argc++;
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == ')') { out->ok = true; return true; }
        return false;
    }
}

/* ---- wrapper emission ---- */

static const char *c_type_decl(const CTypeRef *t) {
    switch (t->kind) {
        case CT_VOID:   return "void";
        case CT_INT:    return "int";
        case CT_UINT:   return "unsigned";
        case CT_LONG:   return "long";
        case CT_FLOAT:  return "float";
        case CT_DOUBLE: return "double";
        case CT_BOOL:   return "int";
        case CT_CHAR:   return "char";
        case CT_UCHAR:  return "unsigned char";
        case CT_STR:    return "const char*";
        case CT_SIZET:  return "size_t";
        case CT_NULL:   return "void*";
        case CT_STRUCT: return t->struct_name;
    }
    return "int";
}

/* Emit a JS-value-to-C-scalar conversion expression. */
static void emit_to_c_scalar(Buffer *out, const CTypeRef *t, const char *jsexpr) {
    switch (t->kind) {
        case CT_INT:    buf_appendf(out, "(int)js_to_number(%s)", jsexpr); break;
        case CT_UINT:   buf_appendf(out, "(unsigned)js_to_number(%s)", jsexpr); break;
        case CT_LONG:   buf_appendf(out, "(long)js_to_number(%s)", jsexpr); break;
        case CT_FLOAT:  buf_appendf(out, "(float)js_to_number(%s)", jsexpr); break;
        case CT_DOUBLE: buf_appendf(out, "js_to_number(%s)", jsexpr); break;
        case CT_BOOL:   buf_appendf(out, "(js_truthy(%s) ? 1 : 0)", jsexpr); break;
        case CT_CHAR:   buf_appendf(out, "(char)js_to_number(%s)", jsexpr); break;
        case CT_UCHAR:  buf_appendf(out, "(unsigned char)js_to_number(%s)", jsexpr); break;
        case CT_SIZET:  buf_appendf(out, "(size_t)js_to_number(%s)", jsexpr); break;
        default: buf_append_str(out, "0"); break;
    }
}

/* Emit the C-to-JS-value wrapping expression for a scalar result. */
static void emit_from_c_scalar(Buffer *out, const CTypeRef *t, const char *cexpr) {
    switch (t->kind) {
        case CT_INT: case CT_UINT: case CT_LONG: case CT_BOOL:
        case CT_CHAR: case CT_UCHAR: case CT_SIZET:
            buf_appendf(out, "js_number((double)(%s))", cexpr); break;
        case CT_FLOAT:
            buf_appendf(out, "js_number((double)(%s))", cexpr); break;
        case CT_DOUBLE:
            buf_appendf(out, "js_number(%s)", cexpr); break;
        case CT_STR:
            buf_appendf(out, "((%s) ? js_string(%s) : js_null())", cexpr, cexpr); break;
        default:
            buf_append_str(out, "js_undefined()"); break;
    }
}

/* Generate `static JsValue* _c_<local>(...) { ... }` for one C import. */
static void emit_c_wrapper_sig(Buffer *out, const char *local, const char *imported,
                                const CSig *sig) {
    buf_appendf(out, "static JsValue* _c_%s(JsValue** _a, int _ac) {\n", local);
    buf_append_str(out, "    (void)_a; (void)_ac;\n");

    /* Marshal each argument into a local C variable `_arg<i>`. */
    for (int i = 0; i < sig->argc; ++i) {
        const CTypeRef *t = &sig->args[i];
        if (t->kind == CT_NULL) {
            buf_appendf(out, "    void* _arg%d = NULL;\n", i);
            continue;
        }
        if (t->kind == CT_STR) {
            buf_appendf(out,
                "    char* _arg%d = (_ac > %d) ? js_to_string(_a[%d]) : NULL;\n",
                i, i, i);
            continue;
        }
        if (t->kind == CT_STRUCT) {
            const CStructDef *def = lookup_struct(t->struct_name);
            buf_appendf(out, "    %s _arg%d = (%s){0};\n",
                        t->struct_name, i, t->struct_name);
            if (def) {
                buf_appendf(out, "    if (_ac > %d && _a[%d]->type == JS_OBJECT) {\n", i, i);
                buf_appendf(out, "        JsValue* _o = _a[%d];\n", i);
                for (int f = 0; f < def->field_count; ++f) {
                    const CStructField *fld = &def->fields[f];
                    char tmp[128];
                    snprintf(tmp, sizeof tmp, "js_object_get(_o, \"%s\")", fld->name);
                    buf_appendf(out, "        _arg%d.%s = ", i, fld->name);
                    emit_to_c_scalar(out, &fld->type, tmp);
                    buf_append_str(out, ";\n");
                }
                buf_append_str(out, "    }\n");
            }
            continue;
        }
        /* Plain scalars (int, double, ...). */
        buf_appendf(out, "    %s _arg%d = (_ac > %d) ? ",
                    c_type_decl(t), i, i);
        {
            char tmp[32];
            snprintf(tmp, sizeof tmp, "_a[%d]", i);
            emit_to_c_scalar(out, t, tmp);
        }
        buf_appendf(out, " : (%s)0;\n", c_type_decl(t));
    }

    /* Build the call. Capture into `_ret` unless return is void. */
    bool ret_void = (sig->ret.kind == CT_VOID);
    bool ret_struct = (sig->ret.kind == CT_STRUCT);
    if (ret_void) {
        buf_appendf(out, "    %s(", imported);
    } else if (ret_struct) {
        buf_appendf(out, "    %s _ret = %s(", sig->ret.struct_name, imported);
    } else {
        buf_appendf(out, "    %s _ret = %s(", c_type_decl(&sig->ret), imported);
    }
    for (int i = 0; i < sig->argc; ++i) {
        if (i) buf_append_str(out, ", ");
        buf_appendf(out, "_arg%d", i);
    }
    buf_append_str(out, ");\n");

    /* Free any js_to_string-allocated argument buffers. */
    for (int i = 0; i < sig->argc; ++i) {
        if (sig->args[i].kind == CT_STR) {
            buf_appendf(out, "    free(_arg%d);\n", i);
        }
    }

    /* Marshal the result. */
    if (ret_void) {
        buf_append_str(out, "    return js_undefined();\n");
    } else if (ret_struct) {
        const CStructDef *def = lookup_struct(sig->ret.struct_name);
        buf_append_str(out, "    JsValue* _ro = js_object_new();\n");
        if (def) {
            for (int f = 0; f < def->field_count; ++f) {
                const CStructField *fld = &def->fields[f];
                char tmp[128];
                snprintf(tmp, sizeof tmp, "_ret.%s", fld->name);
                buf_appendf(out, "    js_object_set(_ro, \"%s\", ", fld->name);
                emit_from_c_scalar(out, &fld->type, tmp);
                buf_append_str(out, ");\n");
            }
        }
        buf_append_str(out, "    return _ro;\n");
    } else {
        buf_append_str(out, "    return ");
        emit_from_c_scalar(out, &sig->ret, "_ret");
        buf_append_str(out, ";\n");
    }

    buf_append_str(out, "}\n");
}

/* Emit a one-shot `static JsValue* _c_const_<local>(void)` builder that
 * materializes a C constant (macro / enum / const variable) as a JsValue. */
static void emit_const_builder(Buffer *out, const char *local, const char *imported,
                                const CTypeRef *t) {
    buf_appendf(out, "static JsValue* _c_const_%s(void) {\n", local);
    if (t->kind == CT_STRUCT) {
        const CStructDef *def = lookup_struct(t->struct_name);
        /* Copy into a local first — compound-literal macros (raylib's
         * `(Color){...}`) are rvalues, so going through `_v` keeps the
         * field-access form uniform. */
        buf_appendf(out, "    %s _v = %s;\n", t->struct_name, imported);
        buf_append_str(out, "    JsValue* _o = js_object_new();\n");
        if (def) {
            for (int f = 0; f < def->field_count; ++f) {
                const CStructField *fld = &def->fields[f];
                char tmp[128];
                snprintf(tmp, sizeof tmp, "_v.%s", fld->name);
                buf_appendf(out, "    js_object_set(_o, \"%s\", ", fld->name);
                emit_from_c_scalar(out, &fld->type, tmp);
                buf_append_str(out, ");\n");
            }
        }
        buf_append_str(out, "    return _o;\n");
    } else if (t->kind == CT_NULL) {
        buf_append_str(out, "    return js_null();\n");
    } else if (t->kind == CT_STR) {
        buf_appendf(out, "    const char* _v = %s;\n", imported);
        buf_append_str(out, "    return _v ? js_string(_v) : js_null();\n");
    } else {
        buf_append_str(out, "    return ");
        emit_from_c_scalar(out, t, imported);
        buf_append_str(out, ";\n");
    }
    buf_append_str(out, "}\n");
}

/* Classification of a C import — function (with sig) or constant (with type). */
typedef enum { CI_FN, CI_CONST } CImportKind;

typedef struct {
    CImportKind kind;
    CSig sig;          /* valid when kind == CI_FN */
    CTypeRef type;     /* valid when kind == CI_CONST */
} CImportResolved;

static void resolve_c_import(Arena *arena, const char *imported, CImportResolved *out) {
    memset(out, 0, sizeof *out);
    const char *sig = lookup_c_fn_sig(imported);
    if (sig) {
        out->kind = CI_FN;
        if (!parse_csig(arena, sig, &out->sig)) parse_csig(arena, "d(d)", &out->sig);
        return;
    }
    const char *ctype = lookup_c_const(imported);
    if (ctype) {
        out->kind = CI_CONST;
        const char *p = ctype;
        if (!parse_ctyperef(arena, &p, &out->type)) {
            /* Malformed constant type — treat as int. */
            out->type.kind = CT_INT;
            out->type.struct_name = NULL;
        }
        return;
    }
    /* Unknown — assume single-arg double function (covers most math.h). */
    out->kind = CI_FN;
    parse_csig(arena, "d(d)", &out->sig);
}

/* ---------- top-level ------------------------------------------------ */

char *codegen_generate(AstNode *program, const char *source, size_t source_len,
                       const char *filename, Arena *arena, CodegenError *out_err,
                       const CImportList *c_imports,
                       const AssetList *assets) {
    Codegen c;
    memset(&c, 0, sizeof c);
    c.arena = arena;
    c.source = source;
    c.source_len = source_len;
    c.filename = filename;
    map_init(&c.known_functions);
    map_init(&c.globals);
    buf_init(&c.lambdas);
    buf_init(&c.fns);
    buf_init(&c.main_body);

    /* Register c-import bindings as globals + emit wrappers or const builders.
     * Wrappers live in `c.fns` so they appear before main(). Dedupe by `local`
     * since two modules importing the same C symbol unaliased share a binding.
     *
     * Resolution map (local -> CImportResolved*) is reused in main() init. */
    Map seen_c_locals;
    Map cimport_resolved;       /* local -> CImportResolved* (arena-allocated) */
    map_init(&seen_c_locals);
    map_init(&cimport_resolved);
    if (c_imports) {
        for (size_t i = 0; i < c_imports->len; ++i) {
            const CImport *ci = &c_imports->items[i];
            if (map_has(&seen_c_locals, ci->local)) continue;
            map_put(&seen_c_locals, ci->local, (void *)1);
            map_put(&c.globals, arena_strdup(c.arena, ci->local), (void *)1);

            CImportResolved *res = (CImportResolved *)arena_alloc(c.arena, sizeof *res);
            resolve_c_import(c.arena, ci->imported, res);
            map_put(&cimport_resolved, arena_strdup(c.arena, ci->local), res);

            if (res->kind == CI_FN) {
                emit_c_wrapper_sig(&c.fns, ci->local, ci->imported, &res->sig);
            } else {
                emit_const_builder(&c.fns, ci->local, ci->imported, &res->type);
            }
        }
    }

    /* Asset imports: register each `_ai_<name>` as a JsValue global and emit
     * its bytes as a file-scope `static const unsigned char` array next to the
     * C-import wrappers. */
    if (assets) {
        for (size_t i = 0; i < assets->len; ++i) {
            const AssetImport *ai = &assets->items[i];
            map_put(&c.globals, arena_strdup(c.arena, ai->local), (void *)1);
            char *escaped = escape_str_for_c(&c, (const char *)ai->data, ai->len);
            buf_appendf(&c.fns,
                "/* asset: %s (%zu bytes) */\n"
                "static const unsigned char %s_data[] = \"%s\";\n",
                ai->source_path ? ai->source_path : "?", ai->len,
                ai->local, escaped);
        }
    }

    collect_function_names(&c, program);

    for (size_t i = 0; i < program->block.body.len; ++i) {
        AstNode *node = program->block.body.items[i];
        if (node->kind == AST_FN_DECL) {
            gen_fn_decl(&c, node, &c.fns);
        } else if (node->kind == AST_CLASS_DECL) {
            emit_loc_update(&c, node, &c.main_body);
            gen_class_decl(&c, node, &c.main_body);
        } else if (node->kind == AST_VAR_DECL) {
            emit_loc_update(&c, node, &c.main_body);
            gen_var_decl(&c, node, true, &c.main_body);
        } else {
            gen_stmt(&c, node, &c.main_body);
        }
        if (c.err.present) goto fail;
    }

    /* Assemble final output */
    Buffer final; buf_init(&final);
    buf_append_str(&final, "#include \"runtime.h\"\n");

    /* C-header imports: emit `#include <hdr>` for each unique header. */
    if (c_imports && c_imports->len) {
        Map seen; map_init(&seen);
        for (size_t i = 0; i < c_imports->len; ++i) {
            const char *h = c_imports->items[i].header;
            if (map_has(&seen, h)) continue;
            map_put(&seen, h, (void *)1);
            buf_appendf(&final, "#include <%s>\n", h);
        }
        map_free(&seen);
    }
    buf_append_char(&final, '\n');

    /* C-import wrappers go FIRST, before any QS bindings, so they can freely
     * use raylib (and other) macro constants without interference. After the
     * wrappers we `#undef` every QS-visible identifier so the QS code below
     * isn't text-mangled by collisions like raylib.h's `RAYWHITE`. */
    if (c.fns.data) buf_append_str(&final, c.fns.data);
    buf_append_char(&final, '\n');

    {
        Map undef_seen; map_init(&undef_seen);
        size_t it; const char *k; void *v;
        it = 0;
        while (map_iter(&c.known_functions, &it, &k, &v)) {
            if (map_has(&undef_seen, k)) continue;
            map_put(&undef_seen, k, (void *)1);
            buf_appendf(&final, "#ifdef %s\n#undef %s\n#endif\n", k, k);
        }
        it = 0;
        while (map_iter(&c.globals, &it, &k, &v)) {
            if (map_has(&undef_seen, k)) continue;
            map_put(&undef_seen, k, (void *)1);
            buf_appendf(&final, "#ifdef %s\n#undef %s\n#endif\n", k, k);
        }
        map_free(&undef_seen);
    }
    buf_append_char(&final, '\n');

    /* Forward declarations */
    {
        size_t it = 0; const char *k; void *v;
        while (map_iter(&c.known_functions, &it, &k, &v)) {
            buf_appendf(&final, "JsValue* %s(JsValue** _a, int _ac);\n", k);
        }
    }
    buf_append_char(&final, '\n');

    /* Globals */
    {
        size_t it = 0; const char *k; void *v;
        while (map_iter(&c.globals, &it, &k, &v)) {
            buf_appendf(&final, "JsValue* %s = NULL;\n", k);
        }
    }
    buf_append_char(&final, '\n');

    /* Lambdas */
    if (c.lambdas.data) buf_append_str(&final, c.lambdas.data);
    buf_append_char(&final, '\n');

    /* main() */
    buf_append_str(&final, "int main(int _qsc_argc, char** _qsc_argv) {\n");
    buf_append_str(&final, "js_runtime_init(_qsc_argc, _qsc_argv);\n");
    /* Bind c-import locals before user code runs. Dedupe via seen_c_locals. */
    if (c_imports) {
        Map seen_init;
        map_init(&seen_init);
        for (size_t i = 0; i < c_imports->len; ++i) {
            const CImport *ci = &c_imports->items[i];
            if (map_has(&seen_init, ci->local)) continue;
            map_put(&seen_init, ci->local, (void *)1);
            CImportResolved *res = (CImportResolved *)map_get(&cimport_resolved, ci->local);
            if (res && res->kind == CI_CONST) {
                buf_appendf(&final, "%s = _c_const_%s();\n", ci->local, ci->local);
            } else {
                buf_appendf(&final, "%s = js_function(_c_%s);\n", ci->local, ci->local);
            }
        }
        map_free(&seen_init);
    }
    /* Bind asset import locals to QS string values backed by the static byte
     * arrays. Text content is fully visible; binary content with embedded
     * NUL bytes is truncated at the first NUL (a v1 limitation). */
    if (assets) {
        for (size_t i = 0; i < assets->len; ++i) {
            const AssetImport *ai = &assets->items[i];
            buf_appendf(&final, "%s = js_string((const char*)%s_data);\n",
                        ai->local, ai->local);
        }
    }
    if (c.main_body.data) buf_append_str(&final, c.main_body.data);
    buf_append_str(&final, "return 0;\n}\n");

    char *out = buf_take(&final);
    buf_free(&c.lambdas);
    buf_free(&c.fns);
    buf_free(&c.main_body);
    map_free(&c.known_functions);
    map_free(&c.globals);
    map_free(&seen_c_locals);
    map_free(&cimport_resolved);
    free(c.ctx_stack);
    if (out_err) memset(out_err, 0, sizeof *out_err);
    return out;

fail:
    if (out_err) *out_err = c.err;
    buf_free(&c.lambdas);
    buf_free(&c.fns);
    buf_free(&c.main_body);
    map_free(&c.known_functions);
    map_free(&c.globals);
    map_free(&seen_c_locals);
    map_free(&cimport_resolved);
    free(c.ctx_stack);
    return NULL;
}
