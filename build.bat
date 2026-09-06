@echo off
REM Windows build script for Mettle
REM Usage: build.bat [gcc|clang] [--skip-tests] [--backend-only] [--clean] [--jobs N]
REM   Or set CC=clang before invoking (defaults to gcc).
REM
REM The build is incremental: every compile unit is recorded into a plan file
REM and tools\ccbuild.ps1 rebuilds only the objects whose source, headers or
REM compile flags changed, in parallel. --clean forces the old behaviour of
REM starting from an empty object tree.
REM
REM --backend-only stops after archiving bin\mtlc.lib: the libmtlc backend
REM alone, with none of the reference frontend. That is what a downstream
REM frontend needs -- the Mettle language repository fetches this tree and
REM builds the archive this way, then links its own driver against it.

setlocal

REM Select compiler: args override CC env var; default gcc.
set "SKIP_TESTS="
set "BACKEND_ONLY="
set "CLEAN="
set "JOBS=0"
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="clang" (
    set "CC=clang"
    shift
    goto parse_args
)
if /I "%~1"=="gcc" (
    set "CC=gcc"
    shift
    goto parse_args
)
if /I "%~1"=="--skip-tests" (
    set "SKIP_TESTS=1"
    shift
    goto parse_args
)
if /I "%~1"=="--no-tests" (
    set "SKIP_TESTS=1"
    shift
    goto parse_args
)
if /I "%~1"=="--backend-only" (
    set "BACKEND_ONLY=1"
    shift
    goto parse_args
)
if /I "%~1"=="--libmtlc-only" (
    set "BACKEND_ONLY=1"
    shift
    goto parse_args
)
if /I "%~1"=="--clean" (
    set "CLEAN=1"
    shift
    goto parse_args
)
if /I "%~1"=="--rebuild" (
    set "CLEAN=1"
    shift
    goto parse_args
)
if /I "%~1"=="--jobs" (
    set "JOBS=%~2"
    shift
    shift
    goto parse_args
)
echo Error: unknown argument '%~1'
echo Usage: build.bat [gcc^|clang] [--skip-tests] [--backend-only] [--clean] [--jobs N]
exit /b 1

:args_done
if not defined CC set "CC=gcc"
if defined METTLE_SKIP_TESTS set "SKIP_TESTS=1"
if defined METTLE_BACKEND_ONLY set "BACKEND_ONLY=1"

REM Every source file binds to the owned host ABI through host_redirect.h.
REM The compiler's own TUs keep Win64 unwind tables. StackWalk64 in the crash
REM handler unwinds x64 frames through .pdata/.xdata, so dropping them blinds
REM the ICE backtrace, and gcc 15.2 segfaults in the -gcodeview emitter when
REM -fno-asynchronous-unwind-tables removes them. .pdata is inert data: it
REM pulls in no unwinder, so the owned-runtime audit below is unaffected.
if not defined METTLE_HOST_OPT set "METTLE_HOST_OPT=-O2"
REM Warnings this tree is clean of, held there. Not a blanket -Werror:
REM another compiler version warns about different things, and a build that
REM breaks on somebody else's new warning teaches nobody anything. These
REM three were each hiding a real question. -Wdiscarded-qualifiers is
REM deliberately still a warning: 44 of them remain, each needing a
REM decision rather than a rule. See the Makefile for the whole note.
set STRICT_WARNINGS=-Werror=incompatible-pointer-types -Werror=implicit-function-declaration -Werror=int-conversion
set CFLAGS=-Wall -Wextra %STRICT_WARNINGS% -std=c99 -g %METTLE_HOST_OPT% -D_GNU_SOURCE -Isrc -Iinclude -fno-omit-frame-pointer -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -include src/runtime/host_redirect.h
set RUNTIME_CFLAGS=-std=c99 -O2 -D_GNU_SOURCE -Isrc -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -mno-stack-arg-probe -ffunction-sections -fdata-sections -fno-jump-tables
REM This build is mingw-ABI throughout: GNU ld, ar/nm, --entry, and the
REM __imp_-only archive audit. A stock LLVM install defaults to
REM x86_64-pc-windows-msvc, which emits _fltused, _tls_index and UCRT calls
REM the owned runtime does not provide, so pin clang to the GNU target.
REM On MSYS2 CLANG64 clang that is already the default and this is a no-op.
REM -femulated-tls picks the TLS model the owned runtime actually implements:
REM __thread through __emutls_get_address, which is what gcc emits on mingw by
REM default. Native Windows TLS instead wants _tls_index from the CRT's tlssup.
set "CCTARGET="
if /I "%CC%"=="clang" (
    set "CCTARGET=--target=x86_64-w64-windows-gnu"
    set "CFLAGS=%CFLAGS% --target=x86_64-w64-windows-gnu -femulated-tls -D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS"
    set "RUNTIME_CFLAGS=%RUNTIME_CFLAGS% --target=x86_64-w64-windows-gnu -femulated-tls"
)
REM Release builds stamp the version via METTLE_VERSION (e.g. set by release.yml);
REM dev builds fall back to the default in main.c.
if defined METTLE_VERSION set "CFLAGS=%CFLAGS% -DMETTLE_VERSION_RAW=%METTLE_VERSION%"
REM CodeView debug info lets DbgHelp resolve ICE backtraces to file:line on Windows.
REM Opt out via METTLE_NO_CODEVIEW=1 (used by CI). The .pdb link flag is dropped
REM with it since there is no CodeView data to emit.
REM clang stays on DWARF: its CodeView emitter names the pre-emulation symbol of
REM every __thread variable, and under -femulated-tls nothing defines those, so
REM the archive audit below sees a screenful of undefined g_* symbols.
if defined METTLE_NO_CODEVIEW (
    set "LDFLAGS=-ldbghelp"
) else (
REM -fno-reorder-blocks-and-partition rides along because the CodeView line
REM table measures every line label from .text, while -O2's block partitioning
REM moves cold blocks into .text.unlikely, where that subtraction has no answer
REM and gas reports "can't resolve .text - .Lcvline<n>". Whether a function
REM splits depends on its size, so any statement added to a large one could
REM break the default Windows build in a file nobody touched.
    if /I "%CC%"=="gcc" (
        set "CFLAGS=%CFLAGS% -gcodeview -fno-reorder-blocks-and-partition"
        set "LDFLAGS=-ldbghelp -Wl,--pdb,bin\mettle.pdb"
    ) else (
        set "LDFLAGS=-ldbghelp"
    )
)

REM Check if selected compiler is available
where %CC% >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: %CC% not found. Please install MinGW-w64, LLVM/Clang, or similar.
    echo You can download MinGW-w64 from: https://www.mingw-w64.org/downloads/
    echo Or LLVM from: https://releases.llvm.org/
    exit /b 1
)

echo Building with %CC%...

REM --clean restores the from-scratch build: an empty object tree and no
REM up-to-date stamps, so every stage below runs again.
if defined CLEAN (
    if exist obj rmdir /S /Q obj
)

REM Create directories
if not exist obj mkdir obj
if not exist obj\lexer mkdir obj\lexer
if not exist obj\parser mkdir obj\parser
if not exist obj\semantic mkdir obj\semantic
if not exist obj\ir mkdir obj\ir
if not exist obj\ir\optimizer mkdir obj\ir\optimizer
if not exist obj\codegen mkdir obj\codegen
if not exist obj\codegen\binary mkdir obj\codegen\binary
if not exist obj\codegen\asm mkdir obj\codegen\asm
if not exist obj\linker mkdir obj\linker
if not exist obj\debug mkdir obj\debug
if not exist obj\error mkdir obj\error
if not exist obj\compiler mkdir obj\compiler
if not exist obj\runtime mkdir obj\runtime
if not exist obj\frontend mkdir obj\frontend
if not exist bin mkdir bin

REM ---------------------------------------------------------------------------
REM Record the compile plan. Nothing is compiled here: :cc appends one
REM "source;object;flags" line per unit and ccbuild.ps1 decides what is stale.
REM ---------------------------------------------------------------------------
set "PLAN=obj\build_plan.txt"
if exist "%PLAN%" del /Q "%PLAN%"

call :cc src\common.c obj\common.o

REM Everything from here to :plan_ir is the reference frontend, which a
REM backend-only build has no use for.
if defined BACKEND_ONLY goto plan_ir

call :cc src\lexer\lexer.c obj\lexer\lexer.o
call :cc src\parser\ast.c obj\parser\ast.o
call :cc src\parser\ast_dump.c obj\parser\ast_dump.o
call :cc src\parser\parser.c obj\parser\parser.o
call :cc src\parser\ast_print.c obj\parser\ast_print.o
call :cc src\semantic\symbol_table.c obj\semantic\symbol_table.o
call :cc src\semantic\comptime_value.c obj\semantic\comptime_value.o
call :cc src\semantic\type_layout.c obj\semantic\type_layout.o
call :cc src\semantic\comptime_expand.c obj\semantic\comptime_expand.o
call :cc src\semantic\rule_reflect.c obj\semantic\rule_reflect.o
call :cc src\semantic\type_checker_refine.c obj\semantic\type_checker_refine.o
call :cc src\semantic\type_checker_uniform.c obj\semantic\type_checker_uniform.o
call :cc src\semantic\type_checker_effects.c obj\semantic\type_checker_effects.o
call :cc src\semantic\target_desc.c obj\semantic\target_desc.o
call :cc src\semantic\type_query.c obj\semantic\type_query.o
call :cc src\semantic\type_checker.c obj\semantic\type_checker.o
call :cc src\semantic\type_checker_types.c obj\semantic\type_checker_types.o
call :cc src\semantic\type_checker_errors.c obj\semantic\type_checker_errors.o
call :cc src\semantic\type_checker_safety.c obj\semantic\type_checker_safety.o
call :cc src\semantic\type_checker_init_tracker.c obj\semantic\type_checker_init_tracker.o
call :cc src\semantic\type_checker_decl.c obj\semantic\type_checker_decl.o
call :cc src\semantic\type_checker_match.c obj\semantic\type_checker_match.o
call :cc src\semantic\type_checker_stmt.c obj\semantic\type_checker_stmt.o
call :cc src\semantic\type_checker_expr.c obj\semantic\type_checker_expr.o
call :cc src\semantic\type_checker_aggregate.c obj\semantic\type_checker_aggregate.o
call :cc src\semantic\type_checker_tensor_epilogue.c obj\semantic\type_checker_tensor_epilogue.o
call :cc src\semantic\machine_desc.c obj\semantic\machine_desc.o
call :cc src\semantic\schedule_expand.c obj\semantic\schedule_expand.o
call :cc src\semantic\type_checker_memory.c obj\semantic\type_checker_memory.o
call :cc src\semantic\register_allocator.c obj\semantic\register_allocator.o
call :cc src\semantic\import_resolver.c obj\semantic\import_resolver.o
call :cc src\semantic\monomorphize.c obj\semantic\monomorphize.o

:plan_ir
for %%f in (src\ir\*.c) do call :cc "%%f" "obj\ir\%%~nf.o"
for %%f in (src\ir\optimizer\*.c) do call :cc "%%f" "obj\ir\optimizer\%%~nf.o"

for %%f in (src\codegen\binary_emitter.c src\codegen\code_generator.c src\codegen\elf_emitter.c src\codegen\gpu_detect.c src\codegen\ptx_emitter.c src\codegen\spirv_emitter.c src\codegen\target.c src\codegen\flat_emitter.c) do call :cc "%%f" "obj\codegen\%%~nf.o"
for %%f in (src\codegen\asm\*.c) do call :cc "%%f" "obj\codegen\asm\%%~nf.o"
for %%f in (src\codegen\binary\*.c) do call :cc "%%f" "obj\codegen\binary\%%~nf.o"
for %%f in (src\linker\*.c) do call :cc "%%f" "obj\linker\%%~nf.o"

call :cc src\debug\debug_info.c obj\debug\debug_info.o

REM The freestanding program runtime, the owned host runtime and the startup
REM stub are built with the runtime flag set, not the compiler's.
call :cc src\runtime\freestanding.c obj\runtime\freestanding.o "%RUNTIME_CFLAGS%"
call :cc src\runtime\freestanding.c obj\runtime\host_runtime.o "%RUNTIME_CFLAGS% -include src/runtime/host_prefix.h"
call :cc src\runtime\host_startup.c obj\runtime\host_startup.o "%RUNTIME_CFLAGS%"

REM The language runtime, the Tracy shim and the driver's allocator all belong
REM to the frontend side; skip them for a backend-only build.
if defined BACKEND_ONLY goto plan_diagnostics

REM Opt-in runtimes: crash handler (-d / -s / -g / IR trap), memory safety
REM (--safe), atomics (std/thread), profiling (--profile-runtime), debug hooks
REM (--debug-hooks) and the Tracy stubs (std/tracy without --tracy).
call :cc src\runtime\crash_handler.c obj\runtime\crash_handler.o "%RUNTIME_CFLAGS%"
call :cc src\runtime\safety.c obj\runtime\safety.o "%RUNTIME_CFLAGS%"
call :cc src\runtime\trace.c obj\runtime\trace.o "%RUNTIME_CFLAGS%"
call :cc src\runtime\atomics.c obj\runtime\atomics.o "%RUNTIME_CFLAGS% -DMETTLE_ATOMICS_IN_FREESTANDING"
call :cc src\runtime\profile.c obj\runtime\profile.o "%RUNTIME_CFLAGS%"
call :cc src\runtime\debug.c obj\runtime\debug.o "%RUNTIME_CFLAGS%"
call :cc stdlib\tracy_helpers.c obj\runtime\tracy_helpers.o "%RUNTIME_CFLAGS%"
call :cc src\tracy_build.c obj\tracy_build.o

rem The swap runtime is written in Mettle and compiled by the compiler this
rem build produces, so it is staged after bin\mettle.exe exists.

:plan_diagnostics
call :cc src\error\diag_style.c obj\error\diag_style.o
call :cc src\error\error_reporter.c obj\error\error_reporter.o
REM error_explain.c renders the driver's optimization report: frontend-side.
if not defined BACKEND_ONLY call :cc src\error\error_explain.c obj\error\error_explain.o

call :cc src\compiler\compiler_context.c obj\compiler\compiler_context.o
call :cc src\compiler\compiler_crash.c obj\compiler\compiler_crash.o
call :cc src\compiler\compiler_self_profile.c obj\compiler\compiler_self_profile.o

call :cc src\mtlc_api.c obj\mtlc_api.o
call :cc src\mtlc_build.c obj\mtlc_build.o
call :cc src\mtlc_lib_fallbacks.c obj\mtlc_lib_fallbacks.o
call :cc src\mtlc_crash_fallback.c obj\mtlc_crash_fallback.o
call :cc src\runtime\verify_owned.c obj\runtime\verify_owned.o

if defined BACKEND_ONLY goto compile_plan

call :cc src\frontend\mtlc_type_from_frontend.c obj\frontend\mtlc_type_from_frontend.o
call :cc src\frontend\mtlc_lower_module.c obj\frontend\mtlc_lower_module.o
call :cc src\main.c obj\main.o

:compile_plan
REM Orphan objects would still be swept into libmtlc by the wildcards below, so
REM a full build prunes anything the plan does not claim. A backend-only plan
REM covers only part of the tree and must not prune the rest.
set "PRUNE=-Prune"
if defined BACKEND_ONLY set "PRUNE="
powershell -ExecutionPolicy Bypass -NoProfile -File tools\ccbuild.ps1 -Plan "%PLAN%" -CC "%CC%" -Jobs %JOBS% %PRUNE%
set "CCRC=%ERRORLEVEL%"
if "%CCRC%"=="1" (
    echo Build failed!
    exit /b 1
)

REM ccbuild reports 2 when it rebuilt something and 0 when every object was
REM already current. Nothing downstream needs redoing in the second case, as
REM long as the previous run got all the way through its stamp.
set "RELINK=1"
if "%CCRC%"=="0" set "RELINK="
if defined BACKEND_ONLY (
    if not exist bin\mtlc.lib set "RELINK=1"
    if not exist obj\lib.ok set "RELINK=1"
) else (
    if not exist bin\mettle.exe set "RELINK=1"
    if not exist obj\link.ok set "RELINK=1"
)
if not defined RELINK (
    echo Everything up to date.
    if defined BACKEND_ONLY goto backend_only_done
    REM The bundled standard library is a copy of stdlib\, and editing a .mettle
    REM file there compiles nothing, so nothing relinks and the copy went stale:
    REM the next build compiled against the library as it was before the edit.
    REM Refreshing it here costs one directory copy.
    echo Refreshing bundled standard library in bin\stdlib...
    if exist bin\stdlib rmdir /S /Q bin\stdlib
    xcopy stdlib bin\stdlib\ /E /I /Y >nul
    goto run_tests
)

REM ---------------------------------------------------------------------------
REM Archive the standalone backend into libmtlc, then link the reference frontend
REM (this driver) against it. libmtlc = the IR core, optimizer + GNN, code
REM generators, and native linker. The AST->IR lowering TUs (ir_lowering,
REM ir_lower_*) are a FRONTEND concern and link into the driver, not the archive.
REM ar does not expand wildcards, so gather objects with a cmd FOR loop (which
REM does) and hand each group to one ar invocation.
REM ---------------------------------------------------------------------------
set "AR=ar"
where %AR% >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    if /I "%CC%"=="clang" set "AR=llvm-ar"
)
where %AR% >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: archiver '%AR%' not found on PATH ^(need binutils ar or llvm-ar^).
    exit /b 1
)

echo Archiving libmtlc ^(backend: IR core, optimizer, codegen, linker^)...
if exist obj\lib.ok del /Q obj\lib.ok
if exist obj\link.ok del /Q obj\link.ok
if exist bin\mtlc.lib del /Q bin\mtlc.lib
REM Backend IR core: everything in src\ir except the lowering TUs, which
REM belong to the frontend. Wildcarded and excluded by name, matching the
REM Makefile rule exactly, because the compile step above already wildcards
REM the same directory. Typed out as a list, the two drifted the moment
REM anybody added a file: it landed in mettle.exe and was silently absent
REM from libmtlc, which surfaces as an unresolved symbol in the freestanding
REM link, far from the change and on one OS only.
set "AROBJS="
for %%o in (obj\ir\*.o) do call :ar_ir_core "%%o"
%AR% rcs bin\mtlc.lib %AROBJS%
if errorlevel 1 exit /b 1

set "AROBJS="
for %%o in (obj\ir\optimizer\*.o) do call set "AROBJS=%%AROBJS%% %%o"
%AR% rcs bin\mtlc.lib %AROBJS%
if errorlevel 1 exit /b 1

set "AROBJS=obj\codegen\binary_emitter.o obj\codegen\code_generator.o obj\codegen\elf_emitter.o obj\codegen\gpu_detect.o obj\codegen\ptx_emitter.o obj\codegen\spirv_emitter.o obj\codegen\target.o obj\codegen\flat_emitter.o"
for %%o in (obj\codegen\asm\*.o) do call set "AROBJS=%%AROBJS%% %%o"
for %%o in (obj\codegen\binary\*.o) do call set "AROBJS=%%AROBJS%% %%o"
%AR% rcs bin\mtlc.lib %AROBJS%
if errorlevel 1 exit /b 1

set "AROBJS="
for %%o in (obj\linker\*.o) do call set "AROBJS=%%AROBJS%% %%o"
for %%o in (obj\compiler\*.o) do call set "AROBJS=%%AROBJS%% %%o"
REM The diagnostics reporter is frontend-neutral (raw source text + SourceLocation,
REM no AST) and the backend comptime interpreter reports through it -> libmtlc.
%AR% rcs bin\mtlc.lib %AROBJS% obj\debug\debug_info.o obj\error\error_reporter.o obj\error\diag_style.o
if errorlevel 1 exit /b 1

%AR% rcs bin\mtlc.lib obj\common.o obj\mtlc_api.o obj\mtlc_build.o obj\mtlc_lib_fallbacks.o obj\mtlc_crash_fallback.o obj\runtime\verify_owned.o obj\runtime\host_runtime.o
if errorlevel 1 exit /b 1

if not exist bin\mtlc.lib (
    echo Build failed: bin\mtlc.lib was not created.
    exit /b 1
)
ld -r --disable-runtime-pseudo-reloc --whole-archive bin\mtlc.lib --no-whole-archive -o obj\runtime\libmtlc-closure.o
if errorlevel 1 (
    echo Build failed: could not compute the libmtlc symbol closure.
    exit /b 1
)
nm -u obj\runtime\libmtlc-closure.o | findstr /V /C:"__imp_" >nul
if not errorlevel 1 (
    echo Build failed: libmtlc contains unresolved non-OS symbols.
    nm -u obj\runtime\libmtlc-closure.o
    exit /b 1
)
nm -u obj\runtime\libmtlc-closure.o | findstr /I /R /C:"__imp_malloc$" /C:"__imp_calloc$" /C:"__imp_realloc$" /C:"__imp_free$" /C:"__imp_memcpy$" /C:"__imp_memset$" /C:"__imp_printf$" /C:"__imp_fprintf$" /C:"__imp_strtod$" /C:"msvcrt" /C:"ucrt" /C:"vcruntime" /C:"libgcc" /C:"libwinpthread" >nul
if not errorlevel 1 (
    echo Build failed: libmtlc imports a C or compiler runtime symbol.
    nm -u obj\runtime\libmtlc-closure.o
    exit /b 1
)
echo ok> obj\lib.ok

if not defined BACKEND_ONLY goto link_frontend

REM A backend-only build is done here: the archive plus include\mtlc is
REM everything a frontend links against. The staging below is reached whether
REM or not the archive had to be rebuilt, so an up-to-date run still leaves a
REM complete tree for the consumer.
:backend_only_done
if exist bin\runtime rmdir /S /Q bin\runtime
xcopy src\runtime bin\runtime\ /E /I /Y >nul
copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.o >nul
copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.obj >nul
echo.
echo libmtlc built: bin\mtlc.lib
echo   headers: include\mtlc ^(public API^), src ^(backend internals^)
echo   runtime: bin\runtime\freestanding.obj
exit /b 0

:link_frontend
echo Linking mettle ^(reference frontend^) against libmtlc...
set "LDFLAGS=%LDFLAGS% -Wl,--disable-runtime-pseudo-reloc -Wl,--stack,67108864"
%CC% %CCTARGET% -nostdlib -nostartfiles -nodefaultlibs -Wl,--entry,mettle_start -Wl,--subsystem,console obj\runtime\host_startup.o obj\lexer\lexer.o obj\parser\ast.o obj\parser\ast_dump.o obj\parser\ast_print.o obj\parser\parser.o obj\semantic\symbol_table.o obj\semantic\comptime_value.o obj\semantic\type_layout.o obj\semantic\comptime_expand.o obj\semantic\rule_reflect.o obj\semantic\type_checker_refine.o obj\semantic\type_checker_uniform.o obj\semantic\type_checker_effects.o obj\semantic\target_desc.o obj\semantic\type_query.o obj\semantic\type_checker.o obj\semantic\type_checker_types.o obj\semantic\type_checker_errors.o obj\semantic\type_checker_safety.o obj\semantic\type_checker_init_tracker.o obj\semantic\type_checker_decl.o obj\semantic\type_checker_match.o obj\semantic\type_checker_stmt.o obj\semantic\type_checker_expr.o obj\semantic\type_checker_aggregate.o obj\semantic\type_checker_tensor_epilogue.o obj\semantic\type_checker_memory.o obj\semantic\schedule_expand.o obj\semantic\machine_desc.o obj\semantic\register_allocator.o obj\semantic\import_resolver.o obj\semantic\monomorphize.o obj\ir\ir_lowering.o obj\ir\ir_lower_address.o obj\ir\ir_lower_defer.o obj\ir\ir_lower_expr.o obj\ir\ir_lower_stmt.o obj\ir\ir_lower_support.o obj\ir\ir_lower_switch_match.o obj\ir\ir_lower_types.o obj\frontend\mtlc_type_from_frontend.o obj\frontend\mtlc_lower_module.o obj\error\error_explain.o obj\tracy_build.o obj\main.o bin\mtlc.lib -o bin\mettle.exe -lkernel32 -ldbghelp %LDFLAGS%

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)
objdump -p bin\mettle.exe | findstr /I /C:"msvcrt" /C:"ucrt" /C:"vcruntime" /C:"api-ms-win-crt" /C:"libgcc" /C:"libwinpthread" >nul
if %ERRORLEVEL% EQU 0 (
    echo Build failed: bin\mettle.exe imports a forbidden C or compiler runtime.
    objdump -p bin\mettle.exe | findstr /I "DLL Name"
    exit /b 1
)

echo Bundling standard library into bin\stdlib...
if exist bin\stdlib rmdir /S /Q bin\stdlib
xcopy stdlib bin\stdlib\ /E /I /Y >nul

echo Bundling runtime into bin\runtime...
if exist bin\runtime rmdir /S /Q bin\runtime
xcopy src\runtime bin\runtime\ /E /I /Y >nul
copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.o >nul
copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.obj >nul
copy /Y obj\runtime\host_startup.o bin\runtime\host_startup.o >nul
copy /Y obj\runtime\host_startup.o bin\runtime\host_startup.obj >nul
copy /Y obj\runtime\crash_handler.o bin\runtime\crash_handler.o >nul
copy /Y obj\runtime\crash_handler.o bin\runtime\crash_handler.obj >nul
copy /Y obj\runtime\safety.o bin\runtime\safety.o >nul
copy /Y obj\runtime\trace.o bin\runtime\trace.o >nul
copy /Y obj\runtime\trace.o bin\runtime\trace.obj >nul
copy /Y obj\runtime\safety.o bin\runtime\safety.obj >nul
echo Compiling swap runtime from Mettle source...
bin\mettle.exe --release --emit-obj src\runtime\swap.mettle -o bin\runtime\swap.o
if %ERRORLEVEL% NEQ 0 exit /b 1
copy /Y bin\runtime\swap.o bin\runtime\swap.obj >nul

echo Compiling string runtime from Mettle source...
bin\mettle.exe --release --emit-obj src\runtime\string.mettle -o bin\runtime\string.o
if %ERRORLEVEL% NEQ 0 exit /b 1
copy /Y bin\runtime\string.o bin\runtime\string.obj >nul
copy /Y obj\runtime\atomics.o bin\runtime\atomics.o >nul
copy /Y obj\runtime\atomics.o bin\runtime\atomics.obj >nul
copy /Y obj\runtime\profile.o bin\runtime\profile.o >nul
copy /Y obj\runtime\profile.o bin\runtime\profile.obj >nul
copy /Y obj\runtime\debug.o bin\runtime\debug.o >nul
copy /Y obj\runtime\debug.o bin\runtime\debug.obj >nul
copy /Y obj\runtime\tracy_helpers.o bin\runtime\tracy_helpers.o >nul
copy /Y obj\runtime\tracy_helpers.o bin\runtime\tracy_helpers.obj >nul

if exist installer\mettle-build.bat copy /Y installer\mettle-build.bat bin\mettle-build.bat >nul

echo Bundling ML optimizer model into bin\mlopt (used by --ml-opt)...
if exist bin\mlopt rmdir /S /Q bin\mlopt
mkdir bin\mlopt
if exist tools\mlopt\gnn_genius.bin copy /Y tools\mlopt\gnn_genius.bin bin\mlopt\gnn_genius.bin >nul
if exist tools\mlopt\bw_lib.txt copy /Y tools\mlopt\bw_lib.txt bin\mlopt\bw_lib.txt >nul
if exist tools\mlopt\gf2_lib1.txt copy /Y tools\mlopt\gf2_lib1.txt bin\mlopt\gf2_lib1.txt >nul

echo Rendering README.html for the installer docs shortcut...
where python >nul 2>&1 && python installer\render_readme.py

REM Every post-link stage got through: the next run may skip all of this when
REM no object changed.
echo ok> obj\link.ok

echo Build successful! Executable created at bin\mettle.exe

:run_tests
if defined SKIP_TESTS (
    echo Tests skipped.
    exit /b 0
)
echo.
echo Running tests...
REM -Parallel runs the suite as several sharded child processes; failures land
REM in tests\test-failures.txt as well as on the console.
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Parallel
if %ERRORLEVEL% NEQ 0 (
    echo Tests failed! See tests\test-failures.txt
    exit /b 1
)
echo All tests passed.
exit /b 0

REM ---------------------------------------------------------------------------
REM :cc <source> <object> [flags]   -- record one compile unit. Defaults to the
REM compiler's own CFLAGS; the runtime TUs pass their own flag set.
REM ---------------------------------------------------------------------------
:ar_ir_core
REM One object for the libmtlc IR core, unless it is a lowering TU.
set "AR_NAME=%~n1"
for %%x in (ir_lowering ir_lower_address ir_lower_defer ir_lower_expr ir_lower_stmt ir_lower_support ir_lower_switch_match ir_lower_types) do if /I "%AR_NAME%"=="%%x" goto :eof
call set "AROBJS=%%AROBJS%% %~1"
goto :eof

:cc
setlocal
set "UFLAGS=%~3"
if not defined UFLAGS set "UFLAGS=%CFLAGS%"
>>"%PLAN%" echo %~1;%~2;%UFLAGS%
endlocal
exit /b 0
