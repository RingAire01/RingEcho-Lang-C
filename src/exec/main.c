#include "compiler.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_usage(void) {
    printf("RingEcho C Compiler v0.2.0\n");
    printf("Usage:\n");
    printf("  rem run <file.reo> [--target c|reo]  Compile and run\n");
    printf("  rem build <file.reo> [-o out]        Compile to executable/asm\n");
    printf("  rem check <file.reo>                 Type check only\n");
    printf("  rem lsp                              Start LSP server\n");
    printf("  rem init [name]                      Initialize new project\n");
    printf("  rem clean                            Remove build artifacts\n");
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char*)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f); buf[rd] = '\0'; fclose(f);
    return buf;
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
        printf("check passed: %s\n", path);
    }

    free(source);
    re0_compiler_destroy(&comp);
    return ok ? 0 : 1;
}

static int cmd_init(const char *name) {
    const char *pname = name ? name : "my_project";
    char path[512];
    snprintf(path, sizeof(path), "%s/main.reo", pname);
    /* create directory */
    char cmd[1024];
#if defined(RE0_PLATFORM_WINDOWS)
    snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", pname);
#else
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", pname);
#endif
    system(cmd);

    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot create %s\n", path); return 1; }
    fprintf(f, "fn main() {\n    println(\"Hello from %s!\");\n}\n", pname);
    fclose(f);

    /* write ringecho.toml */
    char toml[512];
    snprintf(toml, sizeof(toml), "%s/ringecho.toml", pname);
    f = fopen(toml, "w");
    if (f) {
        fprintf(f, "[package]\nname = \"%s\"\nversion = \"0.1.0\"\nentry = \"main.reo\"\n", pname);
        fclose(f);
    }

    printf("Created project '%s'\n", pname);
    printf("  %s/main.reo\n", pname);
    printf("  %s/ringecho.toml\n", pname);
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
