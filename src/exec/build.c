#include "build.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#if !defined(RE0_PLATFORM_WINDOWS)
#include <unistd.h>
#include <sys/wait.h>
#endif

#if defined(RE0_PLATFORM_WINDOWS)
#include <direct.h>
#include <process.h>
#include <windows.h>
#define RE0_MKDIR(path) _mkdir(path)
#define RE0_PROCESS_ID() ((unsigned long)_getpid())
#define RE0_TEMP_DIRECTORY "target\\Temp"
#define RE0_PATH_SEPARATOR "\\"
#elif defined(RE0_PLATFORM_MACOS)
#include <sys/stat.h>
#include <unistd.h>
#define RE0_MKDIR(path) mkdir((path), 0700)
#define RE0_PROCESS_ID() ((unsigned long)getpid())
#define RE0_TEMP_DIRECTORY "target/Temp"
#define RE0_PATH_SEPARATOR "/"
#elif defined(RE0_PLATFORM_LINUX)
#include <sys/stat.h>
#include <unistd.h>
#define RE0_MKDIR(path) mkdir((path), 0700)
#define RE0_PROCESS_ID() ((unsigned long)getpid())
#define RE0_TEMP_DIRECTORY "target/Temp"
#define RE0_PATH_SEPARATOR "/"
#endif

#define RE0_TARGET_DIRECTORY "target"
#define RE0_TEMP_PATH_CAPACITY 512

static atomic_uint_fast64_t re0_temp_counter = ATOMIC_VAR_INIT(0);

static bool ensure_directory(Re0Build *b, const char *path) {
    if (RE0_MKDIR(path) == 0 || errno == EEXIST) return true;
    re0_error_append(b->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                     "cannot create build directory '%s': error %d", path, errno);
    return false;
}

static bool ensure_temp_directory(Re0Build *b) {
    return ensure_directory(b, RE0_TARGET_DIRECTORY) &&
           ensure_directory(b, RE0_TEMP_DIRECTORY);
}

static bool make_temp_path(Re0Build *b, char *path, size_t path_size,
                           const char *stem, const char *suffix) {
    if (!b || !path || path_size == 0 || !stem || !suffix) return false;
    if (!ensure_temp_directory(b)) return false;

    struct timespec now = {0};
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        now.tv_sec = time(NULL);
        now.tv_nsec = 0;
    }
    uint_fast64_t serial = atomic_fetch_add_explicit(&re0_temp_counter, 1,
                                                      memory_order_relaxed);
    int written = snprintf(path, path_size, "%s%s%s_%lu_%lld_%llu%s",
                           RE0_TEMP_DIRECTORY, RE0_PATH_SEPARATOR, stem,
                           RE0_PROCESS_ID(),
                           (long long)now.tv_nsec, (unsigned long long)serial, suffix);
    if (written < 0 || (size_t)written >= path_size) {
        re0_error_append(b->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "temporary build path exceeds %zu bytes", path_size - 1);
        path[0] = '\0';
        return false;
    }
    return true;
}

void re0_build_init(Re0Build *b, Re0ErrorList *errors) {
    b->errors = errors;
    const char *env_cc = getenv("REO_CC");
    b->cc_path = (env_cc && *env_cc) ? env_cc : RE0_PLATFORM_DEFAULT_C_COMPILER;
    b->output_path = NULL;
    b->tmp_file[0] = '\0';
    b->keep_c = false;
}

bool re0_build_compile(Re0Build *b, const char *c_code, const char *output_path) {
    if (!b || !c_code || !output_path) return false;

    if (!make_temp_path(b, b->tmp_file, sizeof(b->tmp_file), "re0_codegen", ".c"))
        return false;
    FILE *f = fopen(b->tmp_file, "wb");
    if (!f) {
        re0_error_append(b->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "cannot write temporary C file '%s'", b->tmp_file);
        return false;
    }
    size_t code_size = strlen(c_code);
    bool write_ok = fwrite(c_code, 1, code_size, f) == code_size;
    bool close_ok = fclose(f) == 0;
    if (!write_ok || !close_ok) {
        re0_error_append(b->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "cannot fully write temporary C file '%s'", b->tmp_file);
        if (!b->keep_c) remove(b->tmp_file);
        return false;
    }

    int rc;
    const char *cc_opt = getenv("REO_CC_OPT");
    if (!cc_opt || !*cc_opt) cc_opt = "-O1";
#if defined(RE0_PLATFORM_WINDOWS)
    /* 使用 CreateProcess 避免命令注入 */
    char args[2048];
    int args_written = snprintf(args, sizeof(args), "%s %s -pthread \"%s\" -o \"%s\"",
                               b->cc_path, cc_opt, b->tmp_file, output_path);
    if (args_written < 0 || (size_t)args_written >= sizeof(args)) {
        re0_error_append(b->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "compiler arguments exceed limit");
        if (!b->keep_c) remove(b->tmp_file);
        return false;
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(
        b->cc_path,           // lpApplicationName
        args,                 // lpCommandLine
        NULL,                 // lpProcessAttributes
        NULL,                 // lpThreadAttributes
        FALSE,                // bInheritHandles
        0,                    // dwCreationFlags
        NULL,                 // lpEnvironment
        NULL,                 // lpCurrentDirectory
        &si,                  // lpStartupInfo
        &pi                   // lpProcessInformation
    );
    if (!ok) {
        re0_error_append(b->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "cannot launch C compiler (error %lu)", GetLastError());
        if (!b->keep_c) remove(b->tmp_file);
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        re0_error_append(b->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "cannot get compiler exit code");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (!b->keep_c) remove(b->tmp_file);
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    rc = (int)exit_code;
#else
    char *cc_argv[] = { (char*)b->cc_path, (char*)cc_opt, "-pthread", b->tmp_file, "-o", (char*)output_path, NULL };
    pid_t cc_pid = fork();
    if (cc_pid < 0) {
        re0_error_append(b->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "cannot launch C compiler");
        if (!b->keep_c) remove(b->tmp_file);
        return false;
    }
    if (cc_pid == 0) { execvp(b->cc_path, cc_argv); _exit(127); }
    int cc_st = 0;
    while (waitpid(cc_pid, &cc_st, 0) < 0 && errno == EINTR) {}
    rc = WIFEXITED(cc_st) ? WEXITSTATUS(cc_st) : -1;
#endif
    if (rc != 0) {
        re0_error_append(b->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "compilation failed (exit code %d)", rc);
        if (!b->keep_c) remove(b->tmp_file);
        return false;
    }
    if (!b->keep_c && remove(b->tmp_file) != 0 && errno != ENOENT) {
        re0_error_append(b->errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                         "cannot remove temporary C file '%s'", b->tmp_file);
    }
    return true;
}

bool re0_build_temp_output_path(Re0Build *b, char *path, size_t path_size) {
    return make_temp_path(b, path, path_size, "re0_run",
                          RE0_PLATFORM_EXECUTABLE_SUFFIX);
}

void re0_build_destroy(Re0Build *b) {
    (void)b;
}
