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

#include "base/safe.h"
#include "platform.h"
#include <stdbool.h>
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
#define RE0_SEP '\\'
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#define RE0_MKDIR(path) mkdir((path), 0755)
#define RE0_SEP '/'
#endif

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
    /* path traversal forms must never reach the versions directory */
    if (strcmp(v, ".") == 0 || strcmp(v, "..") == 0) return false;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) return false;
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

static void get_home_dir(char *out, size_t cap) {
#if defined(RE0_PLATFORM_WINDOWS)
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEDRIVE");
    if (home) {
        const char *home_path = getenv("HOMEPATH");
        if (home_path) {
            snprintf(out, cap, "%s%s", home, home_path);
            return;
        }
    }
    home = getenv("HOME");
    if (home) { snprintf(out, cap, "%s", home); return; }
    snprintf(out, cap, ".");
#else
    const char *home = getenv("HOME");
    if (home) { snprintf(out, cap, "%s", home); return; }
    snprintf(out, cap, ".");
#endif
}

static void ensure_rvm_dir(void) {
    char home[512];
    get_home_dir(home, sizeof(home));
    home[sizeof(home) - 1] = '\0';
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/%s", home, RVM_DIR);
    RE0_MKDIR(dir);
    snprintf(dir, sizeof(dir), "%s/%s", home, RVM_VERSIONS_DIR);
    RE0_MKDIR(dir);
}

static void get_versions_dir(char *out, size_t cap) {
    char home[512];
    get_home_dir(home, sizeof(home));
    snprintf(out, cap, "%s/%s", home, RVM_VERSIONS_DIR);
}

static void get_current_link(char *out, size_t cap) {
    char home[512];
    get_home_dir(home, sizeof(home));
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

    char versions_dir[600];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    char version_dir[700];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", versions_dir, version);

    struct stat st;
    if (stat(version_dir, &st) == 0) {
        printf("Version %s is already installed\n", version);
        return 0;
    }

    printf("Installing RingEcho %s...\n", version);
    char url[1024];
#if defined(RE0_PLATFORM_WINDOWS)
    snprintf(url, sizeof(url),
             "https://github.com/Ringaire/RingEcho-Lang-C/releases/download/v%s/rev-windows-x86_64.exe",
             version);
#else
    snprintf(url, sizeof(url),
             "https://github.com/Ringaire/RingEcho-Lang-C/releases/download/v%s/rev-linux-x86_64",
             version);
#endif

    if (!make_path(version_dir)) {
        fprintf(stderr, "cannot create version directory\n");
        return 1;
    }

    char rev_path[1100];
    snprintf(rev_path, sizeof(rev_path), "%s/rev%s", version_dir,
             RE0_PLATFORM_EXECUTABLE_SUFFIX);
    char *curl_argv[] = { "curl", "-sL", url, "-o", rev_path, NULL };
    int rc = run_program("curl", curl_argv);
    if (rc == 0) {
#if !defined(RE0_PLATFORM_WINDOWS)
        char *chmod_argv[] = { "chmod", "+x", rev_path, NULL };
        run_program("chmod", chmod_argv);
#endif
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

    char versions_dir[600];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    char version_dir[700];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", versions_dir, version);

    struct stat st;
    if (stat(version_dir, &st) != 0) {
        fprintf(stderr, "version %s is not installed (run 'rvm install %s')\n", version, version);
        return 1;
    }

    char current_link[600];
    get_current_link(current_link, sizeof(current_link));
#if defined(RE0_PLATFORM_WINDOWS)
    DeleteFileA(current_link);
    CreateSymbolicLinkA(current_link, version_dir, 0);
#else
    unlink(current_link);
    symlink(version_dir, current_link);
#endif

    printf("Now using RingEcho %s\n", version);
    printf("Add to PATH: export PATH=\"%s:$PATH\"\n", current_link);
    return 0;
}

static int cmd_list(void) {
    char versions_dir[600];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    struct stat st;
    if (stat(versions_dir, &st) != 0) {
        printf("No versions installed\n");
        printf("Run 'rvm install <version>' to install\n");
        return 0;
    }

    printf("Installed versions:\n");
#if defined(RE0_PLATFORM_WINDOWS)
    {
        WIN32_FIND_DATAA fd;
        char pattern[700];
        snprintf(pattern, sizeof(pattern), "%s\\*", versions_dir);
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
    DIR *d = opendir(versions_dir);
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

static int cmd_current(void) {
    char current_link[600];
    get_current_link(current_link, sizeof(current_link));

    char buf[512];
#if defined(RE0_PLATFORM_WINDOWS)
    DWORD len = 0;
    {
        HANDLE h = CreateFileA(current_link, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            printf("No version selected (using system default: %s)\n", RVM_DEFAULT_VERSION);
            return 0;
        }
        len = GetFinalPathNameByHandleA(h, buf, sizeof(buf) - 1, 0);
        CloseHandle(h);
    }
    if (len == 0 || len >= sizeof(buf)) {
        printf("No version selected (using system default: %s)\n", RVM_DEFAULT_VERSION);
        return 0;
    }
    buf[len] = '\0';
#else
    ssize_t len = readlink(current_link, buf, sizeof(buf) - 1);
    if (len <= 0) {
        printf("No version selected (using system default: %s)\n", RVM_DEFAULT_VERSION);
        return 0;
    }
    buf[len] = '\0';
#endif

    char *ver = strrchr(buf, RE0_SEP);
    if (ver) ver++;
    else ver = buf;

    printf("Current: %s\n", ver);
    return 0;
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

static int cmd_uninstall(const char *version) {
    if (!version) {
        fprintf(stderr, "usage: rvm uninstall <version>\n");
        return 1;
    }
    if (!valid_version(version)) {
        fprintf(stderr, "invalid version '%s'\n", version);
        return 1;
    }

    char versions_dir[600];
    get_versions_dir(versions_dir, sizeof(versions_dir));

    char version_dir[700];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", versions_dir, version);

    struct stat st;
    if (stat(version_dir, &st) != 0) {
        printf("Version %s is not installed\n", version);
        return 1;
    }

#if defined(RE0_PLATFORM_WINDOWS)
    remove_directory_recursive(version_dir);
#else
    char *rm_argv[] = { "rm", "-rf", version_dir, NULL };
    run_program("rm", rm_argv);
#endif
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
