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

# 鸿蒙 HarmonyOS: 不支持。如果检测到 OHOS 或 __HARMONY__，直接拒绝。
ifeq ($(findstring OHOS,$(UNAME_S)),OHOS)
    $(error HarmonyOS is not supported and will not be supported.)
endif

CPPFLAGS := -Iinclude
CFLAGS_COMMON := -Wall -Wextra -Wpedantic -std=c11 -pipe -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=

# Each configuration owns its binary and object files so flags never leak across builds.
BUILD_DIR := target/$(CONFIG)
OBJECT_DIR := $(BUILD_DIR)/obj
TARGET := $(BUILD_DIR)/rem$(PLATFORM_SUFFIX)

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
BASE_SRCS := $(BASE)/arena.c $(BASE)/buffer.c $(BASE)/error.c $(BASE)/span.c $(BASE)/types.c
FRONT_SRCS := $(FRONT)/lexer.c $(FRONT)/token.c $(FRONT)/parser.c $(FRONT)/ast.c $(FRONT)/stream.c
ANALYSIS_SRCS := $(ANALYSIS)/sema.c $(ANALYSIS)/scope.c $(ANALYSIS)/model.c $(ANALYSIS)/builtins.c $(ANALYSIS)/lint.c
BACKEND_SRCS := $(BACKEND)/codegen.c $(BACKEND)/backend_c.c $(BACKEND)/backend_reo.c
EXEC_SRCS := $(EXEC)/compiler.c $(EXEC)/build.c $(EXEC)/main.c $(EXEC)/workspace.c
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

SRCS := $(BASE_SRCS) $(FRONT_SRCS) $(ANALYSIS_SRCS) $(BACKEND_SRCS) $(EXEC_SRCS) $(LSP_SRCS) $(EXTRA_SRCS) $(GC_SRCS)
OBJS := $(patsubst %.c,$(OBJECT_DIR)/%.o,$(SRCS))
CHECK_FAILURE_TESTS := $(wildcard tests/invalid_*.reo) tests/sema_error.reo
RUNTIME_FAILURE_TESTS := tests/divzero.reo
POSITIVE_TESTS := $(filter-out $(CHECK_FAILURE_TESTS) $(RUNTIME_FAILURE_TESTS),$(wildcard tests/*.reo))

.DEFAULT_GOAL := release
.PHONY: all build release debug alpha test clean platform-info

all: release

platform-info:
	@printf 'platform=%s compiler=%s executable-suffix=%s\n' "$(PLATFORM)" "$(CC)" "$(PLATFORM_SUFFIX)"

build: $(TARGET)

release:
	$(MAKE) CONFIG=Release build

debug:
	$(MAKE) CONFIG=Debug build

alpha:
	$(MAKE) CONFIG=Alpha build

$(TARGET): $(OBJS)
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -o "$@" $^ $(LDFLAGS)

$(OBJECT_DIR)/%.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

test: $(TARGET)
	@set -e; for f in $(POSITIVE_TESTS); do \
		echo "--- $$(basename "$$f") ---"; \
		"./$(TARGET)" run "$$f"; \
	done; \
	for f in $(CHECK_FAILURE_TESTS); do \
		echo "--- $$(basename "$$f") (expected failure) ---"; \
		if "./$(TARGET)" check "$$f"; then \
			echo "expected semantic or syntax failure: $$f" >&2; \
			exit 1; \
		fi; \
	done; \
	for f in $(RUNTIME_FAILURE_TESTS); do \
		echo "--- $$(basename "$$f") (expected runtime failure) ---"; \
		if "./$(TARGET)" run "$$f"; then \
			echo "expected runtime failure: $$f" >&2; \
			exit 1; \
		fi; \
	done

clean:
	rm -rf target
	rm -f rem rem.exe re0_output.c re0_tmp_out output.reo.asm a.out a.exe
