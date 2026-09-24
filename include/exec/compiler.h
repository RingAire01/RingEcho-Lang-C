#ifndef RE0_COMPILER_H
#define RE0_COMPILER_H
#include "base/arena.h"
#include "base/error.h"
#include "analysis/model.h"
#include "analysis/builtins.h"
#include "front/lexer.h"
#include "front/parser.h"
#include "analysis/sema.h"
#include "backend/backend.h"
#include "exec/build.h"
#include "analysis/lint.h"
#include "extra/re0_gc.h"
#include "extra/re0_event.h"
#include "extra/re0_manager.h"

typedef struct {
    Re0Arena           *arena;
    Re0ErrorList        errors;
    Re0SemanticModel    model;
    Re0BuiltinRegistry  builtins;
    Re0Lexer            lexer;
    Re0Parser           parser;
    Re0Sema             sema;
    Re0Codegen          codegen;
    Re0Build            build;
    Re0Backend         *backend;
    Re0GcPool          *gc;
    Re0GcMode           gc_mode;
    bool                had_error;
    /* When true, compile as a shared library: the C backend emits no `main`
     * entry point and the build links with `-shared -fPIC`. */
    bool                shared;
    /* When true, compile to WebAssembly (WASI) via the freestanding backend
     * plus the wasi-sdk clang. The build writes the C source and invokes
     * clang --target=wasm32-wasi instead of gcc. */
    bool                wasm;
    /* Native backend: emit an ELF64 relocatable object instead of linking. */
    bool                emit_object;
    Re0EventBus         bus;
} Re0Compiler;

void re0_compiler_init(Re0Compiler *c, Re0Backend *backend);
bool re0_compiler_compile_file(Re0Compiler *c, const char *path, const char *output);
bool re0_compiler_run(Re0Compiler *c, const char *path);
void re0_compiler_destroy(Re0Compiler *c);

#endif
