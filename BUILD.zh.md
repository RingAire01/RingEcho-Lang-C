# RingEcho-Lang-C — 构建配置

[English](BUILD.md) | 简体中文 | [繁體中文](BUILD.zht.md)

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

## CI 安装包矩阵

工作流：[构建与安装包](.github/workflows/ci.yml)。ARM 均为 ARM64/AArch64，不包含 32 位 ARM。

| 平台 | 架构 | 构建 Runner | 原生安装包 |
|------|------|-------------|------------|
| Windows | x64 | ubuntu-24.04，LLVM-MinGW 交叉编译 | `.exe` |
| Windows | x86 | ubuntu-24.04，LLVM-MinGW 交叉编译 | `.exe` |
| Windows | ARM64 | ubuntu-24.04，LLVM-MinGW 交叉编译 | `.exe` |
| macOS | Intel x64 | macos-15-intel | `.pkg` |
| macOS | Apple Silicon ARM64 | macos-15 | `.pkg` |
| Linux | x64 | ubuntu-24.04 | `.deb` |
| Linux | x86 | ubuntu-24.04，gcc -m32 | `.deb` |
| Linux | ARM64 | ubuntu-24.04-arm | `.deb` |

每个包包含 `rev`、`rem`、`rvm`、使用说明、许可证和构建清单；另附 Windows `.zip` / Unix `.tar.gz` 便携包及 `SHA256SUMS`。Windows 安装程序在 windows-2025 上生成，x64/x86 安装测试在 windows-2025 上运行，ARM64 安装测试在 windows-11-arm 上运行。

触发方式：main 推送、面向 main 的 PR、`v*` 标签或手动 `workflow_dispatch`。产物在 Actions 对应运行的 Artifacts 中保留 30 天。标签版本使用标签名去掉 v；其他构建使用 `0.0.0-dev.<运行编号>.<提交短哈希>`。未自动创建或发布 GitHub Release，也未自动提交代码。

打包前解析 ELF/PE/Mach-O 头验证真实架构；Windows 静态链接工具运行库并检查外部 DLL 依赖。Linux 由 dpkg-shlibdeps 计算 ABI 依赖。Windows 工具链版本和上游 SHA256、Actions 的提交哈希已固定，构建启用 `-Werror`。Runner 标签依据 [GitHub 官方列表](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)；工具链依据 [LLVM-MinGW 20260616 发布](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260616)。

Linux/macOS 64 位目标执行现有完整回归和转换测试，然后安装软件包验证。Windows 当前验证安装、工具启动、语义检查和卸载，完整运行时测试待下一阶段补齐。x86 的生成 C 运行时依赖原生 `__int128`，目前只验证启动和语义检查，不能把打包通过当作完整运行时支持。总检查任务要求所有构建和安装验证成功，不忽略失败。

本地打包（示例为 Linux x64，需 dpkg-dev）：

```sh
make CONFIG=Release build
python3 -m unittest discover -s tests/ci -p 'test_*.py'
python3 scripts/ci/package.py --platform linux --arch x64 \
  --version 0.0.0-dev.local --revision "$(git rev-parse HEAD)" \
  --epoch "$(git show -s --format=%ct HEAD)"
```

输出位于 `target/packages/`。安装步骤、依赖、未签名包及功能边界见 [安装说明](packaging/INSTALL.zh.md)。本地验证不能代替尚未运行的 macOS/Windows Runner 结果。

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
target/<Release|Debug|Alpha>/rev[.exe]   编译器（含 LSP）
target/<Release|Debug|Alpha>/rem[.exe]   包管理器
target/<Release|Debug|Alpha>/rvm[.exe]   版本管理器
target/<Release|Debug|Alpha>/obj/...     中间对象文件
target/Temp/                             生成的临时 C 文件
```
