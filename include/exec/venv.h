/*
 * venv.h — RingEcho virtual environment management
 *
 * Similar to Python venv: create an isolated project environment
 *
 * .renv/
 *   lib/
 *     std/           standard library (installed with rem)
 *     packages/      third-party packages
 *   bin/
 *     rem            rem wrapper
 *   reo.toml         environment lock file
 */
#ifndef RE0_VENV_H
#define RE0_VENV_H

#include <stdbool.h>
#include <stddef.h>

#define RE0_VENV_DIR ".renv"
#define RE0_VENV_LIB "lib"
#define RE0_VENV_STD "std"
#define RE0_VENV_PACKAGES "packages"
#define RE0_VENV_BIN "bin"
#define RE0_VENV_CONFIG "reo.toml"

#define RE0_GLOBAL_LIB_DIR ".re/lib"
#define RE0_STD_MODULES_COUNT 4

/* standard library module list */
__attribute__((unused)) static const char *RE0_STD_MODULES[RE0_STD_MODULES_COUNT] = {
    "io", "string", "math", "vec"
};

/* create a virtual environment */
bool reo_venv_create(const char *project_dir);

/* detect whether currently inside a virtual environment (search upward for .renv/) */
bool reo_venv_detect(char *out_env_dir, size_t cap);

/* resolve import path (multi-level fallback)
 * returns true if the file is found; full path written to out_path */
bool reo_venv_resolve_import(const char *module_name,
                              const char *project_dir,
                              const char *env_dir,
                              char *out_path, size_t cap);

/* install the standard library into the virtual environment */
bool reo_venv_install_std(const char *env_dir);

/* print the shell activation script */
void reo_venv_print_activate(const char *env_dir);

#endif
