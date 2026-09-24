# RingEcho-Lang-C

A zero-dependency C implementation of the RingEcho compiler toolchain.

Compiles `.reo` source → C source → gcc → native binary. No LLVM, no external libraries required.

English | [简体中文](README.zh.md) | [繁體中文](README.zht.md)

## Status

| Item | Value |
|------|-------|
| Version | 0.2.0 |
| Tests | Passing (make test) |
| Toolchain | 3 binaries: `rev` / `rem` / `rvm` |
| Backends | C source, freestanding C, reo ISA, shared library, WebAssembly (WASI) |
| License | MIT |

## Toolchain

An experimental `--backend native` emits x86-64 machine code and ELF64 objects
directly, using the system linker for Linux executables without invoking a C
compiler. It currently supports 8–64-bit integers, floats, booleans and byte characters; see
[NATIVE.md](NATIVE.md) for commands, supported features and limitations.
The full type/layout migration is tracked in [TYPE_SYSTEM.md](TYPE_SYSTEM.md).

| Binary | Role | Description |
|--------|------|-------------|
| `rev` | Compiler / evaluator | Compile, run, type-check, LSP server, project init, venv |
| `rem` | Package manager | Install/list/remove/search packages (npm-like) |
| `rvm` | Version manager | Manage installed RingEcho compiler versions (nvm-like) |

## Build

```bash
make CONFIG=Release build
```

| Profile | Flags |
|---------|-------|
| Release | `-O3 -DNDEBUG` |
| Debug | `-O0 -g3` |
| Alpha | `-O2 -g` |

Requires a C11 compiler (gcc/clang) with pthread support. See [BUILD.md](BUILD.md) for platform matrix.

## Usage

### rev — compiler

```bash
rev run <file.reo> [--target c|reo|c-freestanding]  # Compile and run
rev build <file.reo> [-o out]                       # Compile to executable/asm
rev build <file.reo> --shared                       # Compile to shared library (.so/.dll)
rev build <file.reo> --target wasm                  # Compile to WebAssembly (WASI)
rev build                                           # Build project (reads ringecho.toml)
rev check <file.reo>                                # Type check only
rev lsp                                             # Start LSP server
rev venv <init|activate>                            # Manage virtual environment
rev init [name]                                     # Create new project
rev clean                                           # Remove build artifacts
```

`rev init` scaffolds a project with `main.reo`, `ringecho.toml`, `.gitignore` and a `.renv/` virtual environment. Argumentless `rev build` reads the entry point from `ringecho.toml` and outputs to `target/Release/`.

### rem — package manager

```bash
rem init [name]        # Initialize new project
rem install <pkg>      # Install a package
rem list               # List installed packages
rem remove <pkg>       # Remove a package
rem update             # Update dependencies
rem search <query>     # Search packages
rem publish            # Publish current package
```

### rvm — version manager

```bash
rvm install <version>  # Install a RingEcho version
rvm use <version>      # Switch to a version
rvm list               # List installed versions
rvm current            # Show current version
rvm uninstall <ver>    # Remove a version
rvm remote             # List available versions
```

## Features

- Full type system (i8-i128, u8-u128, f32/f64, bool, char, str)
- Struct, Enum, Trait + impl + static method dispatch
- Generic functions + structs with lazy monomorphization
- Option/Result + `?` operator
- Lambda/closures (no capture yet)
- Match expressions (integer, float, bool, char, string content + enum tag; typed scalar results)
- Pipeline operator `|>`
- Component keyword
- for-in range + string iteration
- spawn/await concurrency (pthread)
- GC engine: 3 modes (none/auto/manual) × 3 algorithms (tracing/arc-cycle/hybrid)
- LSP server: JSON-RPC + diagnostics
- Multi-file modules (import with recursive resolution)
- extern C FFI
- char type with full escape support
- Virtual environments (`.renv/`) with bundled stdlib (`std::io`, `std::math`, `std::string`, `std::vec`)
- Project configuration via `ringecho.toml`

## Testing

`match` evaluates its subject once and executes only the first matching arm.
Patterns must be assignable to the subject type; arm results must be assignable
to the first arm's type. A wildcard `_` must be last. Exhaustiveness is not yet
checked: unmatched numeric results default to zero and string results to the
empty string. Prefer an explicit wildcard arm.

Run output-asserting match regressions with `make CONFIG=Release test-match`
(requires Python 3).

```bash
make CONFIG=Debug test          # End-to-end: compile + run all positive tests,
                                # verify expected failures (sema/syntax/runtime)
make test-one FILE=tests/hello.reo   # Run a single test
make check-one FILE=tests/hello.reo  # Type-check a single test
make test-list                       # List all tests
```

## License

[MIT](LICENSE) - Copyright (c) 2025-2026 辉夜铃 (KaguyaRing) & Ringaire
