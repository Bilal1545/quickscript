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
