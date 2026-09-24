# 实验性原生后端

## 状态与使用

目标为 `x86_64-unknown-linux-gnu`，使用 System V AMD64 ABI。当前文件输出
需要 Linux 宿主，`run` 需要 x86-64 Linux。默认后端仍为 C，现有
`--target c|c-freestanding|reo|wasm` 保留。

```bash
make CONFIG=Release build

# 稳定的 C/GCC 路线
target/Release/rev build tests/native/basic.reo --backend c -o /tmp/basic-c

# REO 自行编码机器指令、写出 ELF64 对象，再调用 ld 链接
target/Release/rev build tests/native/basic.reo --backend native \
  --target x86_64-unknown-linux-gnu -o /tmp/basic-native
/tmp/basic-native

# 只输出对象，不产生 _start，也不调用链接器
target/Release/rev build tests/native/basic.reo --backend native \
  --emit obj -o /tmp/basic.o

target/Release/rev run tests/native/basic.reo --backend native
make CONFIG=Release test-native
make CONFIG=Release test-native-unit
```

`--emit exe` 为默认值。`REO_LD` 可指定链接器可执行文件路径，默认 `ld`；
该值是单个程序名/路径，不是 shell 命令或附带参数的字符串。需要链接器支持
`elf_x86_64` 仿真。原生构建不使用 `REO_CC`，不调用 C 编译器或汇编器。

可执行程序含 `_start`，直接调用无参数 `main()`，随后通过 Linux `exit`
系统调用退出。unit 返回值视为 0，整数返回值作为退出状态（OS 保留低 8 位）。
没有 libc 初始化、动态加载器、堆、GC 或参数环境初始化。

对象中的函数按原名导出（原生对象的 `main` 不重命名为 `main_`）。外部函数
以 ELF 未定义符号和 `R_X86_64_PLT32` 重定位表达。若需 C 库或自定义运行时，
使用 `--emit obj`，交给外部构建工具链接相应对象/库。集成的可执行链接模式
目前不接受额外库参数，未解析的 extern 将明确链接失败。

## 已支持的语言子集

- `i8/i16/i32/i64/isize`、`u8/u16/u32/u64/usize`、`bool`、`char`、
  `f32/f64`，以及函数的 `unit` 返回类型。
- 初始化局部变量、普通赋值、`+=`、`-=`、`*=`、`/=`。
- 整数算术、位运算、移位、比较，布尔逻辑及短路求值。
- 上述数值、bool、char 之间的非 checked `as` 转换。
- SSE2 浮点加减乘除、比较、取负，NaN/无穷/有符号零和浮点饱和转整数。
- `if/else` 语句、同类型分支的 if 表达式及表达式块。
- `while`、无值 `break`、`continue`。
- 直接函数调用、递归、前向调用、显式 return。
- 最多六个标量参数，遵循 SysV 的整数/SSE 独立寄存器分配及 16 字节调用栈对齐。
- `extern { fn name(...) -> ...; }` 非可变参数函数。
- `assert(bool)` 内建函数。

非 unit 函数的所有可达结束路径需要显式 return。条件必须为 bool。
混合符号的非字面量整数运算需显式转换到同类型。所有参数和局部变量均需
属于上述子集，不会将不支持的类型悄悄当作 i64。

暂不支持：128 位整数、字符串与 println、数组、结构体、枚举、
match、指针/引用、间接调用、泛型、闭包、for、模块声明、属性、静态数据、
类型别名、checked casts、异步/线程、GC、可变参数及 `--shared`。
未实现节点在原生代码生成阶段报错，即使位于不可达源代码中也不静默跳过。
`rev check` 仍是公共语义检查；确认原生子集支持性需要执行 native build。

## 运算与失败语义

- 整数加减乘与取负按实际类型位宽回绕；移位计数按该位宽取模。
- 有符号右移进行符号扩展；无符号右移补零。
- 有符号除法向零截断；余数与被除数同号。
- 整数除零以及有符号最小值除以 -1（含余数操作）触发 CPU 异常；窄类型溢出由显式检查触发 `ud2`。
- 浮点遵循 SSE 的 IEEE 运算；转换 NaN 为整数得到零，越界按目标整数范围饱和。
- assert 失败执行 `ud2`，由操作系统终止进程。
- 尚无带源码位置的原生 panic 运行时，也没有 C 后端的递归深度保护。
  运行时错误均为非零退出，但信号和错误文本不与 C 后端相同。

该阶段是用户态原生程序后端，不是裸机启动支持或自举编译器。

## 实现结构

```text
共享词法 / 语法 / 语义检查
  ├── 原有 C 后端 → C 编译器
  └── native_lower.c：有类型的栈式 IR + CFG/栈高度验证
        → native_x64.c：寄存器/栈布局与机器码编码
          + native_x64_float.c：浮点运算、比较与数值转换
        → native_elf.c：显式小端 ELF64 序列化、符号和重定位
        → native_build.c：原子发布对象 / 调用链接器
```

IR 当前用于原生后端，C 后端尚未迁移到该 IR。前端类型和行为通过差分测试
保持一致。表达式暂存采用栈槽和少量 scratch 寄存器，优先保证正确性；尚未
实现优化寄存器分配或调试信息。每次编译独占状态，无新增全局可变后端状态。

## 验证与边界审查

测试覆盖：直接构建和运行、独立于 C 编译器、ELF 类型/无动态加载器/不可执行栈、
对象确定性、双后端控制流、800 组整数运算的 C/Python 对照、外部符号重定位、
六参数调用、嵌套调用栈对齐、布尔参数/返回值 ABI、非法参数、资源限制、链接与
输出失败、运行时异常。回归目标为 `make CONFIG=Release test-native`，Linux x64
CI 自动执行；其他宿主不会把原生执行测试伪装为通过。

`test-native-unit` 使用 GNU 链接器的 `--wrap=realloc` 对 ELF 写入的缓冲区
扩容逐点注入分配失败，并验证非法 IR 的栈下溢、无效分支、槽位、调用和汇合。
本地验证还包括 ASan/UBSan（含泄漏检查）下的原生测试。短写通过文件大小限制
注入，确认失败不会覆盖已有对象。生成缓冲区使用可恢复的 realloc 失败状态，
ELF 对齐填充采用有界循环，避免 OOM 时因长度不再增长而无限循环。

审查范围为新增原生流水线、CLI/文件发布边界，以及对照测试涉及的赋值和块表达式。
源代码及路径视为不可信输入；linker 配置和输出目录为调用者控制的可信环境。
函数/局部/IR/文本大小和递归深度有命名上限；IR 验证分支与栈高度，ELF 检查
重定位范围；无 shell 拼接。对象/可执行文件先写独占临时文件，成功才 rename
替换目标，失败保留已有文件。调用外部链接器不构成沙箱：编译和运行不可信程序
应由调用方隔离，`run` 按定义执行完整的本机程序。

后续重点：更多参数的栈传参、128 位与聚合值、指针和数据布局、独立可配置 panic 接口、
原生调试信息，再扩展到完整语言覆盖和自举验证。

## TODO: AArch64（arm-v8）原生后端

目标：在 x86-64 之后新增 `--target aarch64-unknown-linux-gnu`，同样只经
`ld` 链接、不依赖 C 编译器。规划要点：

1. **目标描述层**：`Re0TargetLayout` 已参数化（指针大小/对齐、int128 对齐、
   对象上限），新增 `re0_target_aarch64_lp64` 即可复用现有布局计算；
   SysV 分类器需增加 AAPCS64 规则（聚合超过 16 字节按 MEMORY 处理，
   无 x86 式 SSE 混合分类）。
2. **机器码编码**：AArch64 为定长 32 位指令，需按 `native_x64*.c` 的划分
   新建 `native_a64*.c`（整数/浮点、`FMADD` 融合乘加可选）；现有栈式 IR
   与验证器可复用，但调用暂存与 16 字节栈对齐规则不同。
3. **ELF 与重定位**：`EM_AARCH64`，调用用 `R_AARCH64_CALL26`（±128MiB
   范围检查），跨节引用需 `ADRP+ADD` 或 GOT 方案；`_start` 入口同样直调
   `main` 后执行 `exit` 系统调用（AArch64 Linux 系统调用号与触发指令不同）。
4. **链接与验证**：`REO_LD` 指定支持 `aarch64elf` 仿真的链接器；执行与
   差分测试需要 qemu-aarch64 或真机，CI 矩阵单独标注，不得在 x86 宿主上
   把 AArch64 执行测试伪装为通过。
5. **前置条件**：先完成 x86-64 的聚合值、栈传参与指针表示，再复制到
   AArch64，避免同时维护两套都不完整的 lowering；CLI 目标校验、
   `--emit obj` 的跨架构行为与错误信息同步扩展。

完整类型迁移的设计、已完成部分与剩余工作见 [TYPE_SYSTEM.md](TYPE_SYSTEM.md)。
