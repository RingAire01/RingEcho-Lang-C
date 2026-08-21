# RingEcho-Lang-C

A zero-dependency C implementation of the RingEcho compiler toolchain.

Compiles `.reo` source → C source → gcc → native binary. No LLVM, no external libraries required.

## Status

| Item | Value |
|------|-------|
| Version | 0.2.0 |
| Code size | ~11,300 lines (9,682 C + 1,577 headers, 89 files) |
| Tests | 150 `.reo` end-to-end + 12 GC unit tests |
| Toolchain | 3 binaries: `rev` / `rem` / `rvm` |
| Backends | C source, freestanding C, reo ISA |
| License | MIT |

## Toolchain

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
- Match expressions (value + enum tag)
- Pipeline operator `|>`
- Component keyword
- for-in range + string iteration
- spawn/await concurrency (pthread)
- **GC engine**: 3 modes (none/auto/manual) × 3 algorithms (tracing/arc-cycle/hybrid)
- **LSP server**: JSON-RPC + diagnostics
- Multi-file modules (import with recursive resolution)
- extern C FFI
- char type with full escape support
- Virtual environments (`.renv/`) with bundled stdlib (`std::io`, `std::math`, `std::string`, `std::vec`)
- Project configuration via `ringecho.toml`

## Architecture

```
.reo → Lexer → Parser → AST → Sema → Lint → Codegen(C source) → gcc → binary
```

### GC Subsystem (`include/gc/` + `src/extra/gc/`)
- `gc_engine.c` — unified engine facade (mode/algo dispatch)
- `gc_tracing.c` — mark-sweep (3-color, gray worklist)
- `gc_arc.c` — reference counting + cycle detection (tracing backup)
- `gc_hybrid.c` — OWNED instant + rest tracing
- `gc_events.c` — GC event listener system
- `gc_stats.c` — allocation/collection statistics

### LSP Server (`src/lsp/`)
- `lsp_json.c` — minimal JSON parser
- `lsp_server.c` — JSON-RPC over stdin/stdout

## Testing

```bash
make CONFIG=Debug test          # End-to-end: compile + run all positive tests,
                                # verify expected failures (sema/syntax/runtime)
make test-one FILE=tests/hello.reo   # Run a single test
make check-one FILE=tests/hello.reo  # Type-check a single test
make test-list                       # List all tests
```

| Suite | Location | Driven by |
|-------|----------|-----------|
| End-to-end (68) | `tests/*.reo`, `tests/stdlib/*.reo` | `make test` |
| Module system (5) | `tests/mod/` | manual |
| Rust compat (10) | `tests/rust_compat/` | manual |
| IR regression (67) | `tests/rust_inline/` | manual |
| GC unit tests (12) | `tests/gc_unit_test.c` | standalone compile |

GC unit tests are compiled standalone:

```bash
gcc -Iinclude tests/gc_unit_test.c src/extra/gc/*.c src/base/*.c -o gc_test && ./gc_test
```

## Project Structure

```
src/
  front/          lexer, parser, token, ast, stream
  analysis/       sema, model, scope, builtins, lint
  backend/        codegen, backend_c, backend_reo
  base/           arena, buffer, error, span, types, log
  exec/           compiler, build, workspace, venv, toml_config,
                  rev_main (compiler), rem_main (packages), rvm_main (versions)
  lsp/            lsp_json, lsp_server
  extra/
    gc/           GC engine modules
    re0_event.c   event bus
    re0_manager.c manager pattern
include/          mirrors src/, one init.h per module + re0.h aggregate
tests/            .reo end-to-end tests + C unit tests
```

## License

[MIT](LICENSE) - Copyright (c) 2025-2026 初然 (KaguyaRing) & Ringaire
