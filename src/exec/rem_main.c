/*
 * rem_main.c — RingEcho Module Manager (rem)
 *
 * 类似 npm：包管理工具
 *   rem init [name]              初始化项目
 *   rem install <pkg>            安装包
 *   rem list                     列出已安装包
 *   rem remove <pkg>             移除包
 *   rem update                   更新依赖
 *   rem search <query>           搜索包
 *   rem publish                  发布包
 */

#include "base/safe.h"
#include "platform.h"
#include "exec/venv.h"
#include "exec/toml_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#if defined(RE0_PLATFORM_WINDOWS)
#include <direct.h>
#include <process.h>
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#define RE0_MKDIR(path) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#define RE0_MKDIR(path) mkdir((path), 0755)
#endif

static void print_usage(void) {
    printf("RingEcho Module Manager (rem) v0.2.0\n");
    printf("Usage:\n");
    printf("  rem init [name]              Initialize new project\n");
    printf("  rem install <pkg>            Install a package\n");
    printf("  rem list                     List installed packages\n");
    printf("  rem remove <pkg>             Remove a package\n");
    printf("  rem update                   Update dependencies\n");
    printf("  rem search <query>           Search packages\n");
    printf("  rem publish                  Publish current package\n");
}

static void get_packages_dir(char *out, size_t cap) {
    char env_dir[600];
    extern bool reo_venv_detect(char *, size_t);
    if (reo_venv_detect(env_dir, sizeof(env_dir))) {
        snprintf(out, cap, "%s/lib/packages", env_dir);
    } else {
        snprintf(out, cap, ".renv/lib/packages");
    }
}

static bool valid_name(const char *name) {
    if (!name || !name[0]) return false;
    if (name[0] == '.') return false;
    if (strstr(name, "..") != NULL) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool make_path(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0 || len >= sizeof(tmp)) return false;
    if (tmp[len-1] == '/' || tmp[len-1] == '\\') tmp[len-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (RE0_MKDIR(tmp) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (RE0_MKDIR(tmp) != 0 && errno != EEXIST) return false;
    return true;
}

static int run_program(const char *prog, char *const argv[]) {
#if defined(RE0_PLATFORM_WINDOWS)
    intptr_t rc = _spawnvp(_P_WAIT, prog, (const char *const *)argv);
    return (rc < 0) ? -1 : (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(prog, argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

#if defined(RE0_PLATFORM_WINDOWS)
static int remove_directory_recursive(const char *path) {
    WIN32_FIND_DATAA fd;
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (RemoveDirectoryA(path)) return 0;
        if (DeleteFileA(path)) return 0;
        return -1;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_directory_recursive(child);
        } else {
            DeleteFileA(child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(path);
    return 0;
}
#endif

static int cmd_init(const char *name) {
    extern bool reo_toml_write_default(const char *, const char *);
    extern bool reo_venv_create(const char *);

    const char *pname = name ? name : "my_project";
    if (!valid_name(pname)) {
        fprintf(stderr, "invalid project name '%s'\n", pname);
        return 1;
    }

    char src_dir[600];
    snprintf(src_dir, sizeof(src_dir), "%s/src", pname);
    if (!make_path(src_dir)) {
        fprintf(stderr, "cannot create directory '%s'\n", src_dir);
        return 1;
    }

    reo_toml_write_default(pname, pname);

    char entry_path[512];
    snprintf(entry_path, sizeof(entry_path), "%s/main.reo", pname);
    FILE *f = fopen(entry_path, "w");
    if (f) {
        fprintf(f, "fn main() {\n    println(\"Hello from %s!\")\n}\n", pname);
        fclose(f);
    }

    char gi_path[512];
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", pname);
    f = fopen(gi_path, "w");
    if (f) {
        fprintf(f, ".renv/\ntarget/\n*.o\n*.exe\nrem\nrem.exe\nrev\nrev.exe\n");
        fclose(f);
    }

    reo_venv_create(pname);

    printf("Created project '%s'\n", pname);
    printf("  %s/main.reo\n", pname);
    printf("  %s/ringecho.toml\n", pname);
    printf("  %s/.gitignore\n", pname);
    printf("  %s/.renv/\n", pname);
    printf("\nNext:\n");
    printf("  cd %s\n", pname);
    printf("  rev build\n");
    return 0;
}

static int cmd_install(const char *pkg_name) {
    if (!pkg_name) {
        fprintf(stderr, "usage: rem install <package>\n");
        return 1;
    }
    if (!valid_name(pkg_name)) {
        fprintf(stderr, "invalid package name '%s'\n", pkg_name);
        return 1;
    }

    char pkg_dir[700];
    get_packages_dir(pkg_dir, sizeof(pkg_dir));

    if (!make_path(pkg_dir)) {
        fprintf(stderr, "cannot create packages directory\n");
        return 1;
    }

    char pkg_path[1024];
    snprintf(pkg_path, sizeof(pkg_path), "%s/%s", pkg_dir, pkg_name);

    struct stat st;
    if (stat(pkg_path, &st) == 0) {
        printf("Package '%s' is already installed\n", pkg_name);
        return 0;
    }

    char url[600];
    snprintf(url, sizeof(url), "https://github.com/Ringaire/reo-pkg-%s.git", pkg_name);
    char *clone_argv[] = { "git", "clone", "--depth", "1", url, pkg_path, NULL };
    printf("Installing '%s'...\n", pkg_name);
    int rc = run_program("git", clone_argv);
    if (rc != 0) {
        printf("Package '%s' not found in registry\n", pkg_name);
        printf("Try: rem search %s\n", pkg_name);
        return 1;
    }

    printf("Installed: %s -> %s\n", pkg_name, pkg_path);
    return 0;
}

static int cmd_list(void) {
    char pkg_dir[700];
    get_packages_dir(pkg_dir, sizeof(pkg_dir));

    struct stat st;
    if (stat(pkg_dir, &st) != 0) {
        printf("No packages installed\n");
        printf("Run 'rem install <package>' to install\n");
        return 0;
    }

    extern bool reo_toml_find_root(char *, size_t);
    extern ReoTomlConfig reo_toml_load(const char *);

    char root[512];
    if (reo_toml_find_root(root, sizeof(root))) {
        char toml_path[600];
        snprintf(toml_path, sizeof(toml_path), "%s/ringecho.toml", root);
        ReoTomlConfig cfg = reo_toml_load(toml_path);
        if (cfg.valid && cfg.dep_count > 0) {
            printf("Dependencies (from ringecho.toml):\n");
            for (int i = 0; i < cfg.dep_count; i++) {
                printf("  %s = \"%s\"\n", cfg.deps[i].name, cfg.deps[i].spec);
            }
        } else {
            printf("No dependencies declared\n");
        }
    }

    printf("\nInstalled packages:\n");
#if defined(RE0_PLATFORM_WINDOWS)
    {
        WIN32_FIND_DATAA fd;
        char pattern[1024];
        snprintf(pattern, sizeof(pattern), "%s\\*", pkg_dir);
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == '.') continue;
                printf("  %s\n", fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    DIR *d = opendir(pkg_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            printf("  %s\n", de->d_name);
        }
        closedir(d);
    }
#endif

    return 0;
}

static int cmd_remove(const char *pkg_name) {
    if (!pkg_name) {
        fprintf(stderr, "usage: rem remove <package>\n");
        return 1;
    }
    if (!valid_name(pkg_name)) {
        fprintf(stderr, "invalid package name '%s'\n", pkg_name);
        return 1;
    }

    char pkg_dir[700];
    get_packages_dir(pkg_dir, sizeof(pkg_dir));

    char pkg_path[1024];
    snprintf(pkg_path, sizeof(pkg_path), "%s/%s", pkg_dir, pkg_name);

    struct stat st;
    if (stat(pkg_path, &st) != 0) {
        printf("Package '%s' is not installed\n", pkg_name);
        return 1;
    }

#if defined(RE0_PLATFORM_WINDOWS)
    if (st.st_mode & S_IFDIR) {
        remove_directory_recursive(pkg_path);
    } else {
        DeleteFileA(pkg_path);
    }
#else
    char *rm_argv[] = { "rm", "-rf", pkg_path, NULL };
    run_program("rm", rm_argv);
#endif
    printf("Removed: %s\n", pkg_name);
    return 0;
}

static int cmd_update(void) {
    char pkg_dir[700];
    get_packages_dir(pkg_dir, sizeof(pkg_dir));

    struct stat st;
    if (stat(pkg_dir, &st) != 0) {
        printf("No packages to update\n");
        return 0;
    }

    printf("Updating all packages...\n");
#if defined(RE0_PLATFORM_WINDOWS)
    {
        WIN32_FIND_DATAA fd;
        char pattern[1024];
        snprintf(pattern, sizeof(pattern), "%s\\*", pkg_dir);
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == '.') continue;
                char sub[1024];
                snprintf(sub, sizeof(sub), "%s\\%s", pkg_dir, fd.cFileName);
                char gitdir[1100];
                snprintf(gitdir, sizeof(gitdir), "%s\\.git", sub);
                struct stat gst;
                if (stat(gitdir, &gst) == 0) {
                    printf("  updating %s\n", fd.cFileName);
                    char saved[1024];
                    _getcwd(saved, sizeof(saved));
                    if (_chdir(sub) == 0) {
                        char *pull_argv[] = { "git", "pull", "--ff-only", NULL };
                        _spawnvp(_P_WAIT, "git", (const char *const *)pull_argv);
                        _chdir(saved);
                    }
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    DIR *d = opendir(pkg_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char sub[600];
            snprintf(sub, sizeof(sub), "%s/%s", pkg_dir, de->d_name);
            char gitdir[640];
            snprintf(gitdir, sizeof(gitdir), "%s/.git", sub);
            struct stat gst;
            if (stat(gitdir, &gst) == 0) {
                printf("  updating %s\n", de->d_name);
                char *pull_argv[] = { "git", "pull", "--ff-only", NULL };
                pid_t pid = fork();
                if (pid == 0) { if (chdir(sub) != 0) _exit(127); execvp("git", pull_argv); _exit(127); }
                int st = 0;
                while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            }
        }
        closedir(d);
    }
#endif
    printf("Update complete\n");
    return 0;
}

static int cmd_search(const char *query) {
    if (!query) {
        fprintf(stderr, "usage: rem search <query>\n");
        return 1;
    }

    printf("Searching for '%s'...\n", query);
    printf("Registry: https://github.com/Ringaire/reo-pkg-%s\n", query);
    printf("\nOnline search not yet available.\n");
    printf("Try: rem install %s\n", query);
    return 0;
}

static int cmd_publish(void) {
    extern bool reo_toml_find_root(char *, size_t);
    extern ReoTomlConfig reo_toml_load(const char *);

    char root[512];
    if (!reo_toml_find_root(root, sizeof(root))) {
        fprintf(stderr, "no ringecho.toml found\n");
        return 1;
    }

    char toml_path[600];
    snprintf(toml_path, sizeof(toml_path), "%s/ringecho.toml", root);
    ReoTomlConfig cfg = reo_toml_load(toml_path);
    if (!cfg.valid) {
        fprintf(stderr, "cannot parse ringecho.toml\n");
        return 1;
    }

    printf("Publishing '%s' v%s...\n", cfg.package_name, cfg.package_version);
    printf("\nPublishing not yet available.\n");
    printf("To publish manually:\n");
    printf("  1. Create repo: Ringaire/reo-pkg-%s\n", cfg.package_name);
    printf("  2. Push your .reo files\n");
    printf("  3. Others can: rem install %s\n", cfg.package_name);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "init") == 0) {
        return cmd_init(argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "install") == 0) {
        return cmd_install(argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        return cmd_list();
    }
    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0) {
        return cmd_remove(argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "update") == 0) {
        return cmd_update();
    }
    if (strcmp(cmd, "search") == 0) {
        return cmd_search(argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "publish") == 0) {
        return cmd_publish();
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("rem v0.2.0\n");
        return 0;
    }

    print_usage();
    return 1;
}
