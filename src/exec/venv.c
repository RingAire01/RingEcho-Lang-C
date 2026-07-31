/*
 * venv.c — RingEcho 虚拟环境管理实现
 */
#include "venv.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if defined(RE0_PLATFORM_WINDOWS)
#include <direct.h>
#include <io.h>
#include <windows.h>
#define mkdir(path, mode) _mkdir(path)
#define access _access
#define R_OK 4
#else
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#endif
#include "re0_log.h"

/* ── 路径工具 ── */

static void join_path(char *out, size_t cap, const char *a, const char *b) {
    if (!out || cap == 0) return;
    int written = snprintf(out, cap, "%s/%s", a, b);
    if (written < 0 || (size_t)written >= cap) out[0] = '\0';
}

static bool dir_exists(const char *path) {
#if defined(RE0_PLATFORM_WINDOWS)
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool ensure_dir(const char *path) {
    if (dir_exists(path)) return true;
#if defined(RE0_PLATFORM_WINDOWS)
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0755) == 0) return true;
#endif
    return errno == EEXIST && dir_exists(path);
}

/* ── 标准库源码（内嵌）── */

static const char *std_sources[] = {
    "/* std/io — I/O */\nfn read_file(path: str) -> str { return file_read(path) }\nfn write_file(path: str, data: str) { file_write(path, data) }\nfn print_line(s: str) { println(s) }\n",
    "/* std/string */\nfn length(s: str) -> i64 { return str_len(s) }\nfn concat(a: str, b: str) -> str { return str_concat(a, b) }\nfn equals(a: str, b: str) -> bool { return str_eq(a, b) }\nfn substring(s: str, start: i64, end: i64) -> str { return str_slice(s, start, end) }\nfn parse_int(s: str) -> i64 { return str_to_int(s) }\n",
    "/* std/math */\nfn abs_val(n: i64) -> i64 { if n < 0 { return 0 - n } return n }\nfn max_val(a: i64, b: i64) -> i64 { if a > b { return a } return b }\nfn min_val(a: i64, b: i64) -> i64 { if a < b { return a } return b }\n",
    "/* std/vec */\nfn new_vec() -> i64 { return vec_new() }\nfn push_val(v: i64, x: i64) { vec_push(v, x) }\nfn get_val(v: i64, i: i64) -> i64 { return vec_get(v, i) }\nfn size(v: i64) -> i64 { return vec_len(v) }\n",
};

/* ── 创建虚拟环境 ── */

bool reo_venv_create(const char *project_dir) {
    char env_dir[512];
    join_path(env_dir, sizeof(env_dir), project_dir, RE0_VENV_DIR);

    /* 创建目录结构 */
    char lib_dir[512], std_dir[512], pkg_dir[512], bin_dir[512];
    join_path(lib_dir, sizeof(lib_dir), env_dir, RE0_VENV_LIB);
    join_path(std_dir, sizeof(std_dir), lib_dir, RE0_VENV_STD);
    join_path(pkg_dir, sizeof(pkg_dir), lib_dir, RE0_VENV_PACKAGES);
    join_path(bin_dir, sizeof(bin_dir), env_dir, RE0_VENV_BIN);

    if (!ensure_dir(env_dir)) return false;
    if (!ensure_dir(lib_dir)) return false;
    if (!ensure_dir(std_dir)) return false;
    if (!ensure_dir(pkg_dir)) return false;
    if (!ensure_dir(bin_dir)) return false;

    /* 写入配置文件 */
    char config_path[512];
    join_path(config_path, sizeof(config_path), env_dir, RE0_VENV_CONFIG);
    FILE *f = fopen(config_path, "w");
    if (!f) return false;
    fprintf(f, "[env]\n");
    fprintf(f, "ringecho_version = \"0.2.0\"\n");
    fprintf(f, "created = \"%s\"\n", "auto");
    fclose(f);

    /* 安装标准库 */
    if (!reo_venv_install_std(env_dir)) {
        re0_log(RE0_LOG_WARN, "failed to install std library");
    }

    printf("Virtual environment created: %s\n", env_dir);
    printf("  %s/lib/std/     standard library\n", RE0_VENV_DIR);
    printf("  %s/lib/packages/ third-party packages\n", RE0_VENV_DIR);
    printf("  %s/bin/         executables\n", RE0_VENV_DIR);
    printf("\nActivate with:\n");
    printf("  export PATH=\"%s/bin:$PATH\"\n", env_dir);
    return true;
}

/* ── 安装标准库 ── */

bool reo_venv_install_std(const char *env_dir) {
    char std_dir[512];
    snprintf(std_dir, sizeof(std_dir), "%s/%s/%s", env_dir, RE0_VENV_LIB, RE0_VENV_STD);
    if (!ensure_dir(std_dir)) return false;

    for (int i = 0; i < RE0_STD_MODULES_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.reo", std_dir, RE0_STD_MODULES[i]);
        FILE *f = fopen(path, "w");
        if (!f) continue;
        fputs(std_sources[i], f);
        fclose(f);
    }
    return true;
}

#if defined(RE0_PLATFORM_WINDOWS)
/* Windows 上的简单 dirname 实现 */
static void dirname_win(char *path) {
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
#endif

/* ── 检测虚拟环境 ── */

bool reo_venv_detect(char *out_env_dir, size_t cap) {
    if (!out_env_dir || cap == 0) return false;
    char cwd[512];
#if defined(RE0_PLATFORM_WINDOWS)
    if (_getcwd(cwd, sizeof(cwd)) == NULL) return false;
#else
    if (!getcwd(cwd, sizeof(cwd))) return false;
#endif

    /* 从当前目录向上查找 .renv/ */
    char dir[512];
    strncpy(dir, cwd, sizeof(dir) - 1);
    dir[sizeof(dir)-1] = '\0';

    for (int i = 0; i < 20; i++) {
        char env_path[512];
        join_path(env_path, sizeof(env_path), dir, RE0_VENV_DIR);
        if (dir_exists(env_path)) {
            strncpy(out_env_dir, env_path, cap - 1);
            out_env_dir[cap - 1] = '\0';
            return true;
        }
        /* 向上一级 */
        char current[sizeof(dir)];
        strncpy(current, dir, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';
#if defined(RE0_PLATFORM_WINDOWS)
        dirname_win(dir);
        char *parent = dir;
#else
        char *parent = dirname(dir);
        memmove(dir, parent, strlen(parent) + 1);
#endif
        if (strcmp(parent, current) == 0) break;
    }
    return false;
}

/* ── import 路径解析（多级 fallback）── */

bool reo_venv_resolve_import(const char *module_name,
                              const char *project_dir,
                              const char *env_dir,
                              char *out_path, size_t cap) {
    /* 1. 项目本地: project_dir/module_name.reo */
    if (project_dir) {
        join_path(out_path, cap, project_dir, module_name);
        strncat(out_path, ".reo", cap - strlen(out_path) - 1);
        if (access(out_path, R_OK) == 0) return true;
    }

    /* 2. 虚拟环境第三方包: env_dir/lib/packages/module_name.reo */
    if (env_dir) {
        char pkg_dir[512];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s/%s", env_dir, RE0_VENV_LIB, RE0_VENV_PACKAGES);
        join_path(out_path, cap, pkg_dir, module_name);
        strncat(out_path, ".reo", cap - strlen(out_path) - 1);
        if (access(out_path, R_OK) == 0) return true;

        /* 3. 虚拟环境标准库: env_dir/lib/std/module_name.reo */
        char std_dir[512];
        snprintf(std_dir, sizeof(std_dir), "%s/%s/%s", env_dir, RE0_VENV_LIB, RE0_VENV_STD);
        join_path(out_path, cap, std_dir, module_name);
        strncat(out_path, ".reo", cap - strlen(out_path) - 1);
        if (access(out_path, R_OK) == 0) return true;
    }

    /* 4. 全局: ~/.re/lib/module_name.reo */
    const char *home = getenv("HOME");
    if (home) {
        char global_dir[512];
        snprintf(global_dir, sizeof(global_dir), "%s/%s", home, RE0_GLOBAL_LIB_DIR);
        join_path(out_path, cap, global_dir, module_name);
        strncat(out_path, ".reo", cap - strlen(out_path) - 1);
        if (access(out_path, R_OK) == 0) return true;
    }

    return false;
}

/* ── 输出激活脚本 ── */

void reo_venv_print_activate(const char *env_dir) {
    printf("# RingEcho virtual environment activation\n");
    printf("# Add to ~/.bashrc or ~/.zshrc:\n");
    printf("export REO_ENV=\"%s\"\n", env_dir);
    printf("export PATH=\"%s/bin:$PATH\"\n", env_dir);
    printf("\n# Or run:\n");
    printf("eval \"$(rem venv activate)\"\n");
}
