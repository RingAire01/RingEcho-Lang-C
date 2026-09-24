#include "exec/native_build.h"
#include "exec/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if defined(__linux__)
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Files are published by rename from an exclusive adjacent temporary file.
 * Link failures leave the prior destination intact. No shell is involved. */
bool re0_native_build(Re0Build *build, const Re0Buffer *object,
                      const char *output, bool emit_object) {
    if (!build || !object || object->failed || !object->len || !output || !*output) return false;
#if defined(__linux__)
    const size_t max_output = 4096;
    size_t length = strlen(output);
    if (length > max_output) {
        re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL, "native output path is too long");
        return false;
    }
    const char suffix[] = ".reo-XXXXXX";
    char *temporary = malloc(length + sizeof(suffix));
    if (!temporary) {
        re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL, "cannot allocate native output path");
        return false;
    }
    memcpy(temporary, output, length); memcpy(temporary + length, suffix, sizeof(suffix));
    int fd = mkstemp(temporary);
    if (fd < 0) {
        re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "cannot create native output: %s", strerror(errno));
        free(temporary); return false;
    }
    char object_path[512] = {0};
    FILE *file = NULL;
    bool ok = true;
    if (emit_object) {
        file = fdopen(fd, "wb");
        if (!file) { close(fd); ok = false; }
    } else {
        if (close(fd) != 0) ok = false;
        if (ok) ok = re0_build_temp_output_path(build, object_path, sizeof(object_path));
        if (ok) { file = fopen(object_path, "wbx"); if (!file) ok = false; }
    }
    if (file) {
        if (fwrite(object->data, 1, object->len, file) != object->len) ok = false;
        if (fclose(file) != 0) ok = false;
    }
    if (ok && !emit_object) {
        const char *linker = getenv("REO_LD");
        if (!linker || !*linker) linker = "ld";
        const char *args[] = {linker, "-m", "elf_x86_64", "-z", "noexecstack",
                             "--build-id=none", "-e", "_start", "-o", temporary, object_path, NULL};
        int rc = re0_process_run(linker, args);
        if (rc != 0) {
            re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                             "native linker failed (exit code %d); extern symbols require external linking of --emit obj output", rc);
            ok = false;
        }
    }
    if (ok && chmod(temporary, emit_object ? 0600 : 0700) != 0) ok = false;
    if (ok && rename(temporary, output) != 0) ok = false;
    if (!ok) {
        re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL, "native output was not published");
        if (remove(temporary) != 0 && errno != ENOENT)
            re0_error_append(build->errors, RE0_WARN, RE0_SPAN_ZERO, NULL, "cannot remove native temporary output");
    }
    if (*object_path && remove(object_path) != 0 && errno != ENOENT)
        re0_error_append(build->errors, RE0_WARN, RE0_SPAN_ZERO, NULL, "cannot remove native temporary object");
    free(temporary);
    return ok;
#else
    (void)emit_object;
    re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                     "experimental native output currently requires a Linux host");
    return false;
#endif
}
