# RingEcho-Lang-C

RingEcho 編譯器工具鏈的零依賴 C 實作。

將 `.reo` 原始碼 → C 原始碼 → gcc → 原生二進位檔。無需 LLVM，無需任何外部函式庫。

[English](README.md) | [简体中文](README.zh.md) | 繁體中文

## 狀態

| 項目 | 值 |
|------|-----|
| 版本 | 0.2.0 |
| 測試 | 通過（make test） |
| 工具鏈 | 3 個二進位檔：`rev` / `rem` / `rvm` |
| 後端 | C 原始碼、freestanding C、reo ISA、共享函式庫、WebAssembly (WASI) |
| 授權 | MIT |

## 工具鏈

新增實驗性原生後端：`--backend native` 直接產生 x86-64 機器碼與 ELF64
目標檔，透過系統連結器產生 Linux 可執行程式，不呼叫 C 編譯器。
目前支援 8–64 位整數、浮點、布林與字元子集，使用方式與限制見 [NATIVE.md](NATIVE.md)。
完整型別系統與系統級布局遷移狀態見 [TYPE_SYSTEM.md](TYPE_SYSTEM.md)。

| 二進位檔 | 角色 | 說明 |
|--------|------|------|
| `rev` | 編譯器 / 求值器 | 編譯、執行、型別檢查、LSP 伺服器、專案初始化、虛擬環境 |
| `rem` | 套件管理器 | 安裝 / 列出 / 移除 / 搜尋套件（類 npm） |
| `rvm` | 版本管理器 | 管理已安裝的 RingEcho 編譯器版本（類 nvm） |

## 建置

```bash
make CONFIG=Release build
```

| 設定 | 編譯旗標 |
|------|---------|
| Release | `-O3 -DNDEBUG` |
| Debug | `-O0 -g3` |
| Alpha | `-O2 -g` |

需要支援 pthread 的 C11 編譯器（gcc/clang）。平台矩陣見 [BUILD.md](BUILD.md)。

## 用法

### rev — 編譯器

```bash
rev run <file.reo> [--target c|reo|c-freestanding]  # 編譯並執行
rev build <file.reo> [-o out]                       # 編譯為可執行檔 / 組語
rev build <file.reo> --shared                       # 編譯為共享函式庫（.so/.dll）
rev build <file.reo> --target wasm                  # 編譯為 WebAssembly (WASI)
rev build                                           # 建置專案（讀取 ringecho.toml）
rev check <file.reo>                                # 僅型別檢查
rev lsp                                             # 啟動 LSP 伺服器
rev venv <init|activate>                            # 管理虛擬環境
rev init [name]                                     # 建立新專案
rev clean                                           # 清理建置產物
```

`rev init` 會產生包含 `main.reo`、`ringecho.toml`、`.gitignore` 與 `.renv/` 虛擬環境的專案骨架。不帶參數的 `rev build` 會從 `ringecho.toml` 讀取入口並輸出到 `target/Release/`。

### rem — 套件管理器

```bash
rem init [name]        # 初始化新專案
rem install <pkg>      # 安裝套件
rem list               # 列出已安裝的套件
rem remove <pkg>       # 移除套件
rem update             # 更新依賴
rem search <query>     # 搜尋套件
rem publish            # 發佈目前套件
```

### rvm — 版本管理器

```bash
rvm install <version>  # 安裝 RingEcho 版本
rvm use <version>      # 切換版本
rvm list               # 列出已安裝版本
rvm current            # 顯示目前版本
rvm uninstall <ver>    # 解除安裝版本
rvm remote             # 列出可用版本
```

## 特性

- 完整型別系統（i8-i128、u8-u128、f32/f64、bool、char、str）
- 結構體、列舉、Trait + impl + 靜態方法分派
- 泛型函式 + 結構體，惰性單態化
- Option/Result + `?` 運算子
- Lambda / 閉包（暫不支援擷取）
- Match 表達式（整數、浮點、布林、字元、字串內容比對 + 列舉標籤；保留純量結果型別）
- 管道運算子 `|>`
- Component 關鍵字
- for-in 區間 + 字串迭代
- spawn/await 並行（pthread）
- GC 引擎：3 種模式（none/auto/manual）× 3 種演算法（tracing/arc-cycle/hybrid）
- LSP 伺服器：JSON-RPC + 診斷
- 多檔案模組（import 遞迴解析）
- extern C FFI
- char 型別，完整跳脫支援
- 虛擬環境（`.renv/`）及內建標準函式庫（`std::io`、`std::math`、`std::string`、`std::vec`）
- 透過 `ringecho.toml` 進行專案設定

## 測試

`match` 的輸入只求值一次，僅執行第一個符合的分支。
模式必須能賦值給輸入型別，各分支結果必須能賦值給第一個分支的型別；
萬用字元 `_` 必須放在最後。目前尚未檢查窮盡性，未命中時數值結果為零、
字串結果為空字串，建議明確提供 `_` 分支。

執行帶輸出斷言的 match 回歸測試：`make CONFIG=Release test-match`（需要 Python 3）。

```bash
make CONFIG=Debug test          # 端到端：編譯並執行全部正向測試，
                                # 校驗預期失敗（語意 / 語法 / 執行期）
make test-one FILE=tests/hello.reo   # 執行單一測試
make check-one FILE=tests/hello.reo  # 對單一測試做型別檢查
make test-list                       # 列出全部測試
```

## 授權

[MIT](LICENSE) - Copyright (c) 2025-2026 輝夜鈴 (KaguyaRing) & Ringaire
