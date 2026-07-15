/*
 * workspace.h — 多文件工作区管理
 *
 * 解析 import 语句，递归加载依赖文件，
 * 将所有顶层语句合并为单一编译单元。
 */
#ifndef RE0_WORKSPACE_H
#define RE0_WORKSPACE_H

#include "ast.h"
#include "error.h"
#include "arena.h"
#include "lexer.h"
#include "parser.h"
#include <stdbool.h>

#define RE0_MAX_FILES 64
#define RE0_MAX_PATH 512

typedef struct {
    char path[RE0_MAX_PATH];
    bool loaded;
} Re0FileEntry;

typedef struct {
    Re0FileEntry files[RE0_MAX_FILES];
    int file_count;
    char base_dir[RE0_MAX_PATH];
} Re0Workspace;

void re0_workspace_init(Re0Workspace *ws, const char *entry_path);

/* 解析 import，递归加载依赖文件，合并所有顶层语句到 out。
 * 已加载文件不会重复加载（去重）。
 * arena 用于 AST 分配，errors 用于报告 IO 错误。
 * 返回合并后的顶层语句数组。 */
Re0StmtVec re0_workspace_load(Re0Workspace *ws, const char *entry_path,
                               Re0Arena *arena, Re0ErrorList *errors,
                               Re0Lexer *lexer, Re0Parser *parser);

void re0_workspace_free(Re0Workspace *ws);

#endif
