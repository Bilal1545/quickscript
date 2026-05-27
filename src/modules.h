/*
 * modules.h — ES-module bundler.
 *
 * Resolves import/export declarations at compile time by inlining every
 * imported file into a single bundled Program AST. Per-module name mangling
 * (prefix _m<N>_) prevents top-level binding collisions.
 *
 * The entry module keeps its original names so error messages stay readable.
 */

#ifndef QSC_MODULES_H
#define QSC_MODULES_H

#include "arena.h"
#include "ast.h"
#include "parser.h"

/* Returns the bundled Program AST. On failure returns NULL and fills out_err.
 * Reuses `entry_ast` for the entry module (no re-parsing). */
AstNode *bundle_modules(AstNode *entry_ast,
                        const char *entry_source, size_t entry_source_len,
                        const char *entry_filename,
                        Arena *arena, ParseError *out_err);

#endif /* QSC_MODULES_H */
