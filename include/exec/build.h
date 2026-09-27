#ifndef RE0_BUILD_H
#define RE0_BUILD_H
#include "base/buffer.h"
#include "base/error.h"
#include <stdbool.h>
#include <stddef.h>

#define RE0_BUILD_MAX_FLAGS 16

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
    /* Host-library integration forwarded to the C compiler: `-include`, `-L`,
     * and `-l` respectively. The strings are borrowed from the caller (argv),
     * not copied, so they must outlive the build. */
    const char   *includes[RE0_BUILD_MAX_FLAGS];
    int           include_count;
    const char   *lib_dirs[RE0_BUILD_MAX_FLAGS];
    int           lib_dir_count;
    const char   *libs[RE0_BUILD_MAX_FLAGS];
    int           lib_count;
} Re0Build;

void re0_build_init(Re0Build *b, Re0ErrorList *errors);
/* Each returns false once RE0_BUILD_MAX_FLAGS entries are already stored. */
bool re0_build_add_include(Re0Build *b, const char *header);
bool re0_build_add_lib_dir(Re0Build *b, const char *dir);
bool re0_build_add_link(Re0Build *b, const char *lib);
bool re0_build_compile(Re0Build *b, const char *c_code, const char *output_path);
bool re0_build_write_source(Re0Build *b, const char *c_code);
bool re0_build_compile_shared(Re0Build *b, const char *c_code, const char *output_path);
bool re0_build_temp_output_path(Re0Build *b, char *path, size_t path_size);
void re0_build_destroy(Re0Build *b);

#endif
