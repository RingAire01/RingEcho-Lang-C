#include "safe.h"
#include "compiler.h"
#include "workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void re0_compiler_init(Re0Compiler *c, Re0Backend *backend) {
    if (!c) return;
    c->had_error = false;
    re0_event_bus_init(&c->bus);
    c->backend = backend;
    re0_error_list_init(&c->errors);
    re0_model_init(&c->model);
    re0_builtin_init(&c->builtins);
    Re0Arena *a = re0_arena_new();
    if (!a) {
        re0_error_append(&c->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "cannot allocate compiler arena");
        c->had_error = true;
        c->arena = NULL;
        return;
    }
    c->arena = a;
    re0_lexer_init(&c->lexer, c->arena, &c->errors);
    re0_parser_init(&c->parser, c->arena, &c->errors);
    re0_sema_init(&c->sema, c->arena, &c->errors, &c->model, &c->builtins);
    re0_codegen_init(&c->codegen, &c->errors, &c->model, backend);
    re0_build_init(&c->build, &c->errors);
    c->gc = re0_gc_new(RE0_GC_DEFAULT_THRESHOLD);
    c->gc_mode = RE0_GC_NONE;
}


typedef struct CompileCtx {
    Re0Compiler   *comp;
    const char    *path;
    const char    *output;
    Re0StmtVec     stmts;
    const char    *code;
} CompileCtx;

typedef struct {
    Re0Manager     base;
    CompileCtx    *ctx;
} StageMgr;

static bool stage_frontend_run(Re0Manager *m) {
    StageMgr *s = (StageMgr*)m;
    CompileCtx *ctx = s->ctx;
    Re0Compiler *c = ctx->comp;
    re0_manager_emit_event(m, RE0_EV_LEXER_START, (void*)ctx->path, ctx->path);
    Re0Workspace ws;
    re0_workspace_init(&ws, ctx->path);
    ctx->stmts = re0_workspace_load(&ws, ctx->path, c->arena, &c->errors,
                                    &c->lexer, &c->parser);
    re0_workspace_free(&ws);
    re0_manager_emit_event(m, RE0_EV_LEXER_DONE, NULL, NULL);
    if (Re0StmtVec_len(&ctx->stmts) == 0) {
        re0_error_append(&c->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "no statements parsed from '%s'", ctx->path);
        c->had_error = true;
        return false;
    }
    return true;
}

static bool stage_sema_run(Re0Manager *m) {
    StageMgr *s = (StageMgr*)m;
    CompileCtx *ctx = s->ctx;
    Re0Compiler *c = ctx->comp;
    if (re0_manager_should_cancel(m)) return false;
    re0_manager_emit_event(m, RE0_EV_SEMA_START, NULL, NULL);
    bool ok = re0_sema_check(&c->sema, &ctx->stmts);
    re0_manager_emit_event(m, RE0_EV_SEMA_DONE, NULL, NULL);
    if (!ok) { c->had_error = true; return false; }
    re0_lint_run(&c->sema.checked, &c->errors);
    return true;
}

static bool stage_codegen_run(Re0Manager *m) {
    StageMgr *s = (StageMgr*)m;
    CompileCtx *ctx = s->ctx;
    Re0Compiler *c = ctx->comp;
    if (re0_manager_should_cancel(m)) return false;
    re0_manager_emit_event(m, RE0_EV_CODEGEN_START, NULL, NULL);
    if (!re0_codegen_generate(&c->codegen, &c->sema.checked)) {
        re0_manager_emit_event(m, RE0_EV_CODEGEN_DONE, NULL, NULL);
        c->had_error = true;
        return false;
    }
    re0_manager_emit_event(m, RE0_EV_CODEGEN_DONE, NULL, NULL);
    ctx->code = re0_codegen_output(&c->codegen);
    if (!ctx->code) {
        re0_error_append(&c->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "cannot obtain generated output");
        c->had_error = true;
        return false;
    }
    return true;
}

static bool stage_build_run(Re0Manager *m) {
    StageMgr *s = (StageMgr*)m;
    CompileCtx *ctx = s->ctx;
    Re0Compiler *c = ctx->comp;
    if (re0_manager_should_cancel(m)) return false;
    re0_manager_emit_event(m, RE0_EV_BUILD_START, NULL, NULL);
    if (c->backend == &re0_backend_c) {
        if (!re0_build_compile(&c->build, ctx->code, ctx->output)) {
            re0_manager_emit_event(m, RE0_EV_BUILD_DONE, NULL, NULL);
            c->had_error = true;
            return false;
        }
    } else {
        const char *fname = ctx->output ? ctx->output : "output.reo.asm";
        FILE *f = fopen(fname, "w");
        if (f) { fputs(ctx->code, f); fclose(f); }
        else {
            re0_error_append(&c->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                             "cannot write assembly to '%s'", fname);
            c->had_error = true;
        }
    }
    re0_manager_emit_event(m, RE0_EV_BUILD_DONE, NULL, NULL);
    return !c->had_error;
}

static StageMgr make_stage(const char *name, Re0EventBus *bus,
                           Re0ErrorList *errors, CompileCtx *ctx,
                           bool (*run)(Re0Manager*)) {
    StageMgr s;
    memset(&s, 0, sizeof(s));
    s.base.name = name;
    s.base.bus = bus;
    s.base.errors = errors;
    s.base.run = run;
    s.ctx = ctx;
    return s;
}

bool re0_compiler_compile_file(Re0Compiler *c, const char *path, const char *output) {
    if (!c || !path) return false;
    re0_event_bus_reset(&c->bus);

    CompileCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.comp = c;
    ctx.path = path;
    ctx.output = output;
    Re0StmtVec_init(&ctx.stmts);

    StageMgr frontend = make_stage("frontend", &c->bus, &c->errors, &ctx, stage_frontend_run);
    StageMgr sema     = make_stage("sema",     &c->bus, &c->errors, &ctx, stage_sema_run);
    StageMgr codegen  = make_stage("codegen",  &c->bus, &c->errors, &ctx, stage_codegen_run);
    StageMgr build    = make_stage("build",    &c->bus, &c->errors, &ctx, stage_build_run);

    Re0Manager root;
    memset(&root, 0, sizeof(root));
    root.name = "compiler";
    root.bus = &c->bus;
    root.errors = &c->errors;
    re0_manager_register_child(&root, &frontend.base);
    re0_manager_register_child(&root, &sema.base);
    re0_manager_register_child(&root, &codegen.base);
    re0_manager_register_child(&root, &build.base);

    bool ok = re0_manager_execute(&root);
    return ok && !c->had_error;
}

bool re0_compiler_run(Re0Compiler *c, const char *path) {
    char tmpname[512];
    if (!re0_build_temp_output_path(&c->build, tmpname, sizeof(tmpname))) {
        c->had_error = true;
        return false;
    }
    if (!re0_compiler_compile_file(c, path, tmpname)) return false;
    if (c->backend == &re0_backend_c) {
        char command[sizeof(tmpname) + 3];
        int command_size = snprintf(command, sizeof(command), "\"%s\"", tmpname);
        if (command_size < 0 || (size_t)command_size >= sizeof(command)) {
            re0_error_append(&c->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                             "temporary executable path exceeds internal limit");
            remove(tmpname);
            c->had_error = true;
            return false;
        }
        int rc = system(command);
        if (remove(tmpname) != 0 && errno != ENOENT) {
            re0_error_append(&c->errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                             "cannot remove temporary executable '%s'", tmpname);
        }
        if (rc != 0) {
            re0_error_append(&c->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                             "program exited with code %d", rc);
            c->had_error = true;
        }
    }
    return !c->had_error;
}

void re0_compiler_destroy(Re0Compiler *c) {
    if (!c) return;
    re0_error_list_free(&c->errors);
    re0_model_free(&c->model);
    re0_builtin_free(&c->builtins);
    re0_lexer_destroy(&c->lexer);
    re0_parser_destroy(&c->parser);
    re0_sema_destroy(&c->sema);
    re0_codegen_destroy(&c->codegen);
    re0_build_destroy(&c->build);
    if (c->gc) re0_gc_destroy(c->gc);
    if (c->arena) re0_arena_free(c->arena);
    re0_event_bus_free(&c->bus);
}
