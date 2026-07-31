/*
 * rvm_main.c — RingEcho Version Manager (rvm)
 *
 * 类似 nvm：管理 RingEcho 编译器版本
 *   rvm install <version>        安装指定版本
 *   rvm use <version>            切换到指定版本
 *   rvm list                     列出已安装版本
 *   rvm current                  显示当前版本
 *   rvm uninstall <version>      移除版本
 *   rvm remote                   列出可用版本
 */

#include "safe.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#define RVM_DIR ".rvm"
#define RVM_VERSIONS_DIR ".rvm/versions"
#define RVM_CURRENT ".rvm/current"
#define RVM_DEFAULT_VERSION "0.2.0"

static void print_usage(void) {
    printf("RingEcho Version Manager (rvm) v0.1.0\n");
    printf("Usage:\n");
    printf("  rvm install <version>        Install a RingEcho version\n");
    printf("  rvm use <version>            Switch to a version\n");
    printf("  rvm list                     List installed versions\n");
    printf("  rvm current                  Show current version\n");
    printf("  rvm uninstall <version>      Remove a version\n");
    printf("  rvm remote                   List available versions\n");
}

static bool valid_version(const char *v) {
    if (!v || !v[0]) return false;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) return false;
    }
    return true;
}

static bool make_path(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0 || len >= sizeof(tmp)) return false;
    if (tmp[len-1] == '/') tmp[len-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static int run_program(const char *prog, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(prog, argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void ensure_rvm_dir(void) {
    char *home = getenv("HOME");
    if (!home) return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", home, RVM_DIR);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/%s", home, RVM_VERSIONS_DIR);
    mkdir(dir, 0755);
}

static void get_versions_dir(char *out, size_t cap) {
    char *home = getenv("HOME");
    if (!home) { snprintf(out, cap, "%s", RVM_VERSIONS_DIR); return; }
    snprintf(out, cap, "%s/%s", home, RVM_VERSIONS_DIR);
}

static void get_current_link(char *out, size_t cap) {
    char *home = getenv("HOME");
    if (!home) { snprintf(out, cap, "%s", RVM_CURRENT); return; }
    snprintf(out, cap, "%s/%s", home, RVM_CURRENT);
}

static int cmd_install(const char *version) {
    if (!version) {
        fprintf(stderr, "usage: rvm install <version>\n");
        return 1;
    }
    if (!valid_version(version)) {
        fprintf(stderr, "invalid version '%s'\n", version);
        return 1;
    }

    ensure_rvm_dir();

    char versions_dir[512];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    char version_dir[512];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", versions_dir, version);

    struct stat st;
    if (stat(version_dir, &st) == 0) {
        printf("Version %s is already installed\n", version);
        return 0;
    }

    /* 从 GitHub releases 下载 */
    printf("Installing RingEcho %s...\n", version);
    char url[1024];
    snprintf(url, sizeof(url),
             "https://github.com/Ringaire/RingEcho-Lang-C/releases/download/v%s/rem-linux-x86_64",
             version);

    if (!make_path(version_dir)) {
        fprintf(stderr, "cannot create version directory\n");
        return 1;
    }

    char rev_path[640];
    snprintf(rev_path, sizeof(rev_path), "%s/rev", version_dir);
    char *curl_argv[] = { "curl", "-sL", url, "-o", rev_path, NULL };
    int rc = run_program("curl", curl_argv);
    if (rc == 0) {
        char *chmod_argv[] = { "chmod", "+x", rev_path, NULL };
        run_program("chmod", chmod_argv);
    }
    if (rc != 0) {
        printf("Failed to download version %s\n", version);
        printf("Check: https://github.com/Ringaire/RingEcho-Lang-C/releases\n");
        return 1;
    }

    printf("Installed: RingEcho %s\n", version);
    printf("Activate with: rvm use %s\n", version);
    return 0;
}

static int cmd_use(const char *version) {
    if (!version) {
        fprintf(stderr, "usage: rvm use <version>\n");
        return 1;
    }
    if (!valid_version(version)) {
        fprintf(stderr, "invalid version '%s'\n", version);
        return 1;
    }

    char versions_dir[512];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    char version_dir[512];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", versions_dir, version);

    struct stat st;
    if (stat(version_dir, &st) != 0) {
        fprintf(stderr, "version %s is not installed (run 'rvm install %s')\n", version, version);
        return 1;
    }

    /* 更新 current 软链接 */
    char current_link[512];
    get_current_link(current_link, sizeof(current_link));
    unlink(current_link);
    symlink(version_dir, current_link);

    printf("Now using RingEcho %s\n", version);
    printf("Add to PATH: export PATH=\"%s:$PATH\"\n", current_link);
    return 0;
}

static int cmd_list(void) {
    char versions_dir[512];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    struct stat st;
    if (stat(versions_dir, &st) != 0) {
        printf("No versions installed\n");
        printf("Run 'rvm install <version>' to install\n");
        return 0;
    }

    printf("Installed versions:\n");
    DIR *d = opendir(versions_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            printf("  %s\n", de->d_name);
        }
        closedir(d);
    }
    return 0;
}

static int cmd_current(void) {
    char current_link[512];
    get_current_link(current_link, sizeof(current_link));

    char buf[512];
    ssize_t len = readlink(current_link, buf, sizeof(buf) - 1);
    if (len <= 0) {
        printf("No version selected (using system default: %s)\n", RVM_DEFAULT_VERSION);
        return 0;
    }
    buf[len] = '\0';

    /* 提取版本号 */
    char *ver = strrchr(buf, '/');
    if (ver) ver++;
    else ver = buf;

    printf("Current: %s\n", ver);
    return 0;
}

static int cmd_uninstall(const char *version) {
    if (!version) {
        fprintf(stderr, "usage: rvm uninstall <version>\n");
        return 1;
    }
    if (!valid_version(version)) {
        fprintf(stderr, "invalid version '%s'\n", version);
        return 1;
    }

    char versions_dir[512];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    char version_dir[512];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", versions_dir, version);

    struct stat st;
    if (stat(version_dir, &st) != 0) {
        printf("Version %s is not installed\n", version);
        return 1;
    }

    char *rm_argv[] = { "rm", "-rf", version_dir, NULL };
    run_program("rm", rm_argv);
    printf("Removed: RingEcho %s\n", version);
    return 0;
}

static int cmd_remote(void) {
    printf("Available versions:\n");
    printf("  0.1.0  (initial release)\n");
    printf("  0.2.0  (current stable)\n");
    printf("\nCheck: https://github.com/Ringaire/RingEcho-Lang-C/releases\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "install") == 0) return cmd_install(argc > 2 ? argv[2] : NULL);
    if (strcmp(cmd, "use") == 0)     return cmd_use(argc > 2 ? argv[2] : NULL);
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0)
                                     return cmd_list();
    if (strcmp(cmd, "current") == 0) return cmd_current();
    if (strcmp(cmd, "uninstall") == 0)
                                     return cmd_uninstall(argc > 2 ? argv[2] : NULL);
    if (strcmp(cmd, "remote") == 0)  return cmd_remote();
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("rvm v0.1.0\n");
        return 0;
    }

    print_usage();
    return 1;
}
