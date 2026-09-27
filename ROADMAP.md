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

| # | 缺口 | 实测证据 | 需求 |
|---|------|----------|------|
| T1 | 没有 C `#include` 通道 | `@` 属性仅识别 `@gc` / `@repr(C)`（`src/backend/codegen.c`） | 引入系统头（如 `<WebView2.h>`） |
| T2 | 没有链接参数通道 | `rev run/build` 仅接受 `--shared/--target/--backend/--emit/-o`；`re0_build_compile` 的 gcc 参数写死 | 链接 C shim 与系统库（`-l`/`-L`） |
| T3 | 不支持整数到函数指针转换 | `invalid cast from 'u64' to 'fn'` | `LoadLibrary`+`GetProcAddress` 式动态派发 |

建议最小方案：为 `rev run/build` 增加可重复的 `--include <h>`、`--lib-dir <d>`、
`--link <l>`，分别透传为 gcc 的 `-include`、`-L`、`-l`。T1+T2 即可解开 M1；
T3 是更通用但非必需的替代。

## P1 — 语言

| # | 缺口 | 现状 | 影响 |
|---|------|------|------|
| L1 | 捕获式闭包 | 仅无捕获 Lambda | 带状态的事件回调/命令处理 |
| L2 | 全局/静态状态 | 无 globals | 运行时单例、注册表、句柄表 |
| L3 | Windows 线程/异步 | `spawn/await` 基于 pthread，Win32 线程模型未验证 | 事件循环与异步 I/O |
| L4 | 调试信息 | 未发射 DWARF/PDB | 宿主排障 |
| L5 | `const` 声明 | 无 | 常量与 ABI 数值（当前只能写成函数或字面量） |
| L6 | 命名空间/模块限定 | 全局扁平，无限定名 | 库无法隔离符号；ABI 名不能加前缀 |

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
