#include "safe.h"
#include "compiler.h"
#include "platform.h"
#include "venv.h"
#include "toml_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#if defined(RE0_PLATFORM_WINDOWS)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

static void print_usage(void) {
    printf("RingEcho Evaluator (rev) v0.2.0\n");
    printf("Usage:\n");
    printf("  rev run <file.reo> [--target c|reo]  Compile and run\n");
    printf("  rev build [file.reo] [-o out]        Compile to executable/asm\n");
    printf("  rev check <file.reo>                 Type check only\n");
    printf("  rev lsp                              Start LSP server\n");
    printf("  rev venv <init|activate>             Manage virtual environment\n");
    printf("  rev init [name]                      Initialize new project\n");
    printf("  rev clean                            Remove build artifacts\n");
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char*)xmalloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f); buf[rd] = '\0'; fclose(f);
    return buf;
}

static bool valid_project_name(const char *name) {
    if (!name || !name[0]) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    return errno == EEXIST;
}

static bool write_path(char *out, size_t cap, const char *format,
                       const char *first, const char *second) {
    int written = snprintf(out, cap, format, first, second);
    return written >= 0 && (size_t)written < cap;
}

/* check: lex + parse + sema + lint, no codegen */
static int cmd_check(const char *path) {
    char *source = read_file(path);
    if (!source) { fprintf(stderr, "cannot read '%s'\n", path); return 1; }

    Re0Compiler comp;
    re0_compiler_init(&comp, &re0_backend_c);

    bool ok = re0_lexer_tokenize(&comp.lexer, source, path);
    if (ok) ok = re0_parser_parse(&comp.parser, &comp.lexer.stream);
    if (ok) {
        re0_sema_check(&comp.sema, &comp.parser.stmts);
        re0_lint_run(&comp.sema.checked, &comp.errors);
    }

    if (re0_error_list_has_errors(&comp.errors)) {
        re0_error_list_print(&comp.errors);
        ok = false;
    } else {
        if (re0_error_list_has_warnings(&comp.errors))
            re0_error_list_print(&comp.errors);
        printf("check passed: %s\n", path);
    }

    free(source);
    re0_compiler_destroy(&comp);
    return ok ? 0 : 1;
}

static int cmd_init(const char *name) {
    const char *pname = name ? name : "my_project";
    if (!valid_project_name(pname)) {
        fprintf(stderr, "project name may contain only letters, digits, '_' and '-'\n");
        return 1;
    }
    char path[512];
    if (!ensure_directory(pname) ||
        !write_path(path, sizeof(path), "%s/%s", pname, "main.reo")) {
        fprintf(stderr, "cannot create project directory or path\n");
        return 1;
    }

    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot create %s\n", path); return 1; }
    fprintf(f, "fn main() {\n    println(\"Hello from %s!\");\n}\n", pname);
    fclose(f);

    /* write ringecho.toml */
    if (!reo_toml_write_default(pname, pname)) {
        fprintf(stderr, "cannot create project configuration\n");
        return 1;
    }

    /* write .gitignore */
    char gi_path[512];
    if (!write_path(gi_path, sizeof(gi_path), "%s/%s", pname, ".gitignore")) return 1;
    f = fopen(gi_path, "w");
    if (f) {
        fprintf(f, ".renv/\ntarget/\n*.o\n*.exe\nrem\nrem.exe\n");
        fclose(f);
    }

    /* create virtual environment */
    if (!reo_venv_create(pname)) {
        fprintf(stderr, "cannot create project virtual environment\n");
        return 1;
    }

    printf("Created project '%s'\n", pname);
    printf("  %s/main.reo\n", pname);
    printf("  %s/ringecho.toml\n", pname);
    printf("  %s/.gitignore\n", pname);
    printf("  %s/.renv/\n", pname);
    return 0;
}

static int cmd_clean(void) {
    const char *files[] = {
        "re0_output.c", "re0_tmp_out", "output.reo.asm", "a.out", NULL
    };
    char platform_output[32];
    snprintf(platform_output, sizeof(platform_output), "a%s",
             RE0_PLATFORM_EXECUTABLE_SUFFIX);
    int count = 0;
    for (int i = 0; files[i]; i++) {
        if (remove(files[i]) == 0) { printf("removed %s\n", files[i]); count++; }
    }
    if (remove(platform_output) == 0) {
        printf("removed %s\n", platform_output);
        count++;
    }
    printf("cleaned %d file(s)\n", count);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "init") == 0) {
        return cmd_init(argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "clean") == 0) {
        return cmd_clean();
    }
    if (strcmp(cmd, "venv") == 0) {
        extern bool reo_venv_create(const char *);
        extern bool reo_venv_detect(char *, size_t);
        extern void reo_venv_print_activate(const char *);
        const char *subcmd = argc > 2 ? argv[2] : "init";
        if (strcmp(subcmd, "init") == 0) {
            char cwd[512];
            if (!getcwd(cwd, sizeof(cwd))) return 1;
            return reo_venv_create(cwd) ? 0 : 1;
        }
        if (strcmp(subcmd, "activate") == 0) {
            char env_dir[512];
            if (reo_venv_detect(env_dir, sizeof(env_dir))) {
                reo_venv_print_activate(env_dir);
                return 0;
            }
            fprintf(stderr, "no virtual environment found\n");
            return 1;
        }
        fprintf(stderr, "usage: rem venv <init|activate>\n");
        return 1;
    }
    if (strcmp(cmd, "build") == 0 && argc < 3) {
        /* rem build（无参数）：从 ringecho.toml 读取入口 */
        extern bool reo_toml_find_root(char *, size_t);
        extern ReoTomlConfig reo_toml_load(const char *);
        char root[512];
        if (!reo_toml_find_root(root, sizeof(root))) {
            fprintf(stderr, "no ringecho.toml found (run 'rem init' first)\n");
            return 1;
        }
        char toml_path[512];
        if (!write_path(toml_path, sizeof(toml_path), "%s/%s", root, "ringecho.toml")) return 1;
        ReoTomlConfig cfg = reo_toml_load(toml_path);
        if (!cfg.valid) {
            fprintf(stderr, "cannot parse ringecho.toml\n");
            return 1;
        }
        char entry_path[512];
        if (!write_path(entry_path, sizeof(entry_path), "%s/%s", root, cfg.entry)) return 1;
        char target_dir[512];
        if (!write_path(target_dir, sizeof(target_dir), "%s/%s", root, "target")) return 1;
        char release_dir[512];
        if (!write_path(release_dir, sizeof(release_dir), "%s/%s", target_dir, "Release")) return 1;
        char output_path[512];
        if (!write_path(output_path, sizeof(output_path), "%s/%s", release_dir, cfg.package_name)) return 1;
        /* 确保输出目录存在 */
        if (!ensure_directory(target_dir) || !ensure_directory(release_dir)) {
            fprintf(stderr, "cannot create build output directory\n");
            return 1;
        }
        Re0Compiler comp;
        re0_compiler_init(&comp, &re0_backend_c);
        bool ok = re0_compiler_compile_file(&comp, entry_path, output_path);
        if (re0_error_list_has_errors(&comp.errors))
            re0_error_list_print(&comp.errors);
        re0_compiler_destroy(&comp);
        if (ok) printf("Built: %s\n", output_path);
        return ok ? 0 : 1;
    }
    if (strcmp(cmd, "check") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: rem check <file.reo>\n"); return 1; }
        return cmd_check(argv[2]);
    }
    if (strcmp(cmd, "lsp") == 0) {
        extern int lsp_server_run(void);
        return lsp_server_run();
    }

    if (argc < 3) { print_usage(); return 1; }

    Re0Backend *backend = &re0_backend_c;
    const char *path = argv[2];
    char default_output[512];
    const char *output = default_output;
    const char *last_slash = strrchr(argv[0], '/');
    const char *last_backslash = strrchr(argv[0], '\\');
    if (last_backslash && (!last_slash || last_backslash > last_slash))
        last_slash = last_backslash;
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - argv[0]);
        if (dir_len >= sizeof(default_output) - 16)
            dir_len = sizeof(default_output) - 17;
        memcpy(default_output, argv[0], dir_len);
        default_output[dir_len] = '\0';
        snprintf(default_output + dir_len,
                 sizeof(default_output) - dir_len,
                 RE0_PLATFORM_PATH_SEPARATOR "a" RE0_PLATFORM_EXECUTABLE_SUFFIX);
    } else {
        snprintf(default_output, sizeof(default_output),
                 "target%cRelease%ca%s", RE0_PLATFORM_PATH_SEPARATOR[0],
                 RE0_PLATFORM_PATH_SEPARATOR[0], RE0_PLATFORM_EXECUTABLE_SUFFIX);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "reo") == 0) {
                backend = &re0_backend_reo;
                char *name = strrchr(default_output, '/');
                if (!name) name = strrchr(default_output, '\\');
                if (name) *(name + 1) = '\0';
                strncat(default_output, "output.reo.asm",
                        sizeof(default_output) - strlen(default_output) - 1);
                output = default_output;
            }
            else if (strcmp(argv[i + 1], "c") == 0) backend = &re0_backend_c;
            else if (strcmp(argv[i + 1], "c-freestanding") == 0)
                backend = &re0_backend_c_freestanding;
        }
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
    }

    Re0Compiler comp;
    re0_compiler_init(&comp, backend);

    bool ok = false;
    if (strcmp(cmd, "run") == 0) ok = re0_compiler_run(&comp, path);
    else if (strcmp(cmd, "build") == 0) ok = re0_compiler_compile_file(&comp, path, output);
    else { print_usage(); re0_compiler_destroy(&comp); return 1; }

    if (re0_error_list_has_errors(&comp.errors))
        re0_error_list_print(&comp.errors);

    re0_compiler_destroy(&comp);
    return ok ? 0 : 1;
}
