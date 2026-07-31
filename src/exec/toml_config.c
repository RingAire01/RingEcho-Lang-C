/*
 * toml_config.c — ringecho.toml 精简解析器
 *
 * 不使用外部 TOML 库，手写极简解析器：
 * 只支持 [section] + key = "value" 格式。
 */
#include "toml_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

ReoTomlConfig reo_toml_default(void) {
    ReoTomlConfig c;
    memset(&c, 0, sizeof(c));
    strncpy(c.package_name, "untitled", sizeof(c.package_name) - 1);
    strncpy(c.package_version, "0.1.0", sizeof(c.package_version) - 1);
    strncpy(c.entry, "main.reo", sizeof(c.entry) - 1);
    c.dep_count = 0;
    c.valid = false;
    return c;
}

/* 去除首尾空白 */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* 去除引号 */
static char *unquote(char *s) {
    char *start = s;
    if (*start == '"') start++;
    char *end = start + strlen(start) - 1;
    if (end >= start && *end == '"') *end = '\0';
    return start;
}

ReoTomlConfig reo_toml_load(const char *path) {
    ReoTomlConfig cfg = reo_toml_default();
    FILE *f = fopen(path, "r");
    if (!f) return cfg;

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;

        /* [section] */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                strncpy(section, p + 1, sizeof(section) - 1);
                section[sizeof(section)-1] = '\0';
            }
            continue;
        }

        /* key = value */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        val = unquote(val);

        if (strcmp(section, "package") == 0) {
            if (strcmp(key, "name") == 0) {
                strncpy(cfg.package_name, val, sizeof(cfg.package_name) - 1);
            } else if (strcmp(key, "version") == 0) {
                strncpy(cfg.package_version, val, sizeof(cfg.package_version) - 1);
            } else if (strcmp(key, "entry") == 0) {
                strncpy(cfg.entry, val, sizeof(cfg.entry) - 1);
            }
        } else if (strcmp(section, "dependencies") == 0) {
            if (cfg.dep_count < RE0_TOML_MAX_DEPS) {
                strncpy(cfg.deps[cfg.dep_count].name, key,
                        sizeof(cfg.deps[cfg.dep_count].name) - 1);
                strncpy(cfg.deps[cfg.dep_count].spec, val,
                        sizeof(cfg.deps[cfg.dep_count].spec) - 1);
                cfg.dep_count++;
            }
        }
    }

    fclose(f);
    cfg.valid = true;
    return cfg;
}

bool reo_toml_find_root(char *out_dir, size_t cap) {
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) return false;

    char dir[512];
    strncpy(dir, cwd, sizeof(dir) - 1);
    dir[sizeof(dir)-1] = '\0';

    for (int i = 0; i < 20; i++) {
        char toml_path[512];
        snprintf(toml_path, sizeof(toml_path), "%s/ringecho.toml", dir);
        if (access(toml_path, R_OK) == 0) {
            strncpy(out_dir, dir, cap - 1);
            out_dir[cap - 1] = '\0';
            return true;
        }
        /* 向上一级 */
        char *p = strrchr(dir, '/');
        if (!p || p == dir) break;
        *p = '\0';
    }
    return false;
}

bool reo_toml_write_default(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/ringecho.toml", dir);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "[package]\n");
    fprintf(f, "name = \"%s\"\n", name ? name : "untitled");
    fprintf(f, "version = \"0.1.0\"\n");
    fprintf(f, "entry = \"main.reo\"\n");
    fprintf(f, "\n[dependencies]\n");
    fclose(f);
    return true;
}
