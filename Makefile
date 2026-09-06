CC = gcc
PYTHON ?= python3
# EXTRA_CFLAGS lets release builds stamp the version, e.g.
#   make EXTRA_CFLAGS='-DMETTLE_VERSION_RAW=v0.13.0'
# (bare token, stringified in main.c - avoids fragile quote escaping)
EXTRA_CFLAGS =
# Warnings this tree is clean of, held there. Not a blanket -Werror: another
# compiler version warns about different things, and a build that breaks on
# somebody else's new warning teaches nobody anything.
#
# -Wdiscarded-qualifiers is deliberately still a warning. The type helpers the
# codegen layer asks about a type now take const, which took the count from 115
# to 44 and found one real defect on the way: the async-copy dump formatted
# into its caller's read-only strings and sized the writes with sizeof on a
# pointer. The 44 that remain are returns, field assignments and parameters
# that each need a decision rather than a rule, and a half-finished sweep
# enforced is a build nobody can complete.
STRICT_WARNINGS = -Werror=incompatible-pointer-types -Werror=implicit-function-declaration -Werror=int-conversion
CFLAGS = -Wall -Wextra $(STRICT_WARNINGS) -std=c99 -g -O2 -D_GNU_SOURCE -Isrc -Iinclude -fno-omit-frame-pointer -MMD -MP $(EXTRA_CFLAGS)
# Native compiler build profile for DGX Spark. GCC/Clang versions without a
# GB10-specific scheduler use ARMv9.2-A; GCC 15 / LLVM 21 users should override
# with `DGX_SPARK_CFLAGS=-mcpu=gb10` as recommended by NVIDIA.
DGX_SPARK ?= 0
DGX_SPARK_CFLAGS ?= -march=armv9.2-a
ifeq ($(DGX_SPARK),1)
CFLAGS += $(DGX_SPARK_CFLAGS)
endif
# Outline atomics reach libgcc's __aarch64_* helpers, which nothing here links.
ifneq (,$(filter aarch64% arm64%,$(shell $(CC) -dumpmachine 2>/dev/null)))
ARCH_CFLAGS := $(shell $(CC) -mno-outline-atomics -E -x c /dev/null > /dev/null 2>&1 \
	&& echo -mno-outline-atomics)
CFLAGS += $(ARCH_CFLAGS)
endif
LDFLAGS =
SRCDIR = src
OBJDIR = obj
BINDIR = bin
STDLIBDIR = stdlib
RUNTIMEDIR = src/runtime

# Install prefix for `make install` / `make install-libmtlc` (honors DESTDIR).
PREFIX ?= /usr/local
# Reported by mtlc_version(); keep in sync with src/mtlc_api.c.
LIBMTLC_VERSION = 0.2.0

# Source files
LEXER_SOURCES = $(SRCDIR)/lexer/lexer.c
PARSER_SOURCES = $(SRCDIR)/parser/parser.c $(SRCDIR)/parser/ast.c $(SRCDIR)/parser/ast_dump.c $(SRCDIR)/parser/ast_print.c
SEMANTIC_SOURCES = $(SRCDIR)/semantic/symbol_table.c $(SRCDIR)/semantic/comptime_value.c $(SRCDIR)/semantic/type_layout.c $(SRCDIR)/semantic/comptime_expand.c $(SRCDIR)/semantic/rule_reflect.c $(SRCDIR)/semantic/type_checker_refine.c $(SRCDIR)/semantic/type_checker_uniform.c $(SRCDIR)/semantic/type_checker_effects.c $(SRCDIR)/semantic/target_desc.c $(SRCDIR)/semantic/type_query.c $(SRCDIR)/semantic/type_checker.c $(SRCDIR)/semantic/type_checker_types.c $(SRCDIR)/semantic/type_checker_errors.c $(SRCDIR)/semantic/type_checker_safety.c $(SRCDIR)/semantic/type_checker_init_tracker.c $(SRCDIR)/semantic/type_checker_decl.c $(SRCDIR)/semantic/type_checker_match.c $(SRCDIR)/semantic/type_checker_stmt.c $(SRCDIR)/semantic/type_checker_expr.c $(SRCDIR)/semantic/type_checker_aggregate.c $(SRCDIR)/semantic/type_checker_tensor_epilogue.c $(SRCDIR)/semantic/type_checker_memory.c $(SRCDIR)/semantic/schedule_expand.c $(SRCDIR)/semantic/machine_desc.c $(SRCDIR)/semantic/register_allocator.c $(SRCDIR)/semantic/import_resolver.c $(SRCDIR)/semantic/monomorphize.c
# The AST->IR lowering pass is a FRONTEND concern (it consumes the frontend AST
# and type system), so it links into the mettle driver, not into libmtlc.
LOWERING_SOURCES = \
	$(SRCDIR)/ir/ir_lowering.c \
	$(SRCDIR)/ir/ir_lower_address.c \
	$(SRCDIR)/ir/ir_lower_defer.c \
	$(SRCDIR)/ir/ir_lower_expr.c \
	$(SRCDIR)/ir/ir_lower_stmt.c \
	$(SRCDIR)/ir/ir_lower_support.c \
	$(SRCDIR)/ir/ir_lower_switch_match.c \
	$(SRCDIR)/ir/ir_lower_types.c
# Backend IR core (everything in src/ir except the lowering TUs) + optimizer.
IR_CORE_SOURCES = $(filter-out $(LOWERING_SOURCES),$(wildcard $(SRCDIR)/ir/*.c)) $(wildcard $(SRCDIR)/ir/optimizer/*.c)
# Public libmtlc API surface, and the frontend-side type-translation adapter.
API_SOURCES = $(SRCDIR)/mtlc_api.c $(SRCDIR)/mtlc_build.c \
	$(SRCDIR)/mtlc_lib_fallbacks.c $(SRCDIR)/mtlc_crash_fallback.c \
	$(RUNTIMEDIR)/verify_owned.c
FRONTEND_ADAPTER_SOURCES = $(SRCDIR)/frontend/mtlc_type_from_frontend.c $(SRCDIR)/frontend/mtlc_lower_module.c
CODEGEN_SOURCES = \
	$(SRCDIR)/codegen/binary_emitter.c \
	$(SRCDIR)/codegen/code_generator.c \
	$(SRCDIR)/codegen/elf_emitter.c \
	$(SRCDIR)/codegen/gpu_detect.c \
	$(SRCDIR)/codegen/ptx_emitter.c \
	$(SRCDIR)/codegen/spirv_emitter.c \
	$(SRCDIR)/codegen/target.c \
	$(SRCDIR)/codegen/flat_emitter.c \
	$(wildcard $(SRCDIR)/codegen/asm/*.c) \
	$(wildcard $(SRCDIR)/codegen/binary/*.c)
LINKER_SOURCES = $(wildcard $(SRCDIR)/linker/*.c)
# error_reporter.c is frontend-NEUTRAL (renders against raw source text +
# SourceLocation; no AST) and the backend's comptime interpreter reports
# through it, so it belongs to libmtlc. error_explain.c stays driver-side.
DIAG_SOURCES = $(SRCDIR)/error/error_reporter.c $(SRCDIR)/error/diag_style.c
ERROR_SOURCES = $(SRCDIR)/error/error_explain.c
DEBUG_SOURCES = $(SRCDIR)/debug/debug_info.c
COMPILER_SOURCES = $(SRCDIR)/compiler/compiler_context.c $(SRCDIR)/compiler/compiler_crash.c $(SRCDIR)/compiler/compiler_self_profile.c
COMMON_SOURCES = $(SRCDIR)/common.c
MAIN_SOURCES = $(SRCDIR)/main.c $(SRCDIR)/tracy_build.c

# libmtlc: the standalone, frontend-agnostic backend (IR core, optimizer + GNN,
# code generators, native linker, public API).
BACKEND_SOURCES = $(COMMON_SOURCES) $(IR_CORE_SOURCES) $(CODEGEN_SOURCES) $(LINKER_SOURCES) $(DEBUG_SOURCES) $(DIAG_SOURCES) $(COMPILER_SOURCES) $(API_SOURCES)
# The reference frontend / driver that consumes libmtlc.
FRONTEND_SOURCES = $(LEXER_SOURCES) $(PARSER_SOURCES) $(SEMANTIC_SOURCES) $(LOWERING_SOURCES) $(FRONTEND_ADAPTER_SOURCES) $(ERROR_SOURCES) $(MAIN_SOURCES)

BACKEND_OBJECTS = $(BACKEND_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
FRONTEND_OBJECTS = $(FRONTEND_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
DEPFILES = $(BACKEND_OBJECTS:.o=.d) $(FRONTEND_OBJECTS:.o=.d)
HOST_RUNTIME_OBJECT = $(OBJDIR)/runtime/host_runtime.o
HOST_STARTUP_OBJECT = $(OBJDIR)/runtime/host_startup.o

AR = ar
LD ?= ld
NM ?= nm
LIBMTLC = $(BINDIR)/libmtlc.a
TARGET = $(BINDIR)/mettle

.PHONY: all clean test check complexity install install-libmtlc dist-libmtlc bundle-stdlib bundle-runtime libmtlc

all: $(TARGET) bundle-stdlib bundle-runtime
libmtlc: $(LIBMTLC)

# The static archive owns every host service it uses. Backend objects call the
# private mtlc_host surface, which reaches the kernel without libc.
$(LIBMTLC): $(BACKEND_OBJECTS) $(HOST_RUNTIME_OBJECT) | $(BINDIR)
	rm -f $@
	$(AR) rcs $@ $(BACKEND_OBJECTS) $(HOST_RUNTIME_OBJECT)
	$(LD) -r --whole-archive $@ --no-whole-archive \
		-o $(OBJDIR)/runtime/libmtlc-closure.o
	@if $(NM) -u $(OBJDIR)/runtime/libmtlc-closure.o | \
		grep -v '_GLOBAL_OFFSET_TABLE_' | grep -q .; then \
		echo "error: $@ has unresolved host symbols"; \
		$(NM) -u $(OBJDIR)/runtime/libmtlc-closure.o; exit 1; fi

$(TARGET): $(HOST_STARTUP_OBJECT) $(FRONTEND_OBJECTS) $(LIBMTLC) | $(BINDIR)
	$(LD) -static -z noexecstack --gc-sections -e _start -o $@ \
		$(HOST_STARTUP_OBJECT) $(FRONTEND_OBJECTS) \
		--start-group $(LIBMTLC) --end-group
	@if readelf -l $@ | grep -q INTERP; then \
		echo "error: $@ contains a dynamic loader"; exit 1; fi
	@if readelf -d $@ 2>&1 | grep -q NEEDED; then \
		echo "error: $@ contains a shared library dependency"; exit 1; fi
	@if test -n "$$($(NM) -u $@)"; then \
		echo "error: $@ contains unresolved symbols"; $(NM) -u $@; exit 1; fi

bundle-stdlib: | $(BINDIR)
	rm -rf $(BINDIR)/stdlib
	cp -r $(STDLIBDIR) $(BINDIR)/stdlib

# Runtime objects are linked into every user program, so build them lean:
# no debug info (-g0 overrides the -g in CFLAGS) and one section per
# function/datum so the ELF link's --gc-sections can drop whatever a given
# program does not use.
RUNTIME_OBJ_CFLAGS = $(FREESTANDING_CFLAGS) -D_GNU_SOURCE -Isrc
# -fno-ident: without it every runtime object carries a .comment naming the
# host gcc, and the linker copies it into the executable -- a section, a
# section header and a producer string in every Mettle binary, for a compiler
# that did not write the program.
FREESTANDING_CFLAGS = -std=c99 -O2 -ffreestanding -fno-builtin -fno-ident \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	-ffunction-sections -fdata-sections -fno-jump-tables $(ARCH_CFLAGS)
HOST_BACKEND_CFLAGS = -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	-include $(RUNTIMEDIR)/host_redirect.h

$(BACKEND_OBJECTS): CFLAGS += $(HOST_BACKEND_CFLAGS)
$(FRONTEND_OBJECTS): CFLAGS += $(HOST_BACKEND_CFLAGS)

$(HOST_RUNTIME_OBJECT): $(RUNTIMEDIR)/freestanding.c \
		$(RUNTIMEDIR)/host_prefix.h | $(OBJDIR)
	$(CC) $(FREESTANDING_CFLAGS) -include $(RUNTIMEDIR)/host_prefix.h \
		-c $(RUNTIMEDIR)/freestanding.c -o $@

$(HOST_STARTUP_OBJECT): $(RUNTIMEDIR)/host_startup.c | $(OBJDIR)
	$(CC) $(FREESTANDING_CFLAGS) -c $< -o $@

bundle-runtime: $(HOST_STARTUP_OBJECT) $(TARGET) | $(BINDIR)
	rm -rf $(BINDIR)/runtime
	cp -r $(RUNTIMEDIR) $(BINDIR)/runtime
	$(CC) $(FREESTANDING_CFLAGS) -Os -c $(RUNTIMEDIR)/freestanding.c -o $(OBJDIR)/runtime/freestanding.o
	cp $(OBJDIR)/runtime/freestanding.o $(BINDIR)/runtime/freestanding.o
	$(CC) $(FREESTANDING_CFLAGS) -Os -DMT_SHARED_RUNTIME -c $(RUNTIMEDIR)/freestanding.c -o $(OBJDIR)/runtime/freestanding_shared.o
	cp $(OBJDIR)/runtime/freestanding_shared.o $(BINDIR)/runtime/freestanding_shared.o
	cp $(HOST_STARTUP_OBJECT) $(BINDIR)/runtime/host_startup.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(STDLIBDIR)/tracy_helpers.c -o $(OBJDIR)/runtime/tracy_helpers.o
	cp $(OBJDIR)/runtime/tracy_helpers.o $(BINDIR)/runtime/tracy_helpers.o
	cp $(OBJDIR)/runtime/tracy_helpers.o $(BINDIR)/runtime/tracy_helpers.obj
	$(CC) $(RUNTIME_OBJ_CFLAGS) -DMETTLE_ATOMICS_IN_FREESTANDING \
		-c $(RUNTIMEDIR)/atomics.c -o $(OBJDIR)/runtime/atomics.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/crash_handler.c -o $(OBJDIR)/runtime/crash_handler.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/safety.c        -o $(OBJDIR)/runtime/safety.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -DMT_SHARED_RUNTIME -c $(RUNTIMEDIR)/safety.c -o $(OBJDIR)/runtime/safety_shared.o
	cp $(OBJDIR)/runtime/safety_shared.o $(BINDIR)/runtime/safety_shared.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/debug.c         -o $(OBJDIR)/runtime/debug.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/profile.c       -o $(OBJDIR)/runtime/profile.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/trace.c         -o $(OBJDIR)/runtime/trace.o
	cp $(OBJDIR)/runtime/atomics.o       $(BINDIR)/runtime/atomics.o
	cp $(OBJDIR)/runtime/crash_handler.o $(BINDIR)/runtime/crash_handler.o
	cp $(OBJDIR)/runtime/safety.o        $(BINDIR)/runtime/safety.o
	cp $(OBJDIR)/runtime/debug.o         $(BINDIR)/runtime/debug.o
	$(TARGET) --release --emit-obj $(RUNTIMEDIR)/swap.mettle -o $(BINDIR)/runtime/swap.o
	$(TARGET) --release --emit-obj $(RUNTIMEDIR)/string.mettle -o $(BINDIR)/runtime/string.o
	cp $(OBJDIR)/runtime/profile.o       $(BINDIR)/runtime/profile.o
	cp $(OBJDIR)/runtime/trace.o         $(BINDIR)/runtime/trace.o
	cp $(OBJDIR)/runtime/trace.o         $(BINDIR)/runtime/trace.obj

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)/lexer $(OBJDIR)/parser $(OBJDIR)/semantic $(OBJDIR)/ir $(OBJDIR)/ir/optimizer $(OBJDIR)/codegen $(OBJDIR)/codegen/binary $(OBJDIR)/linker $(OBJDIR)/error $(OBJDIR)/debug $(OBJDIR)/compiler $(OBJDIR)/runtime $(OBJDIR)/frontend

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

test: $(TARGET) bundle-runtime
	@echo "Running owned runtime tests..."
	$(TARGET) --build --native-heap tests/test_native_heap_threads.mettle \
		-o $(BINDIR)/owned-threads-test
	@$(BINDIR)/owned-threads-test
	$(TARGET) --build tests/test_thread_posix_owned.mettle \
		-o $(BINDIR)/owned-pthread-test
	@$(BINDIR)/owned-pthread-test
	$(TARGET) --build tests/test_owned_dir.mettle \
		-o $(BINDIR)/owned-dir-test
	@$(BINDIR)/owned-dir-test
	$(TARGET) --build -s tests/test_runtime_null_deref_check.mettle \
		-o $(BINDIR)/owned-crash-test
	@$(BINDIR)/owned-crash-test >/dev/null 2>&1; test $$? -eq 1
	@for product in $(TARGET) $(BINDIR)/owned-threads-test \
		$(BINDIR)/owned-pthread-test $(BINDIR)/owned-dir-test \
		$(BINDIR)/owned-crash-test; do \
		if readelf -l $$product | grep -q INTERP; then \
			echo "error: $$product has a dynamic loader"; exit 1; fi; \
		if readelf -d $$product 2>&1 | grep -q NEEDED; then \
			echo "error: $$product has a shared dependency"; exit 1; fi; \
		if test -n "$$($(NM) -u $$product)"; then \
			echo "error: $$product has unresolved symbols"; exit 1; fi; \
	done

# The same suite the Windows build gates on, run through PowerShell Core.
# `test` above stays the quick owned-runtime check; this is the full gate.
check: $(TARGET) bundle-stdlib bundle-runtime
	@command -v pwsh >/dev/null 2>&1 || { \
		echo "error: pwsh is required for the test suite"; \
		echo "  install PowerShell Core: https://aka.ms/powershell"; \
		exit 1; }
	pwsh -NoProfile -File tests/run_tests.ps1 -CompilerPath $(TARGET)

complexity:
	$(PYTHON) tools/ci/complexity_audit.py --self-test
	$(PYTHON) tools/ci/complexity_audit.py
	$(PYTHON) tools/ci/complexity_audit.py --check

# Install the full reference toolchain (the mettle driver + stdlib + runtime).
install: $(TARGET) bundle-stdlib bundle-runtime
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/stdlib $(DESTDIR)$(PREFIX)/runtime
	cp $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	cp -r $(BINDIR)/stdlib/* $(DESTDIR)$(PREFIX)/stdlib/
	cp -r $(BINDIR)/runtime/* $(DESTDIR)$(PREFIX)/runtime/

# Install ONLY the backend for embedding: the public headers, the static
# library, and a pkg-config file. This is all a frontend developer needs
# (`cc $(pkg-config --cflags --libs libmtlc) app.c`).
install-libmtlc: $(LIBMTLC) $(HOST_STARTUP_OBJECT) bundle-runtime
	mkdir -p $(DESTDIR)$(PREFIX)/include/mtlc $(DESTDIR)$(PREFIX)/lib/pkgconfig $(DESTDIR)$(PREFIX)/lib/libmtlc/runtime
	cp include/mtlc/*.h $(DESTDIR)$(PREFIX)/include/mtlc/
	cp $(RUNTIMEDIR)/host_redirect.h $(RUNTIMEDIR)/host_prefix.h \
		$(DESTDIR)$(PREFIX)/include/mtlc/
	cp $(LIBMTLC) $(DESTDIR)$(PREFIX)/lib/
	cp $(BINDIR)/runtime/freestanding.o $(DESTDIR)$(PREFIX)/lib/libmtlc/runtime/
	cp $(HOST_STARTUP_OBJECT) $(DESTDIR)$(PREFIX)/lib/libmtlc/runtime/
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libmtlc\nDescription: Freestanding native compiler backend\nVersion: %s\nLibs: -nostdlib -nostartfiles -nodefaultlibs -no-pie -static -Wl,-e,_start,--gc-sections $${libdir}/libmtlc/runtime/host_startup.o -L$${libdir} -lmtlc\nCflags: -I$${includedir} -include mtlc/host_redirect.h -ffreestanding -fno-builtin -fno-stack-protector\n' '$(PREFIX)' '$(LIBMTLC_VERSION)' > $(DESTDIR)$(PREFIX)/lib/pkgconfig/libmtlc.pc
	@echo "installed libmtlc $(LIBMTLC_VERSION) to $(DESTDIR)$(PREFIX) (include/mtlc, libmtlc.a, libmtlc.pc)"

# Stage the backend into ./dist/libmtlc (headers + lib) with no root needed:
# a self-contained folder to copy into another project or zip up.
dist-libmtlc: $(LIBMTLC) bundle-runtime
	rm -rf dist/libmtlc
	mkdir -p dist/libmtlc/include/mtlc dist/libmtlc/lib dist/libmtlc/runtime
	cp include/mtlc/*.h dist/libmtlc/include/mtlc/
	cp $(LIBMTLC) dist/libmtlc/lib/
	cp $(BINDIR)/runtime/freestanding.o dist/libmtlc/runtime/
	@echo "staged dist/libmtlc (link with: cc -Idist/libmtlc/include app.c dist/libmtlc/lib/libmtlc.a)"

.PHONY: debug
debug: CFLAGS += -DDEBUG
debug: $(TARGET)

-include $(DEPFILES)
