/*
 * venv.h — RingEcho 虚拟环境管理
 *
 * 类似 Python venv：创建隔离的项目环境
 *
 * .renv/
 *   lib/
 *     std/           标准库（随 rem 安装）
 *     packages/      第三方包
 *   bin/
 *     rem            rem wrapper
 *   reo.toml         环境锁文件
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

/* 标准库模块列表 */
__attribute__((unused)) static const char *RE0_STD_MODULES[RE0_STD_MODULES_COUNT] = {
    "io", "string", "math", "vec"
};

/* 创建虚拟环境 */
bool reo_venv_create(const char *project_dir);

/* 检测当前是否在虚拟环境中（向上查找 .renv/） */
bool reo_venv_detect(char *out_env_dir, size_t cap);

/* 解析 import 路径（多级 fallback）
 * 返回 true 表示找到文件，path 写入完整路径 */
bool reo_venv_resolve_import(const char *module_name,
                              const char *project_dir,
                              const char *env_dir,
                              char *out_path, size_t cap);

/* 安装标准库到虚拟环境 */
bool reo_venv_install_std(const char *env_dir);

/* 输出 shell 激活脚本 */
void reo_venv_print_activate(const char *env_dir);

#endif
