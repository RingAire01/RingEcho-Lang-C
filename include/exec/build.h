#ifndef RE0_BUILD_H
#define RE0_BUILD_H
#include "base/buffer.h"
#include "base/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    Re0ErrorList *errors;
    const char   *cc_path;
    const char   *output_path;
    char          tmp_file[512];
    char          temp_dir[512];
    bool          keep_c;
    /* When true, link as a shared library (`-shared -fPIC`) instead of an
     * executable. */
    bool          shared;
} Re0Build;

void re0_build_init(Re0Build *b, Re0ErrorList *errors);
bool re0_build_compile(Re0Build *b, const char *c_code, const char *output_path);
bool re0_build_write_source(Re0Build *b, const char *c_code);
bool re0_build_compile_shared(Re0Build *b, const char *c_code, const char *output_path);
bool re0_build_temp_output_path(Re0Build *b, char *path, size_t path_size);
void re0_build_destroy(Re0Build *b);

#endif
