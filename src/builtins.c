#include "builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *k; const char *v; } Pair;

/* Sorted alphabetically for bsearch. */
static const Pair BUILTIN_FUNCTIONS[] = {
    {"Boolean",    "js_Boolean_fn"},
    {"Number",     "js_Number_fn"},
    {"String",     "js_String_fn"},
    {"ask",        "js_ask_wrapper"},
    {"execute",    "js_execute_wrapper"},
    {"isFinite",   "js_isFinite_fn"},
    {"isNaN",      "js_isNaN_fn"},
    {"parseFloat", "js_parseFloat_fn"},
    {"parseInt",   "js_parseInt_fn"},
    {"print",      "js_print"},
};
#define BUILTIN_FUNCTIONS_LEN (sizeof BUILTIN_FUNCTIONS / sizeof BUILTIN_FUNCTIONS[0])

static const Pair GLOBAL_VARS[] = {
    {"Array",       "js_Array_global"},
    {"Date",        "js_Date_global"},
    {"Error",       "js_Error_global"},
    {"Infinity",    "js_number(INFINITY)"},
    {"JSON",        "js_json"},
    {"Map",         "js_Map_global"},
    {"Math",        "js_math"},
    {"NaN",         "js_number(NAN)"},
    {"Object",      "js_Object_global"},
    {"RangeError",  "js_RangeError_global"},
    {"RegExp",      "js_RegExp_global"},
    {"Set",         "js_Set_global"},
    {"SyntaxError", "js_SyntaxError_global"},
    {"TypeError",   "js_TypeError_global"},
    {"console",     "js_console"},
    {"fs",          "js_fs"},
    {"path",        "js_path"},
    {"process",     "js_process"},
    {"undefined",   "js_undefined()"},
};
#define GLOBAL_VARS_LEN (sizeof GLOBAL_VARS / sizeof GLOBAL_VARS[0])

static const Pair BINARY_OPS[] = {
    {"!=",  "js_neq"},
    {"!==", "js_strict_neq"},
    {"%",   "js_mod"},
    {"&",   "js_bitand"},
    {"*",   "js_mul"},
    {"**",  "js_pow"},
    {"+",   "js_add"},
    {"-",   "js_sub"},
    {"/",   "js_div"},
    {"<",   "js_lt"},
    {"<<",  "js_shl"},
    {"<=",  "js_lte"},
    {"==",  "js_eq"},
    {"===", "js_strict_eq"},
    {">",   "js_gt"},
    {">=",  "js_gte"},
    {">>",  "js_shr"},
    {">>>", "js_ushr"},
    {"^",   "js_bitxor"},
    {"in",  "js_in"},
    {"instanceof", "js_instanceof"},
    {"|",   "js_bitor"},
};
#define BINARY_OPS_LEN (sizeof BINARY_OPS / sizeof BINARY_OPS[0])

static const Pair COMPOUND_OPS[] = {
    {"%=",  "js_mod"},
    {"&=",  "js_bitand"},
    {"**=", "js_pow"},     /* '*' < '=' so **= sorts before *= */
    {"*=",  "js_mul"},
    {"+=",  "js_add"},
    {"-=",  "js_sub"},
    {"/=",  "js_div"},
    {"<<=", "js_shl"},     /* '<' < '=' so <<= sorts before <= */
    {">>>=","js_ushr"},    /* '>' < '=' so >>>= sorts before >>= sorts before >= */
    {">>=", "js_shr"},
    {"^=",  "js_bitxor"},
    {"|=",  "js_bitor"},
};
#define COMPOUND_OPS_LEN (sizeof COMPOUND_OPS / sizeof COMPOUND_OPS[0])

static const char *C_KEYWORDS[] = {
    "EOF", "INFINITY", "NAN", "NULL",
    "_Bool", "_Complex", "_Imaginary",
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "main", "register", "restrict", "return",
    "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
    "union", "unsigned", "void", "volatile", "while",
};
#define C_KEYWORDS_LEN (sizeof C_KEYWORDS / sizeof C_KEYWORDS[0])

static int pair_cmp(const void *a, const void *b) {
    return strcmp(((const Pair *)a)->k, ((const Pair *)b)->k);
}
static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static const char *table_lookup(const Pair *tbl, size_t n, const char *key) {
    Pair k = {key, NULL};
    const Pair *hit = (const Pair *)bsearch(&k, tbl, n, sizeof(Pair), pair_cmp);
    return hit ? hit->v : NULL;
}

bool is_c_keyword(const char *name) {
    return bsearch(&name, C_KEYWORDS, C_KEYWORDS_LEN, sizeof(const char *), str_cmp) != NULL;
}

const char *safe_name(const char *name) {
    if (!is_c_keyword(name)) return name;
    /* "_js_" + name. Caller may keep the result indefinitely; we leak by
     * static buffer for the common short names, but for safety allocate. */
    static __thread char buf[128];
    snprintf(buf, sizeof buf, "_js_%s", name);
    return buf;
    /* NOTE: this is not thread-safe across simultaneous calls within one
     * codegen because the static buffer is reused. Callers MUST arena-copy
     * the result if they need to keep multiple safe-named identifiers live.
     * The codegen layer wraps this via codegen_safe_name() which does so. */
}

const char *builtin_function(const char *name) {
    return table_lookup(BUILTIN_FUNCTIONS, BUILTIN_FUNCTIONS_LEN, name);
}

const char *global_var(const char *name) {
    return table_lookup(GLOBAL_VARS, GLOBAL_VARS_LEN, name);
}

const char *binary_op(const char *op) {
    return table_lookup(BINARY_OPS, BINARY_OPS_LEN, op);
}

const char *compound_op(const char *op) {
    return table_lookup(COMPOUND_OPS, COMPOUND_OPS_LEN, op);
}
