/*
 * toml_config.h — ringecho.toml project configuration parsing
 *
 * Format:
 * [package]
 * name = "myapp"
 * version = "0.1.0"
 * entry = "main.reo"
 *
 * [dependencies]
 * json = "0.1.0"
 * parser = "git@github.com:user/reo-parser"
 */
#ifndef RE0_TOML_CONFIG_H
#define RE0_TOML_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define RE0_TOML_MAX_NAME 128
#define RE0_TOML_MAX_ENTRY 256
#define RE0_TOML_MAX_VERSION 32
#define RE0_TOML_MAX_DEPS 32
#define RE0_TOML_MAX_DEP_NAME 64
#define RE0_TOML_MAX_DEP_SPEC 256

typedef struct {
    char name[RE0_TOML_MAX_NAME];
    char spec[RE0_TOML_MAX_DEP_SPEC];
} ReoDependency;

typedef struct {
    char package_name[RE0_TOML_MAX_NAME];
    char package_version[RE0_TOML_MAX_VERSION];
    char entry[RE0_TOML_MAX_ENTRY];
    ReoDependency deps[RE0_TOML_MAX_DEPS];
    int dep_count;
    bool valid;
} ReoTomlConfig;

/* default configuration (fallback) */
ReoTomlConfig reo_toml_default(void);

/* parse ringecho.toml from file; returns default config on failure */
ReoTomlConfig reo_toml_load(const char *path);

/* find project root directory (search upward for ringecho.toml) */
bool reo_toml_find_root(char *out_dir, size_t cap);

/* write default ringecho.toml */
bool reo_toml_write_default(const char *dir, const char *name);

#endif
