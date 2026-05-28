<div align="center">
  <img src="logo.svg" alt="QuickScript logo" width="200" />
  <br>
  <a href="https://github.com/Bilal1545/quickscript/stargazers">
    <img src="https://img.shields.io/github/stars/Bilal1545/quickscript?style=for-the-badge&logo=github&color=E3B341&logoColor=D9E0EE&labelColor=000000" alt="GitHub stars">
  </a>
  <a href="https://github.com/Bilal1545/quickscript/forks">
    <img src="https://img.shields.io/github/forks/Bilal1545/quickscript?style=for-the-badge&logo=github&color=E3B341&logoColor=D9E0EE&labelColor=000000" alt="GitHub stars">
  </a>
</div>

---

## Why QuickScript?

JavaScript is a widely used and practical language, but its distribution model comes with a recurring limitation: it requires a runtime. In most cases, running a program means installing Node.js or a similar environment, which introduces external dependencies, large node_modules trees, and additional startup overhead even for small programs.

QuickScript is designed to address this specific gap. It keeps a JavaScript-like syntax while compiling .qs files into standalone native binaries. The goal is to remove the need for an external interpreter or runtime on the target system.

## Install

On Linux or macOS:

```sh
curl -fsSL https://raw.githubusercontent.com/Bilal1545/quickscript/main/install.sh | sh
```

On Windows (PowerShell):

```powershell
iwr -useb https://raw.githubusercontent.com/Bilal1545/quickscript/main/install.ps1 | iex
```

That's it — run `qsc --help` to get started. Prebuilt binaries for all three platforms are also available on the [Releases page](https://github.com/Bilal1545/quickscript/releases/latest) if you'd rather download by hand.

### Twilight channel (latest CI build)

Want the freshest build straight from `main` without waiting for a release? Set `QSC_VERSION=twilight` and the installer pulls the most recent CI artifact via [nightly.link](https://nightly.link):

```sh
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/Bilal1545/quickscript/main/install.sh | QSC_VERSION=twilight sh
```

```powershell
# Windows (PowerShell)
$env:QSC_VERSION = "twilight"; iwr -useb https://raw.githubusercontent.com/Bilal1545/quickscript/main/install.ps1 | iex
```

The Windows installer bundles a portable Tiny C Compiler (TCC), so no separate
gcc / MSYS2 setup is needed. On Linux and macOS, `gcc` (or any compatible
`cc`) must already be on your `PATH` — set `QSC_CC` to override the choice
(e.g. `QSC_CC=clang qsc file.qs`).

## Quick start

```sh
qsc test.qs            # produces ./test
qsc test.qs --run      # build and run
qsc -S test.qs -o -    # emit generated C to stdout
```

## Importing C libraries

Because programs are compiled down to C, you can pull functions out of any
C header that is reachable from your toolchain — no FFI, no bindings, no
runtime cost. Prefix the import path with `c:` and list the symbols by
name, just like a regular ES-module import:

```js
import { sqrt, pow } from "c:math"
import { puts, putchar } from "c:stdio"
import { rand, srand, atoi } from "c:stdlib"
import { strlen } from "c:string"

print("sqrt(2) =", sqrt(2))
print("pow(2, 10) =", pow(2, 10))

puts("hello from libc!")
srand(42)
print("rand() =", rand())
```

The string after `c:` is what gets dropped into `#include <…>` — `c:math`
is shorthand for `c:math.h`, and `c:sys/time.h` works too. The compiler
generates a small `JsValue` wrapper around each imported symbol and the
QuickScript-side binding is renamed to avoid colliding with the C symbol
of the same name.

Each C function the compiler knows about carries a signature describing
its return type and arguments. The signatures cover scalars (`int`,
`double`, `float`, `unsigned`, `const char*`, `size_t`, …) and structs
that get marshaled to and from plain QS objects. Unknown symbols fall
back to a default `double f(double)` shape — fine for additional math
functions, but pass the exact number of arguments the C prototype expects.

### Structs

Built-in struct shapes (currently `Color`, `Vector2`, `Vector3`,
`Vector4`, `Rectangle`) are marshaled field-for-field with plain QS
objects. Passing `{ r: 245, g: 245, b: 245, a: 255 }` to a function
expecting `Color` works; returning a `Vector2` yields `{ x, y }`.

### Constants

C macros, enum values, and `const` variables can be imported by name
alongside functions. The compiler materializes each constant once at
startup — scalar values come through as numbers, struct constants
(like raylib's `RAYWHITE` or `BLACK`) become QS objects with the
same fields. Common raylib colors, key codes, mouse-button codes, and
libc math constants (`M_PI`, `M_E`, …) are registered out of the box.

### Raylib example

```js
// @link raylib

import {
    InitWindow, CloseWindow, WindowShouldClose,
    BeginDrawing, EndDrawing, ClearBackground,
    DrawText, DrawCircle, SetTargetFPS,
    GetMouseX, GetMouseY, IsKeyPressed,
    RAYWHITE, BLACK, RED, BLUE, GREEN, YELLOW,
    KEY_SPACE, KEY_R, KEY_G, KEY_B,
} from "c:raylib"

InitWindow(800, 450, "QuickScript meets raylib")
SetTargetFPS(60)

let bg = RAYWHITE
let dot = RED

while (!WindowShouldClose()) {
    if (IsKeyPressed(KEY_R)) { dot = RED }
    if (IsKeyPressed(KEY_G)) { dot = GREEN }
    if (IsKeyPressed(KEY_B)) { dot = BLUE }
    if (IsKeyPressed(KEY_SPACE)) {
        bg = (bg === RAYWHITE) ? YELLOW : RAYWHITE
    }

    BeginDrawing()
    ClearBackground(bg)
    DrawText("R/G/B = color, SPACE = toggle bg", 80, 180, 20, BLACK)
    DrawCircle(GetMouseX(), GetMouseY(), 20, dot)
    EndDrawing()
}

CloseWindow()
```

### Linking extra libraries

`libc` and `libm` are linked automatically. For anything else, either pass
`-l<name>` on the command line or drop a `// @link` directive into the
source:

```js
// @link raylib

import { GetTime, WindowShouldClose, CloseWindow } from "c:raylib"
print("raylib GetTime() =", GetTime())
```

Equivalently:

```sh
qsc demo.qs -l raylib --run
qsc demo.qs -lraylib --run
qsc demo.qs --link raylib --run
```

The directive must be the first non-whitespace content of its comment
(`// no @link here` and `@link` mentions inside string literals are
intentionally ignored). Each imported module is scanned, so a library
dependency travels with the file that needs it.

## Scripting stdlib

Three globals are available out of the box for shell-style scripts —
`process`, `fs`, and `path`. They mirror the corresponding Node.js
surface for the most common operations:

```js
// arguments + environment
print(process.argv)          // [ binary_path, ...user_args ]
print(process.cwd())
print(process.env.HOME)
print(process.platform)      // "linux" | "darwin" | "win32"
if (process.argv.length < 2) process.exit(1)

// path utilities
path.join("src", "x", "..", "y.qs")   // "src/x/../y.qs"
path.basename("/tmp/foo.txt")          // "foo.txt"
path.basename("/tmp/foo.txt", ".txt")  // "foo"
path.dirname("/tmp/foo.txt")           // "/tmp"
path.extname("note.tar.gz")            // ".gz"
path.isAbsolute("/etc")                // true
path.sep                                // "/" on POSIX, "\\" on Windows

// file system
fs.writeFile("out.txt", "hello\n")
let body = fs.readFile("out.txt")
fs.exists("out.txt")          // true
fs.readDir(".")               // [ "src", "README.md", ... ]
fs.mkdir("build")
fs.remove("out.txt")          // file or empty directory
```

`fs.readFile` returns the file body as a string and throws on missing
files; `fs.writeFile` overwrites. `fs.readDir` lists entries excluding
`.` / `..`. Error cases (`Error` instances) are thrown with a descriptive
message — catch them with `try { ... } catch (e) { ... }` if you'd rather
handle them yourself.

### Subprocess

```js
let r = spawn("git", ["rev-parse", "HEAD"])
if (r.status !== 0) {
    process.exit(r.status)
}
print("HEAD =", r.stdout.trim())
```

`spawn(cmd, [args])` blocks until the child exits and returns
`{ stdout, stderr, status, signal }`. The argv list is `execvp`-style —
no shell parsing, no globbing. Pass an explicit shell if you need one:
`spawn("sh", ["-c", "ls | wc -l"])`. POSIX only for now; Windows throws a
clear "not yet supported" error.

### HTTP client

```js
let r = http.get("http://example.com/")
print(r.status, r.statusText)
print(r.headers["content-type"])
print(r.body.slice(0, 80))

let p = http.post("http://api.example.com/items", JSON.stringify({n: 1}),
                  { contentType: "application/json" })
print("posted:", p.status)
```

Synchronous, plaintext HTTP/1.0 with `Connection: close` — no chunked
transfer, no keep-alive, no TLS. Hitting an `https://` URL raises a
clear error; for HTTPS today, `spawn("curl", [...])` is the pragmatic
escape hatch. The response object carries `status`, `statusText`, a
lowercase-keyed `headers` object, and `body` as a string.

## Embedded assets

Prefix an import source with `asset:` to bake a file's contents directly
into the compiled binary. Useful for distributing a single self-contained
executable with templates, configs, SQL schemas, HTML, or any other text
payload — no sidecar files to ship:

```js
import config   from "asset:./config.json"
import schema   from "asset:./schema.sql"
import template from "asset:./welcome.html"

let cfg = JSON.parse(config)
print("loaded", cfg.name, "from", config.length, "bytes")
```

The path is resolved relative to the importing module, read at compile
time, and emitted as a `static const unsigned char[]` array next to the
generated C. Only the default-import form is supported, and the binding
behaves like a plain QS string. Embedded NUL bytes in binary assets will
truncate the string at the first NUL — for binary payloads, reach into
the underlying `_ai_<local>_data` symbol from an inline C block (the
full byte length is available as `sizeof(_ai_<local>_data) - 1`).

## Inline C blocks

When a function-by-function FFI binding feels too coarse, you can drop raw C
straight into a QuickScript source with a `__c {{{ ... }}}` block. The body
is emitted verbatim into the generated C, wrapped in its own scope:

```js
let n = 100

__c {{{
    int sum = 0;
    for (int i = 0; i < (int)n->number; i++) {
        sum += i;
    }
    printf("sum of 0..%d = %d\n", (int)n->number, sum);
}}}

print("done")
```

QuickScript variables in scope are visible inside the block as `JsValue *`
locals — read `n->number`, `n->string`, etc., and assign with the usual
`js_number(...) / js_string(...)` constructors. Names that collide with C
keywords (`int`, `new`, `class`, …) are mangled to `_js_<name>` in the
generated C, so reference them by that mangled form.

The delimiter is `{{{` … `}}}`; embedded C string literals, char literals,
and `//` / `/* */` comments are scanned over, so `printf("}}}\n")` inside a
block does not end it early. For file-scope helpers (structs, typedefs,
static functions), keep using an external `.c` file with `// @link`.

## Cross-compiling

Pass `--target=<name>` to build for another platform from your dev box.
`qsc` picks the toolchain in this order:

1. `zig cc -target <triple>` if [`zig`](https://ziglang.org/) is on `PATH`
   (recommended — one binary covers every target out of the box)
2. `<prefix>-gcc` matching the target's GNU triple (e.g. `x86_64-w64-mingw32-gcc`)

Friendly aliases plus any raw triple are accepted:

```sh
qsc --target=windows hello.qs        # → ./hello.exe (static, no DLLs)
qsc --target=win32   hello.qs        # 32-bit MinGW
qsc --target=linux   hello.qs        # ELF, host arch
qsc --target=aarch64-linux-musl app.qs   # raw triple (zig handles this)
```

Windows targets enable `-static` automatically so the resulting `.exe` is
a single self-contained file — no `libwinpthread-1.dll` alongside.
`--run` is skipped for cross-builds (the produced binary may not run
natively on the host).

## Usage

```
qsc file.qs                 build to ./file (extension stripped)
qsc file.qs -o name         build to ./name
qsc -S file.qs              emit C source (no linking) to out.c
qsc -S file.qs -o foo.c     emit C source to foo.c
qsc -S file.qs -o -         emit C source to stdout
qsc --run file.qs           build and run

qsc --ast file.qs           dump AST after parse + bundle
qsc --tokens file.qs        dump lexer token stream
qsc --self-test             run internal foundation tests
```

Environment variables:

- `QSC_RUNTIME_DIR` — overrides where `runtime.c` is looked up (default: path baked in at build time).
- `QSC_CC` — overrides the C compiler used for the final link step. By default qsc picks the bundled `tcc.exe` next to `runtime.c` if present (Windows installer ships one), otherwise falls back to `gcc`.

## Advanced: build from source

Requirements: `gcc` (or compatible CC), `make`, a POSIX environment (Linux, macOS, or Windows via MSYS2).

```sh
cd src
make                # builds the qsc binary
./qsc ../test.qs    # run it directly from the build dir
make clean          # removes intermediate artifacts
```

Default compiler flags: `-std=gnu11 -Wall -Wextra -O2 -g`. The `RUNTIME_DIR` variable bakes the path to `runtime.c` into the binary at build time — override it (`make RUNTIME_DIR=/some/path`) if you intend to relocate the binary, or set `QSC_RUNTIME_DIR` at runtime.

## License

Licensed under the Apache License, Version 2.0. See [`LICENSE`](LICENSE) for the full text.
