/*
 * modules.h — ES-module bundler.
 *
 * Resolves import/export declarations at compile time by inlining every
 * imported file into a single bundled Program AST. Per-module name mangling
 * (prefix _m<N>_) prevents top-level binding collisions.
 *
 * The entry module keeps its original names so error messages stay readable.
 *
 * C-header imports: a module source starting with "c:" (e.g. "c:math.h") is
 * not parsed as a QS file. The named specifiers are collected into a
 * CImportList that the codegen consumes to emit `#include <hdr>` directives
 * and JsValue wrappers around the underlying C functions.
 */

#ifndef QSC_MODULES_H
#define QSC_MODULES_H

#include <stddef.h>

#include "arena.h"
#include "ast.h"
#include "parser.h"

typedef struct {
    const char *header;     /* e.g. "math.h" — placed inside #include <...> */
    const char *imported;   /* C symbol name, e.g. "sqrt" */
    const char *local;      /* binding name in generated C (post-rename) */
} CImport;

typedef struct {
    CImport *items;
    size_t len;
    size_t cap;
} CImportList;

void cimport_list_init(CImportList *l);
void cimport_list_free(CImportList *l);

/* Asset import: `import data from "asset:./path"` reads the file at compile
 * time and embeds its bytes verbatim into the binary. `local` is the
 * post-rename C symbol the codegen binds to; `data`/`len` is the file
 * content read into the arena. */
typedef struct {
    const char *local;          /* arena-owned, e.g. "_ai_data" */
    const unsigned char *data;  /* arena-owned, NUL-terminated */
    size_t len;                 /* byte length excluding the trailing NUL */
    const char *source_path;    /* arena-owned, absolute */
} AssetImport;

typedef struct {
    AssetImport *items;
    size_t len;
    size_t cap;
} AssetList;

void asset_list_init(AssetList *l);
void asset_list_free(AssetList *l);

/* Library names to pass to the linker as `-l<name>` (e.g. "raylib", "pthread").
 * Collected from `// @link <names>` directives scanned across all bundled
 * sources, and merged with anything the caller adds via CLI. */
typedef struct {
    const char **items;     /* arena-owned strings */
    size_t len;
    size_t cap;
} LinkList;

void link_list_init(LinkList *l);
void link_list_free(LinkList *l);
/* Append `name` if not already present. */
void link_list_add(LinkList *l, const char *name);

/* Returns the bundled Program AST. On failure returns NULL and fills out_err.
 * Reuses `entry_ast` for the entry module (no re-parsing).
 *
 * `out_c_imports` may be NULL if the caller does not care about C imports
 * (e.g. AST dump). When non-NULL, the bundler appends one entry per imported
 * C symbol; the caller owns the list and must free it. */
AstNode *bundle_modules(AstNode *entry_ast,
                        const char *entry_source, size_t entry_source_len,
                        const char *entry_filename,
                        Arena *arena, ParseError *out_err,
                        CImportList *out_c_imports,
                        LinkList *out_link_libs,
                        AssetList *out_assets);

#endif /* QSC_MODULES_H */
