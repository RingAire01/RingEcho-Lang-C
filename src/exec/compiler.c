#include "compiler.h"
#include "workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void re0_compiler_init(Re0Compiler *c, Re0Backend *backend) {
    if (!c) return;
    c->had_error = false;
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

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

bool re0_compiler_compile_file(Re0Compiler *c, const char *path, const char *output) {
    /* 多文件模块化：workspace 递归加载 import，合并所有顶层语句 */
    Re0Workspace ws;
    re0_workspace_init(&ws, path);
    Re0StmtVec all_stmts = re0_workspace_load(&ws, path, c->arena, &c->errors,
                                               &c->lexer, &c->parser);
    re0_workspace_free(&ws);

    if (Re0StmtVec_len(&all_stmts) == 0) {
        re0_error_append(&c->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "no statements parsed from '%s'", path);
        c->had_error = true;
        return false;
    }

    bool sema_ok = re0_sema_check(&c->sema, &all_stmts);
    if (!sema_ok) { c->had_error = true; return false; }

    re0_lint_run(&c->sema.checked, &c->errors);
    if (!re0_codegen_generate(&c->codegen, &c->sema.checked)) { c->had_error = true; return false; }

    const char *code = re0_codegen_output(&c->codegen);
    if (!code) {
        re0_error_append(&c->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "cannot obtain generated output");
        c->had_error = true;
        return false;
    }

    if (c->backend == &re0_backend_c) {
        if (!re0_build_compile(&c->build, code, output)) { c->had_error = true; return false; }
    } else {
        const char *fname = output ? output : "output.reo.asm";
        FILE *f = fopen(fname, "w");
        if (f) { fputs(code, f); fclose(f); }
        else {
            re0_error_append(&c->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                             "cannot write assembly to '%s'", fname);
            c->had_error = true;
        }
    }

    return !c->had_error;
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
}
