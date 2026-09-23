# RingEcho-Lang-C

RingEcho 编译器工具链的零依赖 C 实现。

将 `.reo` 源码 → C 源码 → gcc → 原生二进制。无需 LLVM，无需任何外部库。

[English](README.md) | 简体中文 | [繁體中文](README.zht.md)

## 状态

| 项目 | 值 |
|------|-----|
| 版本 | 0.2.0 |
| 测试 | 通过（make test） |
| 工具链 | 3 个二进制：`rev` / `rem` / `rvm` |
| 后端 | C 源码、freestanding C、reo ISA、共享库、WebAssembly (WASI) |
| 许可证 | MIT |

## 工具链

| 二进制 | 角色 | 说明 |
|--------|------|------|
| `rev` | 编译器 / 求值器 | 编译、运行、类型检查、LSP 服务器、项目初始化、虚拟环境 |
| `rem` | 包管理器 | 安装 / 列出 / 移除 / 搜索包（类 npm） |
| `rvm` | 版本管理器 | 管理已安装的 RingEcho 编译器版本（类 nvm） |

## 构建

```bash
make CONFIG=Release build
```

| 配置 | 编译标志 |
|------|---------|
| Release | `-O3 -DNDEBUG` |
| Debug | `-O0 -g3` |
| Alpha | `-O2 -g` |

需要支持 pthread 的 C11 编译器（gcc/clang）。平台矩阵见 [BUILD.md](BUILD.md)。

## 用法

### rev — 编译器

```bash
rev run <file.reo> [--target c|reo|c-freestanding]  # 编译并运行
rev build <file.reo> [-o out]                       # 编译为可执行文件 / 汇编
rev build <file.reo> --shared                       # 编译为共享库（.so/.dll）
rev build <file.reo> --target wasm                  # 编译为 WebAssembly (WASI)
rev build                                           # 构建项目（读取 ringecho.toml）
rev check <file.reo>                                # 仅类型检查
rev lsp                                             # 启动 LSP 服务器
rev venv <init|activate>                            # 管理虚拟环境
rev init [name]                                     # 创建新项目
rev clean                                           # 清理构建产物
```

`rev init` 会生成包含 `main.reo`、`ringecho.toml`、`.gitignore` 和 `.renv/` 虚拟环境的项目脚手架。不带参数的 `rev build` 从 `ringecho.toml` 读取入口并输出到 `target/Release/`。

### rem — 包管理器

```bash
rem init [name]        # 初始化新项目
rem install <pkg>      # 安装包
rem list               # 列出已安装的包
rem remove <pkg>       # 移除包
rem update             # 更新依赖
rem search <query>     # 搜索包
rem publish            # 发布当前包
```

### rvm — 版本管理器

```bash
rvm install <version>  # 安装 RingEcho 版本
rvm use <version>      # 切换版本
rvm list               # 列出已安装版本
rvm current            # 显示当前版本
rvm uninstall <ver>    # 卸载版本
rvm remote             # 列出可用版本
```

## 特性

- 完整类型系统（i8-i128、u8-u128、f32/f64、bool、char、str）
- 结构体、枚举、Trait + impl + 静态方法分派
- 泛型函数 + 结构体，惰性单态化
- Option/Result + `?` 运算符
- Lambda / 闭包（暂不支持捕获）
- Match 表达式（值匹配 + 枚举标签）
- 管道运算符 `|>`
- Component 关键字
- for-in 区间 + 字符串迭代
- spawn/await 并发（pthread）
- GC 引擎：3 种模式（none/auto/manual）× 3 种算法（tracing/arc-cycle/hybrid）
- LSP 服务器：JSON-RPC + 诊断
- 多文件模块（import 递归解析）
- extern C FFI
- char 类型，完整转义支持
- 虚拟环境（`.renv/`）及内置标准库（`std::io`、`std::math`、`std::string`、`std::vec`）
- 通过 `ringecho.toml` 进行项目配置

## 测试

```bash
make CONFIG=Debug test          # 端到端：编译并运行全部正向测试，
                                # 校验预期失败（语义 / 语法 / 运行时）
make test-one FILE=tests/hello.reo   # 运行单个测试
make check-one FILE=tests/hello.reo  # 对单个测试做类型检查
make test-list                       # 列出全部测试
```

## 许可证

[MIT](LICENSE) - Copyright (c) 2025-2026 辉夜铃 (KaguyaRing) & Ringaire
