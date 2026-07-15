# RingEcho-Lang-C — 构建配置

## 编译器

| 平台 | 默认编译器 | 可执行后缀 |
|------|-----------|-----------|
| Linux | gcc | (无) |
| macOS | clang | (无) |
| Windows | gcc (MSYS2/MinGW) | .exe |

覆盖: `make CC=clang CONFIG=Release build`

## Profiles

| Profile | CFLAGS | 用途 |
|---------|--------|------|
| Release | `-O3 -DNDEBUG` | 生产 |
| Debug | `-O0 -g3 -DRE0_DEBUG=1` | 调试 |
| Alpha | `-O2 -g -DRE0_ALPHA=1` | 内部 |

## 目标架构

| 架构 | CI Runner | 状态 |
|------|-----------|------|
| x86_64 | ubuntu-latest, windows-latest | ✅ |
| aarch64 | ubuntu-24.04-arm, macos-latest | ✅ |

## 目标平台

| 平台 | 状态 |
|------|------|
| Linux | ✅ |
| macOS | ✅ |
| Windows | ✅ |
| HarmonyOS | ❌ |

## pthread

生成的 C 程序使用 `-pthread` 链接（spawn/await 运行时）。

## 构建产物

```
target/<Release|Debug|Alpha>/rem[.exe]
target/<Release|Debug|Alpha>/obj/...
target/Temp/              生成的临时 C 文件
```
