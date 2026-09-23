# RingEcho-Lang-C — 建置設定

[English](BUILD.md) | [简体中文](BUILD.zh.md) | 繁體中文

## 編譯器

| 平台 | 預設編譯器 | 可執行檔後綴 |
|------|-----------|-------------|
| Linux | gcc | (無) |
| macOS | clang | (無) |
| Windows | gcc (MSYS2/MinGW) | .exe |

覆寫：`make CC=clang CONFIG=Release build`

## Profiles

| Profile | CFLAGS | 用途 |
|---------|--------|------|
| Release | `-O3 -DNDEBUG` | 生產 |
| Debug | `-O0 -g3 -DRE0_DEBUG=1` | 除錯 |
| Alpha | `-O2 -g -DRE0_ALPHA=1` | 內部 |

## 目標架構

| 架構 | CI Runner | 狀態 |
|------|-----------|------|
| x86_64 | ubuntu-latest, windows-latest | ✅ |
| aarch64 | ubuntu-24.04-arm, macos-latest | ✅ |

## 目標平台

| 平台 | 狀態 |
|------|------|
| Linux | ✅ |
| macOS | ✅ |
| Windows | ✅ |
| HarmonyOS | ❌ |

## pthread

產生的 C 程式使用 `-pthread` 連結（spawn/await 執行期）。

## 建置產物

```
target/<Release|Debug|Alpha>/rev[.exe]   編譯器（含 LSP）
target/<Release|Debug|Alpha>/rem[.exe]   套件管理器
target/<Release|Debug|Alpha>/rvm[.exe]   版本管理器
target/<Release|Debug|Alpha>/obj/...     中間目的檔
target/Temp/                             產生的暫時 C 檔案
```
