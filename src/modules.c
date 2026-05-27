#include "modules.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "map.h"

/* ------- module info --------------------------------------------------- */

typedef struct ModuleInfo {
    const char *abs_path;      /* absolute, canonical file path */
    const char *prefix;        /* "_m0_", "_m1_", ... ; empty string for entry */
    AstNode *ast;              /* Program */
    const char *source;
    size_t source_len;

    /* parallel arrays of import declarations and their resolved source modules */
    AstNode **import_nodes;
    struct ModuleInfo **import_sources;
    size_t imports_len;
    size_t imports_cap;

    Map bindings;              /* set: binding name -> (void*)1 */
    Map exports;               /* export name -> local binding name (const char*) */
    Map rename_map;            /* original name -> new name (const char*) */
} ModuleInfo;

typedef struct {
    Arena *arena;
    Map modules;               /* abs_path -> ModuleInfo* */
    ModuleInfo **order;        /* dependency-order list */
    size_t order_len, order_cap;
    int counter;
    ParseError err;
} Bundler;

static void mod_set_error(Bundler *b, AstNode *node, const char *file, const char *fmt, ...) {
    if (b->err.present) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->err.message, sizeof b->err.message, fmt, ap);
    va_end(ap);
    b->err.line = node ? node->line : 0;
    b->err.col = node ? node->col : 0;
    b->err.present = true;
    (void)file;
}

/* ------- file/path helpers -------------------------------------------- */

static char *read_file_to_arena(Bundler *b, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)arena_alloc(b->arena, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static bool is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *path_join(Arena *a, const char *dir, const char *rel) {
    size_t dl = strlen(dir), rl = strlen(rel);
    bool need_sep = dl > 0 && dir[dl - 1] != '/';
    char *out = (char *)arena_alloc(a, dl + (need_sep ? 1 : 0) + rl + 1);
    memcpy(out, dir, dl);
    size_t off = dl;
    if (need_sep) out[off++] = '/';
    memcpy(out + off, rel, rl);
    out[off + rl] = '\0';
    return out;
}

static const char *path_dirname(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return arena_strdup(a, ".");
    size_t n = (size_t)(slash - path);
    if (n == 0) return arena_strdup(a, "/");
    return arena_strndup(a, path, n);
}

static bool has_extension(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    return strchr(base, '.') != NULL;
}

static char *canonicalize(Arena *a, const char *path) {
    char *resolved = realpath(path, NULL);
    if (!resolved) return NULL;
    char *out = arena_strdup(a, resolved);
    free(resolved);
    return out;
}

/* Try `base`, then `base.qs`, then `base.js`. Returns canonical path or NULL. */
static char *resolve_import(Bundler *b, const char *from_dir, const char *import_path) {
    Arena *a = b->arena;
    char *base = import_path[0] == '/' ? arena_strdup(a, import_path)
                                       : path_join(a, from_dir, import_path);

    if (is_file(base)) return canonicalize(a, base);
    if (!has_extension(base)) {
        char *with_qs = (char *)arena_alloc(a, strlen(base) + 4);
        sprintf(with_qs, "%s.qs", base);
        if (is_file(with_qs)) return canonicalize(a, with_qs);
        char *with_js = (char *)arena_alloc(a, strlen(base) + 4);
        sprintf(with_js, "%s.js", base);
        if (is_file(with_js)) return canonicalize(a, with_js);
    }
    return NULL;
}

/* ------- binding name collection -------------------------------------- */

static void collect_pattern_names(AstNode *p, Map *out) {
    if (!p) return;
    switch (p->kind) {
        case AST_IDENTIFIER:
            map_put(out, p->ident.name, (void *)1);
            return;
        case AST_ARRAY_PATTERN:
            for (size_t i = 0; i < p->array_pattern.elements.len; ++i)
                collect_pattern_names(p->array_pattern.elements.items[i], out);
            return;
        case AST_OBJECT_PATTERN:
            for (size_t i = 0; i < p->object_pattern.properties.len; ++i) {
                AstNode *prop = p->object_pattern.properties.items[i];
                if (prop->kind == AST_REST_ELEMENT) collect_pattern_names(prop->spread_or_rest.arg, out);
                else if (prop->kind == AST_PROPERTY) collect_pattern_names(prop->property.value, out);
            }
            return;
        case AST_ASSIGN_PATTERN:
            collect_pattern_names(p->assign_pattern.left, out);
            return;
        case AST_REST_ELEMENT:
            collect_pattern_names(p->spread_or_rest.arg, out);
            return;
        default: return;
    }
}

static void collect_bindings_and_exports(Bundler *b, ModuleInfo *info) {
    for (size_t i = 0; i < info->ast->block.body.len; ++i) {
        AstNode *node = info->ast->block.body.items[i];
        switch (node->kind) {
            case AST_FN_DECL:
            case AST_CLASS_DECL:
                if (node->func.id || (node->kind == AST_CLASS_DECL && node->class_.id)) {
                    AstNode *id = (node->kind == AST_FN_DECL) ? node->func.id : node->class_.id;
                    map_put(&info->bindings, id->ident.name, (void *)1);
                }
                break;
            case AST_VAR_DECL:
                for (size_t j = 0; j < node->var_decl.declarations.len; ++j) {
                    AstNode *d = node->var_decl.declarations.items[j];
                    collect_pattern_names(d->var_declarator.id, &info->bindings);
                }
                break;
            case AST_EXPORT_NAMED_DECL: {
                AstNode *d = node->export_named.declaration;
                if (d) {
                    if (d->kind == AST_FN_DECL || d->kind == AST_CLASS_DECL) {
                        AstNode *id = (d->kind == AST_FN_DECL) ? d->func.id : d->class_.id;
                        if (id) {
                            map_put(&info->bindings, id->ident.name, (void *)1);
                            map_put(&info->exports, id->ident.name, (void *)id->ident.name);
                        }
                    } else if (d->kind == AST_VAR_DECL) {
                        Map names; map_init(&names);
                        for (size_t j = 0; j < d->var_decl.declarations.len; ++j)
                            collect_pattern_names(d->var_decl.declarations.items[j]->var_declarator.id, &names);
                        size_t it = 0; const char *k; void *v;
                        while (map_iter(&names, &it, &k, &v)) {
                            map_put(&info->bindings, k, (void *)1);
                            map_put(&info->exports, k, (void *)k);
                        }
                        map_free(&names);
                    }
                }
                for (size_t j = 0; j < node->export_named.specifiers.len; ++j) {
                    AstNode *spec = node->export_named.specifiers.items[j];
                    map_put(&info->exports, spec->export_specifier.exported->ident.name,
                                            (void *)spec->export_specifier.local->ident.name);
                }
                break;
            }
            case AST_EXPORT_DEFAULT_DECL: {
                AstNode *d = node->export_default.declaration;
                if ((d->kind == AST_FN_DECL || d->kind == AST_CLASS_DECL)) {
                    AstNode *id = (d->kind == AST_FN_DECL) ? d->func.id : d->class_.id;
                    if (id) {
                        map_put(&info->bindings, id->ident.name, (void *)1);
                        map_put(&info->exports, "default", (void *)id->ident.name);
                        break;
                    }
                }
                map_put(&info->bindings, "_default", (void *)1);
                map_put(&info->exports, "default", (void *)"_default");
                break;
            }
            default: break;
        }
    }
    (void)b;
}

/* ------- rename map ---------------------------------------------------- */

static char *prefixed(Arena *a, const char *prefix, const char *name) {
    size_t pl = strlen(prefix), nl = strlen(name);
    char *out = (char *)arena_alloc(a, pl + nl + 1);
    memcpy(out, prefix, pl);
    memcpy(out + pl, name, nl);
    out[pl + nl] = '\0';
    return out;
}

static bool build_rename_map(Bundler *b, ModuleInfo *info, bool is_entry) {
    if (!is_entry) {
        size_t it = 0; const char *k; void *v;
        while (map_iter(&info->bindings, &it, &k, &v)) {
            map_put(&info->rename_map, k, prefixed(b->arena, info->prefix, k));
        }
    }
    for (size_t i = 0; i < info->imports_len; ++i) {
        AstNode *node = info->import_nodes[i];
        ModuleInfo *src = info->import_sources[i];
        for (size_t j = 0; j < node->import_decl.specifiers.len; ++j) {
            AstNode *spec = node->import_decl.specifiers.items[j];
            if (spec->kind == AST_IMPORT_DEFAULT_SPECIFIER) {
                const char *local_binding = (const char *)map_get(&src->exports, "default");
                if (!local_binding) {
                    mod_set_error(b, node, info->abs_path,
                                  "Module '%s' has no default export", node->import_decl.source);
                    return false;
                }
                map_put(&info->rename_map, spec->import_specifier.local->ident.name,
                                           prefixed(b->arena, src->prefix, local_binding));
            } else if (spec->kind == AST_IMPORT_SPECIFIER) {
                const char *imported = spec->import_specifier.imported->ident.name;
                const char *local_binding = (const char *)map_get(&src->exports, imported);
                if (!local_binding) {
                    mod_set_error(b, node, info->abs_path,
                                  "Module '%s' does not export '%s'", node->import_decl.source, imported);
                    return false;
                }
                map_put(&info->rename_map, spec->import_specifier.local->ident.name,
                                           prefixed(b->arena, src->prefix, local_binding));
            } else if (spec->kind == AST_IMPORT_NAMESPACE_SPECIFIER && !is_entry) {
                const char *local = spec->import_specifier.local->ident.name;
                char *ns_name = (char *)arena_alloc(b->arena, strlen(info->prefix) + 4 + strlen(local) + 1);
                sprintf(ns_name, "%s_ns_%s", info->prefix, local);
                map_put(&info->rename_map, local, ns_name);
            }
        }
    }
    return true;
}

/* ------- identifier rename walk (with parent context) ---------------- */

static bool should_skip_rename(AstNode *parent, const char *parent_key) {
    if (!parent || !parent_key) return false;
    if (parent->kind == AST_PROPERTY && strcmp(parent_key, "key") == 0 && !parent->property.computed) return true;
    if (parent->kind == AST_MEMBER_EXPR && strcmp(parent_key, "property") == 0 && !parent->member.computed) return true;
    if (parent->kind == AST_METHOD_DEF && strcmp(parent_key, "key") == 0 && !parent->method_def.computed) return true;
    if (parent->kind == AST_LABELED_STMT && strcmp(parent_key, "label") == 0) return true;
    if (parent->kind == AST_BREAK_STMT && strcmp(parent_key, "label") == 0) return true;
    if (parent->kind == AST_CONTINUE_STMT && strcmp(parent_key, "label") == 0) return true;
    if (parent->kind == AST_IMPORT_SPECIFIER) return true;
    if (parent->kind == AST_IMPORT_DEFAULT_SPECIFIER) return true;
    if (parent->kind == AST_IMPORT_NAMESPACE_SPECIFIER) return true;
    if (parent->kind == AST_EXPORT_SPECIFIER && strcmp(parent_key, "exported") == 0) return true;
    return false;
}

static void rename_walk(AstNode *node, Map *rmap, AstNode *parent, const char *parent_key);

static void rename_list(NodeList *l, Map *rmap, AstNode *parent, const char *key) {
    for (size_t i = 0; i < l->len; ++i) rename_walk(l->items[i], rmap, parent, key);
}

static void rename_walk(AstNode *node, Map *rmap, AstNode *parent, const char *parent_key) {
    if (!node) return;

    /* Shorthand property fix: acorn shares one Identifier between key and value.
     * In our AST, parse_property already creates a separate value node for shorthand,
     * so we're safe. But mark shorthand=false to be safe (so codegen emits key:value).
     * (Actually our codegen handles shorthand fine, so leave it.) */

    if (node->kind == AST_IDENTIFIER && !should_skip_rename(parent, parent_key)) {
        const char *replacement = (const char *)map_get(rmap, node->ident.name);
        if (replacement) node->ident.name = replacement;
    }

    switch (node->kind) {
        case AST_PROGRAM:
        case AST_BLOCK_STMT:
            rename_list(&node->block.body, rmap, node, "body");
            break;
        case AST_VAR_DECL:
            rename_list(&node->var_decl.declarations, rmap, node, "declarations");
            break;
        case AST_VAR_DECLARATOR:
            rename_walk(node->var_declarator.id, rmap, node, "id");
            rename_walk(node->var_declarator.init, rmap, node, "init");
            break;
        case AST_EXPR_STMT:
            rename_walk(node->expr_stmt.expr, rmap, node, "expression");
            break;
        case AST_IF_STMT:
            rename_walk(node->if_.test, rmap, node, "test");
            rename_walk(node->if_.consequent, rmap, node, "consequent");
            rename_walk(node->if_.alternate, rmap, node, "alternate");
            break;
        case AST_WHILE_STMT:
        case AST_DO_WHILE_STMT:
            rename_walk(node->while_.test, rmap, node, "test");
            rename_walk(node->while_.body, rmap, node, "body");
            break;
        case AST_FOR_STMT:
            rename_walk(node->for_.init, rmap, node, "init");
            rename_walk(node->for_.test, rmap, node, "test");
            rename_walk(node->for_.update, rmap, node, "update");
            rename_walk(node->for_.body, rmap, node, "body");
            break;
        case AST_FOR_IN_STMT:
        case AST_FOR_OF_STMT:
            rename_walk(node->for_each.left, rmap, node, "left");
            rename_walk(node->for_each.right, rmap, node, "right");
            rename_walk(node->for_each.body, rmap, node, "body");
            break;
        case AST_SWITCH_STMT:
            rename_walk(node->switch_.discriminant, rmap, node, "discriminant");
            rename_list(&node->switch_.cases, rmap, node, "cases");
            break;
        case AST_SWITCH_CASE:
            rename_walk(node->switch_case.test, rmap, node, "test");
            rename_list(&node->switch_case.consequent, rmap, node, "consequent");
            break;
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            rename_walk(node->break_continue.label, rmap, node, "label");
            break;
        case AST_RETURN_STMT:
        case AST_THROW_STMT:
            rename_walk(node->unary_stmt.arg, rmap, node, "argument");
            break;
        case AST_TRY_STMT:
            rename_walk(node->try_.block, rmap, node, "block");
            rename_walk(node->try_.handler, rmap, node, "handler");
            rename_walk(node->try_.finalizer, rmap, node, "finalizer");
            break;
        case AST_CATCH_CLAUSE:
            rename_walk(node->catch_.param, rmap, node, "param");
            rename_walk(node->catch_.body, rmap, node, "body");
            break;
        case AST_FN_DECL:
        case AST_FN_EXPR:
        case AST_ARROW_FN_EXPR:
            rename_walk(node->func.id, rmap, node, "id");
            rename_list(&node->func.params, rmap, node, "params");
            rename_walk(node->func.body, rmap, node, "body");
            break;
        case AST_CLASS_DECL:
            rename_walk(node->class_.id, rmap, node, "id");
            rename_walk(node->class_.super_class, rmap, node, "superClass");
            rename_walk(node->class_.body, rmap, node, "body");
            break;
        case AST_CLASS_BODY:
            rename_list(&node->class_body.body, rmap, node, "body");
            break;
        case AST_METHOD_DEF:
            rename_walk(node->method_def.key, rmap, node, "key");
            rename_walk(node->method_def.value, rmap, node, "value");
            break;
        case AST_LABELED_STMT:
            rename_walk(node->labeled.label, rmap, node, "label");
            rename_walk(node->labeled.body, rmap, node, "body");
            break;
        case AST_BINARY_EXPR:
        case AST_LOGICAL_EXPR:
            rename_walk(node->binary.left, rmap, node, "left");
            rename_walk(node->binary.right, rmap, node, "right");
            break;
        case AST_UNARY_EXPR:
            rename_walk(node->unary.arg, rmap, node, "argument");
            break;
        case AST_UPDATE_EXPR:
            rename_walk(node->update.arg, rmap, node, "argument");
            break;
        case AST_ASSIGN_EXPR:
            rename_walk(node->assign.left, rmap, node, "left");
            rename_walk(node->assign.right, rmap, node, "right");
            break;
        case AST_MEMBER_EXPR:
            rename_walk(node->member.object, rmap, node, "object");
            rename_walk(node->member.property, rmap, node, "property");
            break;
        case AST_CALL_EXPR:
        case AST_NEW_EXPR:
            rename_walk(node->call.callee, rmap, node, "callee");
            rename_list(&node->call.args, rmap, node, "arguments");
            break;
        case AST_CONDITIONAL_EXPR:
            rename_walk(node->conditional.test, rmap, node, "test");
            rename_walk(node->conditional.consequent, rmap, node, "consequent");
            rename_walk(node->conditional.alternate, rmap, node, "alternate");
            break;
        case AST_ARRAY_EXPR:
            rename_list(&node->array.elements, rmap, node, "elements");
            break;
        case AST_OBJECT_EXPR:
            rename_list(&node->object.properties, rmap, node, "properties");
            break;
        case AST_PROPERTY:
            rename_walk(node->property.key, rmap, node, "key");
            rename_walk(node->property.value, rmap, node, "value");
            break;
        case AST_TEMPLATE_LITERAL:
            rename_list(&node->template_.expressions, rmap, node, "expressions");
            break;
        case AST_TAGGED_TEMPLATE_EXPR:
            rename_walk(node->tagged_template.tag, rmap, node, "tag");
            rename_walk(node->tagged_template.quasi, rmap, node, "quasi");
            break;
        case AST_SEQUENCE_EXPR:
            rename_list(&node->sequence.expressions, rmap, node, "expressions");
            break;
        case AST_CHAIN_EXPR:
            rename_walk(node->chain.expr, rmap, node, "expression");
            break;
        case AST_SPREAD_ELEMENT:
        case AST_REST_ELEMENT:
            rename_walk(node->spread_or_rest.arg, rmap, node, "argument");
            break;
        case AST_YIELD_EXPR:
        case AST_AWAIT_EXPR:
            rename_walk(node->yield_await.arg, rmap, node, "argument");
            break;
        case AST_ARRAY_PATTERN:
            rename_list(&node->array_pattern.elements, rmap, node, "elements");
            break;
        case AST_OBJECT_PATTERN:
            rename_list(&node->object_pattern.properties, rmap, node, "properties");
            break;
        case AST_ASSIGN_PATTERN:
            rename_walk(node->assign_pattern.left, rmap, node, "left");
            rename_walk(node->assign_pattern.right, rmap, node, "right");
            break;
        case AST_IMPORT_DECL:
            rename_list(&node->import_decl.specifiers, rmap, node, "specifiers");
            break;
        case AST_IMPORT_SPECIFIER:
        case AST_IMPORT_DEFAULT_SPECIFIER:
        case AST_IMPORT_NAMESPACE_SPECIFIER:
            rename_walk(node->import_specifier.imported, rmap, node, "imported");
            rename_walk(node->import_specifier.local, rmap, node, "local");
            break;
        case AST_EXPORT_NAMED_DECL:
            rename_walk(node->export_named.declaration, rmap, node, "declaration");
            rename_list(&node->export_named.specifiers, rmap, node, "specifiers");
            break;
        case AST_EXPORT_DEFAULT_DECL:
            rename_walk(node->export_default.declaration, rmap, node, "declaration");
            break;
        case AST_EXPORT_SPECIFIER:
            rename_walk(node->export_specifier.local, rmap, node, "local");
            rename_walk(node->export_specifier.exported, rmap, node, "exported");
            break;
        case AST_META_PROPERTY:
            rename_walk(node->meta_property.meta, rmap, node, "meta");
            rename_walk(node->meta_property.property, rmap, node, "property");
            break;
        default: break;
    }
}

/* ------- transform body ------------------------------------------------ */

static AstNode *make_ident(Arena *a, const char *name) {
    AstNode *n = ast_new(a, AST_IDENTIFIER);
    n->ident.name = name;
    return n;
}

static AstNode *build_namespace_object(Bundler *b, ModuleInfo *src) {
    AstNode *obj = ast_new(b->arena, AST_OBJECT_EXPR);
    size_t it = 0; const char *export_name; void *v;
    while (map_iter(&src->exports, &it, &export_name, &v)) {
        const char *local = (const char *)v;
        AstNode *prop = ast_new(b->arena, AST_PROPERTY);
        prop->property.key = make_ident(b->arena, export_name);
        prop->property.value = make_ident(b->arena, prefixed(b->arena, src->prefix, local));
        prop->property.computed = false;
        prop->property.shorthand = false;
        nl_push(b->arena, &obj->object.properties, prop);
    }
    return obj;
}

static AstNode *make_const_decl(Arena *a, const char *name, AstNode *init) {
    AstNode *d = ast_new(a, AST_VAR_DECL);
    d->var_decl.kind = VK_CONST;
    AstNode *vd = ast_new(a, AST_VAR_DECLARATOR);
    vd->var_declarator.id = make_ident(a, name);
    vd->var_declarator.init = init;
    nl_push(a, &d->var_decl.declarations, vd);
    return d;
}

static void transform_body(Bundler *b, ModuleInfo *info, bool is_entry, NodeList *out) {
    for (size_t i = 0; i < info->ast->block.body.len; ++i) {
        AstNode *node = info->ast->block.body.items[i];

        if (node->kind == AST_IMPORT_DECL) {
            /* find the source module info for this import decl */
            ModuleInfo *sub = NULL;
            for (size_t k = 0; k < info->imports_len; ++k) {
                if (info->import_nodes[k] == node) { sub = info->import_sources[k]; break; }
            }
            for (size_t j = 0; j < node->import_decl.specifiers.len; ++j) {
                AstNode *spec = node->import_decl.specifiers.items[j];
                if (spec->kind == AST_IMPORT_NAMESPACE_SPECIFIER && sub) {
                    const char *local = spec->import_specifier.local->ident.name;
                    char *local_name;
                    if (is_entry) {
                        local_name = (char *)local;
                    } else {
                        local_name = (char *)arena_alloc(b->arena, strlen(info->prefix) + 4 + strlen(local) + 1);
                        sprintf(local_name, "%s_ns_%s", info->prefix, local);
                    }
                    AstNode *ns = build_namespace_object(b, sub);
                    nl_push(b->arena, out, make_const_decl(b->arena, local_name, ns));
                }
            }
            continue;
        }

        if (node->kind == AST_EXPORT_NAMED_DECL) {
            if (node->export_named.declaration) nl_push(b->arena, out, node->export_named.declaration);
            continue;
        }
        if (node->kind == AST_EXPORT_ALL_DECL) continue;
        if (node->kind == AST_EXPORT_DEFAULT_DECL) {
            AstNode *d = node->export_default.declaration;
            if ((d->kind == AST_FN_DECL || d->kind == AST_CLASS_DECL)) {
                AstNode *id = (d->kind == AST_FN_DECL) ? d->func.id : d->class_.id;
                if (id) { nl_push(b->arena, out, d); continue; }
            }
            /* synthetic _default = <expr> */
            const char *synth = is_entry ? "_default"
                                         : prefixed(b->arena, info->prefix, "_default");
            AstNode *init = d;
            if (d->kind == AST_FN_DECL) {
                AstNode *fe = ast_new(b->arena, AST_FN_EXPR);
                fe->func = d->func;
                init = fe;
            } else if (d->kind == AST_CLASS_DECL) {
                /* no ClassExpression node distinct from ClassDecl in our AST; reuse */
                init = d;
            }
            nl_push(b->arena, out, make_const_decl(b->arena, synth, init));
            continue;
        }

        nl_push(b->arena, out, node);
    }
}

/* ------- module discovery --------------------------------------------- */

static void order_push(Bundler *b, ModuleInfo *m) {
    if (b->order_len == b->order_cap) {
        size_t nc = b->order_cap ? b->order_cap * 2 : 8;
        ModuleInfo **p = (ModuleInfo **)realloc(b->order, nc * sizeof(*p));
        if (!p) { fprintf(stderr, "qsc: oom\n"); abort(); }
        b->order = p;
        b->order_cap = nc;
    }
    b->order[b->order_len++] = m;
}

static void imports_push(Bundler *b, ModuleInfo *info, AstNode *node, ModuleInfo *src) {
    if (info->imports_len == info->imports_cap) {
        size_t nc = info->imports_cap ? info->imports_cap * 2 : 4;
        AstNode **n = (AstNode **)realloc(info->import_nodes, nc * sizeof(*n));
        ModuleInfo **s = (ModuleInfo **)realloc(info->import_sources, nc * sizeof(*s));
        if (!n || !s) { fprintf(stderr, "qsc: oom\n"); abort(); }
        info->import_nodes = n;
        info->import_sources = s;
        info->imports_cap = nc;
    }
    info->import_nodes[info->imports_len] = node;
    info->import_sources[info->imports_len] = src;
    info->imports_len++;
    (void)b;
}

static ModuleInfo *process_module(Bundler *b, const char *abs_path,
                                  AstNode *preparsed_ast,
                                  const char *preparsed_source, size_t preparsed_len) {
    ModuleInfo *cached = (ModuleInfo *)map_get(&b->modules, abs_path);
    if (cached) return cached;

    ModuleInfo *info = (ModuleInfo *)arena_alloc(b->arena, sizeof(ModuleInfo));
    info->abs_path = abs_path;
    char *pre = (char *)arena_alloc(b->arena, 16);
    snprintf(pre, 16, "_m%d_", b->counter++);
    info->prefix = pre;
    map_init(&info->bindings);
    map_init(&info->exports);
    map_init(&info->rename_map);
    map_put(&b->modules, abs_path, info);

    if (preparsed_ast) {
        info->ast = preparsed_ast;
        info->source = preparsed_source;
        info->source_len = preparsed_len;
    } else {
        size_t len = 0;
        char *src = read_file_to_arena(b, abs_path, &len);
        if (!src) {
            mod_set_error(b, NULL, abs_path, "Cannot read module file: %s", abs_path);
            return NULL;
        }
        info->source = src;
        info->source_len = len;
        ParseError perr = {0};
        info->ast = parse_source(src, len, abs_path, b->arena, &perr);
        if (!info->ast) {
            b->err = perr;
            return NULL;
        }
    }

    collect_bindings_and_exports(b, info);

    const char *dir = path_dirname(b->arena, abs_path);
    for (size_t i = 0; i < info->ast->block.body.len; ++i) {
        AstNode *node = info->ast->block.body.items[i];
        if (node->kind == AST_IMPORT_DECL) {
            const char *src_path = node->import_decl.source;
            if (!(src_path[0] == '.' || src_path[0] == '/')) {
                mod_set_error(b, node, abs_path, "Only relative imports are supported: '%s'", src_path);
                return NULL;
            }
            char *resolved = resolve_import(b, dir, src_path);
            if (!resolved) {
                mod_set_error(b, node, abs_path, "Cannot find module '%s'", src_path);
                return NULL;
            }
            ModuleInfo *sub = process_module(b, resolved, NULL, NULL, 0);
            if (!sub) return NULL;
            imports_push(b, info, node, sub);
        }
    }

    order_push(b, info);
    return info;
}

/* ------- top-level ----------------------------------------------------- */

static bool entry_has_modules_decl(AstNode *ast) {
    for (size_t i = 0; i < ast->block.body.len; ++i) {
        AstKind k = ast->block.body.items[i]->kind;
        if (k == AST_IMPORT_DECL || k == AST_EXPORT_NAMED_DECL ||
            k == AST_EXPORT_DEFAULT_DECL || k == AST_EXPORT_ALL_DECL) return true;
    }
    return false;
}

AstNode *bundle_modules(AstNode *entry_ast,
                        const char *entry_source, size_t entry_source_len,
                        const char *entry_filename,
                        Arena *arena, ParseError *out_err) {
    Bundler b;
    memset(&b, 0, sizeof b);
    b.arena = arena;
    map_init(&b.modules);

    /* canonicalize entry path */
    char *entry_abs = canonicalize(arena, entry_filename);
    if (!entry_abs) entry_abs = arena_strdup(arena, entry_filename); /* fall back literal */

    ModuleInfo *entry = process_module(&b, entry_abs, entry_ast, entry_source, entry_source_len);
    if (!entry) {
        if (out_err) *out_err = b.err;
        goto cleanup;
    }
    /* Entry module: empty prefix override. */
    entry->prefix = "";

    /* Fast path */
    if (map_len(&b.modules) == 1 && !entry_has_modules_decl(entry_ast)) {
        if (out_err) memset(out_err, 0, sizeof *out_err);
        free(b.order);
        return entry_ast;
    }

    for (size_t i = 0; i < b.order_len; ++i) {
        ModuleInfo *m = b.order[i];
        if (!build_rename_map(&b, m, m == entry)) {
            if (out_err) *out_err = b.err;
            goto cleanup;
        }
    }
    for (size_t i = 0; i < b.order_len; ++i) {
        ModuleInfo *m = b.order[i];
        rename_walk(m->ast, &m->rename_map, NULL, NULL);
    }

    AstNode *bundle = ast_new(arena, AST_PROGRAM);
    for (size_t i = 0; i < b.order_len; ++i) {
        ModuleInfo *m = b.order[i];
        transform_body(&b, m, m == entry, &bundle->block.body);
    }

    if (out_err) memset(out_err, 0, sizeof *out_err);
    free(b.order);
    /* leak module bindings/exports/rename_map allocations — arena cleans up eventually */
    return bundle;

cleanup:
    free(b.order);
    return NULL;
}
