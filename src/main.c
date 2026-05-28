/*
 * qsc — runscript compiler.
 *
 * gcc-style usage:
 *     qsc input.qs              -> ./a.out
 *     qsc input.qs -o myprog    -> ./myprog
 *     qsc -S input.qs           -> writes out.c (no linking)
 *     qsc -S input.qs -o foo.c  -> writes foo.c
 *     qsc --run input.qs        -> compile + run
 *     qsc --ast input.qs        -> dump AST
 *     qsc --tokens input.qs     -> dump token stream
 *     qsc --self-test           -> run internal foundation tests
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <io.h>          /* _access, _isatty, _fileno */
  #include <process.h>     /* _spawnvp, _getpid, _P_WAIT */
  #ifndef F_OK
  #define F_OK 0
  #endif
  #define access  _access
  #define getpid  _getpid
  #define isatty  _isatty
  #define fileno  _fileno
  #define PATH_SEP '\\'
#else
  #include <sys/wait.h>
  #include <unistd.h>
  #define PATH_SEP '/'
#endif

#include "arena.h"
#include "ast.h"
#include "buffer.h"
#include "codegen.h"
#include "errors.h"
#include "lexer.h"
#include "map.h"
#include "modules.h"
#include "parser.h"

#ifndef QSC_RUNTIME_DIR
#define QSC_RUNTIME_DIR "."
#endif

/* ---- cross-compile target ------------------------------------------- */

typedef struct {
    const char *name;        /* canonical CLI name */
    const char *zig_triple;  /* passed to `zig cc -target ...` */
    const char *gcc_prefix;  /* used to look up `<prefix>-gcc` */
    const char *suffix;      /* output extension, e.g. ".exe" or "" */
    bool is_windows;
} TargetSpec;

static const TargetSpec TARGETS[] = {
    {"windows", "x86_64-windows-gnu", "x86_64-w64-mingw32", ".exe", true},
    {"win64",   "x86_64-windows-gnu", "x86_64-w64-mingw32", ".exe", true},
    {"win32",   "i686-windows-gnu",   "i686-w64-mingw32",   ".exe", true},
    {"linux",   "x86_64-linux-gnu",   "x86_64-linux-gnu",   "",     false},
    {"linux64", "x86_64-linux-gnu",   "x86_64-linux-gnu",   "",     false},
};
#define TARGETS_LEN (sizeof TARGETS / sizeof TARGETS[0])

/* Resolves `name` into a target spec. Returns a static spec when the name is
 * one of the friendly aliases; otherwise synthesizes one from the raw triple
 * (e.g. "aarch64-linux-musl") and writes it into `buf`. */
static const TargetSpec *resolve_target(const char *name, TargetSpec *buf) {
    for (size_t i = 0; i < TARGETS_LEN; ++i) {
        if (strcmp(TARGETS[i].name, name) == 0) return &TARGETS[i];
    }
    buf->name = name;
    buf->zig_triple = name;
    buf->gcc_prefix = name;
    buf->suffix = strstr(name, "windows") || strstr(name, "mingw") ? ".exe" : "";
    buf->is_windows = (strstr(name, "windows") != NULL) || (strstr(name, "mingw") != NULL);
    return buf;
}

/* ---- portable subprocess + temp file helpers ------------------------- */

/* Wait-for-completion subprocess. Returns child exit status, or -1 on failure
 * to spawn / abnormal exit. Diagnostic message printed on failure. */
static int run_subprocess(const char *prog, char *const argv[]) {
#ifdef _WIN32
    intptr_t rc = _spawnvp(_P_WAIT, prog, (const char *const *)argv);
    if (rc == -1) {
        fprintf(stderr, "qsc: cannot exec %s: %s\n", prog, strerror(errno));
        return -1;
    }
    return (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "qsc: fork failed: %s\n", strerror(errno)); return -1; }
    if (pid == 0) {
        execvp(prog, argv);
        fprintf(stderr, "qsc: cannot exec %s: %s\n", prog, strerror(errno));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* Create a temp .c file. Writes path to out_path and opens it for writing.
 * Returns NULL on failure. Caller fclose()s and unlink()s. */
static FILE *open_temp_c(char *out_path, size_t out_size) {
    const char *tmpdir;
#ifdef _WIN32
    tmpdir = getenv("TEMP");
    if (!tmpdir || !*tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir || !*tmpdir) tmpdir = ".";
#else
    tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
#endif
    static unsigned counter = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        unsigned u = ((unsigned)time(NULL) ^ ((unsigned)getpid() << 8)) + counter++;
        snprintf(out_path, out_size, "%s%cqsc-%u-%d.c", tmpdir, PATH_SEP, u, attempt);
        /* Open with exclusive create where the C library supports it; fall back to
         * plain "wb" otherwise (race window is small and qsc is single-user). */
        FILE *f = fopen(out_path, "wb");
        if (f) return f;
    }
    return NULL;
}

/* ANSI colors — disabled when stderr is not a TTY. */
static const char *C_RED = "", *C_GREEN = "", *C_RESET = "";

static void init_colors(void) {
    if (isatty(fileno(stderr))) { C_RED = "\x1b[31m"; C_GREEN = "\x1b[32m"; C_RESET = "\x1b[0m"; }
}

/* ---- helpers --------------------------------------------------------- */

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%sqsc: cannot open '%s': %s%s\n", C_RED, path, strerror(errno), C_RESET);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static const char *runtime_dir(void) {
    const char *env = getenv("QSC_RUNTIME_DIR");
    return env && env[0] ? env : QSC_RUNTIME_DIR;
}

/* Build runtime path "<dir>/runtime.c" (returned in static buffer). */
static const char *runtime_c_path(void) {
    static char buf[1024];
    snprintf(buf, sizeof buf, "%s/runtime.c", runtime_dir());
    return buf;
}

/* Print a CompileError-style failure (also used for non-parse stages). */
static void emit_err(QscErrorType type, const char *file, const char *msg,
                     uint32_t line, uint32_t col, const char *src, size_t src_len) {
    QscError e = {.type = type, .message = msg, .file = file,
                  .line = line, .column = col, .has_loc = line != 0};
    fputs(C_RED, stderr);
    qsc_error_print(&e, src, src_len);
    fputs(C_RESET, stderr);
}

/* ---- pipeline: parse → bundle → codegen ----------------------------- */

/* Returns C source string (caller frees) or NULL on failure.
 * `link_libs` may be NULL (no scan output) or pre-populated by the caller
 * with CLI -l flags; the bundler appends `// @link` directives discovered
 * across all bundled sources. */
static char *compile_to_c(const char *input_path, char **out_source, size_t *out_len,
                          LinkList *link_libs) {
    size_t len = 0;
    char *src = read_file(input_path, &len);
    if (!src) return NULL;
    *out_source = src;
    *out_len = len;

    static Arena a;
    arena_init(&a);

    ParseError perr = {0};
    AstNode *prog = parse_source(src, len, input_path, &a, &perr);
    if (!prog) { emit_err(QSC_ERR_PARSE, input_path, perr.message, perr.line, perr.col, src, len); arena_free(&a); return NULL; }

    CImportList c_imports;
    AssetList assets;
    cimport_list_init(&c_imports);
    asset_list_init(&assets);
    AstNode *bundled = bundle_modules(prog, src, len, input_path, &a, &perr, &c_imports, link_libs, &assets);
    if (!bundled) {
        emit_err(QSC_ERR_PARSE, input_path, perr.message, perr.line, perr.col, src, len);
        cimport_list_free(&c_imports);
        asset_list_free(&assets);
        arena_free(&a);
        return NULL;
    }

    CodegenError cerr = {0};
    char *c_src = codegen_generate(bundled, src, len, input_path, &a, &cerr, &c_imports, &assets);
    cimport_list_free(&c_imports);
    asset_list_free(&assets);
    if (!c_src) { emit_err(QSC_ERR_CODEGEN, input_path, cerr.message, cerr.line, cerr.col, src, len); arena_free(&a); return NULL; }

    /* Note: we leak the arena across builds intentionally; main exits afterward. */
    return c_src;
}

/* ---- C compiler invocation ------------------------------------------- */

/* Search $PATH for an executable named `name`. Returns a static buffer with
 * the resolved full path, or NULL if not found. Caller must not free. */
static const char *find_in_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*path) return NULL;
    static char buf[1024];
    size_t nl = strlen(name);
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, ':');
#ifdef _WIN32
        const char *sep2 = strchr(p, ';');
        if (sep2 && (!sep || sep2 < sep)) sep = sep2;
#endif
        size_t dl = sep ? (size_t)(sep - p) : strlen(p);
        if (dl > 0 && dl + 1 + nl < sizeof buf) {
            memcpy(buf, p, dl);
            buf[dl] = '/';
            memcpy(buf + dl + 1, name, nl + 1);
            if (access(buf, F_OK) == 0) return buf;
#ifdef _WIN32
            /* Also try with .exe suffix on Windows. */
            if (dl + 1 + nl + 4 < sizeof buf) {
                memcpy(buf + dl + 1 + nl, ".exe", 5);
                if (access(buf, F_OK) == 0) return buf;
            }
#endif
        }
        if (!sep) break;
        p = sep + 1;
    }
    return NULL;
}

/* Compiler selection precedence (host build):
 *   1. $QSC_CC override
 *   2. bundled tcc.exe / tcc next to runtime.c (Windows installer ships this)
 *   3. system gcc
 *
 * Cross-compile (target != NULL):
 *   1. zig cc -target <triple>     — if `zig` is on PATH
 *   2. <prefix>-gcc                — e.g. x86_64-w64-mingw32-gcc
 *   On failure returns NULL and prints a diagnostic.
 */
static const char *pick_cc(void) {
    const char *env = getenv("QSC_CC");
    if (env && env[0]) return env;

    static char buf[1024];
    const char *dir = runtime_dir();
    snprintf(buf, sizeof buf, "%s/tcc.exe", dir);
    if (access(buf, F_OK) == 0) return buf;
    snprintf(buf, sizeof buf, "%s/tcc", dir);
    if (access(buf, F_OK) == 0) return buf;
    return "gcc";
}

/* Returns either the host pick_cc() result with empty prefix args, or a
 * cross-compiler invocation (e.g. `zig cc -target X`). Writes prefix args
 * into `pre_args` (terminating with NULL) and returns the executable name.
 * On failure returns NULL. */
static const char *pick_cross_cc(const TargetSpec *t, char **pre_args, size_t pre_cap) {
    pre_args[0] = NULL;
    if (!t) return pick_cc();

    if (find_in_path("zig")) {
        if (pre_cap < 3) return NULL;
        pre_args[0] = "cc";
        pre_args[1] = "-target";
        pre_args[2] = (char *)t->zig_triple;
        pre_args[3] = NULL;
        return "zig";
    }

    static char gcc_name[256];
    snprintf(gcc_name, sizeof gcc_name, "%s-gcc", t->gcc_prefix);
    if (find_in_path(gcc_name)) {
        pre_args[0] = NULL;
        return gcc_name;
    }

    fprintf(stderr,
        "qsc: no cross-compiler found for target '%s'.\n"
        "     install `zig` (recommended) or `%s-gcc` and retry.\n",
        t->name, t->gcc_prefix);
    return NULL;
}

static int cc_is_tcc(const char *cc) {
    const char *base = strrchr(cc, '/');
    base = base ? base + 1 : cc;
    const char *back = strrchr(base, '\\');
    if (back) base = back + 1;
    return strncmp(base, "tcc", 3) == 0;
}

/* Windows TCC has no separate libm — math is in the bundled runtime.
 * Detect by .exe suffix; Linux/macOS tcc still wants -lm. */
static int cc_skip_libm(const char *cc) {
    if (!cc_is_tcc(cc)) return 0;
    size_t n = strlen(cc);
    return n >= 4 && strcmp(cc + n - 4, ".exe") == 0;
}

static int run_cc(const char *c_path, const char *out_path,
                  const LinkList *extra_libs, const TargetSpec *target) {
    char *pre_args[8] = {0};
    const char *cc = target ? pick_cross_cc(target, pre_args, 8) : pick_cc();
    if (!cc) return 1;
    int skip_libm = (!target) && cc_skip_libm(cc);
    const char *rt = runtime_c_path();
    char include_flag[1024];
    snprintf(include_flag, sizeof include_flag, "-I%s", runtime_dir());

    /* Build argv dynamically so we can append `-l<name>` for each extra lib. */
    size_t pre_n = 0;
    while (pre_n < 8 && pre_args[pre_n]) pre_n++;
    size_t extras = extra_libs ? extra_libs->len : 0;
    size_t fixed = skip_libm ? 7 : 8;  /* cc, -o, out, -I..., c_path, rt, [-lm,] -w */
    /* +2 leaves headroom for an optional `-static` on Windows targets. */
    char **argv = (char **)calloc(fixed + pre_n + extras + 2, sizeof(char *));
    if (!argv) { fprintf(stderr, "qsc: oom\n"); return 1; }
    size_t k = 0;
    argv[k++] = (char *)cc;
    for (size_t i = 0; i < pre_n; ++i) argv[k++] = pre_args[i];
    argv[k++] = "-o";
    argv[k++] = (char *)out_path;
    argv[k++] = include_flag;
    argv[k++] = (char *)c_path;
    argv[k++] = (char *)rt;
    if (!skip_libm) argv[k++] = "-lm";
    argv[k++] = "-w";
    /* Windows cross-builds: link statically so the resulting .exe is a single
     * file. Mingw's default pulls in libwinpthread-1.dll otherwise. */
    if (target && target->is_windows) argv[k++] = "-static";
    for (size_t i = 0; i < extras; ++i) {
        const char *name = extra_libs->items[i];
        size_t nl = strlen(name);
        char *flag = (char *)malloc(nl + 3);
        if (!flag) { fprintf(stderr, "qsc: oom\n"); free(argv); return 1; }
        flag[0] = '-'; flag[1] = 'l';
        memcpy(flag + 2, name, nl + 1);
        argv[k++] = flag;
    }
    argv[k] = NULL;

    int rc = run_subprocess(cc, argv);
    /* Leak the -l<name> strings — process is about to exit anyway. */
    free(argv);
    if (rc == 0) return 0;
    QscError e = {.type = QSC_ERR_BUILD, .message = "C compiler invocation failed", .file = NULL};
    fputs(C_RED, stderr);
    qsc_error_print(&e, NULL, 0);
    fputs(C_RESET, stderr);
    return 1;
}

static int run_binary(const char *path) {
    char *argv[2] = { (char *)path, NULL };
    int rc = run_subprocess(path, argv);
    return rc < 0 ? 1 : rc;
}

/* ---- modes ----------------------------------------------------------- */

static int write_string_to(const char *path, const char *content) {
    if (strcmp(path, "-") == 0) { fputs(content, stdout); return 0; }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "qsc: cannot write '%s': %s\n", path, strerror(errno)); return 1; }
    fputs(content, f);
    fclose(f);
    return 0;
}

static int mode_emit_c(const char *input, const char *output) {
    char *src; size_t len;
    LinkList libs; link_list_init(&libs);
    char *c = compile_to_c(input, &src, &len, &libs);
    link_list_free(&libs);
    free(src);
    if (!c) return 1;
    int rc = write_string_to(output ? output : "out.c", c);
    free(c);
    return rc;
}

/* Derive default output name from input path:
 *   /path/to/hello.qs -> hello   (hello.exe on Windows)
 *   utils.js          -> utils
 *   Makefile          -> a       (no extension; avoid overwriting input)
 * Result is owned by the caller's buffer. */
static void default_output_name(const char *input, char *out, size_t out_size,
                                const TargetSpec *target) {
    const char *base = strrchr(input, '/');
    base = base ? base + 1 : input;
#ifdef _WIN32
    const char *back = strrchr(base, '\\');
    if (back) base = back + 1;
#endif
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || !dot) { snprintf(out, out_size, "a"); goto suffix; }
    if (n >= out_size) n = out_size - 1;
    memcpy(out, base, n);
    out[n] = '\0';
suffix: {
    /* Target suffix takes precedence; for native builds fall back to the
     * host convention (.exe on Windows hosts). */
    const char *want = target ? target->suffix : NULL;
#ifdef _WIN32
    if (!want || !want[0]) want = ".exe";
#endif
    if (want && want[0]) {
        size_t used = strlen(out);
        size_t sl = strlen(want);
        if (used >= sl && strcmp(out + used - sl, want) == 0) return;
        if (used + sl + 1 <= out_size) {
            memcpy(out + used, want, sl + 1);
        }
    }
    return;
}
}

static int mode_build(const char *input, const char *output, bool then_run,
                      const LinkList *cli_libs, const TargetSpec *target) {
    char *src; size_t len;
    LinkList libs; link_list_init(&libs);
    /* Seed with CLI -l flags so they appear before scanned directives. */
    if (cli_libs) {
        for (size_t i = 0; i < cli_libs->len; ++i) link_list_add(&libs, cli_libs->items[i]);
    }
    char *c = compile_to_c(input, &src, &len, &libs);
    free(src);
    if (!c) { link_list_free(&libs); return 1; }

    char tmpl[1024];
    FILE *tf = open_temp_c(tmpl, sizeof tmpl);
    if (!tf) { fprintf(stderr, "qsc: cannot create temp file: %s\n", strerror(errno)); free(c); link_list_free(&libs); return 1; }
    fputs(c, tf);
    fclose(tf);
    free(c);

    char default_name[1024];
    if (!output) {
        default_output_name(input, default_name, sizeof default_name, target);
        output = default_name;
    }
    const char *out = output;
    int rc = run_cc(tmpl, out, &libs, target);
    link_list_free(&libs);
    remove(tmpl);
    if (rc != 0) return rc;

    /* Display path: prepend "./" only when the user gave a bare filename
     * so the user knows where it landed. Absolute/relative paths stay as-is. */
    const char *display = out;
    char dbuf[1100];
    if (out[0] != '/' && out[0] != '.') {
        snprintf(dbuf, sizeof dbuf, "./%s", out);
        display = dbuf;
    }
    fprintf(stderr, "%s✔ compiled → %s%s\n", C_GREEN, display, C_RESET);

    if (then_run) {
        if (target) {
            fprintf(stderr, "qsc: --run skipped: cross-compiled for '%s'\n", target->name);
            return 0;
        }
        return run_binary(display);
    }
    return 0;
}

/* ---- debug subcommands ---------------------------------------------- */

typedef struct { int depth; } AstPrintCtx;

static bool ast_print_enter(AstNode *n, void *user) {
    AstPrintCtx *c = (AstPrintCtx *)user;
    for (int i = 0; i < c->depth; ++i) fputs("  ", stdout);
    printf("%s @%u:%u", ast_kind_name(n->kind), n->line, n->col);
    if (n->kind == AST_IDENTIFIER) printf(" %s", n->ident.name);
    else if (n->kind == AST_LITERAL) {
        switch (n->literal.kind) {
            case LIT_STRING: printf(" \"%.*s\"", (int)n->literal.v.str.len, n->literal.v.str.value); break;
            case LIT_NUMBER: printf(" %g", n->literal.v.number); break;
            case LIT_BOOL:   printf(" %s", n->literal.v.boolean ? "true" : "false"); break;
            case LIT_NULL:   printf(" null"); break;
            case LIT_REGEX:  printf(" /%s/%s", n->literal.v.regex.pattern, n->literal.v.regex.flags); break;
        }
    } else if (n->kind == AST_BINARY_EXPR || n->kind == AST_LOGICAL_EXPR) printf(" %s", n->binary.op);
    else if (n->kind == AST_UNARY_EXPR) printf(" %s", n->unary.op);
    else if (n->kind == AST_UPDATE_EXPR) printf(" %s %s", n->update.op, n->update.prefix ? "(prefix)" : "(postfix)");
    else if (n->kind == AST_ASSIGN_EXPR) printf(" %s", n->assign.op);
    else if (n->kind == AST_VAR_DECL) printf(" %s", n->var_decl.kind == VK_LET ? "let" : n->var_decl.kind == VK_CONST ? "const" : "var");
    else if (n->kind == AST_TEMPLATE_ELEMENT) printf(" \"%.*s\"%s", (int)n->template_element.cooked_len, n->template_element.cooked, n->template_element.tail ? " [tail]" : "");
    printf("\n");
    c->depth++;
    return true;
}
static void ast_print_leave(AstNode *n, void *user) { (void)n; ((AstPrintCtx *)user)->depth--; }

static int mode_dump_ast(const char *path) {
    size_t len; char *src = read_file(path, &len);
    if (!src) return 1;
    Arena a; arena_init(&a);
    ParseError err = {0};
    AstNode *prog = parse_source(src, len, path, &a, &err);
    if (!prog) { emit_err(QSC_ERR_PARSE, path, err.message, err.line, err.col, src, len); arena_free(&a); free(src); return 1; }
    AstNode *bundled = bundle_modules(prog, src, len, path, &a, &err, NULL, NULL, NULL);
    if (!bundled) { emit_err(QSC_ERR_PARSE, path, err.message, err.line, err.col, src, len); arena_free(&a); free(src); return 1; }
    AstPrintCtx ctx = {0};
    AstVisitor v = {.user = &ctx, .enter = ast_print_enter, .leave = ast_print_leave};
    ast_walk(bundled, &v);
    arena_free(&a);
    free(src);
    return 0;
}

static int mode_dump_tokens(const char *path) {
    size_t len; char *src = read_file(path, &len);
    if (!src) return 1;
    Arena a; arena_init(&a);
    Lexer l; lex_init(&l, src, len, path, &a);
    Token tok;
    while (lex_next(&l, &tok)) {
        printf("%4u:%-3u %-18s", tok.line, tok.col, tk_name(tok.kind));
        if (tok.had_line_break_before) printf(" [nl]");
        if (tok.kind == TK_IDENT) printf(" %.*s", (int)(tok.end - tok.start), src + tok.start);
        else if (tok.kind == TK_NUMBER) printf(" %g", tok.number_val);
        else if (tok.kind == TK_STRING || tok.kind == TK_TEMPLATE_HEAD ||
                 tok.kind == TK_TEMPLATE_MIDDLE || tok.kind == TK_TEMPLATE_TAIL ||
                 tok.kind == TK_REGEX) {
            printf(" %.*s", (int)tok.str_len, tok.str_val);
            if (tok.kind == TK_REGEX && tok.flags_val) printf(" /%s", tok.flags_val);
        }
        printf("\n");
        if (tok.kind == TK_EOF) break;
    }
    int rc = l.err.present ? 1 : 0;
    if (rc) fprintf(stderr, "%s:%u:%u lex error: %s\n", path, l.err.line, l.err.col, l.err.message);
    lex_free(&l);
    arena_free(&a);
    free(src);
    return rc;
}

/* ---- self-test (foundation modules) --------------------------------- */

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "self-test FAIL: %s @ %s:%d\n", #cond, __FILE__, __LINE__); return 1; } } while (0)

static int run_self_test(void) {
    Arena a; arena_init(&a);
    char *s1 = arena_strdup(&a, "hello");
    char *s2 = arena_strndup(&a, "world!!", 5);
    CHECK(strcmp(s1, "hello") == 0);
    CHECK(strcmp(s2, "world") == 0);
    CHECK(arena_alloc(&a, 100000) != NULL);
    arena_free(&a);

    Buffer b; buf_init(&b);
    buf_append_str(&b, "hello, ");
    buf_appendf(&b, "%s = %d", "answer", 42);
    char *out = buf_take(&b);
    CHECK(strcmp(out, "hello, answer = 42") == 0);
    free(out);
    buf_free(&b);

    Map m; map_init(&m);
    CHECK(!map_put(&m, "alpha", (void *)1));
    CHECK(!map_put(&m, "beta",  (void *)2));
    CHECK(map_put(&m, "beta",  (void *)20));
    CHECK(map_get(&m, "beta") == (void *)20);
    size_t it = 0; const char *k; void *v;
    CHECK(map_iter(&m, &it, &k, &v) && strcmp(k, "alpha") == 0);
    CHECK(map_iter(&m, &it, &k, &v) && strcmp(k, "beta") == 0);
    CHECK(!map_iter(&m, &it, &k, &v));
    map_free(&m);

    fprintf(stderr, "self-test OK\n");
    return 0;
}

/* ---- argv parsing ---------------------------------------------------- */

static void print_usage(FILE *f) {
    fprintf(f,
        "Usage: qsc [options] <input.qs>\n"
        "\n"
        "  qsc file.qs                 build to ./file       (basename, extension stripped)\n"
        "  qsc file.qs -o name         build to ./name\n"
        "  qsc -S file.qs              emit generated C to out.c (no linking)\n"
        "  qsc -S file.qs -o foo.c     emit generated C to foo.c\n"
        "  qsc -S file.qs -o -         emit generated C to stdout\n"
        "  qsc --run file.qs           build and run\n"
        "\n"
        "Cross-compile:\n"
        "  --target <name>             windows | win64 | win32 | linux | <raw triple>\n"
        "                              uses `zig cc` if available, otherwise `<prefix>-gcc`\n"
        "\n"
        "Link flags:\n"
        "  -l<name>  /  -l <name>      link against lib<name> (repeatable)\n"
        "  --link <name>               same as -l <name>\n"
        "  Sources may also include `// @link raylib GL pthread` directives.\n"
        "\n"
        "Debug subcommands:\n"
        "  qsc --ast file.qs           dump AST after parse + bundle\n"
        "  qsc --tokens file.qs        dump lexer tokens\n"
        "  qsc --self-test             run internal foundation tests\n"
        "\n"
        "Environment:\n"
        "  QSC_RUNTIME_DIR             override runtime.c lookup directory\n"
        "                              (default: %s)\n",
        QSC_RUNTIME_DIR);
}

int main(int argc, char **argv) {
    init_colors();

    if (argc < 2) { print_usage(stderr); return 2; }

    bool emit_c_only = false;
    bool then_run = false;
    bool dump_ast = false;
    bool dump_tokens = false;
    const char *output = NULL;
    const char *input = NULL;
    const char *target_name = NULL;
    LinkList cli_libs; link_list_init(&cli_libs);

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { print_usage(stdout); link_list_free(&cli_libs); return 0; }
        if (strcmp(a, "--self-test") == 0) { link_list_free(&cli_libs); return run_self_test(); }
        if (strcmp(a, "-S") == 0 || strcmp(a, "-c") == 0) { emit_c_only = true; continue; }
        if (strcmp(a, "--run") == 0) { then_run = true; continue; }
        if (strcmp(a, "--ast") == 0) { dump_ast = true; continue; }
        if (strcmp(a, "--tokens") == 0) { dump_tokens = true; continue; }
        if (strncmp(a, "--target=", 9) == 0) { target_name = a + 9; continue; }
        if (strcmp(a, "--target") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "qsc: --target needs an argument\n"); link_list_free(&cli_libs); return 2; }
            target_name = argv[++i];
            continue;
        }
        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "qsc: -o needs an argument\n"); link_list_free(&cli_libs); return 2; }
            output = argv[++i];
            continue;
        }
        if (strcmp(a, "--link") == 0 || strcmp(a, "-l") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "qsc: %s needs an argument\n", a); link_list_free(&cli_libs); return 2; }
            link_list_add(&cli_libs, argv[++i]);
            continue;
        }
        if (a[0] == '-' && a[1] == 'l' && a[2] != '\0') {  /* -lname (gcc-style) */
            link_list_add(&cli_libs, a + 2);
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "qsc: unknown option '%s' (try --help)\n", a);
            link_list_free(&cli_libs);
            return 2;
        }
        if (input) { fprintf(stderr, "qsc: multiple input files not supported\n"); link_list_free(&cli_libs); return 2; }
        input = a;
    }

    if (!input) { fprintf(stderr, "qsc: no input file\n"); print_usage(stderr); link_list_free(&cli_libs); return 2; }

    TargetSpec custom_target;
    const TargetSpec *target = target_name ? resolve_target(target_name, &custom_target) : NULL;

    int rc;
    if (dump_tokens) rc = mode_dump_tokens(input);
    else if (dump_ast) rc = mode_dump_ast(input);
    else if (emit_c_only) rc = mode_emit_c(input, output);
    else rc = mode_build(input, output, then_run, &cli_libs, target);
    link_list_free(&cli_libs);
    return rc;
}
