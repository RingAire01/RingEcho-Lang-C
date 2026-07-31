CONFIG ?= Release

# Auto-detect the host, while allowing `make PLATFORM=macOS` in CI or SDK builds.
UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(PLATFORM),)
    ifeq ($(OS),Windows_NT)
        PLATFORM := Windows
    else ifeq ($(UNAME_S),Darwin)
        PLATFORM := macOS
    else ifeq ($(UNAME_S),Linux)
        PLATFORM := Linux
    else
        $(error cannot detect host platform; set PLATFORM=Windows, Linux, or macOS)
    endif
endif

ifeq ($(PLATFORM),Windows)
    PLATFORM_SUFFIX := .exe
    ifeq ($(origin CC),default)
        CC := gcc
    endif
else ifeq ($(PLATFORM),Linux)
    PLATFORM_SUFFIX :=
    ifeq ($(origin CC),default)
        CC := gcc
    endif
else ifeq ($(PLATFORM),macOS)
    PLATFORM_SUFFIX :=
    ifeq ($(origin CC),default)
        CC := clang
    endif
else
    $(error unsupported PLATFORM '$(PLATFORM)'; use Windows, Linux, or macOS)
endif

# 鸿蒙 HarmonyOS: 不支持。
ifeq ($(findstring OHOS,$(UNAME_S)),OHOS)
    $(error HarmonyOS is not supported and will not be supported.)
endif

CPPFLAGS := -Iinclude
CFLAGS_COMMON := -Wno-overlength-strings -Wall -Wextra -Wpedantic -std=c11 -pipe -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -pthread

BUILD_DIR := target/$(CONFIG)
OBJECT_DIR := $(BUILD_DIR)/obj
TARGET_REV := $(BUILD_DIR)/rev$(PLATFORM_SUFFIX)
TARGET_REM := $(BUILD_DIR)/rem$(PLATFORM_SUFFIX)
TARGET_RVM := $(BUILD_DIR)/rvm$(PLATFORM_SUFFIX)

ifeq ($(CONFIG),Release)
    CFLAGS_PROFILE := -O3 -DNDEBUG
else ifeq ($(CONFIG),Debug)
    CFLAGS_PROFILE := -O0 -g3 -DRE0_DEBUG=1
else ifeq ($(CONFIG),Alpha)
    CFLAGS_PROFILE := -O2 -g -DRE0_ALPHA=1
else
    $(error unsupported CONFIG '$(CONFIG)'; use Release, Debug, or Alpha)
endif

CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_PROFILE)

# ── module directories ──
BASE := src/base
FRONT := src/front
ANALYSIS := src/analysis
BACKEND := src/backend
EXEC := src/exec
EXTRA := src/extra

# ── source files grouped by module ──
BASE_SRCS := $(BASE)/arena.c $(BASE)/buffer.c $(BASE)/error.c $(BASE)/re0_log.c $(BASE)/span.c $(BASE)/types.c
FRONT_SRCS := $(FRONT)/lexer.c $(FRONT)/token.c $(FRONT)/parser.c $(FRONT)/ast.c $(FRONT)/stream.c
ANALYSIS_SRCS := $(ANALYSIS)/sema.c $(ANALYSIS)/scope.c $(ANALYSIS)/model.c $(ANALYSIS)/builtins.c $(ANALYSIS)/lint.c
BACKEND_SRCS := $(BACKEND)/codegen.c $(BACKEND)/backend_c.c $(BACKEND)/backend_reo.c
LSP_DIR := src/lsp
LSP_SRCS := $(LSP_DIR)/lsp_json.c $(LSP_DIR)/lsp_server.c
EXTRA_SRCS := $(EXTRA)/re0_event.c $(EXTRA)/re0_manager.c

# ── GC 子系统模块化源文件 ──
GC_DIR := $(EXTRA)/gc
GC_SRCS := \
	$(GC_DIR)/gc_object.c \
	$(GC_DIR)/gc_roots.c \
	$(GC_DIR)/gc_stats.c \
	$(GC_DIR)/gc_events.c \
	$(GC_DIR)/gc_tracing.c \
	$(GC_DIR)/gc_arc.c \
	$(GC_DIR)/gc_hybrid.c \
	$(GC_DIR)/gc_engine.c

# ── 共享库对象文件（不含 main，三个二进制共用） ──
LIB_SRCS := $(BASE_SRCS) $(FRONT_SRCS) $(ANALYSIS_SRCS) $(BACKEND_SRCS) \
            $(EXEC)/compiler.c $(EXEC)/build.c $(EXEC)/workspace.c \
            $(EXEC)/venv.c $(EXEC)/toml_config.c \
            $(LSP_SRCS) $(EXTRA_SRCS) $(GC_SRCS)
LIB_OBJS := $(patsubst %.c,$(OBJECT_DIR)/%.o,$(LIB_SRCS))

# ── 各 main 的对象文件 ──
REV_MAIN_OBJ := $(OBJECT_DIR)/$(EXEC)/rev_main.o
REM_MAIN_OBJ := $(OBJECT_DIR)/$(EXEC)/rem_main.o
RVM_MAIN_OBJ := $(OBJECT_DIR)/$(EXEC)/rvm_main.o

CHECK_FAILURE_TESTS := $(wildcard tests/invalid_*.reo) tests/sema_error.reo
RUNTIME_FAILURE_TESTS := tests/divzero.reo
POSITIVE_TESTS := $(filter-out $(CHECK_FAILURE_TESTS) $(RUNTIME_FAILURE_TESTS),$(wildcard tests/*.reo tests/stdlib/*.reo))

.DEFAULT_GOAL := rev
.PHONY: all build rev rem rvm release debug alpha test test-one check-one test-list clean clean-temp platform-info

all: release

platform-info:
	@printf 'platform=%s compiler=%s targets=rev+rem+rvm\n' "$(PLATFORM)" "$(CC)"

# ── 单独编译目标（避免一次性编译全部）──
rev: $(TARGET_REV)
rem: $(TARGET_REM)
rvm: $(TARGET_RVM)

# ── 全部编译（显式调用）──
build: $(TARGET_REV) $(TARGET_REM) $(TARGET_RVM)

release:
	$(MAKE) CONFIG=Release build

debug:
	$(MAKE) CONFIG=Debug build

alpha:
	$(MAKE) CONFIG=Alpha build

# rev: 编译器（链接完整库）
$(TARGET_REV): $(LIB_OBJS) $(REV_MAIN_OBJ)
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -o "$@" $^ $(LDFLAGS)

# rem: 包管理器（轻量，只需 venv/toml）
$(TARGET_REM): $(OBJECT_DIR)/$(EXEC)/venv.o $(OBJECT_DIR)/$(EXEC)/toml_config.o $(OBJECT_DIR)/$(BASE)/re0_log.o $(REM_MAIN_OBJ)
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -o "$@" $^ $(LDFLAGS)

# rvm: 版本管理器（最轻量，纯文件操作）
$(TARGET_RVM): $(RVM_MAIN_OBJ)
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -o "$@" $^ $(LDFLAGS)

$(OBJECT_DIR)/%.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

test: $(TARGET_REV)
	@set -e; for f in $(POSITIVE_TESTS); do \
		echo "--- $$(basename "$$f") ---"; \
		"./$(TARGET_REV)" run "$$f"; \
	done; \
	for f in $(CHECK_FAILURE_TESTS); do \
		echo "--- $$(basename "$$f") (expected failure) ---"; \
		if "./$(TARGET_REV)" check "$$f"; then \
			echo "expected semantic or syntax failure: $$f" >&2; \
			exit 1; \
		fi; \
	done; \
	for f in $(RUNTIME_FAILURE_TESTS); do \
		echo "--- $$(basename "$$f") (expected runtime failure) ---"; \
		if "./$(TARGET_REV)" run "$$f"; then \
			echo "expected runtime failure: $$f" >&2; \
			exit 1; \
		fi; \
	done

# ── 单文件测试（避免一次跑全部）──
test-one: $(TARGET_REV)
	@if [ -z "$(FILE)" ]; then echo "Usage: make test-one FILE=tests/hello.reo"; exit 1; fi
	@echo "--- $(FILE) ---"
	"./$(TARGET_REV)" run "$(FILE)"

# ── 单文件类型检查 ──
check-one: $(TARGET_REV)
	@if [ -z "$(FILE)" ]; then echo "Usage: make check-one FILE=tests/hello.reo"; exit 1; fi
	"./$(TARGET_REV)" check "$(FILE)"

# ── 列出所有可用测试 ──
test-list:
	@echo "=== Positive tests (rev run) ==="
	@for f in $(POSITIVE_TESTS); do echo "  make test-one FILE=$$f"; done
	@echo "=== Check-failure tests (rev check, expect fail) ==="
	@for f in $(CHECK_FAILURE_TESTS); do echo "  $$f"; done
	@echo "=== Runtime-failure tests (rev run, expect fail) ==="
	@for f in $(RUNTIME_FAILURE_TESTS); do echo "  $$f"; done

# ── 清理临时编译文件 ──
clean-temp:
	@rm -f target/Temp/re0_codegen_*.c target/Temp/re0_run_* 2>/dev/null || true
	@echo "cleaned temp files"

clean:
	rm -rf target
	rm -f rev rev.exe rem rem.exe rvm rvm.exe re0_output.c re0_tmp_out output.reo.asm a.out a.exe
