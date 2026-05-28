/*
 * ast.h — ESTree-compatible AST node definitions.
 *
 * All nodes are allocated from a single Arena and never freed individually.
 * Source ranges (start/end byte offsets) and line/column information are
 * tracked on every node for error reporting.
 *
 * The node type enum mirrors ESTree names since codegen mirrors codegen.js
 * one-to-one. Some niche nodes are omitted (WithStatement is technically
 * supported by codegen.js but we don't parse it; runscript code shouldn't
 * use it).
 */

#ifndef QSC_AST_H
#define QSC_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"

typedef enum {
    /* program / module */
    AST_PROGRAM,
    AST_IMPORT_DECL,
    AST_IMPORT_SPECIFIER,           /* { x } or { x as y } */
    AST_IMPORT_DEFAULT_SPECIFIER,   /* default */
    AST_IMPORT_NAMESPACE_SPECIFIER, /* * as ns */
    AST_EXPORT_NAMED_DECL,
    AST_EXPORT_DEFAULT_DECL,
    AST_EXPORT_ALL_DECL,
    AST_EXPORT_SPECIFIER,

    /* statements */
    AST_BLOCK_STMT,
    AST_VAR_DECL,
    AST_VAR_DECLARATOR,
    AST_EXPR_STMT,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_DO_WHILE_STMT,
    AST_FOR_STMT,
    AST_FOR_IN_STMT,
    AST_FOR_OF_STMT,
    AST_SWITCH_STMT,
    AST_SWITCH_CASE,
    AST_BREAK_STMT,
    AST_CONTINUE_STMT,
    AST_RETURN_STMT,
    AST_THROW_STMT,
    AST_TRY_STMT,
    AST_CATCH_CLAUSE,
    AST_FN_DECL,
    AST_CLASS_DECL,
    AST_CLASS_BODY,
    AST_METHOD_DEF,         /* method or constructor inside a class */
    AST_EMPTY_STMT,
    AST_LABELED_STMT,
    AST_DEBUGGER_STMT,
    AST_C_BLOCK,            /* raw C code emitted verbatim (`__c {{{ ... }}}`) */

    /* expressions */
    AST_LITERAL,
    AST_IDENTIFIER,
    AST_THIS_EXPR,
    AST_BINARY_EXPR,
    AST_LOGICAL_EXPR,
    AST_UNARY_EXPR,
    AST_UPDATE_EXPR,
    AST_ASSIGN_EXPR,
    AST_MEMBER_EXPR,
    AST_CALL_EXPR,
    AST_NEW_EXPR,
    AST_CONDITIONAL_EXPR,
    AST_ARRAY_EXPR,
    AST_OBJECT_EXPR,
    AST_PROPERTY,           /* key/value pair inside ObjectExpression / pattern */
    AST_ARROW_FN_EXPR,
    AST_FN_EXPR,
    AST_TEMPLATE_LITERAL,
    AST_TEMPLATE_ELEMENT,   /* raw/cooked piece inside a template literal */
    AST_TAGGED_TEMPLATE_EXPR,
    AST_SEQUENCE_EXPR,
    AST_CHAIN_EXPR,         /* wrapper around an optional-chain expression */
    AST_SPREAD_ELEMENT,
    AST_YIELD_EXPR,
    AST_AWAIT_EXPR,
    AST_META_PROPERTY,
    AST_PAREN_EXPR,         /* not in ESTree, but useful internally; collapsed before codegen */

    /* patterns */
    AST_ARRAY_PATTERN,
    AST_OBJECT_PATTERN,
    AST_REST_ELEMENT,
    AST_ASSIGN_PATTERN,     /* binding with default — used in params and destructuring */

    AST_COUNT
} AstKind;

typedef enum {
    /* literal sub-types */
    LIT_STRING,
    LIT_NUMBER,
    LIT_BOOL,
    LIT_NULL,
    LIT_REGEX,
} LiteralKind;

typedef enum {
    /* variable declaration kind */
    VK_VAR,
    VK_LET,
    VK_CONST,
} VarKind;

typedef enum {
    /* method definition kind */
    MK_METHOD,
    MK_CONSTRUCTOR,
    MK_GETTER,
    MK_SETTER,
} MethodKind;

typedef struct AstNode AstNode;

/* Resizable child array. */
typedef struct {
    AstNode **items;
    size_t len;
    size_t cap;
} NodeList;

struct AstNode {
    AstKind kind;

    /* source range */
    uint32_t start;
    uint32_t end;
    uint32_t line;
    uint32_t col;
    const char *filename;       /* set by parser via make_node; may be NULL */

    /* per-kind payload — only the relevant fields are filled. Memory is cheap
     * (arena-backed), so a single fat struct is simpler than a tagged union. */
    union {
        /* literal */
        struct {
            LiteralKind kind;
            union {
                struct { const char *value; uint32_t len; } str;  /* LIT_STRING */
                double number;                                     /* LIT_NUMBER */
                bool boolean;                                      /* LIT_BOOL */
                struct { const char *pattern; const char *flags; } regex; /* LIT_REGEX */
            } v;
        } literal;

        /* identifier */
        struct { const char *name; } ident;

        /* binary/logical: op string ("+", "&&", etc.) plus left/right */
        struct { const char *op; AstNode *left; AstNode *right; } binary;

        /* unary: prefix=true for ++/-- prefix, false for postfix; op is "+", "-", "!", "~", "typeof", "void", "delete" */
        struct { const char *op; AstNode *arg; bool prefix; } unary;

        /* update: like unary, ++/-- only */
        struct { const char *op; AstNode *arg; bool prefix; } update;

        /* assignment: op is "=", "+=", etc. */
        struct { const char *op; AstNode *left; AstNode *right; } assign;

        /* member: obj.prop or obj[expr]; computed=true if [expr]. optional=true if ?. */
        struct { AstNode *object; AstNode *property; bool computed; bool optional; } member;

        /* call: callee(args...). optional=true if ?.() */
        struct { AstNode *callee; NodeList args; bool optional; } call;

        /* conditional: test ? consequent : alternate */
        struct { AstNode *test; AstNode *consequent; AstNode *alternate; } conditional;

        /* array literal: elements (may contain NULL for holes, SpreadElement for spread) */
        struct { NodeList elements; } array;

        /* object literal: properties (Property or SpreadElement nodes) */
        struct { NodeList properties; } object;

        /* property: key/value with optional shorthand/computed/method flags */
        struct {
            AstNode *key;
            AstNode *value;
            bool computed;
            bool shorthand;
            bool is_method;
            MethodKind method_kind; /* MK_GETTER/MK_SETTER if applicable */
        } property;

        /* function (decl/expr/arrow): id (NULL for anonymous), params, body */
        struct {
            AstNode *id;
            NodeList params;
            AstNode *body;          /* BlockStatement or expression (for arrow) */
            bool is_arrow;
            bool body_is_expr;      /* arrow with expression body */
            bool is_async;
            bool is_generator;
        } func;

        /* class declaration / expression */
        struct {
            AstNode *id;
            AstNode *super_class;   /* optional */
            AstNode *body;          /* AST_CLASS_BODY */
        } class_;

        struct { NodeList body; } class_body;

        struct {
            AstNode *key;
            AstNode *value;         /* FunctionExpression */
            MethodKind kind;
            bool computed;
            bool is_static;
        } method_def;

        /* variable declaration */
        struct { VarKind kind; NodeList declarations; } var_decl;

        /* variable declarator: id (pattern) = init */
        struct { AstNode *id; AstNode *init; } var_declarator;

        /* block statement / program: list of statements */
        struct { NodeList body; } block;

        /* expression statement */
        struct { AstNode *expr; } expr_stmt;

        /* if statement */
        struct { AstNode *test; AstNode *consequent; AstNode *alternate; } if_;

        /* while / do-while */
        struct { AstNode *test; AstNode *body; } while_;

        /* for statement: init/test/update/body. init may be VarDecl or expression. */
        struct { AstNode *init; AstNode *test; AstNode *update; AstNode *body; } for_;

        /* for-in / for-of */
        struct { AstNode *left; AstNode *right; AstNode *body; } for_each;

        /* switch */
        struct { AstNode *discriminant; NodeList cases; } switch_;

        /* switch case */
        struct { AstNode *test; NodeList consequent; } switch_case;

        /* break/continue: optional label */
        struct { AstNode *label; } break_continue;

        /* return / throw */
        struct { AstNode *arg; } unary_stmt;

        /* try/catch/finally */
        struct { AstNode *block; AstNode *handler; AstNode *finalizer; } try_;

        /* catch clause */
        struct { AstNode *param; AstNode *body; } catch_;

        /* labeled statement */
        struct { AstNode *label; AstNode *body; } labeled;

        /* template literal */
        struct { NodeList quasis; NodeList expressions; } template_;

        /* template element */
        struct { const char *cooked; uint32_t cooked_len; bool tail; } template_element;

        /* tagged template */
        struct { AstNode *tag; AstNode *quasi; } tagged_template;

        /* sequence */
        struct { NodeList expressions; } sequence;

        /* chain wrapper */
        struct { AstNode *expr; } chain;

        /* spread / rest */
        struct { AstNode *arg; } spread_or_rest;

        /* yield / await */
        struct { AstNode *arg; bool delegate; } yield_await;

        /* paren expression (collapsed before codegen) */
        struct { AstNode *expr; } paren;

        /* import declaration */
        struct {
            const char *source;     /* module source string */
            NodeList specifiers;
        } import_decl;

        /* import specifier (named) */
        struct {
            AstNode *imported;      /* identifier in the source module (or NULL for default) */
            AstNode *local;         /* identifier in this scope */
        } import_specifier;

        /* export named */
        struct {
            AstNode *declaration;   /* may be NULL */
            NodeList specifiers;    /* may be empty if declaration set */
            const char *source;     /* re-export source, may be NULL */
        } export_named;

        /* export default */
        struct { AstNode *declaration; } export_default;

        /* export all */
        struct { const char *source; AstNode *exported_name; } export_all;

        /* export specifier */
        struct { AstNode *local; AstNode *exported; } export_specifier;

        /* meta property */
        struct { AstNode *meta; AstNode *property; } meta_property;

        /* array pattern / object pattern: element list (may include AssignmentPattern/RestElement) */
        struct { NodeList elements; } array_pattern;
        struct { NodeList properties; } object_pattern;

        /* assignment pattern: target = default */
        struct { AstNode *left; AstNode *right; } assign_pattern;

        /* raw C block — code points into the source arena, NUL-terminated. */
        struct { const char *code; uint32_t len; } c_block;
    };
};

/* Allocation helper. Initializes node->kind and zeroes everything else. */
AstNode *ast_new(Arena *a, AstKind kind);

/* Append a child to a NodeList (arena-backed reallocation via copy on grow). */
void nl_push(Arena *a, NodeList *list, AstNode *child);

/* Walk visitor: enter returns false to skip children. Either callback may be NULL. */
typedef struct {
    void *user;
    bool (*enter)(AstNode *node, void *user);
    void (*leave)(AstNode *node, void *user);
} AstVisitor;

void ast_walk(AstNode *node, const AstVisitor *v);

const char *ast_kind_name(AstKind k);

#endif /* QSC_AST_H */
