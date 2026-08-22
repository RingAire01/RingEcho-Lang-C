#include "base/safe.h"
/*
 * workspace.c — multi-file workspace implementation
 *
 * Starting from the entry file, parse import statements,
 * recursively load .reo dependency files, merge all top-level statements.
 */
#include "exec/workspace.h"
#include "platform.h"
#include "front/lexer.h"
#include "front/parser.h"
#include "exec/venv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(RE0_PLATFORM_WINDOWS)
#include <direct.h>
#include <io.h>
#define access _access
#ifndef R_OK
#define R_OK 4
#endif

static void dirname_impl(char *path) {
    if (!path || !*path) return;
    char *last_sep = strrchr(path, '\\');
    if (!last_sep) last_sep = strrchr(path, '/');
    if (last_sep) {
        *last_sep = '\0';
        if (!*path) strcpy(path, ".");
    } else {
        strcpy(path, ".");
    }
}
#else
#include <unistd.h>
#include <libgen.h>
#define dirname_impl(path) do { char *_p = dirname(path); memmove(path, _p, strlen(_p) + 1); } while(0)
#endif

void re0_workspace_init(Re0Workspace *ws, const char *entry_path) {
    memset(ws, 0, sizeof(*ws));
    /* extract entry file directory as base path */
    char tmp[RE0_MAX_PATH];
    strncpy(tmp, entry_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp)-1] = '\0';
    dirname_impl(tmp);
    snprintf(ws->base_dir, sizeof(ws->base_dir), "%s", tmp);
}

/* check if a file is already loaded (deduplication) */
static bool already_loaded(Re0Workspace *ws, const char *path) {
    for (int i = 0; i < ws->file_count; i++)
        if (strcmp(ws->files[i].path, path) == 0) return true;
    return false;
}

/* record loaded file */
static void mark_loaded(Re0Workspace *ws, const char *path) {
    if (ws->file_count >= RE0_MAX_FILES) return;
    strncpy(ws->files[ws->file_count].path, path, RE0_MAX_PATH - 1);
    ws->files[ws->file_count].path[RE0_MAX_PATH-1] = '\0';
    ws->files[ws->file_count].loaded = true;
    ws->file_count++;
}

/* extract dependency file path from import statement
 * import "math"           → math.reo
 * import "utils/helpers"  → utils/helpers.reo
 * from "math" { add }     → math.reo
 */
static bool write_import_path(char *out, size_t out_cap, const char *format,
                              const char *first, const char *second,
                              const char *third, const char *fourth) {
    int written = snprintf(out, out_cap, format, first, second, third, fourth);
    if (written < 0 || (size_t)written >= out_cap) {
        if (out_cap > 0) out[0] = '\0';
        return false;
    }
    return true;
}

/* Single source of truth for untrusted relative-path validation.
 * Handles both separators so Windows backslash tricks cannot bypass
 * the '..' check; rejects ':' to block drive letters and NTFS ADS. */
bool re0_is_safe_rel_path(const char *path) {
    if (!path || !*path) return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    for (const char *p = path; *p; ) {
        const char *seg = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t seglen = (size_t)(p - seg);
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') return false;
        for (size_t i = 0; i < seglen; i++)
            if (seg[i] == ':') return false;
        if (*p == '/' || *p == '\\') p++;
    }
    return true;
}

static bool is_safe_module_path(const char *mod) {
    return re0_is_safe_rel_path(mod);
}

static bool resolve_import_path(Re0Stmt *stmt, char *out, size_t out_cap,
                                const char *base_dir) {
    out[0] = '\0';
    const char *mod = NULL;
    if (stmt->kind == STMT_IMPORT) {
        mod = stmt->import.module;
    }
    if (!mod || out_cap == 0) return false;
    if (!is_safe_module_path(mod)) return false;
    /* 1. try project-local: base_dir/mod.reo */
    if (!write_import_path(out, out_cap, "%s/%s.reo", base_dir, mod, "", "")) return false;
    if (access(out, R_OK) == 0) return true;

    /* 2. try virtual environment: .renv/lib/std/mod.reo and packages/mod.reo */
    char env_dir[512];
    if (reo_venv_detect(env_dir, sizeof(env_dir))) {
        /* standard library */
        if (write_import_path(out, out_cap, "%s/%s/%s/%s.reo", env_dir, RE0_VENV_LIB, RE0_VENV_STD, mod) &&
            access(out, R_OK) == 0) return true;
        /* third-party packages */
        if (write_import_path(out, out_cap, "%s/%s/%s/%s.reo", env_dir, RE0_VENV_LIB, RE0_VENV_PACKAGES, mod) &&
            access(out, R_OK) == 0) return true;
    }

    /* 3. global: ~/.re/lib/mod.reo */
    const char *home =
#if defined(RE0_PLATFORM_WINDOWS)
        getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
        getenv("HOME");
#endif
    if (home) {
        if (write_import_path(out, out_cap, "%s/%s/%s.reo", home, RE0_GLOBAL_LIB_DIR, mod, "") &&
            access(out, R_OK) == 0) return true;
    }

    /* 4. fallback: base_dir/mod.reo (return even if missing; let caller report error) */
    return write_import_path(out, out_cap, "%s/%s.reo", base_dir, mod, "", "");
}

/* read file content */
static char *read_file_content(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    
    /* file size limit: prevent large-file DoS */
    if (sz > RE0_MAX_SOURCE_BYTES) {
        fclose(f);
        return NULL;
    }
    
    char *buf = (char*)xmalloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t rd = fread(buf, 1, (size_t)sz, f);
    if (rd != (size_t)sz) {
        /* partial read or error */
        free(buf);
        fclose(f);
        return NULL;
    }
    
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* recursively load files, return merged top-level stmts */
static void load_file_recursive(Re0Workspace *ws, const char *path,
                                 Re0Arena *arena, Re0ErrorList *errors,
                                 Re0Lexer *lexer, Re0Parser *parser,
                                 Re0StmtVec *out_stmts) {
    if (already_loaded(ws, path)) return;
    mark_loaded(ws, path);

    char *source = read_file_content(path);
    if (!source) {
        re0_error_append(errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "cannot read imported file '%s'", path);
        return;
    }

    /* lexical analysis */
    if (!re0_lexer_tokenize(lexer, source, path)) {
        free(source);
        return;
    }

    /* parse */
    re0_parser_init(parser, arena, errors);
    if (!re0_parser_parse(parser, &lexer->stream)) {
        free(source);
        return;
    }

    /* snapshot current file's statements (recursive calls reset parser→stmts) */
    int stmt_count = (int)Re0StmtVec_len(&parser->stmts);
    Re0Stmt **local_stmts = NULL;
    if (stmt_count > 0) {
        local_stmts = (Re0Stmt**)xcalloc((size_t)stmt_count, sizeof(Re0Stmt*));
        for (int i = 0; i < stmt_count; i++)
            local_stmts[i] = parser->stmts.data[i];
    }

    /* first pass: recursively load import dependencies */
    for (int i = 0; i < stmt_count; i++) {
        Re0Stmt *s = local_stmts[i];
        if (s && s->kind == STMT_IMPORT) {
            char dep_path[RE0_MAX_PATH];
            if (resolve_import_path(s, dep_path, sizeof(dep_path), ws->base_dir) && dep_path[0])
                load_file_recursive(ws, dep_path, arena, errors,
                                     lexer, parser, out_stmts);
            else
                re0_error_append(errors, RE0_ERR_IO, s->span, NULL,
                                 "import path exceeds %d bytes", RE0_MAX_PATH - 1);
        }
    }

    /* second pass: collect non-import statements */
    for (int i = 0; i < stmt_count; i++) {
        if (local_stmts[i] && local_stmts[i]->kind != STMT_IMPORT)
            Re0StmtVec_push(out_stmts, local_stmts[i]);
    }

    free(local_stmts);
    free(source);
}

Re0StmtVec re0_workspace_load(Re0Workspace *ws, const char *entry_path,
                               Re0Arena *arena, Re0ErrorList *errors,
                               Re0Lexer *lexer, Re0Parser *parser) {
    Re0StmtVec all_stmts;
    Re0StmtVec_init(&all_stmts);

    load_file_recursive(ws, entry_path, arena, errors,
                        lexer, parser, &all_stmts);

    return all_stmts;
}

void re0_workspace_free(Re0Workspace *ws) {
    memset(ws, 0, sizeof(*ws));
}
