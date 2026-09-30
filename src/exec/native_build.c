#include "exec/native_build.h"
#include "backend/native_target.h"
#include "exec/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/stat.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
/* GNU ld emulation name for an ELF target, or NULL when unsupported. */
static const char *elf_emulation(const Re0NativeTarget *target) {
    switch (target->arch) {
        case RE0_ARCH_X86_64: return "elf_x86_64";
        case RE0_ARCH_X86: return "elf_i386";
        case RE0_ARCH_AARCH64: return "aarch64elf";
        case RE0_ARCH_ARM: return "armelf_linux_eabi";
        default: return NULL;
    }
}
#endif
#if defined(__linux__) || defined(__APPLE__)
/* Mach-O architecture name for ld64. */
static const char *macho_arch(const Re0NativeTarget *target) {
    return target->arch == RE0_ARCH_X86_64 ? "x86_64" : "arm64";
}
#endif

/* Files are published by rename from an exclusive adjacent temporary file.
 * Link failures leave the prior destination intact. No shell is involved. */
bool re0_native_build(Re0Build *build, const Re0Buffer *object,
                      const char *output, bool emit_object,
                      const Re0NativeTarget *target) {
    if (!build || !object || object->failed || !object->len || !output || !*output) return false;
    const Re0NativeTarget *t = target ? target : re0_native_target_host();
    if (!t) {
        re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                         "no native target for this host; pass --target <triple>");
        return false;
    }
#if defined(__linux__) || defined(__APPLE__)
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
        if (t->os == RE0_OS_MACOS && (!linker || !*linker)) {
#if defined(__APPLE__)
            linker = "ld";
#else
            linker = "ld64.lld";
#endif
        }
        if (!linker || !*linker) linker = "ld";
        int rc = 0;
        if (t->os == RE0_OS_MACOS) {
            const char *args[] = {linker, "-arch", macho_arch(t),
                                  "-platform_version", "macos", "11.0", "11.0",
                                  "-static", "-e", "_start", "-o", temporary, object_path, NULL};
            rc = re0_process_run(linker, args);
        } else {
            const char *emulation = elf_emulation(t);
            if (!emulation) {
                re0_error_append(build->errors, RE0_ERR_IO, RE0_SPAN_ZERO, NULL,
                                 "linking this target is not supported yet; use --emit obj");
                ok = false;
            } else {
                const char *args[] = {linker, "-m", emulation, "-z", "noexecstack",
                                     "--build-id=none", "-e", "_start", "-o", temporary, object_path, NULL};
                rc = re0_process_run(linker, args);
            }
        }
        if (ok && rc != 0) {
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
