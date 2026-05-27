<div align="center">
  <img src="logo.svg" alt="Quickscript logo" width="200" />
  <p style="font-size: 3rem;">Quickscript</p>
  <a href="https://github.com/Bilal1545/quickscript/stargazers">
    <img src="https://img.shields.io/github/stars/Bilal1545/quickscript?style=for-the-badge&logo=github&color=E3B341&logoColor=D9E0EE&labelColor=000000" alt="GitHub stars">
  </a>
  <a href="https://github.com/Bilal1545/quickscript/forks">
    <img src="https://img.shields.io/github/forks/Bilal1545/quickscript?style=for-the-badge&logo=github&color=E3B341&logoColor=D9E0EE&labelColor=000000" alt="GitHub stars">
  </a>
  <img src="https://img.shields.io/github/actions/workflow/status/Bilal1545/quickscript/ci.yml?style=for-the-badge" />
</div>

---

## Why quickscript?

JavaScript is a widely used and practical language, but its distribution model comes with a recurring limitation: it requires a runtime. In most cases, running a program means installing Node.js or a similar environment, which introduces external dependencies, large node_modules trees, and additional startup overhead even for small programs.

quickscript is designed to address this specific gap. It keeps a JavaScript-like syntax while compiling .qs files into standalone native binaries. The goal is to remove the need for an external interpreter or runtime on the target system.

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
