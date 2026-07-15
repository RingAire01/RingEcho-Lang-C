#include "build.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#if defined(RE0_PLATFORM_WINDOWS)
#include <direct.h>
#include <process.h>
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
    b->cc_path = RE0_PLATFORM_DEFAULT_C_COMPILER;
    b->output_path = NULL;
    b->tmp_file[0] = '\0';
    b->keep_c = true;
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

    char cmd[1024];
    int command_size = snprintf(cmd, sizeof(cmd), "%s -O2 -pthread \"%s\" -o \"%s\" 2>&1",
                                b->cc_path, b->tmp_file, output_path);
    if (command_size < 0 || (size_t)command_size >= sizeof(cmd)) {
        re0_error_append(b->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "C compiler command exceeds internal limit");
        if (!b->keep_c) remove(b->tmp_file);
        return false;
    }
    int rc = system(cmd);
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
