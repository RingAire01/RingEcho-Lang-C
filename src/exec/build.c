#include "exec/build.h"
#include "exec/process.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#if !defined(RE0_PLATFORM_WINDOWS)
#include <unistd.h>
#include <sys/wait.h>
#endif

#if defined(RE0_PLATFORM_WINDOWS)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <process.h>
#include <windows.h>
#define RE0_PROCESS_ID() ((unsigned long)_getpid())
#define RE0_PATH_SEPARATOR "\\"
#elif defined(RE0_PLATFORM_MACOS)
#include <sys/stat.h>
#include <unistd.h>
#define RE0_PROCESS_ID() ((unsigned long)getpid())
#define RE0_PATH_SEPARATOR "/"
#elif defined(RE0_PLATFORM_LINUX)
#include <sys/stat.h>
#include <unistd.h>
#define RE0_PROCESS_ID() ((unsigned long)getpid())
#define RE0_PATH_SEPARATOR "/"
#endif


static atomic_uint_fast64_t re0_temp_counter = ATOMIC_VAR_INIT(0);

static bool ensure_temp_directory(Re0Build *b) {
    if (b->temp_dir[0]) return true;
#if defined(RE0_PLATFORM_WINDOWS)
    char root[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(root), root);
    if (!length || length >= sizeof(root)) goto failed;
    if (!GetTempFileNameA(root, "reo", 0, b->temp_dir)) goto failed;
    if (!DeleteFileA(b->temp_dir) || !CreateDirectoryA(b->temp_dir, NULL)) goto failed;
#else
    /* mkdtemp atomically creates a private directory under the system temp root. */
    snprintf(b->temp_dir, sizeof(b->temp_dir), "/tmp/ringecho-XXXXXX");
    if (!mkdtemp(b->temp_dir)) goto failed;
#endif
    return true;
failed:
    b->temp_dir[0] = 0;
    re0_error_append(b->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                     "cannot create a private temporary build directory");
    return false;
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
                           b->temp_dir, RE0_PATH_SEPARATOR, stem,
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
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->errors = errors;
    const char *env_cc = getenv("REO_CC");
    b->cc_path = (env_cc && *env_cc) ? env_cc : RE0_PLATFORM_DEFAULT_C_COMPILER;
    b->output_path = NULL;
    b->tmp_file[0] = '\0';
    b->keep_c = true;
    const char *keep_source = getenv("REO_KEEP_C");
    if (keep_source && strcmp(keep_source, "0") == 0) b->keep_c = false;
    else if (keep_source && strcmp(keep_source, "1") != 0)
        re0_error_append(errors, RE0_WARN, RE0_SPAN_ZERO, NULL,
                         "invalid REO_KEEP_C; using default 1");
    b->shared = false;
}

bool re0_build_write_source(Re0Build *b, const char *c_code) {
    if (!b || !c_code) return false;
    if (!make_temp_path(b, b->tmp_file, sizeof(b->tmp_file), "re0_codegen", ".c"))
        return false;
#if defined(RE0_PLATFORM_WINDOWS)
    int descriptor = _open(b->tmp_file, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    FILE *f = descriptor < 0 ? NULL : _fdopen(descriptor, "wb");
    if (descriptor >= 0 && !f) _close(descriptor);
#else
    int descriptor = open(b->tmp_file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    FILE *f = descriptor < 0 ? NULL : fdopen(descriptor, "wb");
    if (descriptor >= 0 && !f) close(descriptor);
#endif
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

    return true;
}

bool re0_build_compile(Re0Build *b, const char *c_code, const char *output_path) {
    if (!b || !c_code || !output_path || !*output_path) return false;
    if (!re0_build_write_source(b, c_code)) return false;
    const char *cc_opt = getenv("REO_CC_OPT");
    if (!cc_opt || !*cc_opt) cc_opt = "-O1";
    const char *arguments[] = {b->cc_path, cc_opt, "-pthread", b->tmp_file,
                              "-o", output_path, "-Werror=int-conversion",
                              "-Werror=incompatible-pointer-types", "-Werror=cast-function-type",
                              "-Werror=return-type", "-Werror=implicit-function-declaration",
                              NULL, NULL, NULL};
    if (b->shared) {
        arguments[11] = "-shared";
        arguments[12] = "-fPIC";
    }
    int rc = re0_process_run(b->cc_path, arguments);
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

bool re0_build_compile_shared(Re0Build *b, const char *c_code, const char *output_path) {
    if (!b) return false;
    const bool prev = b->shared;
    b->shared = true;
    const bool ok = re0_build_compile(b, c_code, output_path);
    b->shared = prev;
    return ok;
}

bool re0_build_temp_output_path(Re0Build *b, char *path, size_t path_size) {
    return make_temp_path(b, path, path_size, "re0_run",
                          RE0_PLATFORM_EXECUTABLE_SUFFIX);
}

void re0_build_destroy(Re0Build *b) {
    if (!b) return;
    if (!b->keep_c && b->tmp_file[0]) remove(b->tmp_file);
    if (b->temp_dir[0]) {
#if defined(RE0_PLATFORM_WINDOWS)
        _rmdir(b->temp_dir);
#else
        rmdir(b->temp_dir);
#endif
    }
}
