#include "safe.h"
/*
 * workspace.c — 多文件工作区实现
 *
 * 从入口文件开始，解析 import 语句，
 * 递归加载 .reo 依赖文件，合并所有顶层语句。
 */
#include "workspace.h"
#include "lexer.h"
#include "parser.h"
#include "venv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

void re0_workspace_init(Re0Workspace *ws, const char *entry_path) {
    memset(ws, 0, sizeof(*ws));
    /* 提取入口文件目录作为基准路径 */
    char tmp[RE0_MAX_PATH];
    strncpy(tmp, entry_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp)-1] = '\0';
    strncpy(ws->base_dir, dirname(tmp), sizeof(ws->base_dir) - 1);
    ws->base_dir[sizeof(ws->base_dir)-1] = '\0';
}

/* 检查文件是否已加载（去重） */
static bool already_loaded(Re0Workspace *ws, const char *path) {
    for (int i = 0; i < ws->file_count; i++)
        if (strcmp(ws->files[i].path, path) == 0) return true;
    return false;
}

/* 记录已加载文件 */
static void mark_loaded(Re0Workspace *ws, const char *path) {
    if (ws->file_count >= RE0_MAX_FILES) return;
    strncpy(ws->files[ws->file_count].path, path, RE0_MAX_PATH - 1);
    ws->files[ws->file_count].path[RE0_MAX_PATH-1] = '\0';
    ws->files[ws->file_count].loaded = true;
    ws->file_count++;
}

/* 从 import 语句提取依赖文件路径
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

static bool is_safe_module_path(const char *mod) {
    if (!mod || !*mod) return false;
    if (mod[0] == '/') return false;
    for (const char *p = mod; *p; ) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - seg);
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') return false;
        if (*p == '/') p++;
    }
    return true;
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
    /* 1. 尝试项目本地: base_dir/mod.reo */
    if (!write_import_path(out, out_cap, "%s/%s.reo", base_dir, mod, "", "")) return false;
    if (access(out, R_OK) == 0) return true;

    /* 2. 尝试虚拟环境: .renv/lib/std/mod.reo 和 packages/mod.reo */
    char env_dir[512];
    if (reo_venv_detect(env_dir, sizeof(env_dir))) {
        /* 标准库 */
        if (write_import_path(out, out_cap, "%s/%s/%s/%s.reo", env_dir, RE0_VENV_LIB, RE0_VENV_STD, mod) &&
            access(out, R_OK) == 0) return true;
        /* 第三方包 */
        if (write_import_path(out, out_cap, "%s/%s/%s/%s.reo", env_dir, RE0_VENV_LIB, RE0_VENV_PACKAGES, mod) &&
            access(out, R_OK) == 0) return true;
    }

    /* 3. 全局: ~/.re/lib/mod.reo */
    const char *home = getenv("HOME");
    if (home) {
        if (write_import_path(out, out_cap, "%s/%s/%s.reo", home, RE0_GLOBAL_LIB_DIR, mod, "") &&
            access(out, R_OK) == 0) return true;
    }

    /* 4. fallback: base_dir/mod.reo（即使不存在也返回，让上层报错） */
    return write_import_path(out, out_cap, "%s/%s.reo", base_dir, mod, "", "");
}

/* 读取文件内容 */
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
    
    /* 文件大小限制：防止大文件 DoS */
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
        /* 部分读取或错误 */
        free(buf);
        fclose(f);
        return NULL;
    }
    
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* 递归加载文件，返回合并的顶层语句 */
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

    /* 词法分析 */
    if (!re0_lexer_tokenize(lexer, source, path)) {
        free(source);
        return;
    }

    /* 解析 */
    re0_parser_init(parser, arena, errors);
    if (!re0_parser_parse(parser, &lexer->stream)) {
        free(source);
        return;
    }

    /* 快照当前文件的语句（递归调用会重置 parser→stmts） */
    int stmt_count = (int)Re0StmtVec_len(&parser->stmts);
    Re0Stmt **local_stmts = NULL;
    if (stmt_count > 0) {
        local_stmts = (Re0Stmt**)xcalloc((size_t)stmt_count, sizeof(Re0Stmt*));
        for (int i = 0; i < stmt_count; i++)
            local_stmts[i] = parser->stmts.data[i];
    }

    /* 第一遍：递归加载 import 依赖 */
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

    /* 第二遍：收集非 import 语句 */
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
