#ifndef RE0_BUILD_H
#define RE0_BUILD_H
#include "buffer.h"
#include "error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    Re0ErrorList *errors;
    const char   *cc_path;
    const char   *output_path;
    char          tmp_file[512];
    bool          keep_c;
} Re0Build;

void re0_build_init(Re0Build *b, Re0ErrorList *errors);
bool re0_build_compile(Re0Build *b, const char *c_code, const char *output_path);
bool re0_build_temp_output_path(Re0Build *b, char *path, size_t path_size);
void re0_build_destroy(Re0Build *b);

#endif
