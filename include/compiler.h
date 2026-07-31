#ifndef RE0_COMPILER_H
#define RE0_COMPILER_H
#include "arena.h"
#include "error.h"
#include "model.h"
#include "builtins.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "backend.h"
#include "build.h"
#include "lint.h"
#include "re0_gc.h"
#include "re0_event.h"
#include "re0_manager.h"

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
    Re0EventBus         bus;
} Re0Compiler;

void re0_compiler_init(Re0Compiler *c, Re0Backend *backend);
bool re0_compiler_compile_file(Re0Compiler *c, const char *path, const char *output);
bool re0_compiler_run(Re0Compiler *c, const char *path);
void re0_compiler_destroy(Re0Compiler *c);

#endif
