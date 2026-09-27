# 特性路线图（宿主集成所需）

本文汇总在把 RingEcho 用作 **宿主语言**（例如 [Reverie](https://github.com/RingAire01/Reverie)，
一套类 Tauri 的 `.reo` 桌面运行时）时暴露出的工具链与语言缺口。既有的
[NATIVE.md](NATIVE.md)、[TYPE_SYSTEM.md](TYPE_SYSTEM.md) 分别跟踪原生子集与
类型/布局迁移；本文只记录这些文档尚未覆盖、但阻塞宿主集成的能力。

所有证据均在 Windows 11 + LLVM-MinGW 22.1.8 + `rev v0.2.0` 上实测。

## 已跟踪（不重复）

| 事项 | 出处 |
|------|------|
| 原生后端子集（无指针/聚合/字符串等） | [NATIVE.md](NATIVE.md) |
| 函数值与函数指针的真实 C 签名 | [TYPE_SYSTEM.md](TYPE_SYSTEM.md) |
| Lambda 不支持捕获环境 | [TYPE_SYSTEM.md](TYPE_SYSTEM.md) |

## P0 — 工具链（阻塞宿主集成）

| # | 缺口 | 状态 | 说明 |
|---|------|------|------|
| T1 | C `#include` 通道 | ✅ 已实现 | `--include <h>` → gcc `-include` |
| T2 | 链接参数通道 | ✅ 已实现 | `--lib-dir <d>` / `--link <l>` → gcc `-L` / `-l` |
| T3 | 整数 ⇄ 函数指针转换 | ✅ 已实现 | `f as u64` 与 `addr as fn(...)`；`LoadLibrary`+`GetProcAddress` 式动态派发 |

`rev run/build` 现支持可重复的 `--include`、`--lib-dir`、`--link`（各上限
`RE0_BUILD_MAX_FLAGS` = 16），分别透传为 gcc 的 `-include`、`-L`、`-l`。
实现位于 `include/exec/build.h`、`src/exec/build.c`、`src/exec/rev_main.c`。

已在 Windows 11 + LLVM-MinGW 22.1.8 验证：链接自建静态库后

```bash
rev build app.reo --include test.h --lib-dir . --link reverietest -o out.exe
```

生成的程序正确调用 C 函数（`reverie_test_add(20, 22)` → `42`）。整仓 `.reo`
回归 78/78 通过，无回归。T1+T2 已解锁宿主集成（如 Reverie M1）；T3 是更通用
但非必需的替代。

## P1 — 语言

| # | 缺口 | 现状 | 影响 |
|---|------|------|------|
| L1 | 捕获式闭包 | 仅无捕获 Lambda | 带状态的事件回调/命令处理 |
| L2 | 全局/静态状态 | 无 globals | 运行时单例、注册表、句柄表 |
| L3 | Windows 线程/异步 | `spawn/await` 基于 pthread，Win32 线程模型未验证 | 事件循环与异步 I/O |
| L4 | 调试信息 | 未发射 DWARF/PDB | 宿主排障 |
| L5 | `const` 声明 | ✅ 已支持（需 `;`） | 顶层/局部常量 |
| L6 | 命名空间/模块限定 | 全局扁平，无限定名 | 库无法隔离符号；ABI 名不能加前缀 |
| L7 | 条件编译 `@cfg(os)` | ✅ 已实现 | 按目标 OS 保留/丢弃顶层声明（不作用于 import） |

## P2 — 诊断与工具

| # | 项 | 现状 |
|---|----|------|
| D1 | `rev check` 不解析 import | 只加载入口文件；报 undefined function 误报 |
| D2 | 生成 C 告警噪声 | clang 下 `-Wunused-value`、`-Wparentheses-equality` |
| D3 | `module` 为保留字 | 常见参数名冲突（如 `GetProcAddress` 的 `module`） |

## 验收方式

每项应给出最小可复现命令与期望输出。例如：

```bash
# T1+T2：宿主能否链接系统库并包含头文件
rev build app.reo --include windows.h --link user32 -o app.exe

# T3：动态派发
#   let addr: u64 = GetProcAddress(lib, "Foo"); let f = addr as fn(i32) -> i32;

# L1：捕获式闭包
#   let base = 10; let f = |x: i32| x + base;   // 现有实现已解析，但不捕获
```

## 优先级

P0（T1、T2）是宿主集成的硬前置；T3、P1 随后。P2 是体验问题，可与 P1 并行。
