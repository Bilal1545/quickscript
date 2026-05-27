/*
 * builtins.h — fixed lookup tables for codegen.
 *
 * Mirrors src/builtins.js: globals, builtin functions, binary/compound op
 * tables, C-keyword reserved set. Tables are static; init lazily on first use.
 */

#ifndef QSC_BUILTINS_H
#define QSC_BUILTINS_H

#include <stdbool.h>

/* Returns either the original name or "_js_<name>" if it collides with a C keyword. */
const char *safe_name(const char *name);

/* Returns C runtime function name for a JS builtin (e.g. "print" -> "js_print")
 * or NULL if not a recognized builtin. */
const char *builtin_function(const char *name);

/* Returns C expression for a JS global (e.g. "undefined" -> "js_undefined()")
 * or NULL if not a recognized global. */
const char *global_var(const char *name);

/* Returns C runtime function for a JS binary op (e.g. "+" -> "js_add")
 * or NULL if unknown. */
const char *binary_op(const char *op);

/* Returns underlying binary op for a compound op (e.g. "+=" -> "js_add")
 * or NULL if not compound. */
const char *compound_op(const char *op);

bool is_c_keyword(const char *name);

#endif /* QSC_BUILTINS_H */
