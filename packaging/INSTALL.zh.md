# RingEcho 安装说明

安装包包含 `rev` 编译器（含 LSP）、`rem` 包管理器、`rvm` 版本管理器及许可证。标准库模板内置于工具中，使用 `rev init` 或 `rem init` 创建项目时生成；不需要把构建机的 `.renv` 复制进安装目录。

## Windows

运行与系统架构匹配的 `*-setup.exe`。默认按当前用户安装到 `%LOCALAPPDATA%\Programs\RingEcho\<架构>`，不需要管理员权限。可从系统“已安装的应用”卸载。

安装后将安装目录中的 `bin` 加入用户 PATH；或者直接使用其中的 `rev.exe`、`rem.exe`、`rvm.exe`。便携版解压后同样使用 `bin` 目录。

生成并运行 C 程序还需要带 Windows 系统库及 pthread 支持的 C 工具链，例如 LLVM-MinGW 或 MinGW-w64。将工具链加入 PATH；默认调用 gcc，可用环境变量 `REO_CC` 指定编译器可执行文件。安装包已静态链接工具自身所需的 MinGW 运行库，启动工具不要求额外安装 winpthread DLL。

当前 Windows 安装包未进行代码签名。CI 验证安装、启动、语义检查与卸载；Windows 完整运行时回归为下一阶段工作，不能将这些安装测试当成完整语言功能验证。

## macOS

打开对应 Intel x64 或 Apple Silicon ARM64 的 `.pkg`，或使用：

```sh
sudo installer -pkg ringecho-<版本>-macos-<架构>.pkg -target /
```

工具安装到 `/usr/local/bin`，说明与清单位于 `/usr/local/share/doc/ringecho`。最低部署目标为 macOS 13。生成本机程序需要 Xcode Command Line Tools：`xcode-select --install`。

安装包目前未签名、未公证。便携版可解压到用户目录并把其中的 `bin` 加入 PATH。卸载 pkg 安装的版本时，删除 `/usr/local/bin` 中的三个对应工具、上述说明目录，再执行 `sudo pkgutil --forget org.ringaire.ringecho`。操作前确认这些文件仍属于本次安装。

## Linux

Debian/Ubuntu 使用与系统架构匹配的 `.deb`：

```sh
sudo apt install ./ringecho-<版本>-linux-<架构>.deb
```

工具安装到 `/usr/bin`，说明位于 `/usr/share/doc/ringecho`。卸载使用 `sudo apt remove ringecho`。其他发行版可使用 `.tar.gz` 便携包，但需要满足相同的 glibc ABI 依赖；这些不是 musl 静态包。包的 libc 依赖由构建产物自动计算。

生成程序需要 GCC 或 Clang 及 pthread 开发支持。检查安装：

```sh
rem version
rvm version
rev check your-source.reo
```

## 校验和功能边界

下载后先用同一构建产物中的 `SHA256SUMS` 校验文件；压缩包内 `manifest.json` 包含实际构建提交、平台、架构及文件摘要。开发包版本标识可能与工具原有固定 `version` 输出不同，以清单为准。

- `x64` 表示 x86_64，`x86` 表示 i686，`arm64` 表示 AArch64；不含 32 位 ARM。
- 当前 x86 的 C 运行时依赖原生 `__int128` 而受限，CI 只验证其工具启动和语义检查。完整运行时支持未完成，不应将 x86 包用于需要完整执行能力的部署。
- 这些产物未更改 rvm 中旧有的下载命名与安装事务；暂不通过 `rvm install` 安装本轮新格式软件包。
