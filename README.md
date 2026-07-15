# RingEcho-Lang-C

A zero-dependency C implementation of the RingEcho compiler.

Compiles `.reo` source → C source → gcc → native binary. No LLVM, no external libraries required.

## Status

| Item | Value |
|------|-------|
| Version | 0.2.0 |
| Lines | ~8,700 |
| Tests | 46 .reo + 12 GC unit |
| Backend | C source generation + reo ISA |
| License | MIT |

## Build

```bash
make CONFIG=Release build
```

| Profile | Flags |
|---------|-------|
| Release | `-O3 -DNDEBUG` |
| Debug | `-O0 -g3` |
| Alpha | `-O2 -g` |

## Usage

```bash
rem run <file.reo>              # Compile and run
rem build <file.reo> -o out     # Compile to executable
rem check <file.reo>            # Type check only
rem lsp                         # Start LSP server
rem init [name]                 # Create new project
rem clean                       # Remove build artifacts
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

## Project Structure

```
src/
  front/          lexer, parser, token, ast
  analysis/       sema, model, scope, builtins, lint
  backend/        codegen, backend_c, backend_reo
  base/           arena, buffer, error, span, types, vec
  exec/           compiler, build, main, workspace
  lsp/            lsp_json, lsp_server
  extra/
    gc/           GC engine modules
    re0_event.c   event bus
    re0_manager.c manager pattern
include/
  gc/             GC module headers
```

## License

[MIT](LICENSE) - Copyright (c) 2025-2026 初然Neko (ChuranNeko) & Ringaire
