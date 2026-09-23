# RingEcho-Lang-C — Build Configuration

English | [简体中文](BUILD.zh.md) | [繁體中文](BUILD.zht.md)

## Compilers

| Platform | Default compiler | Executable suffix |
|----------|------------------|-------------------|
| Linux | gcc | (none) |
| macOS | clang | (none) |
| Windows | gcc (MSYS2/MinGW) | .exe |

Override: `make CC=clang CONFIG=Release build`

## Profiles

| Profile | CFLAGS | Purpose |
|---------|--------|---------|
| Release | `-O3 -DNDEBUG` | Production |
| Debug | `-O0 -g3 -DRE0_DEBUG=1` | Debugging |
| Alpha | `-O2 -g -DRE0_ALPHA=1` | Internal |

## Target architectures

| Architecture | CI Runner | Status |
|--------------|-----------|--------|
| x86_64 | ubuntu-latest, windows-latest | ✅ |
| aarch64 | ubuntu-24.04-arm, macos-latest | ✅ |

## Target platforms

| Platform | Status |
|----------|--------|
| Linux | ✅ |
| macOS | ✅ |
| Windows | ✅ |
| HarmonyOS | ❌ |

## pthread

Generated C programs link with `-pthread` (spawn/await runtime).

## Build artifacts

```
target/<Release|Debug|Alpha>/rev[.exe]   compiler (with LSP)
target/<Release|Debug|Alpha>/rem[.exe]   package manager
target/<Release|Debug|Alpha>/rvm[.exe]   version manager
target/<Release|Debug|Alpha>/obj/...     intermediate object files
target/Temp/                             generated temporary C files
```
