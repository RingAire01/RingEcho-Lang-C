/*
 * workspace.h — multi-file workspace management
 *
 * Parse import statements, recursively load dependency files,
 * and merge all top-level statements into a single compilation unit.
 */
#ifndef RE0_WORKSPACE_H
#define RE0_WORKSPACE_H

#include "front/ast.h"
#include "base/error.h"
#include "base/arena.h"
#include "front/lexer.h"
#include "front/parser.h"
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

/* Validate an untrusted relative path (import module, toml entry, ...):
 * rejects absolute paths, '..' segments, and ':' (drive letters / NTFS ADS),
 * on both '/' and '\' separators. Single source of truth for path safety. */
bool re0_is_safe_rel_path(const char *path);

/* Parse imports, recursively load dependency files, merge all top-level statements into out.
 * Already-loaded files are not loaded twice (deduplication).
 * arena is used for AST allocation; errors for reporting I/O errors.
 * Returns the merged array of top-level statements. */
Re0StmtVec re0_workspace_load(Re0Workspace *ws, const char *entry_path,
                               Re0Arena *arena, Re0ErrorList *errors,
                               Re0Lexer *lexer, Re0Parser *parser);

void re0_workspace_free(Re0Workspace *ws);

#endif
