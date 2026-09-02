#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "main.h"
#include "common.h"
#include "parser/ast_dump.h"
#include "parser/ast_print.h"
#include "codegen/binary/startup.h"
#include "codegen/binary/mir_annotate.h"
#include "codegen/binary_emitter.h"
#include "codegen/binary/arm64_ir.h"
#include "codegen/gpu_detect.h"
#include "codegen/ptx_emitter.h"
#include "codegen/spirv_emitter.h"
#include "codegen/target.h"
#include "codegen/flat_emitter.h"
#include "linker/elf_image.h"
#include "linker/elf_shared.h"
#include "linker/pe_emitter.h"
#include "string_intern.h"
#include "compiler/compiler_context.h"
#include "compiler/compiler_crash.h"
#include "compiler/compiler_self_profile.h"
#include "runtime/owned.h"
#include "runtime/verify_owned.h"
#include "tracy_build.h"
#include "ir/ir.h"
#include "ir/ir_lowering.h" // ir_lower_program / ir_lowering_set_explain (frontend boundary)
#include "ir/ir_optimize.h"
#include "ir/ir_explain_memory.h"
#include "ir/ir_profile.h"
#include "ir/ir_debug_hooks.h"
#include "semantic/import_resolver.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) || defined(__MINGW32__)
#include <sys/time.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#if !defined(__MINGW32__)
/* Avoid windows.h here: winnt.h defines TokenType, which clashes with lexer.h. */
typedef long long MettleQpcTicks;
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(MettleQpcTicks *frequency);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(MettleQpcTicks *counter);
#endif
#else
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
/* clang's limits.h only chains to the host header when __STDC_HOSTED__ is 1. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#endif

#define METTLE_STRINGIFY_(x) #x
#define METTLE_STRINGIFY(x) METTLE_STRINGIFY_(x)
#ifndef METTLE_VERSION
#ifdef METTLE_VERSION_RAW
#define METTLE_VERSION METTLE_STRINGIFY(METTLE_VERSION_RAW)
#else
#define METTLE_VERSION "v0.17.0"
#endif
#endif

#define PROFILE_PHASE_READ_INPUT METTLE_COMPILER_PHASE_READ_INPUT
#define PROFILE_PHASE_LEXICAL_VALIDATION METTLE_COMPILER_PHASE_LEXICAL_VALIDATION
#define PROFILE_PHASE_INIT METTLE_COMPILER_PHASE_INIT
#define PROFILE_PHASE_PARSE METTLE_COMPILER_PHASE_PARSE
#define PROFILE_PHASE_PRELUDE METTLE_COMPILER_PHASE_PRELUDE
#define PROFILE_PHASE_IMPORTS METTLE_COMPILER_PHASE_IMPORTS
#define PROFILE_PHASE_MONOMORPHIZE METTLE_COMPILER_PHASE_MONOMORPHIZE
#define PROFILE_PHASE_TYPE_CHECK METTLE_COMPILER_PHASE_TYPE_CHECK
#define PROFILE_PHASE_IR_LOWERING METTLE_COMPILER_PHASE_IR_LOWERING
#define PROFILE_PHASE_IR_OPTIMIZATION METTLE_COMPILER_PHASE_IR_OPTIMIZATION
#define PROFILE_PHASE_IR_DUMP METTLE_COMPILER_PHASE_IR_DUMP
#define PROFILE_PHASE_CODEGEN METTLE_COMPILER_PHASE_CODEGEN
#define PROFILE_PHASE_WRITE_OUTPUT METTLE_COMPILER_PHASE_WRITE_OUTPUT
#define PROFILE_PHASE_DEBUG_INFO METTLE_COMPILER_PHASE_DEBUG_INFO
#define PROFILE_PHASE_CLEANUP METTLE_COMPILER_PHASE_CLEANUP
#define PROFILE_PHASE_COUNT METTLE_COMPILER_PHASE_COUNT

static int compiler_options_use_profile_runtime(const CompilerOptions *options) {
  return options &&
         (options->profile_runtime || options->profile_runtime_ops);
}

/* Does this build install the crash handler at startup? Either the programmer
 * asked for full stack traces, or the default function-granularity report is
 * in effect -- which needs this driver to be the one producing the executable,
 * since it is the link built here that carries crash_handler.o. */
static int compiler_options_install_crash_handler(const CompilerOptions *options) {
  if (!options) {
    return 0;
  }
  if (options->generate_stack_trace_support) {
    return 1;
  }
  return options->generate_crash_report && options->building_executable &&
         !options->flat_output && !mtlc_target()->freestanding;
}

typedef struct {
  int enabled;
  double phases_ms[PROFILE_PHASE_COUNT];
} CompilerProfile;

static double compiler_profile_now_ms(void) {
  return mettle_now_ms();
}

static void compiler_profile_init(CompilerProfile *profile, int enabled) {
  if (!profile) {
    return;
  }
  memset(profile, 0, sizeof(*profile));
  profile->enabled = enabled;
}

static double compiler_profile_begin(const CompilerProfile *profile) {
  return (profile && profile->enabled) ? compiler_profile_now_ms() : 0.0;
}

static void compiler_profile_add(CompilerProfile *profile,
                                 MettleCompilerPhase phase,
                                 double started_ms) {
  if (!profile || !profile->enabled || phase < 0 ||
      phase >= PROFILE_PHASE_COUNT) {
    return;
  }
  profile->phases_ms[phase] += compiler_profile_now_ms() - started_ms;
}

static void compiler_profile_print_compile(const CompilerProfile *profile,
                                           const char *input_filename,
                                           int result) {
  double total_ms = 0.0;

  if (!profile || !profile->enabled) {
    return;
  }

  for (int i = 0; i < PROFILE_PHASE_COUNT; i++) {
    total_ms += profile->phases_ms[i];
  }

  fprintf(stderr, "Compilation profile for '%s'%s:\n",
          input_filename ? input_filename : "(unknown)",
          result == 0 ? "" : " (failed)");
  for (int i = 0; i < PROFILE_PHASE_COUNT; i++) {
    double ms = profile->phases_ms[i];
    double percent = total_ms > 0.0 ? (ms * 100.0) / total_ms : 0.0;

    if (ms <= 0.0) {
      continue;
    }
    fprintf(stderr, "  %-20s %9.3f ms  %6.2f%%\n",
            mettle_compiler_phase_name((MettleCompilerPhase)i), ms, percent);
  }
  fprintf(stderr, "  %-20s %9.3f ms  %6.2f%%\n", "total", total_ms, 100.0);
  if (getenv("METTLE_ALLOC_REPORT")) {
    mettle_alloc_report();
  }
}

static void compiler_set_phase(MettleCompilerPhase phase) {
  mettle_compiler_ctx_set_phase(phase);
}

static int directory_exists(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }
  return mettle_path_is_directory(path);
}

static char *join_paths(const char *left, const char *right) {
  if (!left || !right) {
    return NULL;
  }

  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  int has_sep = left_len > 0 &&
                (left[left_len - 1] == '/' || left[left_len - 1] == '\\');
  size_t total = left_len + right_len + (has_sep ? 1 : 2);

  char *joined = malloc(total);
  if (!joined) {
    return NULL;
  }

  memcpy(joined, left, left_len);
  if (!has_sep) {
#ifdef _WIN32
    joined[left_len++] = '\\';
#else
    joined[left_len++] = '/';
#endif
  }
  memcpy(joined + left_len, right, right_len);
  joined[left_len + right_len] = '\0';
  return joined;
}

static char *directory_from_path(const char *path) {
  if (!path || path[0] == '\0') {
    return NULL;
  }

  const char *last_slash = strrchr(path, '/');
  const char *last_backslash = strrchr(path, '\\');
  const char *last_sep =
      (last_slash > last_backslash) ? last_slash : last_backslash;
  if (!last_sep) {
    return NULL;
  }

  size_t len = (size_t)(last_sep - path);
  char *dir = malloc(len + 1);
  if (!dir) {
    return NULL;
  }

  memcpy(dir, path, len);
  dir[len] = '\0';
  return dir;
}

static char *get_executable_path(const char *argv0) {
  static char *cached_path = NULL;
  static int cached = 0;
  if (cached) return cached_path ? strdup(cached_path) : NULL;
  cached = 1;

#ifdef _WIN32
  char program_path[4096];
  long long path_length =
      mettle_executable_path(program_path, sizeof(program_path));
  if (path_length > 0 && path_length < (long long)sizeof(program_path)) {
    program_path[path_length] = '\0';
    cached_path = strdup(program_path);
    return cached_path ? strdup(cached_path) : NULL;
  }
  if (argv0 && argv0[0] != '\0') {
    cached_path = strdup(argv0);
    return cached_path ? strdup(cached_path) : NULL;
  }
  return NULL;
#elif defined(__APPLE__)
  uint32_t size = 0;
  if (_NSGetExecutablePath(NULL, &size) != -1 || size == 0) {
    return NULL;
  }
  char *buffer = malloc((size_t)size + 1);
  if (!buffer) {
    return NULL;
  }
  if (_NSGetExecutablePath(buffer, &size) != 0) {
    free(buffer);
    return NULL;
  }
  buffer[size] = '\0';
  cached_path = buffer;
  return strdup(cached_path);
#else
  char buffer[PATH_MAX + 1];
  ssize_t len = (ssize_t)mettle_readlink("/proc/self/exe", buffer, PATH_MAX);
  if (len > 0) {
    buffer[len] = '\0';
    cached_path = strdup(buffer);
    return cached_path ? strdup(cached_path) : NULL;
  }
  if (argv0 && argv0[0] != '\0') {
    cached_path = strdup(argv0);
    return cached_path ? strdup(cached_path) : NULL;
  }
  return NULL;
#endif
}

static char *infer_default_sibling_directory(const char *argv0,
                                             const char *leaf_name,
                                             const char *fallback_path) {
  char *exe_path = get_executable_path(argv0);
  char *exe_dir = directory_from_path(exe_path);

  if (exe_dir) {
    char *parent_dir = join_paths(exe_dir, "..");
    if (parent_dir) {
      char *packaged = join_paths(parent_dir, leaf_name);
      free(parent_dir);
      if (packaged && directory_exists(packaged)) {
        free(exe_path);
        free(exe_dir);
        return packaged;
      }
      free(packaged);
    }

    char *local = join_paths(exe_dir, leaf_name);
    if (local && directory_exists(local)) {
      free(exe_path);
      free(exe_dir);
      return local;
    }
    free(local);
  }

  free(exe_path);
  free(exe_dir);

  if (directory_exists(leaf_name)) {
    return strdup(leaf_name);
  }

  return fallback_path ? strdup(fallback_path) : NULL;
}

static char *infer_default_stdlib_directory(const char *argv0) {
  return infer_default_sibling_directory(argv0, "stdlib", "stdlib");
}

static char *infer_default_runtime_directory(const char *argv0) {
  return infer_default_sibling_directory(argv0, "runtime", NULL);
}

/* Point the ML optimizer at its bundled model/libraries (bin/mlopt by the exe, or
 * tools/mlopt in a dev tree) via env vars; a user-set value always wins. */
static void ml_opt_set_default_paths(const char *argv0) {
  char *dir = infer_default_sibling_directory(argv0, "mlopt", "tools/mlopt");
  if (!dir) {
    return;
  }
  static const struct {
    const char *env;
    const char *file;
  } resources[] = {
      {"METTLE_ML_MODEL", "gnn_genius.bin"},
      {"METTLE_ML_BWLIB", "bw_lib.txt"},
      {"METTLE_ML_GF2LIB", "gf2_lib1.txt"},
  };
  for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); i++) {
    if (getenv(resources[i].env)) {
      continue;
    }
    char *path = join_paths(dir, resources[i].file);
    if (path) {
      char *kv = malloc(strlen(resources[i].env) + strlen(path) + 2);
      if (kv) {
        sprintf(kv, "%s=%s", resources[i].env, path);
        putenv(kv); /* putenv keeps the pointer; intentionally not freed */
      }
      free(path);
    }
  }
  free(dir);
}

static char *infer_default_docs_directory(const char *argv0) {
  return infer_default_sibling_directory(argv0, "docs", NULL);
}

static void print_doc_reference(const char *argv0, const char *relative_path) {
  char *docs_dir = infer_default_docs_directory(argv0);
  if (docs_dir && relative_path) {
    char *full_path = join_paths(docs_dir, relative_path);
    if (full_path) {
      printf("Doc: %s\n", full_path);
      free(full_path);
      free(docs_dir);
      return;
    }
  }

  if (relative_path) {
    printf("Doc: docs/%s\n", relative_path);
  }
  free(docs_dir);
}

/* Single source of truth for the help-topic list. Referenced by print_usage,
 * the topic dispatcher, and the unknown-topic error so they cannot drift. */
#define METTLE_HELP_TOPICS "build, runtime (alias: heap, gc), interop, stdlib, web, diagnostics (alias: errors), verify, test (alias: trace)"

static int print_help_topic(const char *program_name, const char *argv0,
                            const char *topic) {
  if (!topic || topic[0] == '\0') {
    print_usage(program_name);
    return 0;
  }

  if (strcmp(topic, "all") == 0) {
    printf("Mettle help topics\n\n");
    print_help_topic(program_name, argv0, "build");
    printf("\n");
    print_help_topic(program_name, argv0, "runtime");
    printf("\n");
    print_help_topic(program_name, argv0, "interop");
    printf("\n");
    print_help_topic(program_name, argv0, "stdlib");
    printf("\n");
    print_help_topic(program_name, argv0, "web");
    return 0;
  }

  if (strcmp(topic, "build") == 0 || strcmp(topic, "compile") == 0) {
    printf("build - compile, assemble, and link an executable\n\n");
    printf("  Common:\n");
    printf("    mettle --build app.mettle -o app.exe\n");
    printf("    mettle --build --release app.mettle -o app.exe              "
           "   (optimized, stripped)\n");
    printf("\n");
    printf("  Notes:\n");
    printf("    --build emits a COFF object and links with the internal PE "
           "linker by default (no NASM/gcc/link.exe needed).\n");
    printf("    --linker auto tries internal, then gcc, then link.exe.\n");
    printf("    --linker internal forces the native PE linker and probes "
           "common Win32 DLLs directly.\n");
    printf("    --link-arg <arg> passes an extra linker argument (repeatable) "
           "for extra DLLs or import libraries.\n");
    printf("    --tracy links std/tracy with the Tracy profiler (requires a "
           "Tracy repo; see --tracy-dir / TRACY_DIR).\n");
    print_doc_reference(argv0, "compilation.md");
    return 0;
  }

  if (strcmp(topic, "runtime") == 0 || strcmp(topic, "heap") == 0 ||
      strcmp(topic, "gc") == 0) {
    printf("runtime - Mettle's owned freestanding runtime\n\n");
    printf("  No GC, C runtime, compiler runtime, async scheduler, or thread "
           "pool.\n");
    printf("  Startup, heap, files, text conversion, clocks, threads, sockets, "
           "and process calls\n");
    printf("  use Mettle code and direct OS calls. Linked executables are "
           "checked before success.\n\n");
    printf("  Optional owned helper objects are linked only when referenced:\n");
    printf("    crash_handler.o - symbolized backtraces; linked when an object "
           "references mettle_crash_*\n");
    printf("                      (compiled with -d, -s, -g, or with IR "
           "null/bounds traps active).\n");
    printf("    atomics.o       - Win32/__sync_* wrappers; linked when an "
           "object references mettle_atomic_*\n");
    printf("                      (any use of std/thread interlocked atomic "
           "helpers).\n");
    print_doc_reference(argv0, "runtime-model.md");
    return 0;
  }

  if (strcmp(topic, "test") == 0 || strcmp(topic, "tests") == 0 ||
      strcmp(topic, "trace") == 0) {
    printf("test / trace - compile-time execution (no codegen, no linking)\n\n");
    printf("  mettle test app.mettle [--filter=SUBSTR]\n");
    printf("      Run every @test function in the compiler's interpreter.\n");
    printf("      assert(cond) / assert_eq(left, right) failures render as\n");
    printf("      diagnostics with the actual values; unfreed allocations and\n");
    printf("      null/out-of-bounds accesses fail or flag the test. @test\n");
    printf("      functions are type-checked in every build but compiled out\n");
    printf("      of normal binaries.\n\n");
    printf("  mettle trace app.mettle sum_range 0 10\n");
    printf("      Interpret one function on concrete arguments and print its\n");
    printf("      source annotated with the values each line produced.\n");
    print_doc_reference(argv0, "testing.md");
    return 0;
  }

  if (strcmp(topic, "verify") == 0 || strcmp(topic, "validation") == 0) {
    printf("verify - per-pass translation validation (self-verifying optimizer)\n\n");
    printf("  mettle --verify app.mettle\n\n");
    printf("  After every optimization pass, each changed function's before/after IR\n");
    printf("  is executed on generated inputs and compared: return value, buffer\n");
    printf("  bytes, extern-call trace, globals. A diverging pass is reported with a\n");
    printf("  concrete counterexample, quarantined for that function, and the build\n");
    printf("  continues from the validated pre-pass IR - the binary is always built\n");
    printf("  from IR that passed validation.\n\n");
    printf("  METTLE_VERIFY_BREAK=pass[:fn]  sabotage self-test (corrupts one\n");
    printf("                                 constant after the named pass; --verify\n");
    printf("                                 must catch and heal it)\n");
    print_doc_reference(argv0, "translation-validation.md");
    return 0;
  }

  if (strcmp(topic, "diagnostics") == 0 || strcmp(topic, "errors") == 0 ||
      strcmp(topic, "warnings") == 0) {
    printf("diagnostics - compile errors, warnings, and tooling output\n\n");
    printf("  Every diagnostic carries a stable code (E0001..E0007, "
           "M0101..M0117), a source snippet\n");
    printf("  with the offending range underlined, and a help suggestion. The "
           "compiler recovers after\n");
    printf("  errors, so one compile reports every problem in the file.\n\n");
    printf("  mettle explain <CODE>       extended docs for a code (try: "
           "mettle explain E0004)\n");
    printf("  mettle explain list         index of every code\n");
    printf("  --error-format=json         one JSON object per diagnostic on "
           "stderr, for editors/CI\n");
    printf("  NO_COLOR / CLICOLOR_FORCE   disable / force ANSI colors\n\n");
    printf("  Warnings include unused variables (prefix a name with '_' to "
           "opt out), unreachable code,\n");
    printf("  and compile-time memory-safety findings (use-after-free, leaks, "
           "double free, ...).\n\n");
    printf("  `explain` also covers the optimizer's decision codes, the ids "
           "--explain prints in\n");
    printf("  brackets after each verdict: mettle explain "
           "dot-shape-address\n");
    print_doc_reference(argv0, "diagnostics.md");
    return 0;
  }

  if (strcmp(topic, "interop") == 0 || strcmp(topic, "c") == 0) {
    printf("interop - calling C and OS APIs\n\n");
    printf("  Declare external C functions with extern fn.\n");
    printf("  Prefer std/win32 for common Windows OS APIs.\n");
    printf("  Use --link-arg for extra linker libraries in --build mode.\n");
    printf("  syscall(number, ...) asks the kernel directly, with no stub to "
           "link.\n");
    printf("  Example:\n");
    printf("    mettle --build --emit-obj --linker internal main.mettle -o "
           "main.exe\n");
    print_doc_reference(argv0, "c-interop.md");
    return 0;
  }

  if (strcmp(topic, "stdlib") == 0) {
    printf("stdlib - standard library resolution\n\n");
    printf("  std/... imports resolve against the bundled stdlib by "
           "default.\n");
    printf("  No project-local stdlib/ folder is required.\n");
    printf("  Override with --stdlib <dir> only when you need a custom "
           "root.\n");
    print_doc_reference(argv0, "standard-library.md");
    return 0;
  }

  if (strcmp(topic, "web") == 0) {
    printf("web - the demo web server example\n\n");
    printf("  Build it with .\\web\\build.bat\n");
    printf("  That delegates to mettle --build with --link-arg -lws2_32.\n");
    print_doc_reference(argv0, "compilation.md");
    return 0;
  }

  if (strcmp(topic, "docs") == 0 || strcmp(topic, "topics") == 0) {
    printf("Help topics: " METTLE_HELP_TOPICS "\n");
    printf("Use 'mettle help <topic>' for one, or 'mettle help all' for "
           "everything.\n");
    print_doc_reference(argv0, "LANGUAGE.md");
    return 0;
  }

  fprintf(stderr, "Error: unknown help topic '%s'\n", topic);
  fprintf(stderr, "Available topics: " METTLE_HELP_TOPICS "\n");
  fprintf(stderr, "Try 'mettle help' for general usage.\n");
  return 1;
}

static char *build_sidecar_filename(const char *base_filename,
                                    const char *suffix) {
  if (!base_filename || !suffix) {
    return NULL;
  }

  size_t base_len = strlen(base_filename);
  size_t suffix_len = strlen(suffix);
  char *path = malloc(base_len + suffix_len + 1);
  if (!path) {
    return NULL;
  }

  memcpy(path, base_filename, base_len);
  memcpy(path + base_len, suffix, suffix_len);
  path[base_len + suffix_len] = '\0';
  return path;
}

static char *replace_extension(const char *path, const char *extension) {
  if (!path || !extension) {
    return NULL;
  }

  const char *last_slash = strrchr(path, '/');
  const char *last_backslash = strrchr(path, '\\');
  const char *last_sep =
      (last_slash > last_backslash) ? last_slash : last_backslash;
  const char *last_dot = strrchr(path, '.');
  size_t stem_len =
      (last_dot && (!last_sep || last_dot > last_sep)) ? (size_t)(last_dot - path)
                                                       : strlen(path);
  size_t ext_len = strlen(extension);

  char *result = malloc(stem_len + ext_len + 1);
  if (!result) {
    return NULL;
  }

  memcpy(result, path, stem_len);
  memcpy(result + stem_len, extension, ext_len);
  result[stem_len + ext_len] = '\0';
  return result;
}

/* Does this host emit ELF? Ask this rather than comparing against one ELF
 * format: a native AArch64 Linux build reports ELF_ARM64, and a test written
 * against ELF_X64 alone quietly hands it the Windows answer. */
static int host_target_is_elf(void) {
  BinaryTargetFormat format = mtlc_target()->format;
  return format == BINARY_TARGET_FORMAT_ELF_X64 ||
         format == BINARY_TARGET_FORMAT_ELF_ARM64;
}

/* Where `--build` writes when the caller passed no -o. COFF hosts name the
 * product `.exe`; an ELF host has no such suffix, so the product takes the
 * source's stem. Returns NULL when that stem would be the source file itself,
 * because the caller named a source with no extension and writing the product
 * over it would destroy their input. */
static char *default_executable_filename(const char *input_filename) {
  if (!input_filename || input_filename[0] == '\0') {
    return NULL;
  }

  if (!host_target_is_elf()) {
    return replace_extension(input_filename, ".exe");
  }

  {
    char *stem = replace_extension(input_filename, "");
    if (stem && strcmp(stem, input_filename) == 0) {
      free(stem);
      return NULL;
    }
    return stem;
  }
}

static const char *default_object_output_filename(void) {
  return host_target_is_elf() ? "output.o" : "output.obj";
}

static const char *linker_mode_name(LinkerMode mode) {
  switch (mode) {
  case LINKER_MODE_INTERNAL:
    return "internal";
  case LINKER_MODE_GCC:
    return "gcc";
  case LINKER_MODE_MSVC:
    return "msvc";
  case LINKER_MODE_AUTO:
  default:
    return "auto";
  }
}

static int parse_linker_mode(const char *text, LinkerMode *mode_out) {
  if (!text || !mode_out) {
    return 0;
  }

  if (strcmp(text, "auto") == 0) {
    *mode_out = LINKER_MODE_AUTO;
    return 1;
  }
  if (strcmp(text, "internal") == 0) {
    *mode_out = LINKER_MODE_INTERNAL;
    return 1;
  }
  if (strcmp(text, "gcc") == 0) {
    *mode_out = LINKER_MODE_GCC;
    return 1;
  }
  if (strcmp(text, "msvc") == 0 || strcmp(text, "link") == 0) {
    *mode_out = LINKER_MODE_MSVC;
    return 1;
  }

  return 0;
}

/* Which bundled runtime objects a finished program actually references.
 * Both link paths, PE and ELF, ask these, so they sit above the platform
 * split rather than inside either arm of it. */
static int object_has_undefined_symbol_prefix(const char *object_path,
                                              const char *prefix) {
  LinkObject *object = NULL;
  char *error_message = NULL;
  size_t i = 0u;
  int found = 0;

  if (!object_path || !prefix) {
    return 0;
  }

  if (!link_object_read(object_path, &object, &error_message)) {
    free(error_message);
    return 1;
  }

  for (i = 0u; i < object->symbol_count; i++) {
    const LinkSymbol *symbol = &object->symbols[i];
    if (symbol->is_auxiliary ||
        symbol->section_index != LINK_SECTION_INDEX_UNDEFINED ||
        !symbol->name) {
      continue;
    }
    if (strncmp(symbol->name, prefix, strlen(prefix)) == 0) {
      found = 1;
      break;
    }
  }

  free(error_message);
  link_object_destroy(object);
  return found;
}

static unsigned short read_u16_le(const unsigned char *p) {
  return (unsigned short)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static unsigned int read_u32_le(const unsigned char *p) {
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
         ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned long long read_u64_le(const unsigned char *p) {
  return (unsigned long long)read_u32_le(p) |
         ((unsigned long long)read_u32_le(p + 4) << 32);
}

/* Reads an ELF64 relocatable object and reports whether it leaves any symbol
 * undefined whose name starts with `prefix`. This is the ELF counterpart of
 * the COFF scan above, and it decides the same thing: which bundled runtime
 * objects a program actually references, so the link pulls in only those.
 * Unreadable or malformed input answers 1, which links the object rather than
 * risking an undefined symbol at link time. */
static int elf_object_has_undefined_symbol_prefix(const char *object_path,
                                                  const char *prefix) {
  static const unsigned char elf_magic[4] = {0x7f, 'E', 'L', 'F'};
  FILE *file = NULL;
  unsigned char *data = NULL;
  long file_size = 0;
  size_t prefix_length = 0u;
  size_t section_count = 0u;
  size_t section_offset = 0u;
  size_t section_header_size = 0u;
  size_t i = 0u;
  int found = 1;

  if (!object_path || !prefix) {
    return 1;
  }

  file = fopen(object_path, "rb");
  if (!file) {
    return 1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 1;
  }
  file_size = ftell(file);
  if (file_size < 64 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 1;
  }
  data = malloc((size_t)file_size);
  if (!data) {
    fclose(file);
    return 1;
  }
  if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
    goto cleanup;
  }

  /* ELFCLASS64 little-endian relocatable objects only; the backend emits no
   * other shape, and anything else falls through to the conservative answer. */
  if (memcmp(data, elf_magic, sizeof(elf_magic)) != 0 || data[4] != 2 ||
      data[5] != 1) {
    goto cleanup;
  }

  section_offset = (size_t)read_u64_le(data + 0x28);
  section_header_size = (size_t)read_u16_le(data + 0x3a);
  section_count = (size_t)read_u16_le(data + 0x3c);
  if (section_header_size < 64u || section_count == 0u ||
      section_offset + section_count * section_header_size >
          (size_t)file_size) {
    goto cleanup;
  }

  prefix_length = strlen(prefix);
  found = 0;

  for (i = 0u; i < section_count; i++) {
    const unsigned char *header = data + section_offset + i * section_header_size;
    size_t symbol_table_offset = 0u;
    size_t symbol_table_size = 0u;
    size_t entry_size = 0u;
    size_t string_index = 0u;
    size_t string_offset = 0u;
    size_t string_size = 0u;
    size_t symbol = 0u;

    if (read_u32_le(header + 4) != 2u) { /* SHT_SYMTAB */
      continue;
    }
    symbol_table_offset = (size_t)read_u64_le(header + 0x18);
    symbol_table_size = (size_t)read_u64_le(header + 0x20);
    string_index = (size_t)read_u32_le(header + 0x28);
    entry_size = (size_t)read_u64_le(header + 0x38);
    if (entry_size < 24u || string_index >= section_count ||
        symbol_table_offset + symbol_table_size > (size_t)file_size) {
      found = 1;
      goto cleanup;
    }

    {
      const unsigned char *string_header =
          data + section_offset + string_index * section_header_size;
      string_offset = (size_t)read_u64_le(string_header + 0x18);
      string_size = (size_t)read_u64_le(string_header + 0x20);
      if (string_offset + string_size > (size_t)file_size) {
        found = 1;
        goto cleanup;
      }
    }

    for (symbol = 0u; symbol + entry_size <= symbol_table_size;
         symbol += entry_size) {
      const unsigned char *entry = data + symbol_table_offset + symbol;
      size_t name_offset = (size_t)read_u32_le(entry);
      const char *name = NULL;

      if (read_u16_le(entry + 6) != 0u) { /* st_shndx != SHN_UNDEF */
        continue;
      }
      if (name_offset == 0u || name_offset >= string_size) {
        continue;
      }
      name = (const char *)(data + string_offset + name_offset);
      if (strncmp(name, prefix, prefix_length) == 0) {
        found = 1;
        goto cleanup;
      }
    }
  }

cleanup:
  free(data);
  fclose(file);
  return found;
}

static int object_needs_runtime_object(const char *object_path,
                                       const char *prefix) {
  if (host_target_is_elf()) {
    return elf_object_has_undefined_symbol_prefix(object_path, prefix);
  }
  return object_has_undefined_symbol_prefix(object_path, prefix);
}

static int object_needs_crash_handler(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_crash_");
}

static int object_needs_atomics(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_atomic_");
}

static int object_needs_profile_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_profile_");
}

static int object_needs_debug_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_dbg_");
}

static int object_needs_safety_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_safety_");
}

/* `quiesce;` lowers to mettle_swap_apply, and staging reaches
 * mettle_swap_stage. A program with no swap point names neither and does not
 * link this object, which is the whole of what opting in costs. */
static int object_needs_swap_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_swap_");
}

/* `==` and `!=` on strings compare contents through mettle_string_eq. A
 * program that never compares strings never names it. */
static int object_needs_string_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_string_");
}

static int object_needs_tracy_helpers(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_tracy_");
}


#define METTLE_DEFAULT_ELF_INTERPRETER "/lib64/ld-linux-x86-64.so.2"

static int mettle_elf_dynamic_link_requested(const CompilerOptions *options) {
  return options && (options->shared_library_count > 0u ||
                     options->shared_output || options->export_dynamic);
}

#ifndef _WIN32
#define METTLE_ELF_DYNAMIC_LINKER "/lib64/ld-linux-x86-64.so.2"

/* Runs `gcc -print-file-name=<file>` and returns the strdup'd path, or NULL
 * when gcc is missing or does not ship the file (gcc echoes the bare name
 * back, without a '/', when it has no path for it). <file> is always a
 * compiled-in literal, so the popen command cannot be influenced by user
 * input. */
static char *mettle_gcc_print_file_name(const char *file) {
  char command[256];
  char line[1024];
  FILE *pipe;
  size_t len;

  if (snprintf(command, sizeof(command), "gcc -print-file-name=%s 2>/dev/null",
               file) >= (int)sizeof(command)) {
    return NULL;
  }
  pipe = popen(command, "r");
  if (!pipe) {
    return NULL;
  }
  if (!fgets(line, sizeof(line), pipe)) {
    pclose(pipe);
    return NULL;
  }
  pclose(pipe);
  len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  if (!strchr(line, '/')) {
    return NULL;
  }
  return strdup(line);
}

/* Returns dirname(reference) + "/" + file (file may be "" to get the bare
 * directory prefix for -L). */
static char *mettle_sibling_path(const char *reference, const char *file) {
  const char *slash = strrchr(reference, '/');
  size_t dir_len;
  char *out;

  if (!slash) {
    return NULL;
  }
  dir_len = (size_t)(slash - reference) + 1u;
  out = malloc(dir_len + strlen(file) + 1u);
  if (!out) {
    return NULL;
  }
  memcpy(out, reference, dir_len);
  strcpy(out + dir_len, file);
  return out;
}

/* Plain builds get their ELF symbol table stripped at link: .symtab/.strtab
 * (every function and string-literal label) are dead weight at runtime and
 * outweigh the program's actual section content several times over for small
 * binaries. This matches the Windows internal PE linker, which never emits a
 * symbol table. Debug/trace/profile builds keep symbols for tooling. */
static int mettle_elf_keep_symbols(const CompilerOptions *options) {
  return options &&
         (options->debug_mode || options->generate_debug_symbols ||
          options->generate_line_mapping ||
          options->generate_stack_trace_support || options->tracy ||
          compiler_options_use_profile_runtime(options));
}

static int mettle_elf_external_linker_requested(const CompilerOptions *options) {
  if (!options) {
    return 0;
  }
  return options->linker_mode == LINKER_MODE_GCC ||
         options->linker_mode == LINKER_MODE_MSVC;
}

/* Turns each -l/--library value into a file. A value naming a path is used as
 * given; a bare name is looked up as lib<name>.so along -L and then the
 * platform directories, the way ld resolves one. */
static char **mettle_resolve_shared_libraries(const CompilerOptions *options,
                                              size_t *count_out) {
  size_t count = options ? options->shared_library_count : 0u;
  char **paths = NULL;
  size_t i = 0u;

  *count_out = 0u;
  if (count == 0u) {
    return NULL;
  }
  paths = calloc(count, sizeof(char *));
  if (!paths) {
    fprintf(stderr, "Error: Out of memory while resolving libraries\n");
    return NULL;
  }
  for (i = 0u; i < count; i++) {
    const char *name = options->shared_libraries[i];
    char *error_message = NULL;

    if (strchr(name, '/') != NULL) {
      paths[i] = mettle_strdup(name);
    } else {
      paths[i] = elf_shared_library_locate(
          name, options->library_search_paths,
          options->library_search_path_count, &error_message);
    }
    if (!paths[i]) {
      fprintf(stderr, "Error: %s\n",
              error_message ? error_message : "Out of memory");
      free(error_message);
      while (i-- > 0u) {
        free(paths[i]);
      }
      free(paths);
      return NULL;
    }
    free(error_message);
  }
  *count_out = count;
  return paths;
}

static void mettle_free_shared_libraries(char **paths, size_t count) {
  size_t i = 0u;

  for (i = 0u; i < count; i++) {
    free(paths[i]);
  }
  free(paths);
}

static int mettle_link_elf_native(const char *startup_object,
                                  const char *object_filename,
                                  const char *executable_filename,
                                  const char *freestanding_object,
                                  const char *const *extra_objects,
                                  size_t extra_object_count,
                                  int strip_symbols,
                                  const CompilerOptions *options) {
  const char *object_paths[32];
  unsigned char runtime_defaults[32];
  LinkResolutionOptions resolution_options;
  ElfImageOptions emission_options;
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  char **library_paths = NULL;
  size_t library_count = 0u;
  size_t count = 0u;
  size_t i = 0u;
  int result = 1;

  memset(&resolution_options, 0, sizeof(resolution_options));
  memset(&emission_options, 0, sizeof(emission_options));
  resolution_options.entry_symbol_name = "_start";
  resolution_options.section_alignment = 16u;
  emission_options.image_base = 0x400000u;
  emission_options.page_size = 0x1000u;
  emission_options.interpreter = METTLE_DEFAULT_ELF_INTERPRETER;

  if (options) {
    if (options->shared_library_count != 0u) {
      library_paths = mettle_resolve_shared_libraries(options, &library_count);
      if (!library_paths) {
        return 1;
      }
    }
    resolution_options.shared_library_paths =
        (const char *const *)library_paths;
    resolution_options.shared_library_path_count = library_count;
    resolution_options.produce_shared_library = options->shared_output;
    if (options->shared_output) {
      resolution_options.entry_symbol_name = NULL;
      emission_options.produce_shared_library = 1;
      emission_options.soname = options->soname;
      emission_options.interpreter = NULL;
      emission_options.image_base = 0u;
    } else if (options->dynamic_linker) {
      emission_options.interpreter = options->dynamic_linker;
    }
    emission_options.export_dynamic = options->export_dynamic;
    emission_options.runpaths = options->runpaths;
    emission_options.runpath_count = options->runpath_count;
  }

  /* --image-base: a freestanding image is placed where its loader puts it, not
   * where a hosted operating system would. An ELF segment is mapped by page, so
   * a base that is not page-aligned cannot be loaded at all. */
  if (mtlc_target()->image_base_set) {
    if (mtlc_target()->image_base % 0x1000u) {
      fprintf(stderr,
              "Error: --image-base 0x%llx is not page-aligned; an ELF image "
              "must load on a 0x1000 boundary\n",
              (unsigned long long)mtlc_target()->image_base);
      mettle_free_shared_libraries(library_paths, library_count);
      return 1;
    }
    emission_options.image_base = mtlc_target()->image_base;
  }

  if ((!startup_object && !emission_options.produce_shared_library) ||
      !object_filename || !executable_filename || !freestanding_object ||
      extra_object_count > sizeof(object_paths) / sizeof(object_paths[0]) - 3u) {
    mettle_free_shared_libraries(library_paths, library_count);
    return 1;
  }

  if (startup_object) {
    object_paths[count] = startup_object;
    runtime_defaults[count++] = 1u;
  }
  object_paths[count] = freestanding_object;
  runtime_defaults[count++] = 1u;
  for (i = 0u; i < extra_object_count; i++) {
    if (!extra_objects[i]) {
      continue;
    }
    object_paths[count] = extra_objects[i];
    runtime_defaults[count++] = 1u;
  }
  object_paths[count] = object_filename;
  runtime_defaults[count++] = 0u;

  resolution_options.object_is_runtime_default = runtime_defaults;
  emission_options.strip_symbols = strip_symbols;

  if (!link_resolution_build(object_paths, count, &resolution_options,
                             &resolution, &error_message)) {
    fprintf(stderr, "Error: Native ELF link failed: %s\n",
            error_message ? error_message : "symbol resolution failed");
    free(error_message);
    mettle_free_shared_libraries(library_paths, library_count);
    return 1;
  }

  if (!elf_image_emit_executable(resolution, executable_filename,
                                 &emission_options, &error_message)) {
    fprintf(stderr, "Error: Native ELF link failed: %s\n",
            error_message ? error_message : "image emission failed");
    free(error_message);
    link_resolution_destroy(resolution);
    mettle_free_shared_libraries(library_paths, library_count);
    return 1;
  }

  free(error_message);
  link_resolution_destroy(resolution);
  mettle_free_shared_libraries(library_paths, library_count);
  result = 0;
  return result;
}

/* Link a static ELF image from Mettle owned startup and runtime objects. */
static int mettle_link_elf_direct(const char *startup_object,
                                  const char *object_filename,
                                  const char *executable_filename,
                                  const char *freestanding_object,
                                  const char *const *extra_objects,
                                  size_t extra_object_count,
                                  int strip_symbols) {
  const char *argv_list[32];
  size_t argc_used = 0u;
  size_t i = 0u;
  int result = 1;

  if (!startup_object || !object_filename || !executable_filename ||
      !freestanding_object ||
      extra_object_count > sizeof(argv_list) / sizeof(argv_list[0]) - 12u) {
    return 1;
  }

  argv_list[argc_used++] = "ld";
  argv_list[argc_used++] = "--gc-sections";
  argv_list[argc_used++] = "-z";
  argv_list[argc_used++] = "noseparate-code";
  argv_list[argc_used++] = "-e";
  argv_list[argc_used++] = "_start";
  if (strip_symbols) {
    argv_list[argc_used++] = "-s";
  }
  argv_list[argc_used++] = "-o";
  argv_list[argc_used++] = executable_filename;
  argv_list[argc_used++] = startup_object;
  argv_list[argc_used++] = freestanding_object;
  argv_list[argc_used++] = object_filename;
  for (i = 0u; i < extra_object_count; i++) {
    if (extra_objects[i]) {
      argv_list[argc_used++] = extra_objects[i];
    }
  }
  argv_list[argc_used] = NULL;

  if (mettle_run_process("ld", argv_list) == 0) {
    result = 0;
  }

  return result;
}

/* Links a native ELF executable from Mettle's startup, freestanding runtime,
 * and generated object. The direct path invokes ld. The fallback invokes gcc
 * only as a linker driver with startup files, default libraries, and compiler
 * support libraries disabled. The finished ELF must pass the dependency gate.
 * Used on ELF hosts. Returns 0 on success. */
static void elf_select_runtime_helpers(const char *runtime_directory,
                                       const char *object_filename,
                                       int stack_trace, int profile_runtime,
                                       int needs_safety,
                                       char **crash_handler_object,
                                       char **profile_object) {
  if (stack_trace || profile_runtime || needs_safety ||
      object_needs_crash_handler(object_filename)) {
    *crash_handler_object = join_paths(runtime_directory, "crash_handler.o");
  }
  if (profile_runtime) {
    *profile_object = join_paths(runtime_directory, "profile.o");
  }
}

static int elf_collect_on_demand_objects(const char *runtime_directory,
                                         const char *object_filename,
                                         int shared_output,
                                         char **extra_objects,
                                         size_t *extra_object_count,
                                         size_t *on_demand_object_count) {
  static const struct {
    const char *file;
    const char *shared_file;
    int (*needed)(const char *);
  } on_demand[] = {
      {"string.o", "string.o", object_needs_string_runtime},
      {"swap.o", "swap.o", object_needs_swap_runtime},
      {"safety.o", "safety_shared.o", object_needs_safety_runtime},
      {"debug.o", "debug.o", object_needs_debug_runtime},
      {"atomics.o", "atomics.o", object_needs_atomics},
  };
  size_t i = 0u;

  for (i = 0u; i < sizeof(on_demand) / sizeof(on_demand[0]); i++) {
    char *candidate = NULL;
    const char *file = shared_output ? on_demand[i].shared_file
                                     : on_demand[i].file;
    if (!on_demand[i].needed(object_filename)) {
      continue;
    }
    candidate = join_paths(runtime_directory, file);
    if (!candidate) {
      continue;
    }
    if (access(candidate, F_OK) != 0) {
      fprintf(stderr,
              "Error: Program references the %s runtime but '%s' is not in "
              "'%s'\n",
              on_demand[i].file, file, runtime_directory);
      free(candidate);
      return 0;
    }
    extra_objects[(*extra_object_count)++] = candidate;
    *on_demand_object_count = *extra_object_count;
  }
  return 1;
}

static int elf_append_runtime_helpers(int stack_trace, int profile_runtime,
                                      char *crash_handler_object,
                                      char *profile_object,
                                      char **extra_objects,
                                      size_t *extra_object_count) {
  if ((stack_trace || profile_runtime) && !crash_handler_object) {
    fprintf(stderr,
            "Error: Could not locate bundled crash_handler.o for Linux runtime "
            "support\n");
    return 0;
  }
  if (profile_runtime && !profile_object) {
    fprintf(stderr,
            "Error: Could not locate bundled profile.o for Linux runtime "
            "profiling\n");
    return 0;
  }

  /* Everything appended past on_demand_object_count is owned by its own
   * variable, so cleanup frees only what the on-demand loop allocated. */

  /* crash_handler and profile join the same list so both link paths carry one
   * ordered set of runtime objects. safety.o calls into the crash handler, so
   * it must precede it here. */
  if (crash_handler_object) {
    extra_objects[(*extra_object_count)++] = crash_handler_object;
  }
  if (profile_object) {
    extra_objects[(*extra_object_count)++] = profile_object;
  }
  return 1;
}

static int mettle_link_elf_executable(const char *object_filename,
                                      const char *executable_filename,
                                      const CompilerOptions *options,
                                      const char *runtime_directory) {
  char **argv_list = NULL;
  char *crash_handler_object = NULL;
  char *profile_object = NULL;
  char *freestanding_object = NULL;
  char *startup_object = NULL;
  /* string, swap, safety, debug, atomics, crash_handler, profile. */
  char *extra_objects[8];
  size_t extra_object_count = 0u;
  size_t on_demand_object_count = 0u;
  size_t extra_index = 0u;
  const char *cc = "gcc";
  int result = 1;
  int profile_runtime =
      options && compiler_options_use_profile_runtime(options) ? 1 : 0;
  int stack_trace = compiler_options_install_crash_handler(options);
  int needs_safety = 0;

  memset(extra_objects, 0, sizeof(extra_objects));

  /* The bundled runtime owns the Linux ABI used by the standard library. Its
   * threads use clone and futex. Its files, clocks, sockets, and processes use
   * direct system calls. No host library appears on the link line.
   */
  if (runtime_directory) {
    /* A shared object gets the build of the runtime that keeps no thread-local
     * state, because a loaded library cannot reach one. */
    freestanding_object = join_paths(runtime_directory,
                                     options && options->shared_output
                                         ? "freestanding_shared.o"
                                         : "freestanding.o");

    /* Same on-demand rule the Windows link follows: an object joins the link
     * only when the program leaves one of its symbols undefined. Naming none
     * of them is what makes a bare compute program cost nothing. */
    needs_safety = object_needs_safety_runtime(object_filename);
    elf_select_runtime_helpers(runtime_directory, object_filename, stack_trace,
                               profile_runtime, needs_safety,
                               &crash_handler_object, &profile_object);

    if (!elf_collect_on_demand_objects(runtime_directory, object_filename,
                                      options && options->shared_output,
                                      extra_objects, &extra_object_count,
                                      &on_demand_object_count)) {
      goto cleanup;
    }
  }

  if (!freestanding_object || access(freestanding_object, F_OK) != 0) {
    fprintf(stderr,
            "Error: Required freestanding runtime object not found in '%s'\n",
            runtime_directory ? runtime_directory : "");
    goto cleanup;
  }
  /* A shared object has no program entry: whoever loads it already has one,
   * and _start would pull in a reference to a main this library does not
   * define. */
  if (!(options && options->shared_output)) {
    startup_object = replace_extension(executable_filename, ".startup.o");
    if (!startup_object ||
        binary_write_program_startup_object(
            startup_object, profile_runtime, stack_trace,
            options && options->main_wants_argc_argv ? 1 : 0) != 0) {
      fprintf(stderr, "Error: Could not generate freestanding ELF startup\n");
      goto cleanup;
    }
  }

  /* The generated startup object defines _start and passes argc and argv to
   * main. The backend emits non-position-independent code, so the fallback
   * driver receives -no-pie as well as all three no-runtime switches. */
  if (!elf_append_runtime_helpers(stack_trace, profile_runtime,
                                 crash_handler_object, profile_object,
                                 extra_objects, &extra_object_count)) {
    goto cleanup;
  }

  if (!(options && options->link_argument_count > 0) &&
      !mettle_elf_external_linker_requested(options) &&
      mettle_link_elf_native(startup_object, object_filename,
                             executable_filename, freestanding_object,
                             (const char *const *)extra_objects,
                             extra_object_count,
                             !mettle_elf_keep_symbols(options), options) == 0) {
    result = 0;
  }

  /* A link that binds shared libraries, or that emits one, has no fallback:
   * ld and gcc would produce an image whose runtime this compiler does not
   * own, so a failure here is reported rather than papered over. */
  if (mettle_elf_dynamic_link_requested(options)) {
    if (result != 0) {
      fprintf(stderr,
              "Error: The internal ELF linker could not produce '%s'\n",
              executable_filename);
    }
    goto cleanup;
  }

  if (result != 0 && !(options && options->link_argument_count > 0) &&
      mettle_link_elf_direct(startup_object, object_filename,
                             executable_filename, freestanding_object,
                             (const char *const *)extra_objects,
                             extra_object_count,
                             !mettle_elf_keep_symbols(options)) == 0) {
    result = 0;
  }

  /* Build the argv vector directly and exec the compiler via fork/execvp
   * instead of handing a constructed command string to system(). Because no
   * shell ever interprets the arguments, none of the caller-controlled
   * strings, the object/executable filenames or the user-supplied
   * --link-arg values, can inject shell commands (CWE-78) or be word-split
   * into unintended options (CWE-88). Each --link-arg is forwarded as exactly
   * one argv element, matching how it was collected at parse time. */
  if (result != 0) {
    /* Upper bound for controls, output, runtime objects, caller arguments,
     * and the NULL terminator. */
    size_t max_args = 20u + 8u + 6u + 1u +
                       (options ? options->link_argument_count : 0u);
    size_t argc_used = 0u;

    argv_list = malloc(sizeof(*argv_list) * max_args);
    if (!argv_list) {
      fprintf(stderr, "Error: Failed to allocate ELF link argv\n");
      goto cleanup;
    }

    /* execvp does not modify argv contents; the const casts are safe. */
    argv_list[argc_used++] = (char *)cc;
    argv_list[argc_used++] = (char *)"-nostdlib";
    argv_list[argc_used++] = (char *)"-nostartfiles";
    argv_list[argc_used++] = (char *)"-nodefaultlibs";
    argv_list[argc_used++] = (char *)"-no-pie";
    argv_list[argc_used++] = (char *)"-Wl,--gc-sections";
    argv_list[argc_used++] = (char *)"-Wl,-e,_start";
    if (!mettle_elf_keep_symbols(options)) {
      argv_list[argc_used++] = (char *)"-s";
    }
    if (options && options->static_link) {
      argv_list[argc_used++] = (char *)"-static";
    }
    argv_list[argc_used++] = startup_object;
    argv_list[argc_used++] = freestanding_object;
    argv_list[argc_used++] = (char *)object_filename;
    for (extra_index = 0u; extra_index < extra_object_count; extra_index++) {
      if (extra_objects[extra_index]) {
        argv_list[argc_used++] = extra_objects[extra_index];
      }
    }
    argv_list[argc_used++] = (char *)"-o";
    argv_list[argc_used++] = (char *)executable_filename;
    if (options) {
      for (size_t i = 0; i < options->link_argument_count; i++) {
        const char *arg = options->link_arguments[i];
        if (!arg || arg[0] == '\0') {
          continue;
        }
        argv_list[argc_used++] = (char *)arg;
      }
    }
    argv_list[argc_used] = NULL;

    if (mettle_run_process(cc, (const char *const *)argv_list) == 0) {
      result = 0;
    } else {
      fprintf(stderr, "Error: %s failed to produce an ELF executable\n", cc);
    }
  }

cleanup:
  if (startup_object) {
    unlink(startup_object);
  }
  for (extra_index = 0u; extra_index < on_demand_object_count; extra_index++) {
    free(extra_objects[extra_index]);
  }
  free(argv_list);
  free(crash_handler_object);
  free(profile_object);
  free(freestanding_object);
  free(startup_object);
  return result;
}
#endif /* !_WIN32 */

/* Set when the linker that produced the executable already proved it owns
 * its runtime, so the driver does not open the finished file to prove it
 * again: on Windows that second open is what a virus scanner charges for. */
static int g_link_output_ownership_verified;

#ifdef _WIN32
typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} StringList;

static void string_list_destroy(StringList *list) {
  size_t i = 0u;

  if (!list) {
    return;
  }

  for (i = 0u; i < list->count; i++) {
    free(list->items[i]);
  }

  free(list->items);
  memset(list, 0, sizeof(*list));
}

static int string_list_contains(const StringList *list, const char *value) {
  size_t i = 0u;

  if (!list || !value) {
    return 0;
  }

  for (i = 0u; i < list->count; i++) {
    if (list->items[i] && strcmp(list->items[i], value) == 0) {
      return 1;
    }
  }

  return 0;
}

static int string_list_append_owned(StringList *list, char *value) {
  char **grown = NULL;
  size_t new_capacity = 0u;

  if (!list || !value) {
    free(value);
    return 0;
  }
  if (string_list_contains(list, value)) {
    free(value);
    return 1;
  }

  if (list->count == list->capacity) {
    new_capacity = list->capacity ? list->capacity * 2u : 4u;
    grown = realloc(list->items, new_capacity * sizeof(char *));
    if (!grown) {
      free(value);
      return 0;
    }
    list->items = grown;
    list->capacity = new_capacity;
  }

  list->items[list->count++] = value;
  return 1;
}

static int string_list_append_copy(StringList *list, const char *value) {
  char *copy = NULL;

  if (!value) {
    return 0;
  }

  copy = strdup(value);
  if (!copy) {
    return 0;
  }

  return string_list_append_owned(list, copy);
}

static int path_exists_windows(const char *path) {
  return path && path[0] != '\0' && _access(path, 0) == 0;
}

static int text_ends_with_ignore_case(const char *text, const char *suffix) {
  size_t text_length = 0u;
  size_t suffix_length = 0u;
  size_t i = 0u;

  if (!text || !suffix) {
    return 0;
  }

  text_length = strlen(text);
  suffix_length = strlen(suffix);
  if (suffix_length > text_length) {
    return 0;
  }

  for (i = 0u; i < suffix_length; i++) {
    unsigned char left =
        (unsigned char)text[text_length - suffix_length + i];
    unsigned char right = (unsigned char)suffix[i];
    if (tolower(left) != tolower(right)) {
      return 0;
    }
  }

  return 1;
}

static char *normalize_link_library_name(const char *argument,
                                         const char *extension) {
  size_t length = 0u;
  size_t extension_length = 0u;
  char *normalized = NULL;

  if (!argument || !extension) {
    return NULL;
  }

  length = strlen(argument);
  extension_length = strlen(extension);
  if (text_ends_with_ignore_case(argument, extension)) {
    return strdup(argument);
  }

  normalized = malloc(length + extension_length + 1u);
  if (!normalized) {
    return NULL;
  }

  memcpy(normalized, argument, length);
  memcpy(normalized + length, extension, extension_length + 1u);
  return normalized;
}

static int resolve_import_library_path(const char *library_name,
                                       const StringList *search_directories,
                                       StringList *resolved_paths) {
  char *candidate = NULL;
  char *env_copy = NULL;
  char *token = NULL;

  if (!library_name || !resolved_paths) {
    return 0;
  }

  if (strchr(library_name, '\\') || strchr(library_name, '/') ||
      strchr(library_name, ':') || path_exists_windows(library_name)) {
    return string_list_append_copy(resolved_paths, library_name);
  }

  if (search_directories) {
    size_t i = 0u;
    for (i = 0u; i < search_directories->count; i++) {
      candidate = join_paths(search_directories->items[i], library_name);
      if (!candidate) {
        return 0;
      }
      if (path_exists_windows(candidate)) {
        return string_list_append_owned(resolved_paths, candidate);
      }
      free(candidate);
      candidate = NULL;
    }
  }

  const char *lib_env = getenv("LIB");
  env_copy = lib_env ? strdup(lib_env) : NULL;
  token = env_copy ? strtok(env_copy, ";") : NULL;
  while (token) {
    candidate = join_paths(token, library_name);
    if (!candidate) {
      free(env_copy);
      return 0;
    }
    if (path_exists_windows(candidate)) {
      free(env_copy);
      return string_list_append_owned(resolved_paths, candidate);
    }
    free(candidate);
    candidate = NULL;
    token = strtok(NULL, ";");
  }
  free(env_copy);

  return string_list_append_copy(resolved_paths, library_name);
}

static int collect_internal_link_imports(const CompilerOptions *options,
                                          int include_shell32,
                                          StringList *import_library_paths,
                                          StringList *import_dll_names,
                                          char **error_message_out) {
  static const char *default_import_dlls[] = {
      "kernel32.dll", "ws2_32.dll", "user32.dll",
      "gdi32.dll",    "advapi32.dll", "winmm.dll"};
  size_t i = 0u;
  StringList search_directories = {0};

  if (error_message_out) {
    *error_message_out = NULL;
  }
  if (!import_library_paths || !import_dll_names) {
    return 0;
  }

  for (i = 0u; i < sizeof(default_import_dlls) / sizeof(default_import_dlls[0]);
       i++) {
    if (!string_list_append_copy(import_dll_names, default_import_dlls[i])) {
      if (error_message_out) {
        *error_message_out =
            strdup("Out of memory while preparing internal linker defaults");
      }
      string_list_destroy(&search_directories);
      return 0;
    }
  }

  if (include_shell32) {
    if (!string_list_append_copy(import_dll_names, "shell32.dll")) {
      if (error_message_out) {
        *error_message_out =
            strdup("Out of memory while preparing internal linker defaults");
      }
      string_list_destroy(&search_directories);
      return 0;
    }
  }

  if (options && options->tracy) {
    static const char *tracy_import_dlls[] = {"secur32.dll", "dbghelp.dll"};
    for (i = 0u; i < sizeof(tracy_import_dlls) / sizeof(tracy_import_dlls[0]); i++) {
      if (!string_list_append_copy(import_dll_names, tracy_import_dlls[i])) {
        if (error_message_out) {
          *error_message_out =
              strdup("Out of memory while preparing Tracy linker imports");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
    }
  }

  if (!options) {
    string_list_destroy(&search_directories);
    return 1;
  }

  for (i = 0u; i < options->link_argument_count; i++) {
    const char *argument = options->link_arguments[i];

    if (!argument || argument[0] == '\0') {
      continue;
    }
    if (strncmp(argument, "-L", 2) == 0 && argument[2] != '\0') {
      if (!string_list_append_copy(&search_directories, argument + 2)) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while storing internal linker search directories");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
      continue;
    }
  }

  for (i = 0u; i < options->link_argument_count; i++) {
    const char *argument = options->link_arguments[i];
    char *normalized = NULL;

    if (!argument || argument[0] == '\0') {
      continue;
    }
    if (strncmp(argument, "-L", 2) == 0 && argument[2] != '\0') {
      continue;
    }
    if (strncmp(argument, "-l", 2) == 0 && argument[2] != '\0') {
      normalized = normalize_link_library_name(argument + 2u, ".dll");
      if (!normalized) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while preparing internal linker DLL imports");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
      if (!string_list_append_owned(import_dll_names, normalized)) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while preparing internal linker DLL imports");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
      continue;
    }
    if (text_ends_with_ignore_case(argument, ".lib")) {
      if (!resolve_import_library_path(argument, &search_directories,
                                       import_library_paths)) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while preparing internal linker import libraries");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
    }
  }

  string_list_destroy(&search_directories);
  return 1;
}

static int append_internal_link_object_args(const CompilerOptions *options,
                                            const char **object_paths,
                                            size_t object_capacity,
                                            size_t *object_count) {
  size_t i = 0u;
  if (!options || !object_paths || !object_count) {
    return 1;
  }

  for (i = 0u; i < options->link_argument_count; i++) {
    const char *argument = options->link_arguments[i];
    if (!argument || argument[0] == '\0') {
      continue;
    }
    if (!text_ends_with_ignore_case(argument, ".o") &&
        !text_ends_with_ignore_case(argument, ".obj")) {
      continue;
    }
    if (*object_count >= object_capacity) {
      return 0;
    }
    object_paths[(*object_count)++] = argument;
  }

  return 1;
}

static int compiler_options_use_tracy(const CompilerOptions *options) {
  return options && options->tracy;
}

static int append_argument_text(char *buffer, size_t buffer_size, size_t *offset,
                                const char *text) {
  if (!buffer || !offset || !text) {
    return 0;
  }

  size_t text_len = strlen(text);
  if (*offset + text_len >= buffer_size) {
    return 0;
  }

  memcpy(buffer + *offset, text, text_len);
  *offset += text_len;
  buffer[*offset] = '\0';
  return 1;
}

static int append_quoted_argument(char *buffer, size_t buffer_size,
                                  size_t *offset, const char *argument) {
  if (!append_argument_text(buffer, buffer_size, offset, "\"")) {
    return 0;
  }
  if (!append_argument_text(buffer, buffer_size, offset, argument)) {
    return 0;
  }
  return append_argument_text(buffer, buffer_size, offset, "\"");
}

static int append_gcc_link_arguments(char *buffer, size_t buffer_size,
                                     size_t *offset,
                                     const CompilerOptions *options) {
  if (!options) {
    return 1;
  }

  for (size_t i = 0; i < options->link_argument_count; i++) {
    const char *arg = options->link_arguments[i];
    if (!arg || arg[0] == '\0') {
      continue;
    }
    if (!append_argument_text(buffer, buffer_size, offset, " ")) {
      return 0;
    }
    if (!append_argument_text(buffer, buffer_size, offset, arg)) {
      return 0;
    }
  }

  return 1;
}

static int append_msvc_link_argument(char *buffer, size_t buffer_size,
                                     size_t *offset, const char *argument) {
  if (!argument || argument[0] == '\0') {
    return 1;
  }

  if (strncmp(argument, "-l", 2) == 0 && argument[2] != '\0') {
    if (!append_argument_text(buffer, buffer_size, offset, " ")) {
      return 0;
    }
    if (!append_argument_text(buffer, buffer_size, offset, argument + 2)) {
      return 0;
    }
    return append_argument_text(buffer, buffer_size, offset, ".lib");
  }

  if (strncmp(argument, "-L", 2) == 0 && argument[2] != '\0') {
    if (!append_argument_text(buffer, buffer_size, offset, " /LIBPATH:\"")) {
      return 0;
    }
    if (!append_argument_text(buffer, buffer_size, offset, argument + 2)) {
      return 0;
    }
    return append_argument_text(buffer, buffer_size, offset, "\"");
  }

  if (!append_argument_text(buffer, buffer_size, offset, " ")) {
    return 0;
  }
  return append_argument_text(buffer, buffer_size, offset, argument);
}

static int append_msvc_link_arguments(char *buffer, size_t buffer_size,
                                      size_t *offset,
                                      const CompilerOptions *options) {
  if (!options) {
    return 1;
  }

  for (size_t i = 0; i < options->link_argument_count; i++) {
    if (!append_msvc_link_argument(buffer, buffer_size, offset,
                                   options->link_arguments[i])) {
      return 0;
    }
  }

  return 1;
}

static int run_system_command(const char *command) {
  if (!command || command[0] == '\0') {
    return 0;
  }
  return system(command);
}

static int windows_tool_exists(const char *tool_name) {
  return mettle_find_executable(tool_name);
}

static int write_internal_startup_object(const char *path, int profile_runtime,
                                         int stack_trace_init,
                                         int main_wants_argc_argv) {
  return binary_write_program_startup_object(path, profile_runtime,
                                             stack_trace_init,
                                             main_wants_argc_argv);
}

/* Build-to-link routing is documented in docs/linker-build-pipelines.md (asm+GCC
 * vs emit-obj+internal vs emit-obj+external GCC). */

static int mettle_link_internal(const char **object_paths,
                                  const unsigned char *object_is_runtime_default,
                                  size_t object_count,
                                  const char *executable_filename,
                                  int include_shell32,
                                  const CompilerOptions *options) {
  LinkResolutionOptions resolution_options = {"mettle_start", 16u, 1,
                                              object_is_runtime_default};
  LinkResolution *resolution = NULL;
  PeEmissionOptions emission_options = {0};
  StringList import_library_paths = {0};
  StringList import_dll_names = {0};
  char *error_message = NULL;
  int result = 1;

  if (!object_paths || object_count == 0u || !executable_filename) {
    fprintf(stderr, "Error: Missing inputs for internal linker\n");
    return 1;
  }

  if (!collect_internal_link_imports(options, include_shell32,
                                     &import_library_paths, &import_dll_names,
                                     &error_message)) {
    fprintf(stderr, "Error: %s\n",
            error_message ? error_message
                          : "Failed to prepare internal linker imports");
    free(error_message);
    string_list_destroy(&import_library_paths);
    string_list_destroy(&import_dll_names);
    return 1;
  }

  if (!link_resolution_build(object_paths, object_count, &resolution_options,
                             &resolution, &error_message)) {
    fprintf(stderr, "Warning: Internal linker symbol resolution failed: %s\n",
            error_message ? error_message : "unknown error");
    goto cleanup;
  }

  if (options && options->windows_subsystem) {
    emission_options.subsystem = 2u;
  }
  /* A PE is relocated in 64K granules, so its ImageBase must sit on one. */
  if (mtlc_target()->image_base_set) {
    if (mtlc_target()->image_base % 0x10000u) {
      fprintf(stderr,
              "Error: --image-base 0x%llx is not 64K-aligned; a PE image must "
              "load on a 0x10000 boundary\n",
              (unsigned long long)mtlc_target()->image_base);
      goto cleanup;
    }
    emission_options.image_base = mtlc_target()->image_base;
  }
  emission_options.import_library_paths =
      (const char **)import_library_paths.items;
  emission_options.import_library_count = import_library_paths.count;
  emission_options.import_dll_names = (const char **)import_dll_names.items;
  emission_options.import_dll_count = import_dll_names.count;
  if (!pe_emit_executable(resolution, executable_filename, &emission_options,
                          &error_message)) {
    fprintf(stderr, "Warning: Internal linker PE emission failed: %s\n",
            error_message ? error_message : "unknown error");
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  string_list_destroy(&import_library_paths);
  string_list_destroy(&import_dll_names);
  link_resolution_destroy(resolution);
  return result;
}

static int mettle_link_objects_with_gxx(const char **object_paths,
                                        size_t object_count,
                                        const char *executable_filename,
                                        const CompilerOptions *options) {
  size_t cmd_len = strlen(executable_filename) + 512u;
  size_t i = 0u;
  size_t offset = 0u;
  char *command = NULL;
  int result = 1;

  if (!object_paths || object_count == 0u || !executable_filename) {
    fprintf(stderr, "Error: Missing inputs for g++ Tracy link\n");
    return 1;
  }

  for (i = 0u; i < object_count; i++) {
    if (object_paths[i] && object_paths[i][0] != '\0') {
      cmd_len += strlen(object_paths[i]) + 4u;
    }
  }
  if (options) {
    for (i = 0u; i < options->link_argument_count; i++) {
      if (options->link_arguments[i]) {
        cmd_len += strlen(options->link_arguments[i]) + 2u;
      }
    }
  }

  command = malloc(cmd_len);
  if (!command) {
    fprintf(stderr, "Error: Failed to allocate g++ Tracy link command\n");
    return 1;
  }

  if (!append_argument_text(command, cmd_len, &offset, "g++ -o ") ||
      !append_quoted_argument(command, cmd_len, &offset, executable_filename)) {
    free(command);
    fprintf(stderr, "Error: Failed to build g++ Tracy link command\n");
    return 1;
  }

  for (i = 0u; i < object_count; i++) {
    if (!object_paths[i] || object_paths[i][0] == '\0') {
      continue;
    }
    if (!append_argument_text(command, cmd_len, &offset, " ") ||
        !append_quoted_argument(command, cmd_len, &offset, object_paths[i])) {
      free(command);
      fprintf(stderr, "Error: Failed to build g++ Tracy link command\n");
      return 1;
    }
  }

  if (!append_argument_text(command, cmd_len, &offset,
                            " -lkernel32 -luser32 -lgdi32 -ladvapi32 -lws2_32 "
                            "-lsecur32 -ldbghelp") ||
      !append_gcc_link_arguments(command, cmd_len, &offset, options)) {
    free(command);
    fprintf(stderr, "Error: Failed to build g++ Tracy link command\n");
    return 1;
  }

  if (run_system_command(command) != 0) {
    fprintf(stderr, "Warning: g++ Tracy link step failed\n");
    result = 1;
  } else {
    result = 0;
  }

  free(command);
  return result;
}

static int mettle_link_object_with_gcc(const char *object_filename,
                                        const char *executable_filename,
                                        const char *const *runtime_objects,
                                        size_t runtime_object_count,
                                        const CompilerOptions *options) {
  size_t link_argument_count = options ? options->link_argument_count : 0u;
  /* The fixed arguments are: gcc, three -no* flags, two -Wl flags, an optional
   * subsystem flag, the object, -o, the executable, six libraries, and the NULL
   * terminator. Eighteen, and the old bound of twelve was already one short
   * whenever the subsystem flag was present and nothing followed it. */
  size_t capacity = 24u + runtime_object_count + link_argument_count;
  const char **arguments = calloc(capacity, sizeof(*arguments));
  size_t count = 0u;
  int result;
  if (!arguments) {
    fprintf(stderr, "Error: Failed to allocate GCC arguments\n");
    return 1;
  }

  arguments[count++] = "gcc";
  arguments[count++] = "-nostdlib";
  arguments[count++] = "-nostartfiles";
  arguments[count++] = "-nodefaultlibs";
  arguments[count++] = "-Wl,--disable-runtime-pseudo-reloc";
  arguments[count++] = "-Wl,-e,mettle_start,--gc-sections";
  if (options && options->windows_subsystem) {
    arguments[count++] = "-Wl,--subsystem,windows";
  }
  arguments[count++] = object_filename;
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (runtime_objects[i] && runtime_objects[i][0]) {
      arguments[count++] = runtime_objects[i];
    }
  }
  arguments[count++] = "-o";
  arguments[count++] = executable_filename;
  /* The same system libraries the internal linker resolves imports against
   * (collect_internal_link_imports) and the Tracy link step already passes.
   * Only -lkernel32 was here, so a program using std/ui or std/net linked with
   * --linker internal and failed with --linker gcc on every Win32 entry point
   * it named. --gc-sections drops what a program does not reach. */
  arguments[count++] = "-lkernel32";
  arguments[count++] = "-luser32";
  arguments[count++] = "-lgdi32";
  arguments[count++] = "-ladvapi32";
  arguments[count++] = "-lws2_32";
  arguments[count++] = "-lwinmm";
  if (options) {
    for (size_t i = 0; i < options->link_argument_count; i++) {
      if (options->link_arguments[i] && options->link_arguments[i][0]) {
        arguments[count++] = options->link_arguments[i];
      }
    }
  }
  arguments[count] = NULL;

  result = mettle_run_process("gcc", arguments);
  free(arguments);
  if (result != 0) {
    fprintf(stderr, "Warning: GCC object link step failed\n");
    return 1;
  }
  return 0;
}

static int mettle_link_object_with_link(const char *object_filename,
                                          const char *executable_filename,
                                          const char *const *runtime_objects,
                                          size_t runtime_object_count,
                                          const CompilerOptions *options) {
  size_t link_len = strlen(object_filename) + strlen(executable_filename) + 320;
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (runtime_objects[i] && runtime_objects[i][0] != '\0') {
      link_len += strlen(runtime_objects[i]) + 16;
    }
  }
  if (options) {
    for (size_t i = 0; i < options->link_argument_count; i++) {
      if (options->link_arguments[i]) {
        link_len += strlen(options->link_arguments[i]) + 16;
      }
    }
  }

  char *link_command = malloc(link_len);
  if (!link_command) {
    fprintf(stderr, "Error: Failed to allocate MSVC link command\n");
    return 1;
  }

  size_t offset = 0;
  if (!append_argument_text(
          link_command, link_len, &offset,
          (options && options->windows_subsystem)
              ? "link.exe /nologo /nodefaultlib /entry:mettle_start "
                "/subsystem:windows /out:"
              : "link.exe /nologo /nodefaultlib /entry:mettle_start "
                "/subsystem:console /out:") ||
      !append_quoted_argument(link_command, link_len, &offset,
                              executable_filename) ||
      !append_argument_text(link_command, link_len, &offset, " ") ||
      !append_quoted_argument(link_command, link_len, &offset, object_filename)) {
    free(link_command);
    fprintf(stderr, "Error: Failed to build MSVC object link command\n");
    return 1;
  }
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (!runtime_objects[i] || runtime_objects[i][0] == '\0') {
      continue;
    }
    if (!append_argument_text(link_command, link_len, &offset, " ") ||
        !append_quoted_argument(link_command, link_len, &offset,
                                runtime_objects[i])) {
      free(link_command);
      fprintf(stderr, "Error: Failed to build MSVC object link command\n");
      return 1;
    }
  }
  if (!append_argument_text(link_command, link_len, &offset,
                             " kernel32.lib") ||
      !append_msvc_link_arguments(link_command, link_len, &offset, options)) {
    free(link_command);
    fprintf(stderr, "Error: Failed to build MSVC object link command\n");
    return 1;
  }

  int result = run_system_command(link_command);
  free(link_command);
  if (result != 0) {
    fprintf(stderr, "Warning: MSVC object link step failed\n");
    return 1;
  }
  return 0;
}

static int mettle_link_object_file(const char *object_filename,
                                     const char *executable_filename,
                                     const char *runtime_directory,
                                     const CompilerOptions *options) {
  LinkerMode linker_mode =
      options ? options->linker_mode : LINKER_MODE_AUTO;
  int has_gcc = 0;
  int has_link = 0;
  char *external_startup_object = NULL;

  if (!object_filename || !executable_filename || !runtime_directory) {
    fprintf(stderr, "Error: Missing build inputs for executable generation\n");
    return 1;
  }

  has_gcc = (linker_mode == LINKER_MODE_AUTO || linker_mode == LINKER_MODE_GCC)
                ? windows_tool_exists("gcc")
                : 0;
  has_link =
      (linker_mode == LINKER_MODE_AUTO || linker_mode == LINKER_MODE_MSVC)
          ? windows_tool_exists("link.exe")
          : 0;
  if (linker_mode == LINKER_MODE_GCC && !has_gcc) {
    fprintf(stderr, "Error: gcc was requested with --linker gcc but was not found.\n");
    return 1;
  }
  if (linker_mode == LINKER_MODE_MSVC && !has_link) {
    fprintf(stderr,
            "Error: link.exe was requested with --linker msvc but was not found.\n");
    return 1;
  }
  char *crash_gcc_object = join_paths(runtime_directory, "crash_handler.o");
  char *crash_msvc_object = join_paths(runtime_directory, "crash_handler.obj");
  char *atomics_gcc_object = join_paths(runtime_directory, "atomics.o");
  char *atomics_msvc_object = join_paths(runtime_directory, "atomics.obj");
  char *profile_gcc_object = join_paths(runtime_directory, "profile.o");
  char *profile_msvc_object = join_paths(runtime_directory, "profile.obj");
  if (!crash_gcc_object || !crash_msvc_object || !atomics_gcc_object ||
      !atomics_msvc_object || !profile_gcc_object || !profile_msvc_object) {
    fprintf(stderr, "Error: Failed to allocate build paths\n");
    free(crash_gcc_object);
    free(crash_msvc_object);
    free(atomics_gcc_object);
    free(atomics_msvc_object);
    free(profile_gcc_object);
    free(profile_msvc_object);
    return 1;
  }

  int needs_crash = object_needs_crash_handler(object_filename);
  int needs_atomics = object_needs_atomics(object_filename);
  int needs_profile = object_needs_profile_runtime(object_filename);
  int profile_runtime =
      options && compiler_options_use_profile_runtime(options) ? 1 : 0;
  if (profile_runtime) {
    needs_profile = 1;
  }
  if (needs_profile) {
    needs_crash = 1;
  }

  /* --debug-hooks: the program references mettle_dbg_* hooks resolved by the
   * bundled debug runtime object (same auto-link pattern as the profiler).
   * Stack buffers, so the error paths above/below need no extra frees. */
  char debug_gcc_object[1024];
  char debug_msvc_object[1024];
  char freestanding_gcc_object[1024];
  char freestanding_msvc_object[1024];
  int needs_debug = object_needs_debug_runtime(object_filename) ||
                    (options && options->debug_hooks);
  snprintf(debug_gcc_object, sizeof(debug_gcc_object), "%s/debug.o",
           runtime_directory);
  snprintf(debug_msvc_object, sizeof(debug_msvc_object), "%s/debug.obj",
           runtime_directory);

  /* --safe: the shadow map behind mettle_safety_check. A program whose checks
   * all compiled to constant-extent comparisons never names it and does not
   * pay for it. It reports through the crash handler, so it drags that in. */
  char safety_gcc_object[1024];
  char safety_msvc_object[1024];
  int needs_safety = object_needs_safety_runtime(object_filename);
  char swap_gcc_object[1024];
  char swap_msvc_object[1024];
  int needs_swap = object_needs_swap_runtime(object_filename);
  char string_gcc_object[1024];
  char string_msvc_object[1024];
  int needs_string = object_needs_string_runtime(object_filename);
  snprintf(string_gcc_object, sizeof(string_gcc_object), "%s/string.o",
           runtime_directory);
  snprintf(string_msvc_object, sizeof(string_msvc_object), "%s/string.obj",
           runtime_directory);
  snprintf(swap_gcc_object, sizeof(swap_gcc_object), "%s/swap.o",
           runtime_directory);
  snprintf(swap_msvc_object, sizeof(swap_msvc_object), "%s/swap.obj",
           runtime_directory);
  snprintf(safety_gcc_object, sizeof(safety_gcc_object), "%s/safety.o",
           runtime_directory);
  snprintf(safety_msvc_object, sizeof(safety_msvc_object), "%s/safety.obj",
           runtime_directory);
  if (needs_safety) {
    needs_crash = 1;
  }
  snprintf(freestanding_gcc_object, sizeof(freestanding_gcc_object),
           "%s/freestanding.o", runtime_directory);
  snprintf(freestanding_msvc_object, sizeof(freestanding_msvc_object),
           "%s/freestanding.obj", runtime_directory);

  const char *freestanding_object =
      (_access(freestanding_msvc_object, 0) == 0) ? freestanding_msvc_object
                                                  : freestanding_gcc_object;

  int use_tracy = compiler_options_use_tracy(options);
  int needs_tracy_helpers =
      use_tracy || object_needs_tracy_helpers(object_filename);
  TracyBuildArtifacts tracy_artifacts = {0};
  char *tracy_directory = NULL;
  char *tracy_error = NULL;
  const char *tracy_helpers_object = NULL;
  char *tracy_helpers_gcc_object =
      join_paths(runtime_directory, "tracy_helpers.o");
  char *tracy_helpers_msvc_object =
      join_paths(runtime_directory, "tracy_helpers.obj");
  if (!tracy_helpers_gcc_object || !tracy_helpers_msvc_object) {
    fprintf(stderr, "Error: Failed to allocate Tracy build paths\n");
    free(tracy_helpers_gcc_object);
    free(tracy_helpers_msvc_object);
    free(crash_gcc_object);
    free(crash_msvc_object);
    free(atomics_gcc_object);
    free(atomics_msvc_object);
    free(profile_gcc_object);
    free(profile_msvc_object);
    return 1;
  }
  if (use_tracy) {
    fprintf(stderr,
            "Error: --tracy cannot use the external TracyClient in owned "
            "runtime mode because it requires a C++ runtime. Use "
            "--profile-runtime instead.\n");
    free(tracy_helpers_gcc_object);
    free(tracy_helpers_msvc_object);
    free(crash_gcc_object);
    free(crash_msvc_object);
    free(atomics_gcc_object);
    free(atomics_msvc_object);
    free(profile_gcc_object);
    free(profile_msvc_object);
    return 1;
  }
  if (_access(freestanding_object, 0) != 0) {
    fprintf(stderr,
            "Error: Required freestanding runtime object not found in '%s'\n",
            runtime_directory);
    free(tracy_helpers_gcc_object);
    free(tracy_helpers_msvc_object);
    free(crash_gcc_object);
    free(crash_msvc_object);
    free(atomics_gcc_object);
    free(atomics_msvc_object);
    free(profile_gcc_object);
    free(profile_msvc_object);
    return 1;
  }

  if (use_tracy) {
    TracyBuildRequest tracy_request = {
        .tracy_directory = options ? options->tracy_directory : NULL,
        .stdlib_directory = options ? options->stdlib_directory : NULL,
        .executable_filename = executable_filename,
    };
    tracy_directory = tracy_resolve_directory(&tracy_request, &tracy_error);
    if (!tracy_directory) {
      fprintf(stderr, "Error: %s\n",
              tracy_error ? tracy_error : "Failed to resolve Tracy directory");
      free(tracy_error);
      free(tracy_helpers_gcc_object);
      free(tracy_helpers_msvc_object);
      free(crash_gcc_object);
      free(crash_msvc_object);
      free(atomics_gcc_object);
      free(atomics_msvc_object);
      free(profile_gcc_object);
      free(profile_msvc_object);
      return 1;
    }
    if (!tracy_build_support_objects(&tracy_request, tracy_directory,
                                     &tracy_artifacts, &tracy_error)) {
      fprintf(stderr, "Error: %s\n",
              tracy_error ? tracy_error
                          : "Failed to build Tracy support objects");
      free(tracy_error);
      free(tracy_directory);
      tracy_free_artifacts(&tracy_artifacts);
      free(tracy_helpers_gcc_object);
      free(tracy_helpers_msvc_object);
      free(crash_gcc_object);
      free(crash_msvc_object);
      free(atomics_gcc_object);
      free(atomics_msvc_object);
      free(profile_gcc_object);
      free(profile_msvc_object);
      return 1;
    }
    free(tracy_error);
    tracy_error = NULL;
    tracy_helpers_object = tracy_artifacts.helpers_object;
  } else if (needs_tracy_helpers) {
    tracy_helpers_object =
        (_access(tracy_helpers_msvc_object, 0) == 0) ? tracy_helpers_msvc_object
                                                     : tracy_helpers_gcc_object;
    if (_access(tracy_helpers_object, 0) != 0) {
      fprintf(stderr,
              "Error: Program references Tracy helpers but bundled stub "
              "object not found in '%s'\n",
              runtime_directory);
      free(tracy_helpers_gcc_object);
      free(tracy_helpers_msvc_object);
      free(crash_gcc_object);
      free(crash_msvc_object);
      free(atomics_gcc_object);
      free(atomics_msvc_object);
      free(profile_gcc_object);
      free(profile_msvc_object);
      return 1;
    }
  }

  int build_result = 1;

  if (use_tracy && tracy_artifacts.use_gxx_link) {
    size_t gxx_capacity =
        4u + (needs_crash ? 1u : 0u) + (needs_atomics ? 1u : 0u) +
        (needs_profile ? 1u : 0u);
    const char **gxx_objects = calloc(gxx_capacity, sizeof(const char *));
    size_t gxx_count = 0u;

    if (!gxx_objects) {
      fprintf(stderr, "Error: Failed to allocate g++ Tracy link object list\n");
      goto cleanup;
    }

    gxx_objects[gxx_count++] = object_filename;
    if (needs_crash) {
      if (_access(crash_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled crash-handler runtime object not found in '%s'\n",
                runtime_directory);
        free(gxx_objects);
        goto cleanup;
      }
      gxx_objects[gxx_count++] = crash_gcc_object;
    }
    if (needs_atomics) {
      if (_access(atomics_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled atomics runtime object not found in '%s'\n",
                runtime_directory);
        free(gxx_objects);
        goto cleanup;
      }
      gxx_objects[gxx_count++] = atomics_gcc_object;
    }
    if (needs_profile) {
      if (_access(profile_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled profile runtime object not found in '%s'\n",
                runtime_directory);
        free(gxx_objects);
        goto cleanup;
      }
      gxx_objects[gxx_count++] = profile_gcc_object;
    }
    gxx_objects[gxx_count++] = tracy_artifacts.helpers_object;
    gxx_objects[gxx_count++] = tracy_artifacts.client_object;

    if (mettle_link_objects_with_gxx(gxx_objects, gxx_count, executable_filename,
                                     options) == 0) {
      build_result = 0;
    } else {
      fprintf(stderr, "Error: g++ Tracy link failed\n");
    }
    free(gxx_objects);
    goto cleanup;
  }

  if (linker_mode == LINKER_MODE_INTERNAL || linker_mode == LINKER_MODE_AUTO) {
    size_t object_capacity =
        8u + (use_tracy ? 2u : (needs_tracy_helpers ? 1u : 0u)) +
        (options ? options->link_argument_count : 0u);
    const char **object_paths = calloc(object_capacity, sizeof(const char *));
    /* Parallel to object_paths: 1 marks a bundled runtime object, whose
     * definitions a program object is allowed to replace. calloc leaves the
     * program's own objects (and any -Wl object arguments) at 0. */
    unsigned char *object_is_default = calloc(object_capacity, 1u);
    const char *crash_object = NULL;
    const char *atomics_object = NULL;
    const char *profile_object = NULL;
    const char *debug_object = NULL;
    const char *safety_object = NULL;
    const char *swap_object_internal = NULL;
    const char *string_object_internal = NULL;
    char *startup_object = replace_extension(executable_filename, ".startup.obj");
    size_t object_count = 0u;
    int startup_ready = 0;

    if (!object_paths || !object_is_default) {
      fprintf(stderr, "Error: Failed to allocate internal-linker object list\n");
      free(object_paths);
      free(object_is_default);
      goto cleanup;
    }

    if (!startup_object) {
      if (linker_mode == LINKER_MODE_INTERNAL || (!has_gcc && !has_link)) {
        fprintf(stderr,
                "Error: Failed to allocate internal-linker startup object path\n");
        free(object_paths);
        free(object_is_default);
        goto cleanup;
      }
      fprintf(stderr,
              "Warning: Failed to allocate internal-linker startup object path, "
              "falling back to external linkers\n");
    } else if (write_internal_startup_object(
                   startup_object, profile_runtime,
                   compiler_options_install_crash_handler(options),
                   options && options->main_wants_argc_argv ? 1 : 0) != 0) {
      if (linker_mode == LINKER_MODE_INTERNAL || (!has_gcc && !has_link)) {
        fprintf(stderr,
                "Error: Failed to generate internal-linker startup object\n");
        free(startup_object);
        free(object_paths);
        free(object_is_default);
        goto cleanup;
      }
      fprintf(stderr,
              "Warning: Failed to generate internal-linker startup object, "
              "falling back to external linkers\n");
    } else {
      startup_ready = 1;
    }

    if (startup_ready) {
      if (needs_crash) {
        crash_object = (_access(crash_msvc_object, 0) == 0) ? crash_msvc_object
                                                            : crash_gcc_object;
      }
      if (needs_atomics) {
        atomics_object = (_access(atomics_msvc_object, 0) == 0)
                             ? atomics_msvc_object
                             : atomics_gcc_object;
      }
      if (needs_profile) {
        profile_object = (_access(profile_msvc_object, 0) == 0)
                             ? profile_msvc_object
                             : profile_gcc_object;
        if (_access(profile_object, 0) != 0) {
          fprintf(stderr,
                  "Error: Bundled profile runtime object not found in '%s'\n",
                  runtime_directory);
          free(object_paths);
          free(object_is_default);
          if (startup_object) {
            if (startup_ready) {
              _unlink(startup_object);
            }
            free(startup_object);
          }
          goto cleanup;
        }
      }
      if (needs_debug) {
        debug_object = (_access(debug_msvc_object, 0) == 0) ? debug_msvc_object
                                                            : debug_gcc_object;
        if (_access(debug_object, 0) != 0) {
          fprintf(stderr,
                  "Error: Bundled debug runtime object not found in '%s'\n",
                  runtime_directory);
          free(object_paths);
          free(object_is_default);
          if (startup_object) {
            if (startup_ready) {
              _unlink(startup_object);
            }
            free(startup_object);
          }
          goto cleanup;
        }
      }
      if (needs_string) {
        string_object_internal = (_access(string_msvc_object, 0) == 0)
                                     ? string_msvc_object
                                     : string_gcc_object;
      }
      if (needs_swap) {
        swap_object_internal = (_access(swap_msvc_object, 0) == 0)
                                   ? swap_msvc_object
                                   : swap_gcc_object;
      }
      if (needs_safety) {
        safety_object = (_access(safety_msvc_object, 0) == 0)
                            ? safety_msvc_object
                            : safety_gcc_object;
        if (_access(safety_object, 0) != 0) {
          fprintf(stderr,
                  "Error: Bundled safety runtime object not found in '%s'\n",
                  runtime_directory);
          free(object_paths);
          free(object_is_default);
          if (startup_object) {
            if (startup_ready) {
              _unlink(startup_object);
            }
            free(startup_object);
          }
          goto cleanup;
        }
      }

      object_paths[object_count++] = startup_object;
      object_is_default[object_count] = 1u;
      object_paths[object_count++] = freestanding_object;
      object_paths[object_count++] = object_filename;
      if (crash_object) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = crash_object;
      }
      if (atomics_object) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = atomics_object;
      }
      if (profile_object) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = profile_object;
      }
      if (debug_object) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = debug_object;
      }
      if (safety_object) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = safety_object;
      }
      if (swap_object_internal) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = swap_object_internal;
      }
      if (string_object_internal) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = string_object_internal;
      }
      if (use_tracy) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = tracy_artifacts.helpers_object;
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = tracy_artifacts.client_object;
      } else if (needs_tracy_helpers && tracy_helpers_object) {
        object_is_default[object_count] = 1u;
        object_paths[object_count++] = tracy_helpers_object;
      }
      if (!append_internal_link_object_args(options, object_paths,
                                            object_capacity, &object_count)) {
        fprintf(stderr, "Error: Too many internal-linker object arguments\n");
        free(object_paths);
        free(object_is_default);
        if (startup_object) {
          if (startup_ready) {
            _unlink(startup_object);
          }
          free(startup_object);
        }
        goto cleanup;
      }

      if (mettle_link_internal(object_paths, object_is_default, object_count,
                                 executable_filename, 0, options) == 0) {
        build_result = 0;
        /* The internal PE emitter checked the image it wrote, from the handle
         * it already had. Reopening the file to check it again is what a virus
         * scanner charges for. */
        g_link_output_ownership_verified = 1;
      } else if (linker_mode == LINKER_MODE_INTERNAL) {
        fprintf(stderr, "Error: Internal linker failed to produce an executable\n");
      } else if (!has_gcc && !has_link) {
        fprintf(stderr,
                "Error: Internal linker failed and no external fallback linker is "
                "available.\n");
      } else {
        fprintf(stderr,
                "Warning: Internal linker failed in auto mode, falling back to "
                "external linkers\n");
      }
    }

    if (startup_object) {
      if (startup_ready) {
        _unlink(startup_object);
      }
      free(startup_object);
    }
    free(object_paths);
    free(object_is_default);

    if (build_result == 0 || linker_mode == LINKER_MODE_INTERNAL ||
        (!has_gcc && !has_link)) {
      goto cleanup;
    }
  }

  if ((has_gcc && linker_mode != LINKER_MODE_MSVC) ||
      (has_link && linker_mode != LINKER_MODE_GCC)) {
    external_startup_object =
        replace_extension(executable_filename, ".external-startup.obj");
    if (!external_startup_object ||
        write_internal_startup_object(
            external_startup_object, profile_runtime,
            compiler_options_install_crash_handler(options),
            options && options->main_wants_argc_argv ? 1 : 0) != 0) {
      fprintf(stderr, "Error: Failed to generate external-linker startup object\n");
      goto cleanup;
    }
  }

  if (has_gcc && linker_mode != LINKER_MODE_MSVC) {
    const char *runtime_objects[9] = {NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, NULL};
    size_t runtime_object_count = 0u;
    runtime_objects[runtime_object_count++] = external_startup_object;
    runtime_objects[runtime_object_count++] = freestanding_gcc_object;
    if (needs_crash) {
      if (_access(crash_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled crash-handler runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = crash_gcc_object;
    }
    if (needs_atomics) {
      if (_access(atomics_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled atomics runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = atomics_gcc_object;
    }
    if (needs_profile) {
      if (_access(profile_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled profile runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = profile_gcc_object;
    }
    if (needs_string) {
      const char *string_object = (_access(string_msvc_object, 0) == 0)
                                      ? string_msvc_object
                                      : string_gcc_object;
      runtime_objects[runtime_object_count++] = string_object;
    }
    if (needs_swap) {
      const char *swap_object = (_access(swap_msvc_object, 0) == 0)
                                    ? swap_msvc_object
                                    : swap_gcc_object;
      if (_access(swap_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled swap runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = swap_object;
    }
    if (needs_safety) {
      if (_access(safety_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled safety runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = safety_gcc_object;
    }
    if (needs_debug) {
      if (_access(debug_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled debug runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = debug_gcc_object;
    }
    if (!use_tracy && needs_tracy_helpers && tracy_helpers_object) {
      runtime_objects[runtime_object_count++] = tracy_helpers_object;
    }
    if (mettle_link_object_with_gcc(object_filename, executable_filename,
                                      runtime_objects, runtime_object_count,
                                      options) == 0) {
      build_result = 0;
      goto cleanup;
    }
  }

  if (has_link && linker_mode != LINKER_MODE_GCC) {
    const char *runtime_objects[9] = {NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, NULL};
    size_t runtime_object_count = 0u;
    runtime_objects[runtime_object_count++] = external_startup_object;
    runtime_objects[runtime_object_count++] = freestanding_object;
    if (needs_crash) {
      const char *crash_object = (_access(crash_msvc_object, 0) == 0)
                                     ? crash_msvc_object
                                     : crash_gcc_object;
      if (_access(crash_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled crash-handler runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = crash_object;
    }
    if (needs_atomics) {
      const char *atomics_object = (_access(atomics_msvc_object, 0) == 0)
                                       ? atomics_msvc_object
                                       : atomics_gcc_object;
      if (_access(atomics_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled atomics runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = atomics_object;
    }
    if (needs_profile) {
      const char *profile_object = (_access(profile_msvc_object, 0) == 0)
                                       ? profile_msvc_object
                                       : profile_gcc_object;
      if (_access(profile_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled profile runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = profile_object;
    }
    if (needs_string) {
      const char *msvc_string_object = (_access(string_msvc_object, 0) == 0)
                                           ? string_msvc_object
                                           : string_gcc_object;
      runtime_objects[runtime_object_count++] = msvc_string_object;
    }
    if (needs_swap) {
      const char *msvc_swap_object = (_access(swap_msvc_object, 0) == 0)
                                         ? swap_msvc_object
                                         : swap_gcc_object;
      if (_access(msvc_swap_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled swap runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = msvc_swap_object;
    }
    if (needs_safety) {
      const char *msvc_safety_object = (_access(safety_msvc_object, 0) == 0)
                                           ? safety_msvc_object
                                           : safety_gcc_object;
      if (_access(msvc_safety_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled safety runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = msvc_safety_object;
    }
    if (needs_debug) {
      const char *msvc_debug_object = (_access(debug_msvc_object, 0) == 0)
                                          ? debug_msvc_object
                                          : debug_gcc_object;
      if (_access(msvc_debug_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled debug runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = msvc_debug_object;
    }
    if (!use_tracy && needs_tracy_helpers && tracy_helpers_object) {
      const char *stub_object =
          (_access(tracy_helpers_msvc_object, 0) == 0) ? tracy_helpers_msvc_object
                                                       : tracy_helpers_gcc_object;
      runtime_objects[runtime_object_count++] = stub_object;
    }
    if (mettle_link_object_with_link(object_filename, executable_filename,
                                       runtime_objects, runtime_object_count,
                                       options) == 0) {
      build_result = 0;
      goto cleanup;
    }
  }

  fprintf(stderr,
          "Error: Failed to link executable with the available linker backends\n");

cleanup:
  if (external_startup_object) {
    _unlink(external_startup_object);
  }
  free(external_startup_object);
  tracy_free_artifacts(&tracy_artifacts);
  free(tracy_directory);
  free(tracy_error);
  free(tracy_helpers_gcc_object);
  free(tracy_helpers_msvc_object);
  free(crash_gcc_object);
  free(crash_msvc_object);
  free(atomics_gcc_object);
  free(atomics_msvc_object);
  free(profile_gcc_object);
  free(profile_msvc_object);
  return build_result;
}
#endif

static int add_import_directory(CompilerOptions *options, const char *path) {
  if (!options || !path || path[0] == '\0') {
    return 0;
  }

  size_t next_count = options->import_directory_count + 1;
  const char **grown = realloc((void *)options->import_directories,
                               next_count * sizeof(const char *));
  if (!grown) {
    return 0;
  }

  grown[options->import_directory_count] = path;
  options->import_directories = grown;
  options->import_directory_count = next_count;
  return 1;
}

static int add_string_option(const char ***list, size_t *count,
                             const char *value) {
  const char **grown = NULL;

  if (!value || value[0] == '\0') {
    return 0;
  }
  grown = realloc((void *)*list, (*count + 1u) * sizeof(const char *));
  if (!grown) {
    return 0;
  }
  grown[*count] = value;
  *list = grown;
  *count += 1u;
  return 1;
}

static int add_link_argument(CompilerOptions *options, const char *argument) {
  if (!options || !argument || argument[0] == '\0') {
    return 0;
  }

  size_t next_count = options->link_argument_count + 1;
  const char **grown = realloc((void *)options->link_arguments,
                               next_count * sizeof(const char *));
  if (!grown) {
    return 0;
  }

  grown[options->link_argument_count] = argument;
  options->link_arguments = grown;
  options->link_argument_count = next_count;
  return 1;
}

/* Resolve the default PTX target from the local GPU when the user gave no
 * --gpu-arch. Queries the driver for the device's compute capability and maps
 * it to the matching sm_ target, taking the architecture-specific `a` variant
 * where one exists (sm_90 onward) so the full instruction surface (block-scaled
 * MMA and friends) is available on the machine that will run the output.
 * Returns 1 and fills `out` on success; returns 0 when no NVIDIA driver is
 * visible or its answer is unparseable, in which case the caller keeps the
 * project default (GB10 sm_121a), preserving cross-compile behavior on hosts
 * with no GPU. */
static int detect_gpu_sm_count(void) {
  const GpuDetectResult *local = gpu_detect_local();
  if (!local->available || local->device_count <= 0) return 0;
  int count = local->devices[0].multiprocessor_count;
  return count > 0 && count < 100000 ? count : 0;
}

/* --report-occupancy: assemble the just-written PTX with `ptxas -v` and print
 * each kernel's registers per thread plus the occupancy ceiling they imply.
 * The resource model is the documented 12.x upper bound (64K 32-bit registers
 * and 48 resident warps per SM, 100KB shared memory, 24 resident blocks,
 * 32-lane warps, register allocation unit 1), so the printed ceiling is an
 * upper bound: real granularity can only lower it. A `kernel(block = ...)`
 * declaration tightens the bound to whole resident blocks.
 *
 * The per-SM ceiling says nothing about whether a launch carries enough work
 * to reach it -- a 16-block launch on a 36-SM card is work-limited at any
 * residency. When the SM count is known (--sms=N, or the local driver when
 * the flag is absent), each line also prints the whole-card fill threshold,
 * so a reader can put their grid size next to it. */
/* `ptxas -v` writes its resource numbers as "<N> bytes <what>". Scan back from
 * the label to the digits that belong to it. */
static long long ptxas_bytes_before(const char *line, const char *label) {
  const char *found = strstr(line, label);
  if (!found) return -1;
  const char *cursor = found;
  while (cursor > line && cursor[-1] == ' ') cursor--;
  const char *end = cursor;
  while (cursor > line && cursor[-1] >= '0' && cursor[-1] <= '9') cursor--;
  if (cursor == end) return -1;
  return atoll(cursor);
}

static void report_ptx_occupancy(const IRProgram *program,
                                 const char *ptx_path, const char *arch,
                                 int sm_count, int sm_count_is_local) {
  char cubin[512];
  char command[1200];
  snprintf(cubin, sizeof(cubin), "%s.occupancy.cubin", ptx_path);
  snprintf(command, sizeof(command), "ptxas -v -arch=%s \"%s\" -o \"%s\" 2>&1",
           arch ? arch : "sm_121a", ptx_path, cubin);
#ifdef _WIN32
  FILE *pipe = _popen(command, "r");
#else
  FILE *pipe = popen(command, "r");
#endif
  if (!pipe) {
    fprintf(stderr, "--report-occupancy: could not run ptxas\n");
    return;
  }
  printf("Occupancy report (%s; upper bound: 64K regs/SM, 48 warps/SM, "
         "100KB smem/SM, allocation unit 1",
         arch ? arch : "sm_121a");
  if (sm_count > 0) {
    printf("; %d SMs, %s", sm_count, sm_count_is_local ? "local GPU" : "--sms");
  }
  printf("):\n");
  char line[512];
  char entry[256] = {0};
  long long entry_smem = 0;
  long long spill_stores = 0;
  long long spill_loads = 0;
  long long stack_frame = 0;
  int reported = 0;
  int any_spill = 0;
  while (fgets(line, sizeof(line), pipe)) {
    char name[256];
    if (sscanf(line, " ptxas info : Compiling entry function '%255[^']'",
               name) == 1) {
      snprintf(entry, sizeof(entry), "%s", name);
      entry_smem = 0;
      spill_stores = 0;
      spill_loads = 0;
      stack_frame = 0;
      continue;
    }
    /* The "Function properties" line precedes this entry's "Used" line and
     * carries the stack frame and spill traffic. Spilling to local memory is
     * a worse signal than any occupancy percentage: it is a per-access
     * memory round trip the register allocator could not avoid. */
    long long stores = ptxas_bytes_before(line, "bytes spill stores");
    if (stores >= 0) {
      long long loads = ptxas_bytes_before(line, "bytes spill loads");
      long long frame = ptxas_bytes_before(line, "bytes stack frame");
      spill_stores = stores;
      spill_loads = loads > 0 ? loads : 0;
      stack_frame = frame > 0 ? frame : 0;
      continue;
    }
    const char *used = strstr(line, "Used ");
    if (!used || !entry[0]) {
      continue;
    }
    int registers = 0;
    if (sscanf(used, "Used %d registers", &registers) != 1) {
      continue;
    }
    long long smem = ptxas_bytes_before(line, "bytes smem");
    if (smem >= 0) {
      entry_smem = smem;
    }
    long long register_warp_limit =
        registers > 0 ? 65536ll / ((long long)registers * 32) : 48;
    if (register_warp_limit > 48) register_warp_limit = 48;

    int block = 0;
    for (size_t f = 0; program && f < program->function_count; f++) {
      const IRFunction *function = program->functions[f];
      if (function && function->is_kernel && function->name &&
          strcmp(function->name, entry) == 0 && function->kernel_block[0] > 0) {
        block = function->kernel_block[0] *
                (function->kernel_block[1] > 0 ? function->kernel_block[1] : 1) *
                (function->kernel_block[2] > 0 ? function->kernel_block[2] : 1);
        break;
      }
    }

    long long warps = register_warp_limit;
    const char *limiter = registers > 0 && register_warp_limit < 48
                              ? ", register-limited"
                              : "";
    if (block > 0) {
      long long warps_per_block = (block + 31) / 32;
      long long blocks = warps_per_block > 0
                             ? register_warp_limit / warps_per_block
                             : 0;
      if (blocks > 24) blocks = 24;
      if (entry_smem > 0) {
        long long smem_blocks = 102400ll / entry_smem;
        if (smem_blocks < blocks) {
          blocks = smem_blocks;
          limiter = ", shared-memory-limited";
        }
      }
      warps = blocks * warps_per_block;
      if (warps > 48) warps = 48;
      if (warps < register_warp_limit && !*limiter) limiter = ", block-limited";
      printf("  %s: %d registers, block %d (%lld warps/block, %lld blocks) -> "
             "%lld/48 resident warps (%lld%%)%s",
             entry, registers, block, warps_per_block, blocks, warps,
             warps * 100 / 48, limiter);
      if (sm_count > 0 && blocks > 0) {
        /* The whole-card fill threshold: launches below this many blocks
         * cannot reach the ceiling above no matter what it says. */
        printf("; full card = %lld blocks (%d SMs x %lld)",
               (long long)sm_count * blocks, sm_count, blocks);
      }
    } else {
      printf("  %s: %d registers -> %lld/48 resident warps (%lld%%)%s",
             entry, registers, warps, warps * 100 / 48, limiter);
      if (sm_count > 0 && warps > 0) {
        printf("; full card = %lld warps (%d SMs x %lld)",
               (long long)sm_count * warps, sm_count, warps);
      }
    }
    if (spill_stores > 0 || spill_loads > 0) {
      printf("; SPILLS %lld bytes stored, %lld loaded", spill_stores,
             spill_loads);
      any_spill = 1;
    } else if (stack_frame > 0) {
      printf("; %lld byte stack frame", stack_frame);
    }
    printf("\n");
    reported = 1;
    entry[0] = '\0';
  }
#ifdef _WIN32
  int status = _pclose(pipe);
#else
  int status = pclose(pipe);
#endif
  remove(cubin);
  if (any_spill) {
    printf("  note: a spilling kernel pays a local-memory round trip per "
           "spilled access; that costs more than the residency above.\n");
  }
  if (!reported) {
    fprintf(stderr,
            "--report-occupancy: no ptxas resource report (is ptxas on PATH "
            "and the target '%s' supported?); ptxas exit %d\n",
            arch ? arch : "sm_121a", status);
  }
}

static int detect_host_gpu_ptx_target(char *out, size_t out_size) {
  return gpu_detect_ptx_target(0, out, out_size);
}

/* --emit-kernel-decls: write the host-side `extern kernel` declaration for
 * every kernel in the module just compiled. A host that imports the generated
 * file cannot disagree with the kernels it launches about their arguments or
 * their block shape, because the declarations are the kernels. Returns 1 on
 * success. */
/* The bare type name a parameter or field spelling refers to: `Ray*` and
 * `Ray[8]` both name `Ray`. Returns the length written, 0 when the spelling
 * names nothing a struct declaration could match. */
static size_t kernel_decl_base_type(const char *spelling, char *out,
                                    size_t capacity) {
  if (!spelling || !out || capacity == 0) return 0;
  size_t n = 0;
  while (spelling[n] && spelling[n] != '*' && spelling[n] != '[' &&
         spelling[n] != ' ' && n + 1 < capacity) {
    out[n] = spelling[n];
    n++;
  }
  out[n] = 0;
  return n;
}

/* Mark `name`'s struct declaration, and every struct its fields reach, as one
 * the generated file has to carry. The host cannot import a declaration that
 * names a record it has never seen. */
static void kernel_decl_mark_record(Program *prog, char *wanted,
                                    const char *name) {
  if (!prog || !wanted || !name || !*name) return;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_STRUCT_DECLARATION || !decl->data) continue;
    StructDeclaration *record = (StructDeclaration *)decl->data;
    if (!record->name || strcmp(record->name, name) != 0 || wanted[i]) continue;
    wanted[i] = 1;
    for (size_t f = 0; f < record->field_count; f++) {
      char base[128];
      if (record->field_types &&
          kernel_decl_base_type(record->field_types[f], base, sizeof(base))) {
        kernel_decl_mark_record(prog, wanted, base);
      }
    }
    return;
  }
}

static int write_kernel_declarations(ASTNode *program, const char *path,
                                     const char *source_name) {
  if (!program || program->type != AST_PROGRAM || !program->data) return 0;
  Program *prog = (Program *)program->data;

  FILE *out = fopen(path, "w");
  if (!out) {
    fprintf(stderr, "Error: could not open '%s' for the kernel declarations\n",
            path);
    return 0;
  }
  fprintf(out,
          "// Generated by `mettle --emit-kernel-decls` from %s.\n"
          "// Import this from the host so `dispatch` checks every launch\n"
          "// against the kernel as it was actually compiled. Do not edit:\n"
          "// re-emit it whenever the kernels change.\n\n",
          source_name ? source_name : "a GPU module");

  /* Kernel parameters may name records, so those declarations come first and
   * the file stays self-contained. */
  char *wanted = prog->declaration_count
                     ? (char *)calloc(prog->declaration_count, 1)
                     : NULL;
  if (prog->declaration_count && !wanted) {
    fclose(out);
    fprintf(stderr, "Error: out of memory writing the kernel declarations\n");
    return 0;
  }
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_FUNCTION_DECLARATION || !decl->data) continue;
    FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
    if (!fn->is_kernel || fn->is_extern) continue;
    for (size_t p = 0; p < fn->parameter_count; p++) {
      char base[128];
      if (fn->parameter_types &&
          kernel_decl_base_type(fn->parameter_types[p], base, sizeof(base))) {
        kernel_decl_mark_record(prog, wanted, base);
      }
    }
  }
  size_t records = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    if (!wanted || !wanted[i]) continue;
    StructDeclaration *record = (StructDeclaration *)prog->declarations[i]->data;
    fprintf(out, "struct %s {\n", record->name ? record->name : "record");
    for (size_t f = 0; f < record->field_count; f++) {
      fprintf(out, "  %s: %s;\n",
              record->field_names && record->field_names[f]
                  ? record->field_names[f]
                  : "field",
              record->field_types && record->field_types[f]
                  ? record->field_types[f]
                  : "int64");
    }
    fprintf(out, "}\n\n");
    records++;
  }
  free(wanted);

  size_t written = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_FUNCTION_DECLARATION || !decl->data) {
      continue;
    }
    FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
    if (!fn->is_kernel || !fn->name || fn->is_extern) continue;

    fprintf(out, "extern kernel");
    if (fn->kernel_block[0] > 0) {
      if (fn->kernel_block[1] > 1 || fn->kernel_block[2] > 1) {
        fprintf(out, "(block = (%d, %d, %d)", fn->kernel_block[0],
                fn->kernel_block[1] > 0 ? fn->kernel_block[1] : 1,
                fn->kernel_block[2] > 0 ? fn->kernel_block[2] : 1);
      } else {
        fprintf(out, "(block = %d", fn->kernel_block[0]);
      }
      if (fn->kernel_threads_per_item > 1) {
        fprintf(out, ", per = warp");
      }
      fprintf(out, ")");
    }
    fprintf(out, " %s(", fn->name);
    for (size_t p = 0; p < fn->parameter_count; p++) {
      const char *name = fn->parameter_names ? fn->parameter_names[p] : NULL;
      const char *type = fn->parameter_types ? fn->parameter_types[p] : NULL;
      fprintf(out, "%s%s: %s", p ? ", " : "", name ? name : "arg",
              type ? type : "int64");
    }
    fprintf(out, ");\n");
    written++;
  }
  fclose(out);
  if (records) {
    printf("Generated kernel declarations: %s (%zu kernel%s, %zu record%s)\n",
           path, written, written == 1 ? "" : "s", records,
           records == 1 ? "" : "s");
  } else {
    printf("Generated kernel declarations: %s (%zu kernel%s)\n", path, written,
           written == 1 ? "" : "s");
  }
  return 1;
}

/* Round a byte count to the nearest tenth of a GiB for the report below. */
static void gpu_info_format_memory(long long bytes, char *out, size_t out_size) {
  if (bytes <= 0) {
    snprintf(out, out_size, "unknown");
    return;
  }
  long long tenths = (bytes * 10 + (1LL << 29)) / (1LL << 30);
  snprintf(out, out_size, "%lld.%lld GiB", tenths / 10, tenths % 10);
}

/* --gpu-info: everything the toolchain knows about this machine's GPUs and
 * the target it would pick without --gpu-arch. Answers "will my kernels run
 * here, and as what?" before a single line is compiled. Returns the process
 * exit status: 0 when a device was found, 1 when none was. */
static int report_gpu_info(const char *default_target, int isa_major,
                           int isa_minor) {
  const GpuDetectResult *local = gpu_detect_local();
  printf("Mettle GPU target report\n");

  if (!local->available) {
    printf("  Local devices     none (%s)\n", local->source);
    printf("  Default target    %s, PTX ISA %d.%d (cross-compile default)\n",
           default_target, isa_major, isa_minor);
    const char *ptxas_version = gpu_detect_ptxas_version();
    printf("  Assembler         %s%s\n", ptxas_version ? "ptxas " : "",
           ptxas_version ? ptxas_version : "ptxas not on PATH");
    printf("\n  --emit-ptx still works: PTX is text the driver compiles at\n"
           "  load time, so kernels can be built here and run elsewhere.\n");
    return 1;
  }

  if (local->driver_version > 0) {
    printf("  Driver            CUDA %d.%d (%s)\n", local->driver_version / 1000,
           (local->driver_version % 1000) / 10, local->source);
  } else {
    printf("  Driver            %s\n", local->source);
  }
  printf("  Devices           %d\n", local->device_count);
  for (int i = 0; i < local->device_count; i++) {
    const GpuDetectDevice *device = &local->devices[i];
    char target[32];
    char memory[32];
    if (!gpu_detect_ptx_target(i, target, sizeof(target))) {
      snprintf(target, sizeof(target), "unknown");
    }
    gpu_info_format_memory(device->total_memory, memory, sizeof(memory));
    printf("  [%d] %s\n", i, device->name[0] ? device->name : "(unnamed)");
    printf("      compute capability   %d.%d  ->  %s\n", device->compute_major,
           device->compute_minor, target);
    if (device->multiprocessor_count > 0) {
      printf("      multiprocessors      %d\n", device->multiprocessor_count);
    }
    if (device->warp_size > 0) {
      printf("      warp size            %d\n", device->warp_size);
    }
    if (device->max_threads_per_block > 0) {
      printf("      max threads / block  %d\n", device->max_threads_per_block);
    }
    if (device->max_shared_memory_per_block > 0) {
      printf("      shared mem / block   %d KiB\n",
             device->max_shared_memory_per_block / 1024);
    }
    printf("      global memory        %s%s\n", memory,
           device->integrated ? " (unified with host)" : "");
  }

  const char *ptxas_version = gpu_detect_ptxas_version();
  char selected[32];
  int have_selected = gpu_detect_ptx_target(0, selected, sizeof(selected));
  if (ptxas_version) {
    printf("  Assembler         ptxas %s", ptxas_version);
    if (have_selected) {
      printf(" (%s %s)", selected,
             gpu_detect_ptxas_supports(selected) ? "supported"
                                                 : "NOT supported");
    }
    printf("\n");
  } else {
    printf("  Assembler         ptxas not on PATH (only --report-occupancy "
           "needs it)\n");
  }
  printf("  Default target    %s, PTX ISA %d.%d\n",
         have_selected ? selected : default_target, isa_major, isa_minor);
  printf("\n  Build kernels for this machine with:\n"
         "    mettle --emit-ptx kernels.mettle -o kernels.ptx\n"
         "  Override the target with --gpu-arch=sm_NN, --gpu-arch=native, or\n"
         "  --gpu-arch=portable to build PTX that runs on older cards.\n");
  return 0;
}

typedef struct {
  int build_executable;
  int linker_mode_explicit;
  int output_filename_explicit;
  int ptx_version_explicit;
  int gpu_arch_explicit;
  char detected_ptx_target[16];
} DriverFlags;

typedef enum {
  DRIVER_FLAG_UNMATCHED = 0,
  DRIVER_FLAG_TAKEN,
  DRIVER_FLAG_FAILED
} DriverFlagResult;

static DriverFlagResult parse_flag_shared_library(CompilerOptions *options,
                                                  int argc, char *argv[],
                                                  int *index) {
  int i = *index;
  (void)argc;

  if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0') {
    if (!add_string_option(&options->shared_libraries,
                           &options->shared_library_count, argv[i] + 2)) {
      fprintf(stderr, "Error: Failed to add library '%s'\n", argv[i] + 2);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--library") == 0 && i + 1 < argc) {
    if (!add_string_option(&options->shared_libraries,
                           &options->shared_library_count, argv[++i])) {
      fprintf(stderr, "Error: Failed to add library '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "-L", 2) == 0 && argv[i][2] != '\0') {
    if (!add_string_option(&options->library_search_paths,
                           &options->library_search_path_count, argv[i] + 2)) {
      fprintf(stderr, "Error: Failed to add library path '%s'\n", argv[i] + 2);
      return DRIVER_FLAG_FAILED;
    }
  } else if ((strcmp(argv[i], "--library-path") == 0 ||
              strcmp(argv[i], "-L") == 0) &&
             i + 1 < argc) {
    if (!add_string_option(&options->library_search_paths,
                           &options->library_search_path_count, argv[++i])) {
      fprintf(stderr, "Error: Failed to add library path '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--rpath") == 0 && i + 1 < argc) {
    if (!add_string_option(&options->runpaths, &options->runpath_count,
                           argv[++i])) {
      fprintf(stderr, "Error: Failed to add rpath '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--shared") == 0) {
    options->shared_output = 1;
  } else if (strcmp(argv[i], "--export-dynamic") == 0 ||
             strcmp(argv[i], "-rdynamic") == 0) {
    options->export_dynamic = 1;
  } else if (strcmp(argv[i], "--soname") == 0 && i + 1 < argc) {
    options->soname = argv[++i];
  } else if (strcmp(argv[i], "--dynamic-linker") == 0 && i + 1 < argc) {
    options->dynamic_linker = argv[++i];
  } else if (strcmp(argv[i], "--soname") == 0 ||
             strcmp(argv[i], "--dynamic-linker") == 0 ||
             strcmp(argv[i], "--rpath") == 0 ||
             strcmp(argv[i], "--library") == 0 ||
             strcmp(argv[i], "--library-path") == 0) {
    fprintf(stderr, "Error: Missing value after '%s'\n", argv[i]);
    return DRIVER_FLAG_FAILED;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_output(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
    options->input_filename = argv[++i];
  } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
    options->output_filename = argv[++i];
    flags->output_filename_explicit = 1;
  } else if (strcmp(argv[i], "-I") == 0) {
    if (i + 1 >= argc) {
      fprintf(stderr, "Error: Missing import directory after '-I'\n");
      return DRIVER_FLAG_FAILED;
    }
    if (!add_import_directory(options, argv[++i])) {
      fprintf(stderr, "Error: Failed to add import directory\n");
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
    if (!add_import_directory(options, argv[i] + 2)) {
      fprintf(stderr, "Error: Failed to add import directory\n");
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--stdlib") == 0 && i + 1 < argc) {
    options->stdlib_directory = argv[++i];
  } else if (strcmp(argv[i], "--build") == 0) {
    flags->build_executable = 1;
  } else if (strcmp(argv[i], "--emit-asm") == 0) {
    fprintf(stderr,
            "Error: --emit-asm has been removed; Mettle only emits native "
            "objects now.\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--emit-obj") == 0) {
    options->emit_object = 1;
  } else if (strcmp(argv[i], "--linker") == 0 && i + 1 < argc) {
    flags->linker_mode_explicit = 1;
    if (!parse_linker_mode(argv[++i], &options->linker_mode)) {
      fprintf(stderr,
              "Error: Unknown linker mode '%s' (expected auto, internal, gcc, or msvc)\n",
              argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--linker") == 0) {
    fprintf(stderr, "Error: Missing linker mode after '--linker'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--subsystem") == 0 && i + 1 < argc) {
    const char *name = argv[++i];
    if (strcmp(name, "windows") == 0 || strcmp(name, "gui") == 0) {
      options->windows_subsystem = 1;
    } else if (strcmp(name, "console") == 0) {
      options->windows_subsystem = 0;
    } else {
      fprintf(stderr,
              "Error: Unknown subsystem '%s' (expected console or windows)\n",
              name);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--subsystem") == 0) {
    fprintf(stderr, "Error: Missing subsystem after '--subsystem'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--link-arg") == 0 && i + 1 < argc) {
    if (!add_link_argument(options, argv[++i])) {
      fprintf(stderr, "Error: Failed to add linker argument\n");
      return DRIVER_FLAG_FAILED;
    }
  } else {
    DriverFlagResult shared =
        parse_flag_shared_library(options, argc, argv, &i);
    if (shared == DRIVER_FLAG_UNMATCHED) {
      return DRIVER_FLAG_UNMATCHED;
    }
    if (shared == DRIVER_FLAG_FAILED) {
      return DRIVER_FLAG_FAILED;
    }
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_diagnostics(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
    options->debug_mode = 1;
    options->generate_debug_symbols = 1;
    options->generate_line_mapping = 1;
    options->generate_stack_trace_support = 1;
  } else if (strcmp(argv[i], "--dump-ast") == 0) {
    options->dump_ast = 1;
  } else if (strcmp(argv[i], "--dump-ir") == 0) {
    options->dump_ir = 1;
  } else if (strcmp(argv[i], "--ml-opt") == 0) {
    options->ml_opt = 1;
    options->optimize = 1;
  } else if (strcmp(argv[i], "--ml-opt-speculative") == 0) {
    /* Unlocks the model's unproven actions (dead-code DELETE). They exist
     * only on the validator's word, so this implies --ml-opt; ml_gnn reads
     * the env to emit the speculative dispositions. */
    options->ml_opt = 1;
    options->optimize = 1;
    putenv("METTLE_ML_SPECULATIVE=1");
  } else if (strncmp(argv[i], "--error-format=", 15) == 0) {
    const char *fmt = argv[i] + 15;
    if (strcmp(fmt, "json") == 0) {
      error_reporter_set_format_json(1);
    } else if (strcmp(fmt, "human") == 0) {
      error_reporter_set_format_json(0);
    } else {
      fprintf(stderr,
              "Error: Unknown error format '%s' (expected human or json)\n",
              fmt);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "--filter=", 9) == 0) {
    options->test_filter = argv[i] + 9;
  } else if (strcmp(argv[i], "--pgo") == 0) {
    options->pgo = 1;
    options->optimize = 1;
  } else if (strcmp(argv[i], "--verify") == 0) {
    ir_verify_set_enabled(1);
    options->optimize = 1;
  } else if (strcmp(argv[i], "--simd-report") == 0) {
    options->simd_report = 1;
  } else if (strcmp(argv[i], "--explain") == 0) {
    options->explain = 1;
  } else if (strncmp(argv[i], "--explain=", 10) == 0) {
    /* A whole program's report runs to hundreds of lines. The selector cuts
     * the prose down to the slice asked for; the JSON sidecar stays whole. */
    options->explain = 1;
    options->explain_filter = argv[i] + 10;
  } else if (strcmp(argv[i], "--explain-all") == 0) {
    /* Whole-program report: no focus-file filter, so imported modules'
     * loops and calls are analyzed too (stdlib included). */
    options->explain = 1;
    options->explain_all = 1;
  } else if (strcmp(argv[i], "--explain-json") == 0) {
    /* Machine-readable sidecar (<output-stem>.explain.json) alongside the
     * prose report; implies --explain. */
    options->explain = 1;
    options->explain_json = 1;
  } else if (strcmp(argv[i], "--annotate-asm") == 0) {
    /* Codegen provenance listing + <stem>.annot.json sidecar. Needs the
     * optimizer's decisions (and remarks) to be interesting, so it implies
     * -O and collects --explain remarks (retained past optimization for the
     * codegen join). The default syntax is both Intel and AT&T (toggle). */
    options->annotate_asm = 1;
    /* Reflect the codegen users actually ship: --release enables every
     * vectorizer/idiom, so the annotation matches release output (otherwise a
     * loop shown "not vectorized" at -O would mislead). */
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
    options->asm_syntax = 2; /* both */
  } else if (strncmp(argv[i], "--annotate-lines=", 17) == 0) {
    /* Focused codegen report for a source line range (LLM-facing): asm + cost
     * + covering loops + live registers + decisions for just those lines.
     * Accepts "A" (single line) or "A-B". Implies --annotate-asm. */
    const char *v = argv[i] + 17;
    int a = 0, b = 0;
    if (sscanf(v, "%d-%d", &a, &b) == 2) {
      /* range */
    } else if (sscanf(v, "%d", &a) == 1) {
      b = a;
    } else {
      fprintf(stderr, "Error: --annotate-lines expects A or A-B (got '%s')\n", v);
      return DRIVER_FLAG_FAILED;
    }
    if (a <= 0 || b < a) {
      fprintf(stderr, "Error: --annotate-lines range invalid: %s\n", v);
      return DRIVER_FLAG_FAILED;
    }
    options->annotate_q_lo = a;
    options->annotate_q_hi = b;
    options->annotate_asm = 1;
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
    if (!options->asm_syntax) options->asm_syntax = 0; /* intel-only is terser */
  } else if (strncmp(argv[i], "--annotate-fn=", 14) == 0) {
    options->annotate_q_fn = argv[i] + 14;
    options->annotate_asm = 1;
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
  } else if (strcmp(argv[i], "--annotate-hot") == 0 ||
             strncmp(argv[i], "--annotate-hot=", 15) == 0) {
    /* Top-N hotspots across the program (LLM-facing "where is the time"). */
    int n = 8;
    if (argv[i][14] == '=') n = atoi(argv[i] + 15);
    if (n <= 0) n = 8;
    options->annotate_hot = n;
    options->annotate_asm = 1;
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
  } else if (strncmp(argv[i], "--asm-syntax=", 13) == 0) {
    const char *v = argv[i] + 13;
    if (strcmp(v, "intel") == 0) {
      options->asm_syntax = 0;
    } else if (strcmp(v, "att") == 0) {
      options->asm_syntax = 1;
    } else if (strcmp(v, "both") == 0) {
      options->asm_syntax = 2;
    } else {
      fprintf(stderr,
              "Error: --asm-syntax must be intel, att, or both (got '%s')\n",
              v);
      return DRIVER_FLAG_FAILED;
    }
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_gpu(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "--emit-ptx") == 0) {
    options->emit_ptx = 1;
  } else if (strncmp(argv[i], "--emit-kernel-decls", 19) == 0) {
    /* Bare, the declarations land next to the PTX as <output>.mettle. */
    options->emit_kernel_decls =
        argv[i][19] == '=' ? argv[i] + 20 : "";
    if (argv[i][19] != '\0' && argv[i][19] != '=') {
      fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "--gpu-arch=", 11) == 0) {
    const char *arch = argv[i] + 11;
    flags->gpu_arch_explicit = 1;
    if (strcmp(arch, "gb10") == 0) {
      /* GB10's compatible sm_121 profile excludes its architecture-specific
       * FP4/block-scaled MMA forms. The named performance target must retain
       * the `a` suffix; callers needing compatible PTX can request sm_121. */
      options->ptx_target = "sm_121a";
      if (!flags->ptx_version_explicit) {
        options->ptx_isa_major = 8;
        options->ptx_isa_minor = 8;
      }
    } else if (strcmp(arch, "native") == 0) {
      /* The default already prefers the local GPU. Asking for it by name
       * says the build is meant for this machine, so a missing driver is an
       * error here rather than a silent fall back to the GB10 default. */
      if (!gpu_detect_ptx_target(0, flags->detected_ptx_target,
                                 sizeof(flags->detected_ptx_target))) {
        fprintf(stderr,
                "Error: --gpu-arch=native found no local NVIDIA device (%s); "
                "name a target with --gpu-arch=sm_NN to cross-compile\n",
                gpu_detect_local()->source);
        return DRIVER_FLAG_FAILED;
      }
      options->ptx_target = flags->detected_ptx_target;
    } else if (strcmp(arch, "portable") == 0) {
      /* Virtual Turing ISA is the oldest forward-compatible baseline still
       * supported for offline assembly by current CUDA 13 toolchains. */
      options->ptx_target = "compute_75";
      if (!flags->ptx_version_explicit) {
        options->ptx_isa_major = 6;
        options->ptx_isa_minor = 4;
      }
    } else if (strncmp(arch, "sm_", 3) == 0 ||
               strncmp(arch, "compute_", 8) == 0) {
      options->ptx_target = arch;
    } else {
      fprintf(stderr,
              "Error: --gpu-arch expects gb10, portable, sm_NN, or "
              "compute_NN (got '%s')\n",
              arch);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "--ptx-version=", 14) == 0) {
    const char *version = argv[i] + 14;
    int major = 0, minor = 0;
    char trailing = '\0';
    if (sscanf(version, "%d.%d%c", &major, &minor, &trailing) != 2 ||
        major < 1 || major > 99 || minor < 0 || minor > 9) {
      fprintf(stderr,
              "Error: --ptx-version expects MAJOR.MINOR (got '%s')\n",
              version);
      return DRIVER_FLAG_FAILED;
    }
    options->ptx_isa_major = major;
    options->ptx_isa_minor = minor;
    flags->ptx_version_explicit = 1;
  } else if (strncmp(argv[i], "--gpu-tensor-tuple-budget=", 26) == 0) {
    const char *value = argv[i] + 26;
    int budget = 0;
    char trailing = '\0';
    if (sscanf(value, "%d%c", &budget, &trailing) != 1 || budget < 0 ||
        budget > 4096) {
      fprintf(stderr,
              "Error: --gpu-tensor-tuple-budget expects 0..4096 (got '%s')\n",
              value);
      return DRIVER_FLAG_FAILED;
    }
    options->ptx_tensor_tuple_budget = budget;
  } else if (strcmp(argv[i], "--report-occupancy") == 0) {
    options->report_occupancy = 1;
  } else if (strcmp(argv[i], "--gpu-checks") == 0) {
    options->gpu_checks = 1;
  } else if (strcmp(argv[i], "--report-launches") == 0) {
    options->report_launches = 1;
  } else if (strcmp(argv[i], "--old") == 0 && i + 1 < argc) {
    options->swap_old_name = argv[++i];
  } else if (strcmp(argv[i], "--new") == 0 && i + 1 < argc) {
    options->swap_new_name = argv[++i];
  } else if (strcmp(argv[i], "--report-expansion") == 0) {
    options->report_expansion = 1;
  } else if (strncmp(argv[i], "--expansion-budget=", 19) == 0) {
    long long budget = atoll(argv[i] + 19);
    if (budget < 0) {
      fprintf(stderr, "--expansion-budget must not be negative\n");
      return DRIVER_FLAG_FAILED;
    }
    /* 0 is a real budget (expand nothing), so remember that one was asked
       for rather than inferring it from the number. */
    options->expansion_budget = (size_t)budget;
    options->expansion_budget_set = 1;
  } else if (strncmp(argv[i], "--sms=", 6) == 0) {
    int sms = atoi(argv[i] + 6);
    if (sms < 1 || sms > 1024) {
      fprintf(stderr, "Error: --sms expects an SM count from 1 to 1024 "
                      "(got '%s')\n",
              argv[i] + 6);
      return DRIVER_FLAG_FAILED;
    }
    options->report_sms = sms;
  } else if (strcmp(argv[i], "--emit-spirv") == 0) {
    options->emit_spirv = 1;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_codegen(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "--emit-arm64") == 0) {
    options->emit_arm64 = 1;
  } else if (strcmp(argv[i], "--emit-arm64-obj") == 0) {
    options->emit_arm64_obj = 1;
  } else if (strcmp(argv[i], "-g") == 0 ||
             strcmp(argv[i], "--debug-symbols") == 0) {
    options->generate_debug_symbols = 1;
  } else if (strcmp(argv[i], "-l") == 0 ||
             strcmp(argv[i], "--line-mapping") == 0) {
    options->generate_line_mapping = 1;
  } else if (strcmp(argv[i], "-s") == 0 ||
             strcmp(argv[i], "--stack-trace") == 0) {
    options->generate_stack_trace_support = 1;
  } else if (strcmp(argv[i], "--no-crash-report") == 0) {
    options->generate_crash_report = 0;
  } else if (strcmp(argv[i], "--debug-format") == 0 && i + 1 < argc) {
    options->debug_format = argv[++i];
  } else if (strcmp(argv[i], "-O") == 0 ||
             strcmp(argv[i], "--optimize") == 0) {
    options->optimize = 1;
  } else if (strcmp(argv[i], "-r") == 0 ||
             strcmp(argv[i], "--release") == 0) {
    options->release = 1;
    options->optimize = 1;
  } else if (strcmp(argv[i], "--strip-comments") == 0) {
    fprintf(stderr,
            "Error: --strip-comments has been removed; Mettle no longer "
            "emits text assembly.\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--prelude") == 0) {
    options->prelude = 1;
  } else if (strcmp(argv[i], "--profile") == 0) {
    options->profile = 1;
  } else if (strcmp(argv[i], "--profile-runtime") == 0) {
    options->profile_runtime = 1;
  } else if (strcmp(argv[i], "--profile-runtime-ops") == 0) {
    options->profile_runtime_ops = 1;
  } else if (strcmp(argv[i], "--profile-blocks") == 0) {
    options->profile_blocks = 1;
    options->profile_runtime = 1;
  } else if (strcmp(argv[i], "--debug-hooks") == 0) {
    options->debug_hooks = 1;
  } else if (strcmp(argv[i], "--safe") == 0) {
    options->safe = 1;
  } else if (strcmp(argv[i], "--native-heap") == 0) {
    options->native_heap = 1;
  } else if (strcmp(argv[i], "--static") == 0) {
    options->static_link = 1;
  } else if (strcmp(argv[i], "--musl") == 0) {
    options->musl_link = 1;
    options->static_link = 1;
  } else if (strcmp(argv[i], "--tracy") == 0) {
    options->tracy = 1;
  } else if (strcmp(argv[i], "--tracy-dir") == 0 && i + 1 < argc) {
    options->tracy_directory = argv[++i];
  } else if (strcmp(argv[i], "--tracy-dir") == 0) {
    fprintf(stderr, "Error: Missing path after '--tracy-dir'\n");
    return DRIVER_FLAG_FAILED;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_target(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
    char target_error[256];
    options->target_triple = argv[++i];
    if (!mtlc_target_select(options->target_triple, target_error,
                            sizeof(target_error))) {
      fprintf(stderr, "Error: %s\n", target_error);
      return DRIVER_FLAG_FAILED;
    }
    if (mtlc_target()->arch == MTLC_TARGET_ARCH_AARCH64) {
      options->emit_arm64_obj = 1;
    }
  } else if (strcmp(argv[i], "--target") == 0) {
    fprintf(stderr, "Error: Missing triple after '--target'; known targets "
                    "are %s\n",
            mtlc_target_triple_list());
    return DRIVER_FLAG_FAILED;
  } else if (strncmp(argv[i], "--target=", 9) == 0) {
    char target_error[256];
    options->target_triple = argv[i] + 9;
    if (!mtlc_target_select(options->target_triple, target_error,
                            sizeof(target_error))) {
      fprintf(stderr, "Error: %s\n", target_error);
      return DRIVER_FLAG_FAILED;
    }
    if (mtlc_target()->arch == MTLC_TARGET_ARCH_AARCH64) {
      options->emit_arm64_obj = 1;
    }
  } else if (strcmp(argv[i], "--image-base") == 0 && i + 1 < argc) {
    char *end = NULL;
    unsigned long long base = strtoull(argv[++i], &end, 0);
    if (!end || *end != '\0') {
      fprintf(stderr, "Error: '--image-base' takes an address, e.g. 0x7c00\n");
      return DRIVER_FLAG_FAILED;
    }
    options->image_base = base;
    options->image_base_set = 1;
    mtlc_target_set_image_base(base);
  } else if (strcmp(argv[i], "--image-base") == 0) {
    fprintf(stderr, "Error: Missing address after '--image-base'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--emit-flat") == 0 && i + 1 < argc) {
    options->flat_output = argv[++i];
    options->emit_object = 1;
  } else if (strcmp(argv[i], "--emit-flat") == 0) {
    fprintf(stderr, "Error: Missing output path after '--emit-flat'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--debug-compiler") == 0) {
    options->debug_compiler = 1;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

typedef DriverFlagResult (*DriverFlagParser)(CompilerOptions *, DriverFlags *,
                                             int, char **, int *);

static const DriverFlagParser DRIVER_FLAG_PARSERS[] = {
    parse_flag_output, parse_flag_diagnostics, parse_flag_gpu,
    parse_flag_codegen, parse_flag_target};

static int parse_arguments(CompilerOptions *options, DriverFlags *flags,
                           int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    DriverFlagResult taken = DRIVER_FLAG_UNMATCHED;
    size_t group;
    for (group = 0;
         group < sizeof(DRIVER_FLAG_PARSERS) / sizeof(DRIVER_FLAG_PARSERS[0]);
         group++) {
      taken = DRIVER_FLAG_PARSERS[group](options, flags, argc, argv, &i);
      if (taken != DRIVER_FLAG_UNMATCHED) {
        break;
      }
    }
    if (taken == DRIVER_FLAG_FAILED) {
      return 1;
    }
    if (taken == DRIVER_FLAG_TAKEN) {
      continue;
    }
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    }
    if (!options->input_filename) {
      options->input_filename = argv[i];
      continue;
    }
    fprintf(stderr, "Error: Unknown or misplaced argument '%s'\n", argv[i]);
    print_usage(argv[0]);
    return 1;
  }
  return -1;
}

static int mettle_check_target_options(CompilerOptions *options,
                                       DriverFlags *flags) {
  /* A flat image IS the linked product: there is nothing left for a linker to
   * do to it, and no container for a linker to put it in. */
  if (options->flat_output && flags->build_executable) {
    fprintf(stderr,
            "Error: --emit-flat writes the linked image itself; drop --build\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  /* No object format here carries 16- or 32-bit relocations, so a narrow
   * target has exactly one product. Saying so beats emitting an object whose
   * code is the wrong width for the header on it. */
  if (!mtlc_target_is_object_capable(mtlc_target()) && !options->flat_output) {
    fprintf(stderr,
            "Error: the %s target emits a flat image only; add --emit-flat "
            "<file> (and --image-base <addr>)\n",
            mtlc_target()->triple);
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  if (flags->build_executable && mtlc_target()->explicit_triple) {
    const MtlcTarget *target = mtlc_target();
    if (target->freestanding) {
      fprintf(stderr,
              "Error: --build links against the host runtime, which the %s "
              "target has none of; emit the image with --emit-flat, or the "
              "object with --emit-obj and link it yourself\n",
              target->triple);
      free((void *)options->import_directories);
      free((void *)options->link_arguments);
      return 1;
    }
    if (target->os != mtlc_target_host_os()) {
      fprintf(stderr,
              "Error: --build for %s has to run that machine's linker against "
              "that machine's runtime, and neither is here; emit the object "
              "with --emit-obj and link it on a %s host\n",
              target->triple, mtlc_target_os_name(target->os));
      free((void *)options->import_directories);
      free((void *)options->link_arguments);
      return 1;
    }
  }

  if (mtlc_target()->image_base_set && !options->flat_output) {
    if (!flags->build_executable) {
      fprintf(stderr,
              "Error: --image-base says where a linked image loads, and this "
              "compile produces a relocatable object; add --build, or "
              "--emit-flat <file> for a raw image\n");
      free((void *)options->import_directories);
      free((void *)options->link_arguments);
      return 1;
    }
    {
      uint64_t base = mtlc_target()->image_base;
      uint64_t alignment =
          mtlc_target()->format == BINARY_TARGET_FORMAT_COFF_WIN64 ? 0x10000u
                                                                   : 0x1000u;
      if (base % alignment) {
        fprintf(stderr,
                "Error: --image-base 0x%llx is not aligned to 0x%llx, which is "
                "the boundary a %s image loads on\n",
                (unsigned long long)base, (unsigned long long)alignment,
                mtlc_target()->format == BINARY_TARGET_FORMAT_COFF_WIN64
                    ? "PE"
                    : "ELF");
        free((void *)options->import_directories);
        free((void *)options->link_arguments);
        return 1;
      }
    }
  }

  if (options->safe && mtlc_target()->freestanding) {
    fprintf(stderr,
            "Error: --safe on the %s target has no runtime to report a "
            "violation to; its checks call into the shadow map the runtime "
            "owns, and a freestanding image links no library\n",
            mtlc_target()->triple);
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  /* No --gpu-arch given: target the GPU that is actually in this machine when
   * one is visible. Detection failure (no driver, headless build host) keeps
   * the GB10 default so cross-compiles for DGX Spark are unchanged. */
  if (options->emit_ptx && !flags->gpu_arch_explicit &&
      detect_host_gpu_ptx_target(flags->detected_ptx_target,
                                 sizeof(flags->detected_ptx_target))) {
    options->ptx_target = flags->detected_ptx_target;
  }

  /* A `.version` above what the local driver understands fails inside
   * cuModuleLoadData at run time, where the only evidence is a status code.
   * When the target came from this machine, take the ISA from it too. */
  if (options->emit_ptx && !flags->ptx_version_explicit) {
    int driver_major = 0, driver_minor = 0;
    if (gpu_detect_ptx_isa(&driver_major, &driver_minor) &&
        (driver_major < options->ptx_isa_major ||
         (driver_major == options->ptx_isa_major &&
          driver_minor < options->ptx_isa_minor))) {
      options->ptx_isa_major = driver_major;
      options->ptx_isa_minor = driver_minor;
    }
  }

  if (options->tracy && !flags->build_executable) {
    fprintf(stderr, "Error: --tracy requires --build\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  if (options->emit_arm64_obj && options->emit_arm64) {
    fprintf(stderr,
            "Error: --emit-arm64 (self-contained AArch64 executable) and "
            "--emit-arm64-obj (AArch64 relocatable object) are different "
            "outputs; pick one\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

#if !defined(__aarch64__) && !defined(_M_ARM64)
  if (options->emit_arm64_obj && flags->build_executable) {
    fprintf(stderr,
            "Error: --emit-arm64-obj cannot be combined with --build on an "
            "x86-64 host: the object is AArch64 and this host's linker cannot "
            "link it. Link it on an ARM machine, or use --emit-arm64 for a "
            "self-contained executable\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }
#endif

  return 0;
}

int main(int argc, char *argv[]) {
  CompilerOptions options = {0};
  mettle_compiler_crash_install(argc, argv);
  mettle_compiler_self_profile_start();
  char *auto_stdlib_directory = NULL;
  char *auto_runtime_directory = NULL;
  char *build_output_filename = NULL;
  char *object_output_filename = NULL;
  DriverFlags flags = {0};
  options.emit_object = 1;
  options.generate_crash_report = 1;
  options.output_filename = default_object_output_filename();
  options.debug_format = "dwarf";
  options.ptx_target = "sm_121a";
  options.ptx_isa_major = 8;
  options.ptx_isa_minor = 8;

  if (argc >= 2) {
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0 ||
        strcmp(argv[1], "version") == 0) {
#if defined(__aarch64__) && defined(__linux__)
      const char *host = "aarch64";
      const char *target = "aarch64-linux (ELF relocatable object)";
#else
      const char *host = "x86_64";
      const char *target =
          host_target_is_elf() ? "x86_64-linux (ELF)" : "x86_64-windows (COFF)";
#endif
      printf("mettle %s\n", METTLE_VERSION);
      printf("host: %s\n", host);
      printf("target: %s\n", target);
      return 0;
    }
    if (strcmp(argv[1], "--gpu-info") == 0 || strcmp(argv[1], "gpu") == 0) {
      return report_gpu_info(options.ptx_target, options.ptx_isa_major,
                             options.ptx_isa_minor);
    }
    if (strcmp(argv[1], "help") == 0) {
      return print_help_topic(argv[0], argv[0], argc >= 3 ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "explain") == 0) {
      return mettle_explain_error_code(argc >= 3 ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "expand") == 0) {
      /* `mettle expand <file> [flags...]`: shift the subcommand out and let
         the normal flag loop see the rest, the same way `test` does. */
      options.expand_mode = 1;
      for (int i = 1; i + 1 < argc; i++) {
        argv[i] = argv[i + 1];
      }
      argc--;
      if (argc < 2) {
        fprintf(stderr, "usage: mettle expand <file.mettle>\n");
        return 1;
      }
    }
    if (strcmp(argv[1], "swap-check") == 0) {
      options.swap_check_mode = 1;
      for (int i = 1; i + 1 < argc; i++) {
        argv[i] = argv[i + 1];
      }
      argc--;
      if (argc < 2) {
        fprintf(stderr,
                "usage: mettle swap-check <file.mettle> --old <fn> --new <fn>\n");
        return 1;
      }
    }
    if (strcmp(argv[1], "test") == 0) {
      /* `mettle test <file> [--filter=S] [flags...]`: shift the subcommand
       * out and let the normal flag loop see the rest. */
      options.test_mode = 1;
      for (int i = 1; i + 1 < argc; i++) {
        argv[i] = argv[i + 1];
      }
      argc--;
      if (argc < 2) {
        fprintf(stderr, "usage: mettle test <file.mettle> [--filter=SUBSTR]\n");
        return 1;
      }
    } else if (strcmp(argv[1], "trace") == 0) {
      /* `mettle trace <file> <fn> [args...]` */
      if (argc < 4) {
        fprintf(stderr,
                "usage: mettle trace <file.mettle> <function> [args...]\n"
                "  int/float parameters take the CLI values in order; pointer\n"
                "  parameters get a synthesized buffer\n");
        return 1;
      }
      options.trace_function = argv[3];
      options.trace_args = (const char *const *)&argv[4];
      options.trace_arg_count = (size_t)(argc - 4);
      argv[1] = argv[2]; /* the input file */
      argc = 2;
    }
    if (strcmp(argv[1], "docs") == 0) {
      if (argc >= 3) {
        return print_help_topic(argv[0], argv[0], argv[2]);
      }
      printf("Mettle documentation topics: build, runtime (alias: heap, gc), interop, stdlib, web\n");
      print_doc_reference(argv[0], "LANGUAGE.md");
      print_doc_reference(argv[0], "compilation.md");
      print_doc_reference(argv[0], "runtime-model.md");
      print_doc_reference(argv[0], "heap-allocation.md");
      return 0;
    }
  }

  {
    int early = parse_arguments(&options, &flags, argc, argv);
    if (early >= 0) {
      return early;
    }
  }

  if (!options.input_filename) {
    fprintf(stderr, "Error: No input file specified.\n");
    print_usage(argv[0]);
    free((void *)options.import_directories);
    free((void *)options.link_arguments);
    return 1;
  }

  {
    int target_status = mettle_check_target_options(&options, &flags);
    if (target_status != 0) {
      return target_status;
    }
  }
  if (flags.build_executable) {
    options.emit_object = 1;
    if (!flags.linker_mode_explicit) {
      options.linker_mode = LINKER_MODE_INTERNAL;
    }
  }

  if (!options.stdlib_directory) {
    auto_stdlib_directory = infer_default_stdlib_directory(argv[0]);
    if (auto_stdlib_directory) {
      options.stdlib_directory = auto_stdlib_directory;
    }
  }

  auto_runtime_directory = infer_default_runtime_directory(argv[0]);

  if (options.ml_opt) {
    ml_opt_set_default_paths(argv[0]);
  }

  /* The native ELF backend supports --build on Linux via an ld-based link of
   * the emitted ELF object plus a self-contained _start. On Linux --build
   * always uses the direct-object backend (no asm/NASM path). */
  BinaryTargetFormat host_format = mtlc_target()->format;
  int elf_build = host_format == BINARY_TARGET_FORMAT_ELF_X64 ||
                  host_format == BINARY_TARGET_FORMAT_ELF_ARM64;

  if (flags.build_executable) {
    if (!elf_build && (options.shared_library_count > 0u ||
                       options.shared_output || options.export_dynamic ||
                       options.runpath_count > 0u || options.soname ||
                       options.dynamic_linker)) {
      fprintf(stderr,
              "Error: -l, -L, --shared, --soname, --rpath, --export-dynamic "
              "and --dynamic-linker are ELF options; a PE build takes its "
              "libraries through --link-arg\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free((void *)options.shared_libraries);
      free((void *)options.library_search_paths);
      free((void *)options.runpaths);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    if (options.static_link && mettle_elf_dynamic_link_requested(&options)) {
      fprintf(stderr,
              "Error: --static and shared libraries ask for opposite images; "
              "drop one\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free((void *)options.shared_libraries);
      free((void *)options.library_search_paths);
      free((void *)options.runpaths);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    if (options.musl_link) {
      fprintf(stderr,
              "Error: --musl is not available in owned runtime mode because "
              "Mettle does not link a C library\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    for (size_t i = 0; i < options.link_argument_count; i++) {
      if (mettle_link_argument_uses_forbidden_runtime(
              options.link_arguments[i])) {
        fprintf(stderr,
                "Error: --link-arg '%s' names a forbidden C or compiler "
                "runtime\n",
                options.link_arguments[i]);
        free((void *)options.import_directories);
        free((void *)options.link_arguments);
        free(auto_stdlib_directory);
        free(auto_runtime_directory);
        return 1;
      }
    }
#ifndef _WIN32
    if (!elf_build) {
      fprintf(stderr,
              "Error: --build is supported on Windows and Linux (ELF) only\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    /* Linux: force direct-object emission; there is no NASM/asm link path. */
    options.emit_object = 1;
#else
    if (!auto_runtime_directory) {
      fprintf(stderr,
              "Error: Could not locate bundled runtime directory for --build\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
#endif
    if (flags.output_filename_explicit) {
      build_output_filename = strdup(options.output_filename);
    } else {
      build_output_filename = default_executable_filename(options.input_filename);
    }
    if (!build_output_filename) {
      /* On an ELF host the product takes the source's stem, so a source with
       * no extension leaves nowhere to put it that is not the source. */
      fprintf(stderr,
              "Error: Could not choose an output name for '%s'. Pass -o "
              "<name>\n",
              options.input_filename ? options.input_filename : "");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }

    /* ELF objects conventionally use .o; COFF uses .obj. */
    object_output_filename = replace_extension(
        build_output_filename, elf_build ? ".o" : ".obj");
    if (!object_output_filename) {
      fprintf(stderr, "Error: Failed to determine object output path\n");
      free(build_output_filename);
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    options.output_filename = object_output_filename;
  }

  options.building_executable = flags.build_executable;

  double command_profile_start =
      options.profile ? compiler_profile_now_ms() : 0.0;
  int result =
      compile_file(options.input_filename, options.output_filename, &options);
  if (result == 0 && flags.build_executable) {
    double build_profile_start =
        options.profile ? compiler_profile_now_ms() : 0.0;
#ifndef _WIN32
    /* Linux: emit the ELF object (done by compile_file above) then link it
     * with our self-contained _start via ld. */
    result = mettle_link_elf_executable(options.output_filename,
                                        build_output_filename, &options,
                                        auto_runtime_directory);
#else
    result = mettle_link_object_file(options.output_filename,
                                     build_output_filename,
                                     auto_runtime_directory, &options);
#endif
    if (result == 0 && !g_link_output_ownership_verified) {
      char ownership_error[256];
      int owned =
          mettle_elf_dynamic_link_requested(&options)
              ? mettle_verify_owned_dynamic_executable(build_output_filename,
                                                       ownership_error,
                                                       sizeof(ownership_error))
              : mettle_verify_owned_executable(build_output_filename,
                                               ownership_error,
                                               sizeof(ownership_error));
      if (!owned) {
        fprintf(stderr,
                "Error: Refusing linked output '%s': %s\n",
                build_output_filename, ownership_error);
        remove(build_output_filename);
        result = 1;
      }
    }
    if (result == 0) {
      printf("Built executable '%s'\n", build_output_filename);
    }
    if (options.profile) {
      fprintf(stderr, "Executable build profile%s:\n",
              result == 0 ? "" : " (failed)");
      fprintf(stderr, "  %-20s %9.3f ms\n", "assemble/link",
              compiler_profile_now_ms() - build_profile_start);
    }
  } else if (result == 0 && auto_runtime_directory && options.debug_mode &&
             !options.dump_ir) {
    fprintf(stderr,
            "Note: transitional runtime objects detected at '%s'. Use --build "
            "to assemble and link them automatically when needed (most "
            "programs link nothing from this directory).\n",
            auto_runtime_directory);
  }
  if (options.profile) {
    fprintf(stderr, "Command profile%s:\n", result == 0 ? "" : " (failed)");
    fprintf(stderr, "  %-20s %9.3f ms\n", "total",
            compiler_profile_now_ms() - command_profile_start);
  }
  free((void *)options.import_directories);
  free((void *)options.link_arguments);
  free((void *)options.shared_libraries);
  free((void *)options.library_search_paths);
  free((void *)options.runpaths);
  free(auto_stdlib_directory);
  free(auto_runtime_directory);
  free(build_output_filename);
  free(object_output_filename);
  if (getenv("METTLE_FULL_CLEANUP")) {
    string_intern_clear();
  }
  mettle_compiler_self_profile_report();
  return result;
}

static int compile_read_source(const char *filename, char **out_source) {
  *out_source = read_file(filename);
  if (!*out_source) {
    fprintf(stderr, "Error: Could not read file '%s'\n", filename);
    return 0;
  }
  return 1;
}

static int compile_lex_and_parse(Parser *parser, ErrorReporter *error_reporter,
                                 ASTNode **out_program) {
  *out_program = parser_parse_program(parser);
  if (!*out_program || parser->had_error ||
      error_reporter_has_errors(error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Parse error: %s\n",
              parser->error_message ? parser->error_message : "Unknown error");
    }
    return 0;
  }
  return 1;
}

static int compile_resolve_imports(ASTNode *program, const char *input_filename,
                                   ErrorReporter *error_reporter,
                                   ImportResolverOptions *import_options) {
  if (!resolve_imports_with_options(program, input_filename, error_reporter,
                                    import_options)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Import resolution error\n");
    }
    return 0;
  }
  return 1;
}

static int compile_monomorphize(ASTNode *program,
                                ErrorReporter *error_reporter) {
  if (!monomorphize_program(program, error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Generic monomorphization error\n");
    }
    return 0;
  }
  if (!closure_convert_program(program, error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Closure conversion error\n");
    }
    return 0;
  }
  if (!closure_adapt_program(program, error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Closure adaptation error\n");
    }
    return 0;
  }
  return 1;
}

/* `mettle expand`'s bridge to the expansion table: the printer asks what
 * generated a block, and this answers with the same note a diagnostic raised
 * inside that block would carry, so the two always agree. */
static const char *expand_annotate(void *context, const ASTNode *block) {
  return type_checker_expansion_note((TypeChecker *)context, block, NULL);
}

static int compile_type_check(TypeChecker *type_checker, ASTNode *program,
                              ErrorReporter *error_reporter) {
  if (!type_checker_check_program(type_checker, program)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Type error: %s\n",
              type_checker->error_message ? type_checker->error_message
                                          : "Unknown error");
    }
    return 0;
  }
  return 1;
}

/* `mettle swap-check <file> --old F --new G`
 *
 * A hot swap asks whether the new function is compatible with the old one at
 * this boundary. Translation validation already answers a question of exactly
 * that shape, so this points the same machinery at two functions instead of
 * at one function before and after a pass: it runs both on generated inputs
 * and compares every observable, reporting a counterexample on divergence.
 *
 * It runs on lowered IR before optimization, so the verdict is about what the
 * two functions mean, not about what any pass did to them. */
static IRFunction *swap_find_function(IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->name && strcmp(fn->name, name) == 0) {
      return fn;
    }
  }
  return NULL;
}

/* A swap replaces a function at a call boundary, so the boundary has to be the
 * same one. The gate runs the old body under the new function's signature, so
 * a mismatch here would not merely be an unswappable change, it would make the
 * comparison meaningless. */
static int swap_signatures_match(const IRFunction *old_fn,
                                 const IRFunction *new_fn, char *why,
                                 size_t why_capacity) {
  if (old_fn->parameter_count != new_fn->parameter_count) {
    snprintf(why, why_capacity,
             "parameter count differs: '%s' takes %zu, '%s' takes %zu",
             old_fn->name, old_fn->parameter_count, new_fn->name,
             new_fn->parameter_count);
    return 0;
  }
  for (size_t i = 0; i < old_fn->parameter_count; i++) {
    const char *a = old_fn->parameter_types ? old_fn->parameter_types[i] : NULL;
    const char *b = new_fn->parameter_types ? new_fn->parameter_types[i] : NULL;
    if ((a == NULL) != (b == NULL) || (a && b && strcmp(a, b) != 0)) {
      snprintf(why, why_capacity,
               "parameter %zu differs: '%s' takes '%s', '%s' takes '%s'",
               i + 1, old_fn->name, a ? a : "?", new_fn->name, b ? b : "?");
      return 0;
    }
  }
  const char *ra = old_fn->return_type_name;
  const char *rb = new_fn->return_type_name;
  if ((ra == NULL) != (rb == NULL) || (ra && rb && strcmp(ra, rb) != 0)) {
    snprintf(why, why_capacity,
             "return type differs: '%s' returns '%s', '%s' returns '%s'",
             old_fn->name, ra ? ra : "?", new_fn->name, rb ? rb : "?");
    return 0;
  }
  return 1;
}

static int compile_run_swap_check(IRProgram *ir_program,
                                  const char *old_name,
                                  const char *new_name) {
  IRFunction *old_fn = swap_find_function(ir_program, old_name);
  IRFunction *new_fn = swap_find_function(ir_program, new_name);
  if (!old_fn) {
    fprintf(stderr, "swap-check: no function named '%s' in this program\n",
            old_name);
    return 1;
  }
  if (!new_fn) {
    fprintf(stderr, "swap-check: no function named '%s' in this program\n",
            new_name);
    return 1;
  }
  if (old_fn == new_fn) {
    fprintf(stderr,
            "swap-check: --old and --new name the same function ('%s')\n",
            old_name);
    return 1;
  }

  char why[512];
  why[0] = '\0';
  if (!swap_signatures_match(old_fn, new_fn, why, sizeof(why))) {
    fprintf(stderr, "swap-check: REFUSED - %s\n", why);
    fprintf(stderr,
            "  A swap has to keep the boundary it replaces. Change the "
            "signature and the callers change with it, which is a rebuild "
            "rather than a swap.\n");
    return 1;
  }

  IRVerifySnapshot *before = ir_verify_snapshot_capture(old_fn);
  if (!before) {
    fprintf(stderr, "swap-check: could not snapshot '%s'\n", old_name);
    return 1;
  }

  char divergence[512];
  char counterexample[512];
  char skip_reason[256];
  IRVerifyRewriteVerdict verdict = ir_verify_check_rewrite(
      ir_program, new_fn, before, divergence, sizeof(divergence),
      counterexample, sizeof(counterexample), skip_reason,
      sizeof(skip_reason));
  ir_verify_snapshot_free(before);

  switch (verdict) {
  case IR_VERIFY_REWRITE_VALIDATED:
    /* Say what was actually checked. The harness runs a fixed set of generated
     * inputs, so agreement across them is evidence and not equivalence, and a
     * verdict that reads as a proof would be the decoration III.2.6 refuses. */
    printf("swap-check: OK - '%s' matched '%s' on %d generated input sets\n",
           new_name, old_name, ir_verify_last_input_run_count());
    printf("  Inputs cover a fixed shape table plus the constants these two "
           "functions compare against, tested on both sides of each.\n");
    printf("  This is a differential test, not a proof: behavior on inputs "
           "outside those sets was not observed.\n");
    return 0;
  case IR_VERIFY_REWRITE_DIVERGED:
    fprintf(stderr, "swap-check: DIVERGED - '%s' does not match '%s'\n",
            new_name, old_name);
    fprintf(stderr, "  %s\n", divergence);
    if (counterexample[0]) {
      fprintf(stderr, "  %s\n", counterexample);
    }
    return 1;
  case IR_VERIFY_REWRITE_UNVERIFIABLE:
  default:
    /* Not a pass: the gate could not run these functions, which is a
     * different answer from "they differ" and must not read as approval. */
    fprintf(stderr,
            "swap-check: UNVERIFIABLE - the gate could not run '%s': %s\n",
            new_name, skip_reason[0] ? skip_reason : "unknown");
    return 2;
  }
}

static int compile_lower_to_ir(ASTNode *program, TypeChecker *type_checker,
                               SymbolTable *symbol_table,
                               int emit_runtime_checks, int emit_safety_checks,
                               IRProgram **out_ir_program,
                               char **out_ir_error) {
  *out_ir_program =
      ir_lower_program(program, type_checker, symbol_table, out_ir_error,
                       emit_runtime_checks, emit_safety_checks);
  if (!*out_ir_program) {
    /* A comptime-only Type/Field that slipped into lowering is a user
     * diagnostic, already on the reporter. Do not wrap it as an ICE. */
    if (type_checker && type_checker->error_reporter &&
        error_reporter_has_errors(type_checker->error_reporter)) {
      error_reporter_print_errors(type_checker->error_reporter);
      return 0;
    }
    mettle_compiler_ice_report("IR lowering failed",
                               *out_ir_error ? *out_ir_error : NULL);
    return 0;
  }
  return 1;
}

#include "ir/ml_opt.h"

/* Collect non-extern, non-exported global integer `var`s whose initializer is
 * an integer literal (optionally negated). The optimizer proves each is never
 * written before folding its reads - this only supplies the candidates. */
static IRGlobalIntConst *collect_global_int_consts(ASTNode *program,
                                                   size_t *out_count) {
  *out_count = 0;
  if (!program || program->type != AST_PROGRAM || !program->data) {
    return NULL;
  }
  Program *prog = (Program *)program->data;
  IRGlobalIntConst *consts = NULL;
  size_t count = 0, capacity = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_VAR_DECLARATION || !decl->data) {
      continue;
    }
    VarDeclaration *vd = (VarDeclaration *)decl->data;
    if (!vd->name || !vd->type_name || vd->is_extern || vd->is_exported ||
        vd->link_name || !vd->initializer) {
      continue;
    }
    if (strcmp(vd->type_name, "int8") != 0 &&
        strcmp(vd->type_name, "int16") != 0 &&
        strcmp(vd->type_name, "int32") != 0 &&
        strcmp(vd->type_name, "int64") != 0 &&
        strcmp(vd->type_name, "uint8") != 0 &&
        strcmp(vd->type_name, "uint16") != 0 &&
        strcmp(vd->type_name, "uint32") != 0 &&
        strcmp(vd->type_name, "uint64") != 0) {
      continue;
    }
    ASTNode *init = vd->initializer;
    long long sign = 1;
    if (init->type == AST_UNARY_EXPRESSION && init->data) {
      UnaryExpression *ue = (UnaryExpression *)init->data;
      if (!ue->operator|| strcmp(ue->operator, "-") != 0 || !ue->operand) {
        continue;
      }
      sign = -1;
      init = ue->operand;
    }
    if (init->type != AST_NUMBER_LITERAL || !init->data) {
      continue;
    }
    NumberLiteral *nl = (NumberLiteral *)init->data;
    if (nl->is_float) {
      continue;
    }
    if (count >= capacity) {
      size_t nc = capacity ? capacity * 2 : 16;
      IRGlobalIntConst *grown =
          (IRGlobalIntConst *)realloc(consts, nc * sizeof(*grown));
      if (!grown) {
        free(consts);
        return NULL;
      }
      consts = grown;
      capacity = nc;
    }
    consts[count].name = vd->name;
    consts[count].value = sign * nl->int_value;
    count++;
  }
  *out_count = count;
  return consts;
}

/* Whether this compile's object output is an AArch64 relocatable object: on an
 * ARM host that is every object; elsewhere it is what --emit-arm64-obj asks
 * for, which is how an x86-64 host reaches (and tests) that backend. */
static int compile_targets_arm64_object(const CompilerOptions *options) {
#if defined(__aarch64__) || defined(_M_ARM64)
  (void)options;
  return 1;
#else
  return options && options->emit_arm64_obj;
#endif
}

static int compile_optimize_ir(IRProgram *ir_program, ASTNode *ast_program,
                               CompilerOptions *options) {
  IROptimizeOptions ir_optimize_options = {0};
  int target_neutral = options->emit_arm64 || options->emit_ptx ||
                       options->emit_spirv ||
                       compile_targets_arm64_object(options);
  if (options->ml_opt && target_neutral) {
    fprintf(stderr,
            "Error: --ml-opt is not target-neutral and cannot be combined "
            "with --emit-arm64, --emit-arm64-obj, --emit-ptx, or "
            "--emit-spirv%s\n",
            compile_targets_arm64_object(options) && !options->emit_arm64_obj
                ? " (this host emits AArch64 objects)"
                : "");
    return 0;
  }
  ir_optimize_options.preserve_function_boundaries =
      options->profile_runtime ? 1 : 0;
  ir_optimize_options.simd_report = options->simd_report;
  ir_optimize_options.explain = options->explain;
  ir_optimize_options.explain_focus_file =
      options->explain_all ? NULL : options->input_filename;
  /* Large --explain reports divert to `<output-stem>.explain.txt`. */
  ir_explain_set_output_path(options->output_filename);
  ir_explain_set_json(options->explain_json ? 1 : 0);
  ir_explain_set_filter(options->explain_filter);
  /* --annotate-asm: arm the codegen annotator before codegen runs, and keep the
   * optimization remarks alive so it can join them onto the emitted asm. */
  /* --explain-json wants the codegen cost model (cycles per iteration, port
   * bottleneck, spills) joined onto the optimizer's decisions, so arm the
   * annotator for its numbers alone: no listing, no .annot.json sidecar. */
  if (options->explain_json && !options->annotate_asm) {
    mir_annotate_set_enabled(1);
    mir_annotate_set_cost_only(1);
    mir_annotate_set_output_path(options->output_filename);
    mir_annotate_set_source_file(options->input_filename);
  }
  /* The call graph and the ranking are assembled after codegen, from the same
   * remark table -- so it has to outlive the optimization-stage flush. */
  if (options->explain_json) {
    ir_explain_set_retain_remarks(1);
  }
  if (options->annotate_asm) {
    mir_annotate_set_enabled(1);
    mir_annotate_set_syntax((MirAnnotSyntax)options->asm_syntax);
    mir_annotate_set_output_path(options->output_filename);
    mir_annotate_set_source_file(options->input_filename);
    if (options->annotate_q_lo)
      mir_annotate_set_line_query(options->annotate_q_lo, options->annotate_q_hi,
                                  options->annotate_q_fn);
    else if (options->annotate_q_fn)
      mir_annotate_set_line_query(0, 0, options->annotate_q_fn);
    if (options->annotate_hot) mir_annotate_set_hot_query(options->annotate_hot);
    ir_explain_set_retain_remarks(1);
  }
  size_t global_const_count = 0;
  IRGlobalIntConst *global_consts =
      collect_global_int_consts(ast_program, &global_const_count);
  ir_optimize_options.global_int_consts = global_consts;
  ir_optimize_options.global_int_const_count = global_const_count;
  ir_optimize_options.whole_program = options->building_executable;
  ir_optimize_options.target_neutral_only = target_neutral;
  ir_optimize_options.gpu_device_only =
      options->emit_ptx || options->emit_spirv;
  int opt_ok = ir_optimize_program(ir_program, &ir_optimize_options);
  free(global_consts);
  if (opt_ok) {
    ir_program_drop_rewrite_rules(ir_program);
  }
  /* --safe describes a stack local at function entry, which outlives the block
   * the declaration sits in. The optimizer is free to fold that block away, so
   * retire the notes it left addressing locals that are no longer declared. */
  if (opt_ok && options->safe && !options->emit_ptx && !options->emit_spirv &&
      !ir_safety_retire_dangling_notes(ir_program)) {
    mettle_compiler_ice_report("Failed to retire --safe stack notes", NULL);
    return 0;
  }
  if (!opt_ok) {
    /* A violated `@simd!` contract is a user error already printed with a
     * source location; don't bury it under a generic internal-error report. */
    if (!ir_optimize_had_user_error()) {
      mettle_compiler_ice_report("IR optimization failed", NULL);
    }
    return 0;
  }
  if (options->ml_opt) {
    MLOptStats ml = {0};
    ir_apply_ml_opt(ir_program, &ml);
    int hoisted = ir_hoist_constants(ir_program);
    fprintf(stderr, "--ml-opt: %d model proposal%s", ml.proposals,
            ml.proposals == 1 ? "" : "s");
    if (ml.proposals > 0) {
      fprintf(stderr, ": %d applied (%d validated equivalent, %d proven-only)",
              ml.validated + ml.proven, ml.validated, ml.proven);
      if (ml.rejected > 0) {
        fprintf(stderr, ", %d REJECTED by the validator", ml.rejected);
      }
      if (ml.skipped > 0) {
        fprintf(stderr, ", %d skipped", ml.skipped);
      }
    }
    fprintf(stderr, "; hoisted %d large constants\n", hoisted);
    if (options->explain) {
      /* ml_gnn wrote _mlopt.explain (TSV). Render it styled like the main report. */
      ir_explain_ml_opt("_mlopt.explain");
    }
  }
  return 1;
}

static int compile_generate_code(CodeGenerator *code_generator) {
  if (!code_generator_generate_program(code_generator)) {
    const char *message = (code_generator && code_generator->error_message)
                              ? code_generator->error_message
                              : "Unknown error";
    /* A GPU-only construct compiled for a CPU target is the programmer's
     * mistake, phrased for them at the point it was found; don't bury it
     * under a generic internal-error report. */
    if (code_generator && code_generator->has_user_error) {
      fprintf(stderr, "error: %s\n", message);
      return 0;
    }
    fprintf(stderr, "Code generation error: %s\n", message);
    mettle_compiler_ice_report("Code generation failed",
                               code_generator && code_generator->error_message
                                   ? code_generator->error_message
                                   : NULL);
    return 0;
  }
  return 1;
}

static void compile_dump_device_ir(IRProgram *program,
                                   const char *output_filename) {
  char *ir_output = build_sidecar_filename(output_filename, ".ir");
  if (!ir_output) {
    fprintf(stderr,
            "Warning: Failed to allocate IR output filename for '%s'\n",
            output_filename ? output_filename : "<device module>");
    return;
  }
  FILE *ir_file = fopen(ir_output, "w");
  if (!ir_file) {
    fprintf(stderr, "Warning: Could not create IR file '%s': %s\n",
            ir_output, strerror(errno));
  } else {
    if (!ir_program_dump(program, ir_file)) {
      fprintf(stderr, "Warning: Failed to write IR dump to '%s'\n",
              ir_output);
    }
    fclose(ir_file);
  }
  free(ir_output);
}

static void compile_dump_ast(ASTNode *program, const char *output_filename) {
  char *ast_output = build_sidecar_filename(output_filename, ".ast");
  if (!ast_output) {
    fprintf(stderr,
            "Warning: Failed to allocate AST output filename for '%s'\n",
            output_filename ? output_filename : "<output>");
    return;
  }
  FILE *ast_file = fopen(ast_output, "w");
  if (!ast_file) {
    fprintf(stderr, "Warning: Could not create AST file '%s': %s\n",
            ast_output, strerror(errno));
  } else {
    int ast_ok = ast_dump_program(ast_file, program);
    if (fclose(ast_file) != 0) {
      ast_ok = 0;
    }
    if (!ast_ok) {
      fprintf(stderr, "Warning: Failed to write AST dump to '%s'\n",
              ast_output);
    }
  }
  free(ast_output);
}

/* `mettle test` / `mettle trace`: execute in the compile-time interpreter and
 * stop - no optimization (unless requested), no codegen, no linking. */
static int compile_run_comptime(IRProgram *ir_program, ASTNode *program,
                                const CompilerOptions *options,
                                ErrorReporter *error_reporter,
                                const char *input_filename,
                                const char *source) {
  if (options->optimize && !compile_optimize_ir(ir_program, program, options)) {
    return 1;
  }
  ir_program_drop_rewrite_rules(ir_program);
  if (options->test_mode) {
    return ir_comptime_run_tests(ir_program, error_reporter, input_filename,
                                 options->test_filter);
  }
  return ir_comptime_trace(ir_program, error_reporter, input_filename, source,
                           options->trace_function, options->trace_args,
                           options->trace_arg_count);
}

static int compile_optimize_device_ir(IRProgram *ir_program, ASTNode *program,
                                      const CompilerOptions *options,
                                      CompilerProfile *profile) {
  double phase_start;
  int opt_ok;
  if (!options->optimize) {
    return 1;
  }
  compiler_set_phase(PROFILE_PHASE_IR_OPTIMIZATION);
  phase_start = compiler_profile_begin(profile);
  opt_ok = compile_optimize_ir(ir_program, program, options);
  compiler_profile_add(profile, PROFILE_PHASE_IR_OPTIMIZATION, phase_start);
  return opt_ok;
}

static int compile_write_kernel_declarations(ASTNode *program,
                                             const CompilerOptions *options,
                                             const char *input_filename,
                                             const char *output_filename) {
  char decls_path[1024];
  if (!options->emit_kernel_decls) {
    return 1;
  }
  if (options->emit_kernel_decls[0]) {
    snprintf(decls_path, sizeof(decls_path), "%s", options->emit_kernel_decls);
  } else {
    snprintf(decls_path, sizeof(decls_path), "%s.mettle", output_filename);
  }
  return write_kernel_declarations(program, decls_path, input_filename);
}

/* --emit-ptx: lower every declared kernel to a PTX `.visible .entry` and write
 * the PTX text to the output file. No object or link is produced -- the CUDA
 * driver JIT-compiles this text at runtime. */
static int compile_emit_ptx(IRProgram *ir_program, ASTNode *program,
                            CodeGenerator *code_generator,
                            const CompilerOptions *options,
                            const char *input_filename,
                            const char *output_filename,
                            CompilerProfile *profile) {
  FILE *ptx_out;
  char *ptx_err = NULL;
  PtxEmitOptions ptx_options = {options->ptx_target, options->ptx_isa_major,
                                options->ptx_isa_minor,
                                options->ptx_tensor_tuple_budget,
                                options->gpu_checks};
  int ok;

  if (!compile_optimize_device_ir(ir_program, program, options, profile)) {
    return 1;
  }
  if (options->dump_ir) {
    compile_dump_device_ir(ir_program, output_filename);
  }
  ptx_out = fopen(output_filename, "w");
  if (!ptx_out) {
    fprintf(stderr, "Error: could not open PTX output '%s'\n", output_filename);
    return 1;
  }
  ok = ptx_emit_program(ir_program, code_generator, ptx_out, &ptx_options,
                        &ptx_err);
  fclose(ptx_out);
  if (!ok) {
    fprintf(stderr, "Error: PTX emission failed: %s\n",
            ptx_err ? ptx_err : "unknown");
    free(ptx_err);
    return 1;
  }
  if (options->explain && options->optimize) {
    ir_explain_target_flush("PTX");
  }
  printf("Generated PTX: %s\n", output_filename);
  if (!compile_write_kernel_declarations(program, options, input_filename,
                                         output_filename)) {
    return 1;
  }
  if (options->report_occupancy) {
    int sm_count = options->report_sms;
    int sm_count_is_local = 0;
    if (sm_count <= 0) {
      sm_count = detect_gpu_sm_count();
      sm_count_is_local = sm_count > 0;
    }
    report_ptx_occupancy(ir_program, output_filename, options->ptx_target,
                         sm_count, sm_count_is_local);
  }
  return 0;
}

/* --emit-spirv: lower every declared kernel to a SPIR-V `Kernel` entry point
 * and write the binary module. Offload-only: no host object or link. An OpenCL
 * runtime JITs the module at load time. */
static int compile_emit_spirv(IRProgram *ir_program, ASTNode *program,
                              CodeGenerator *code_generator,
                              const CompilerOptions *options,
                              const char *output_filename,
                              CompilerProfile *profile) {
  FILE *spv_out;
  char *spv_err = NULL;
  int ok;

  if (!compile_optimize_device_ir(ir_program, program, options, profile)) {
    return 1;
  }
  if (options->dump_ir) {
    compile_dump_device_ir(ir_program, output_filename);
  }
  spv_out = fopen(output_filename, "wb");
  if (!spv_out) {
    fprintf(stderr, "Error: could not open SPIR-V output '%s'\n",
            output_filename);
    return 1;
  }
  ok = spirv_emit_program(ir_program, code_generator, spv_out, &spv_err);
  fclose(spv_out);
  if (!ok) {
    fprintf(stderr, "Error: SPIR-V emission failed: %s\n",
            spv_err ? spv_err : "unknown");
    free(spv_err);
    return 1;
  }
  if (options->explain && options->optimize) {
    ir_explain_target_flush("SPIR-V");
  }
  printf("Generated SPIR-V: %s\n", output_filename);
  return 0;
}

/* --emit-arm64: lower the scalar subset of every function directly to a
 * self-contained AArch64 ELF executable (from-scratch backend, no external
 * assembler and no linker). A `_start` calls main() and exits with its return
 * value; module globals and the freestanding allocator live in a second,
 * writable segment. No x86 object.
 *
 * -O/--release runs the target-neutral half of the optimizer: scalar and
 * control-flow transforms that keep the shared IR instruction set. The x86
 * SIMD idiom recognizers stay off -- they form ops this backend has no
 * encoding for -- so what reaches the lowering is the same shape it already
 * consumes, just less of it. */
static int compile_emit_arm64(IRProgram *ir_program, ASTNode *program,
                              const CompilerOptions *options,
                              const char *output_filename) {
  Arm64Emit ae;
  unsigned char *arm64_data = NULL;
  size_t arm64_data_len = 0;
  int ok;

  if (options->optimize && !compile_optimize_ir(ir_program, program, options)) {
    return 1;
  }
  ir_program_drop_rewrite_rules(ir_program);
  arm64_emit_init(&ae);
  ok = arm64_ir_encode_program(&ae, ir_program, "main", &arm64_data,
                               &arm64_data_len) &&
       arm64_emit_finalize(&ae);
  if (ok) {
    ok = arm64_write_elf(output_filename, ae.code.data, ae.code.len,
                         arm64_data, arm64_data_len);
    if (!ok) {
      fprintf(stderr,
              "Error: could not write AArch64 ELF '%s' (I/O failure, or the "
              "program's %zu bytes of code reach the fixed address of the "
              "writable segment; use the object path for a program this "
              "large)\n",
              output_filename, ae.code.len);
    }
  } else {
    fprintf(stderr, "Error: AArch64 lowering failed: %s\n",
            arm64_error_reason(&ae));
  }
  free(arm64_data);
  arm64_emit_free(&ae);
  if (!ok) {
    return 1;
  }
  printf("Generated AArch64 ELF: %s\n", output_filename);
  return 0;
}


static int compile_wants_debug_info(const CompilerOptions *options) {
  return options->debug_mode || options->generate_debug_symbols ||
         options->generate_line_mapping ||
         options->generate_stack_trace_support ||
         (options->generate_crash_report && options->building_executable);
}

static int compile_wants_debug_sidecar(const CompilerOptions *options) {
  return options->debug_mode || options->generate_debug_symbols ||
         options->generate_line_mapping
             ? 1
             : 0;
}

static void compile_prepend_auto_imports(const CompilerOptions *options,
                                         ASTNode *program) {
  const char *auto_imports[2];
  size_t auto_import_count = 0;
  if (options->prelude) {
    auto_imports[auto_import_count++] = "std/prelude";
  }
  if (options->native_heap) {
    auto_imports[auto_import_count++] = "std/alloc";
  }
  for (size_t ai = 0; ai < auto_import_count; ai++) {
    Program *prog_data = (Program *)program->data;
    SourceLocation auto_loc = {0, 0, NULL};
    ASTNode *auto_import = ast_create_import_declaration(
        auto_imports[ai], NULL, NULL, 0, auto_loc);
    if (auto_import) {
      // Prepend the import before all user declarations.
      ASTNode **grown =
          realloc(prog_data->declarations,
                  (prog_data->declaration_count + 1) * sizeof(ASTNode *));
      if (grown) {
        memmove(grown + 1, grown,
                prog_data->declaration_count * sizeof(ASTNode *));
        grown[0] = auto_import;
        prog_data->declarations = grown;
        prog_data->declaration_count++;
        ast_add_child(program, auto_import);
      } else {
        ast_destroy_node(auto_import);
      }
    }
  }
}

int compile_file(const char *input_filename, const char *output_filename,
                 CompilerOptions *options) {
  CompilerProfile profile;
  double phase_start = 0.0;
  const int arm64_object_output = compile_targets_arm64_object(options);

  compiler_profile_init(&profile, options && options->profile);

  mettle_compiler_ctx_reset();
  mettle_compiler_ctx_set_input_filename(input_filename);
  mettle_compiler_ctx_set_current_filename(input_filename);
  if (options) {
    mettle_compiler_ctx_set_options(options->debug_compiler, options->dump_ir);
  }

  compiler_set_phase(PROFILE_PHASE_READ_INPUT);
  phase_start = compiler_profile_begin(&profile);
  char *source = NULL;
  int read_ok = compile_read_source(input_filename, &source);
  compiler_profile_add(&profile, PROFILE_PHASE_READ_INPUT, phase_start);
  if (!read_ok) {
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  compiler_set_phase(PROFILE_PHASE_INIT);
  phase_start = compiler_profile_begin(&profile);
  ErrorReporter *error_reporter = error_reporter_create(input_filename, source);
  compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
  if (!error_reporter) {
    fprintf(stderr, "Error: Could not initialize error reporter\n");
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  /* Lexical errors are reported inline by the parser (parser_advance calls
   * parser_report_lexer_token_error on any TOKEN_ERROR, into this same
   * error_reporter, and the post-parse check below aborts before codegen).
   * A separate pre-pass that re-tokenized the whole source just to find those
   * same errors was pure duplicate work -- a full extra lexer pass over the
   * input -- so it has been removed. The phase slot is kept (recorded as 0 ms)
   * to preserve the --profile output layout. */
  compiler_set_phase(PROFILE_PHASE_LEXICAL_VALIDATION);
  phase_start = compiler_profile_begin(&profile);
  compiler_profile_add(&profile, PROFILE_PHASE_LEXICAL_VALIDATION, phase_start);

  // Initialize compiler components
  compiler_set_phase(PROFILE_PHASE_INIT);
  phase_start = compiler_profile_begin(&profile);
  Lexer *lexer = lexer_create(source);
  Parser *parser = NULL;
  SymbolTable *symbol_table = symbol_table_create();
  TypeChecker *type_checker = NULL;
  RegisterAllocator *register_allocator = register_allocator_create();
  ASTNode *program = NULL;

  // Initialize debug info if debug mode is enabled
  DebugInfo *debug_info = NULL;
  CodeGenerator *code_generator = NULL;
  IRProgram *ir_program = NULL;
  char *ir_error_message = NULL;

  if (!lexer || !symbol_table || !register_allocator) {
    compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
    error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                             source_location_create(0, 0),
                             "Failed to initialize compiler components");
    error_reporter_print_errors(error_reporter);
    if (lexer)
      lexer_destroy(lexer);
    if (symbol_table)
      symbol_table_destroy(symbol_table);
    if (register_allocator)
      register_allocator_destroy(register_allocator);
    error_reporter_destroy(error_reporter);
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  parser = parser_create_with_error_reporter(lexer, error_reporter);
  if (parser) {
    /* Enable kernel index built-ins (thread.x etc.) for GPU compiles. */
    parser->gpu_mode = options->emit_ptx || options->emit_spirv;
  }
  type_checker =
      type_checker_create_with_error_reporter(symbol_table, error_reporter);
  type_checker_set_launch_report(options->report_launches);
  if (!parser || !type_checker) {
    compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
    error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                             source_location_create(0, 0),
                             "Failed to initialize parser or type checker");
    error_reporter_print_errors(error_reporter);
    if (parser)
      parser_destroy(parser);
    if (type_checker)
      type_checker_destroy(type_checker);
    register_allocator_destroy(register_allocator);
    symbol_table_destroy(symbol_table);
    lexer_destroy(lexer);
    error_reporter_destroy(error_reporter);
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  if (compile_wants_debug_info(options)) {
    debug_info = debug_info_create(input_filename, output_filename);
    if (!debug_info) {
      compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
      error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                               source_location_create(0, 0),
                               "Failed to initialize debug information");
      error_reporter_print_errors(error_reporter);
      parser_destroy(parser);
      type_checker_destroy(type_checker);
      register_allocator_destroy(register_allocator);
      symbol_table_destroy(symbol_table);
      lexer_destroy(lexer);
      error_reporter_destroy(error_reporter);
      free(source);
      compiler_profile_print_compile(&profile, input_filename, 1);
      return 1;
    }
    code_generator = code_generator_create_with_debug(debug_info);
  } else {
    code_generator = code_generator_create();
  }

  if (!code_generator) {
    compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
    error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                             source_location_create(0, 0),
                             "Failed to initialize code generator");
    error_reporter_print_errors(error_reporter);
    parser_destroy(parser);
    type_checker_destroy(type_checker);
    register_allocator_destroy(register_allocator);
    symbol_table_destroy(symbol_table);
    lexer_destroy(lexer);
    if (debug_info)
      debug_info_destroy(debug_info);
    error_reporter_destroy(error_reporter);
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  if (debug_info) {
    code_generator_set_debug_sidecar_emission(
        code_generator,
        compile_wants_debug_sidecar(options));
  }
  code_generator_set_stack_trace_support(
      code_generator, options->generate_stack_trace_support ? 1 : 0);
  /* Crash reporting is on by default, but only where this driver produces the
   * executable and there is a runtime to report through: it adds references to
   * mettle_crash_*, and only the link this driver builds is guaranteed to
   * carry crash_handler.o. A bare object handed to someone else's linker, and
   * a freestanding image with no runtime at all, stay as they were. */
  code_generator_set_crash_report(
      code_generator,
      (options->generate_crash_report && options->building_executable &&
       !options->flat_output && !mtlc_target()->freestanding)
          ? 1
          : 0);
  code_generator_set_eliminate_unreachable_functions(
      code_generator, options->release ? 1 : 0);
  code_generator_set_profile_runtime(code_generator,
                                     compiler_options_use_profile_runtime(options)
                                         ? 1
                                         : 0);
  code_generator_set_debug_hooks(code_generator, options->debug_hooks ? 1 : 0);
  code_generator->whole_program = options->building_executable ? 1 : 0;
  compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);

  int result = 0;
  options->emit_object = 1;

  compiler_set_phase(PROFILE_PHASE_PARSE);
  phase_start = compiler_profile_begin(&profile);
  int parse_ok = compile_lex_and_parse(parser, error_reporter, &program);
  compiler_profile_add(&profile, PROFILE_PHASE_PARSE, phase_start);
  if (!parse_ok) {
    result = 1;
    goto cleanup;
  }
  if (options->dump_ast) {
    compile_dump_ast(program, output_filename);
  }

  // Resolve imports (flatten imported module ASTs into the main program)
  ImportResolverOptions import_options = {0};
  if (options) {
    import_options.import_directories = options->import_directories;
    import_options.import_directory_count = options->import_directory_count;
    import_options.stdlib_directory =
        (options->stdlib_directory && options->stdlib_directory[0] != '\0')
            ? options->stdlib_directory
            : "stdlib";
  } else {
    import_options.stdlib_directory = "stdlib";
  }
  /* Select standard library OS variants from the output target, not the host
   * that runs the compiler. Both AArch64 modes emit Linux ELF even when a
   * Windows compiler produces them. */
  import_options.target_is_elf =
      (options && (options->emit_arm64 || options->emit_arm64_obj)) ||
      host_target_is_elf();

  // Auto-inject the standard prelude only when --prelude was specified, and
  // std/alloc when --native-heap was specified (it provides the mettle_heap_*
  // shims the backend rewrites new/malloc/calloc/realloc/free to call).
  compiler_set_phase(PROFILE_PHASE_PRELUDE);
  phase_start = compiler_profile_begin(&profile);
  compile_prepend_auto_imports(options, program);
  compiler_profile_add(&profile, PROFILE_PHASE_PRELUDE, phase_start);

  compiler_set_phase(PROFILE_PHASE_IMPORTS);
  phase_start = compiler_profile_begin(&profile);
  int imports_ok = compile_resolve_imports(program, input_filename,
                                           error_reporter, &import_options);
  compiler_profile_add(&profile, PROFILE_PHASE_IMPORTS, phase_start);
  if (!imports_ok) {
    result = 1;
    goto cleanup;
  }

  compiler_set_phase(PROFILE_PHASE_MONOMORPHIZE);
  phase_start = compiler_profile_begin(&profile);
  int mono_ok = compile_monomorphize(program, error_reporter);
  compiler_profile_add(&profile, PROFILE_PHASE_MONOMORPHIZE, phase_start);
  if (!mono_ok) {
    result = 1;
    goto cleanup;
  }

  /* --explain: collect the memory analyzer's diagnostics so the optimization
   * report can surface them in a "memory" section. Enabled before type-check
   * (where they fire) and only when the optimizer will run -- the only path
   * that produces a report. */
  ir_explain_memory_set_collect(options->explain && options->optimize,
                                options->explain_all ? NULL
                                                     : options->input_filename);

  compiler_set_phase(PROFILE_PHASE_TYPE_CHECK);
  phase_start = compiler_profile_begin(&profile);
  int tc_ok = compile_type_check(type_checker, program, error_reporter);
  compiler_profile_add(&profile, PROFILE_PHASE_TYPE_CHECK, phase_start);
  if (!tc_ok) {
    result = 1;
    goto cleanup;
  }

  /* Expansion has run by now, so the ledger is complete and the AST is the
     expanded one. A budget is a contract: check it before anything downstream
     benefits from work the author did not authorize. */
  if (options->expansion_budget_set &&
      !type_checker_check_expansion_budget(type_checker,
                                           options->expansion_budget)) {
    error_reporter_print_errors(error_reporter);
    result = 1;
    goto cleanup;
  }
  if (options->report_expansion) {
    type_checker_report_expansion(type_checker, stdout);
  }
  if (options->expand_mode) {
    size_t unprintable =
        ast_print_program(stdout, program, expand_annotate, type_checker);
    if (unprintable > 0) {
      fprintf(stderr,
              "\nnote: %zu node%s had no source form and were printed as "
              "marked comments; this output is not a complete program\n",
              unprintable, unprintable == 1 ? "" : "s");
    }
    result = 0;
    goto cleanup;
  }

  /* @test functions are type-checked in every build (so they can't rot) but
   * compiled only under `mettle test`: drop them before lowering. The node
   * lives in BOTH program->children and Program->declarations; remove it
   * from both before destroying it. */
  if (!options->test_mode) {
    Program *prog_data = (Program *)program->data;
    if (prog_data) {
      size_t kept = 0;
      for (size_t i = 0; i < prog_data->declaration_count; i++) {
        ASTNode *decl = prog_data->declarations[i];
        FunctionDeclaration *fd =
            decl && decl->type == AST_FUNCTION_DECLARATION && decl->data
                ? (FunctionDeclaration *)decl->data
                : NULL;
        if (fd && fd->is_test) {
          size_t child_kept = 0;
          for (size_t c = 0; c < program->child_count; c++) {
            if (program->children[c] == decl) {
              continue;
            }
            program->children[child_kept++] = program->children[c];
          }
          program->child_count = child_kept;
          ast_destroy_node(decl);
          continue;
        }
        prog_data->declarations[kept++] = decl;
      }
      prog_data->declaration_count = kept;
    }
  }

  /* --explain reports optimizer decisions, so it only means something when the
   * optimizer runs; lowering then brackets every loop with report-only markers
   * for the verifier to report on. */
  if (options->explain && !options->optimize) {
    fprintf(stderr, "note: --explain has no effect without -O/--release (it "
                    "reports optimization decisions)\n");
  }
  ir_lowering_set_explain(options->explain && options->optimize &&
                          !options->emit_ptx && !options->emit_spirv);

  /* --emit-arm64 keeps the checks: its traps print the message and exit(1)
   * like the x86 backend's, so debug semantics match across targets. (The
   * exclusion dated from bring-up, when the trap calls could not lower.) */
  int emit_runtime_checks =
      (options->release || options->emit_ptx || options->emit_spirv ||
       mtlc_target()->freestanding)
          ? 0
          : 1;
  /* The device backends have their own bounds story (--gpu-checks) and their
   * pointers are not host addresses the shadow map could describe. */
  int emit_safety_checks =
      options->safe && !options->emit_ptx && !options->emit_spirv;
  compiler_set_phase(PROFILE_PHASE_IR_LOWERING);
  phase_start = compiler_profile_begin(&profile);
  int ir_ok = compile_lower_to_ir(program, type_checker, symbol_table,
                                   emit_runtime_checks, emit_safety_checks,
                                   &ir_program, &ir_error_message);
  compiler_profile_add(&profile, PROFILE_PHASE_IR_LOWERING, phase_start);
  if (!ir_ok) {
    result = 1;
    goto cleanup;
  }

  if (options->swap_check_mode) {
    if (!options->swap_old_name || !options->swap_new_name) {
      fprintf(stderr,
              "swap-check needs both functions: --old <fn> --new <fn>\n");
      result = 1;
      goto cleanup;
    }
    result = compile_run_swap_check(ir_program, options->swap_old_name,
                                    options->swap_new_name);
    goto cleanup;
  }

  /* Resolve the access marks before anything else looks at the IR: prove away
   * what cannot fail, compile the rest into ordinary instructions. Every later
   * stage, the interpreter included, sees IR with no safety opcodes in it. */
  if (emit_safety_checks) {
    /* Collection has to be armed before the pass, which runs well before the
     * optimizer's own --explain state comes up. */
    ir_explain_safety_set_collect(options->explain && options->optimize,
                                  input_filename);
    IRSafetyStats safety_stats = {0};
    if (!ir_safety_resolve_program(ir_program, &safety_stats)) {
      mettle_compiler_ice_report("Safety check resolution failed", NULL);
      result = 1;
      goto cleanup;
    }
    ir_explain_safety_totals(safety_stats.emitted, safety_stats.proved,
                             safety_stats.hoisted, safety_stats.spanned,
                             safety_stats.exempt, safety_stats.extent_tests,
                             safety_stats.region_calls);
  }

  mettle_compiler_ctx_set_ir_program(ir_program);
  options->main_wants_argc_argv = ir_program->main_wants_argc_argv;

  /* --pgo: interpret main() now, before optimization, so the optimizer can
   * consume measured call frequencies instead of static guesses. */
  if (options->pgo) {
    ir_pgo_profile_program(ir_program);
    ir_pgo_print_summary();
  }

  if (options->test_mode || options->trace_function) {
    result = compile_run_comptime(ir_program, program, options, error_reporter,
                                  input_filename, source);
    goto cleanup;
  }

  if (options->emit_ptx) {
    result = compile_emit_ptx(ir_program, program, code_generator, options,
                              input_filename, output_filename, &profile);
    goto cleanup;
  }

  if (options->emit_spirv) {
    result = compile_emit_spirv(ir_program, program, code_generator, options,
                                output_filename, &profile);
    goto cleanup;
  }

  /* Device-module emitters consume semantic kernel IR directly. Host targets
   * now lower semantic launch operations to the stable runtime-provider ABI;
   * parsing and frontend type checking never mention CUDA argument arrays. */
  if (!ir_program_lower_gpu_launches(ir_program)) {
    fprintf(stderr, "Error: Failed to lower GPU launches for the host runtime\n");
    result = 1;
    goto cleanup;
  }

  if (options->emit_arm64) {
    result = compile_emit_arm64(ir_program, program, options, output_filename);
    goto cleanup;
  }

  /* --native-heap: retarget new/malloc/calloc/realloc/free onto the std/alloc
   * Mettle allocator at the IR level (before optimization, so the rewritten
   * calls inline/optimize like any other). std/alloc is auto-injected above. */
  if (options->native_heap && !ir_program_route_to_native_heap(ir_program)) {
    fprintf(stderr, "Error: Failed to route allocation to the native heap\n");
    result = 1;
    goto cleanup;
  }

  /* --safe: describe each allocation to the runtime once the allocator is
   * settled, so a pointer check has something to resolve against. Before
   * optimization for the same reason the routing above is: the bookkeeping
   * calls should inline and move like any others. */
  if (emit_safety_checks && !ir_safety_register_allocations(ir_program)) {
    fprintf(stderr, "Error: Failed to instrument allocations for --safe\n");
    result = 1;
    goto cleanup;
  }

  if (compiler_options_use_profile_runtime(options)) {
    if (!ir_profile_instrument_program(ir_program)) {
      fprintf(stderr, "Error: Failed to instrument IR for runtime profiling\n");
      result = 1;
      goto cleanup;
    }
  }

  /* --debug-hooks: interactive debugger instrumentation (enter/exit/line
   * hooks + live-pointer variable registrations). Mutually exclusive with
   * the profiler (both own the fn-id registry) and intended for -O0: the
   * optimizer would move or delete the hooks. */
  if (options->debug_hooks) {
    if (compiler_options_use_profile_runtime(options)) {
      fprintf(stderr,
              "Error: --debug-hooks and --profile-runtime are mutually "
              "exclusive\n");
      result = 1;
      goto cleanup;
    }
    if (options->optimize) {
      fprintf(stderr,
              "Error: --debug-hooks requires an unoptimized build (drop "
              "--release/-O; optimized code moves and deletes the hooks)\n");
      result = 1;
      goto cleanup;
    }
    if (!ir_debug_hooks_instrument_program(ir_program)) {
      fprintf(stderr, "Error: Failed to instrument IR for debugging\n");
      result = 1;
      goto cleanup;
    }
  }

  /* Executable builds sweep functions unreachable from main. Importing a
   * stdlib module emits the whole module, so without this every binary carries
   * the unused siblings of each function it actually calls. Skipped for
   * profile/tracy/debug-hook builds, whose instrumentation tables enumerate
   * every function. */
  int sweep_dead_functions = options->building_executable && !options->tracy &&
                             !compiler_options_use_profile_runtime(options) &&
                             !options->debug_hooks;
  /* A foreign object on the link line (`--link-arg caller.o`) may call any
   * `export fn` without a single Mettle instruction naming it, so the exports
   * have to stay rooted whenever one is present. A shared object, and a program
   * that publishes its symbols for one to bind, are the same situation: the
   * caller is on the other side of the link. */
  int keep_exports = options->link_argument_count > 0 ||
                     options->shared_output || options->export_dynamic;

  /* Sweep once before the optimizer: a body that will not ship should not cost
   * a full pipeline first. The optimizer never synthesizes a call to a Mettle
   * function, so nothing dropped here can come back. */
  if (sweep_dead_functions &&
      !ir_program_eliminate_dead_functions(ir_program, keep_exports)) {
    fprintf(stderr, "Error: Failed to eliminate dead functions\n");
    result = 1;
    goto cleanup;
  }

  if (options->optimize) {
    compiler_set_phase(PROFILE_PHASE_IR_OPTIMIZATION);
    phase_start = compiler_profile_begin(&profile);
    int opt_ok = compile_optimize_ir(ir_program, program, options);
    compiler_profile_add(&profile, PROFILE_PHASE_IR_OPTIMIZATION, phase_start);
    if (!opt_ok) {
      result = 1;
      goto cleanup;
    }
  } else {
    /* Vectorization (and thus `@simd` contract verification) only runs under
     * -O/--release. Tell the user their `@simd` loops went unchecked and strip
     * the markers so they never reach codegen. */
    ir_note_simd_contracts_unverified(ir_program);
  }
  ir_program_drop_rewrite_rules(ir_program);

  /* And again after the optimizer, so a helper the inliner absorbed into its
   * only caller is swept as well. */
  if (sweep_dead_functions &&
      !ir_program_eliminate_dead_functions(ir_program, keep_exports)) {
    fprintf(stderr, "Error: Failed to eliminate dead functions\n");
    result = 1;
    goto cleanup;
  }

  /* Retiring an instruction leaves a NOP behind, and after a release pipeline
   * close to half the body is holes. The optimizer keeps them because several
   * passes rewrite into the slack they provide; nothing downstream needs it,
   * and codegen walks every function several times. */
  for (size_t i = 0; i < ir_program->function_count; i++) {
    ir_function_drop_dead_nops(ir_program->functions[i]);
  }

  if (options->profile_runtime_ops) {
    if (!ir_profile_instrument_operation_counters(ir_program)) {
      fprintf(stderr,
              "Error: Failed to instrument IR operation counters for runtime profiling\n");
      result = 1;
      goto cleanup;
    }
  }

  if (options->profile_blocks) {
    if (!ir_profile_instrument_blocks(ir_program)) {
      fprintf(stderr,
              "Error: Failed to instrument IR basic-block counters for the "
              "codegen profile view\n");
      result = 1;
      goto cleanup;
    }
  }

  code_generator_set_ir_program(code_generator, ir_program);

  if (options->debug_mode || options->dump_ir) {
    compiler_set_phase(PROFILE_PHASE_IR_DUMP);
    phase_start = compiler_profile_begin(&profile);
    char *ir_output = build_sidecar_filename(output_filename, ".ir");
    if (!ir_output) {
      fprintf(stderr,
              "Warning: Failed to allocate IR output filename for '%s'\n",
              output_filename);
    } else {
      FILE *ir_file = fopen(ir_output, "w");
      if (!ir_file) {
        fprintf(stderr, "Warning: Could not create IR file '%s': %s\n",
                ir_output, strerror(errno));
      } else {
        if (!ir_program_dump(ir_program, ir_file)) {
          fprintf(stderr, "Warning: Failed to write IR dump to '%s'\n",
                  ir_output);
        }
        fclose(ir_file);
        if (options->debug_mode) {
          printf("Generated IR dump: %s\n", ir_output);
        }
      }
      free(ir_output);
    }
    compiler_profile_add(&profile, PROFILE_PHASE_IR_DUMP, phase_start);
  }

  compiler_set_phase(PROFILE_PHASE_CODEGEN);
  phase_start = compiler_profile_begin(&profile);
  /* The native Arm path is a first-class IR backend. It deliberately bypasses
   * the x86 MIR/encoder while sharing the frontend-neutral IR and linker flow.
   * An ARM host takes it for every object; --emit-arm64-obj asks for it from
   * any host, which is what lets an x86-64 box exercise and test it. */
  int codegen_ok = arm64_object_output ? 1
                                       : compile_generate_code(code_generator);
  compiler_profile_add(&profile, PROFILE_PHASE_CODEGEN, phase_start);
  if (!codegen_ok) {
    result = 1;
    goto cleanup;
  }

  /* The annotator goes first: it publishes the cost model (cycles per
   * iteration, port bottlenecks, spills) into the --explain report, which the
   * backend flush below then writes out. It also emits its own listing and
   * .annot.json sidecar when --annotate-asm asked for them. */
  if (options->annotate_asm || (options->explain_json && mir_annotate_enabled())) {
    mir_annotate_flush();
  }

  /* --explain: the MIR eligibility gate recorded, per function, whether it got
   * the register-allocating backend; print that section now that codegen ran.
   * (No-op unless --explain is on.) */
  if (options->explain && options->optimize) {
    ir_explain_backend_flush();
  }

  compiler_set_phase(PROFILE_PHASE_WRITE_OUTPUT);
  phase_start = compiler_profile_begin(&profile);
  if (arm64_object_output) {
    char arm64_error[512] = {0};
    if (!arm64_ir_write_object(ir_program, output_filename, arm64_error,
                               sizeof(arm64_error))) {
      compiler_profile_add(&profile, PROFILE_PHASE_WRITE_OUTPUT, phase_start);
      fprintf(stderr, "Error: Could not create AArch64 object file '%s': %s\n",
              output_filename,
              arm64_error[0] ? arm64_error : "Unknown error");
      result = 1;
      goto cleanup;
    }
  } else if (options->flat_output) {
    BinaryEmitter *binary_emitter =
        code_generator_get_binary_emitter(code_generator);
    const char *flat_entry_symbol = "main";
    for (size_t fi = 0; fi < ir_program->function_count; fi++) {
      if (ir_program->functions[fi] && ir_program->functions[fi]->name &&
          strcmp(ir_program->functions[fi]->name, "_start") == 0) {
        flat_entry_symbol = "_start";
        break;
      }
    }
    char flat_error[512] = {0};
    const unsigned char boot_signature[2] = {0x55, 0xAA};
    const unsigned char *trailer = NULL;
    size_t pad_to = 0;
    size_t trailer_size = 0;
    /* A flat image loaded at 0x7C00 is a boot sector by definition: the
     * firmware reads exactly 512 bytes and refuses them without the signature
     * in the last two, so the compiler writes both rather than making every
     * caller remember. */
    if (mtlc_target()->image_base == 0x7C00ull) {
      pad_to = 512;
      trailer = boot_signature;
      trailer_size = sizeof(boot_signature);
    }
    if (!binary_emitter_write_flat(binary_emitter, options->flat_output,
                                   mtlc_target()->image_base,
                                   flat_entry_symbol, pad_to, 0x00, trailer,
                                   trailer_size, flat_error,
                                   sizeof(flat_error))) {
      compiler_profile_add(&profile, PROFILE_PHASE_WRITE_OUTPUT, phase_start);
      fprintf(stderr, "Error: Could not create flat image '%s': %s\n",
              options->flat_output,
              flat_error[0] ? flat_error : "Unknown error");
      result = 1;
      goto cleanup;
    }
    fprintf(stderr, "Wrote flat image '%s' at 0x%llx\n", options->flat_output,
            (unsigned long long)mtlc_target()->image_base);
  } else {
    BinaryEmitter *binary_emitter =
        code_generator_get_binary_emitter(code_generator);
    if (!binary_emitter_write_object_file(binary_emitter, output_filename)) {
      compiler_profile_add(&profile, PROFILE_PHASE_WRITE_OUTPUT, phase_start);
      fprintf(stderr, "Error: Could not create object file '%s': %s\n",
              output_filename,
              binary_emitter_get_error(binary_emitter)
                  ? binary_emitter_get_error(binary_emitter)
                  : "Unknown error");
      result = 1;
      goto cleanup;
    }
  }
  compiler_profile_add(&profile, PROFILE_PHASE_WRITE_OUTPUT, phase_start);

  // Generate debug information files if requested
  compiler_set_phase(PROFILE_PHASE_DEBUG_INFO);
  phase_start = compiler_profile_begin(&profile);
  if (debug_info) {
    if (options->debug_mode || options->generate_debug_symbols ||
        options->generate_line_mapping) {
      const char *format =
          (options->debug_format && options->debug_format[0] != '\0')
              ? options->debug_format
              : "dwarf";
      const char *suffix = ".dwarf";

      if (strcasecmp(format, "stabs") == 0) {
        suffix = ".stabs";
      } else if (strcasecmp(format, "map") == 0) {
        suffix = ".map";
      } else if (strcasecmp(format, "dwarf") != 0) {
        fprintf(stderr,
                "Warning: Unknown debug format '%s', defaulting to dwarf\n",
                format);
      }

      char *debug_output = build_sidecar_filename(output_filename, suffix);
      if (!debug_output) {
        compiler_profile_add(&profile, PROFILE_PHASE_DEBUG_INFO, phase_start);
        fprintf(stderr,
                "Error: Failed to allocate debug output filename for '%s'\n",
                output_filename);
        result = 1;
        goto cleanup;
      }

      if (strcasecmp(format, "stabs") == 0) {
        debug_info_generate_stabs(debug_info, debug_output);
      } else if (strcasecmp(format, "map") == 0) {
        debug_info_generate_debug_map(debug_info, debug_output);
      } else {
        debug_info_generate_dwarf(debug_info, debug_output);
      }

      if (options->debug_mode) {
        printf("Generated debug info: %s\n", debug_output);
      }
      free(debug_output);
    }

    if (options->generate_stack_trace_support && options->debug_mode) {
      printf("Embedded runtime stack trace support enabled\n");
    }
  }
  compiler_profile_add(&profile, PROFILE_PHASE_DEBUG_INFO, phase_start);

  if (options->debug_mode) {
    if (error_reporter->count > 0) {
      error_reporter_print_errors(error_reporter);
    }
    printf("Successfully compiled '%s' to '%s'\n", input_filename,
           output_filename);
  } else if (error_reporter->count > 0) {
    // Surface non-fatal diagnostics (e.g. circular/duplicate import warnings)
    // even on successful compilation.
    error_reporter_print_errors(error_reporter);
  }

cleanup:
  // Clean up resources
  compiler_set_phase(PROFILE_PHASE_CLEANUP);
  phase_start = compiler_profile_begin(&profile);
  /* compile_file runs once per process and the caller exits right after
   * linking, so the recursive AST/IR/type/symbol teardown (hundreds of ms on
   * large inputs) buys nothing: leave those to process exit by default.
   * METTLE_FULL_CLEANUP=1 restores the deep teardown for leak-hunting under
   * sanitizers or heap tooling. */
  if (getenv("METTLE_FULL_CLEANUP")) {
    if (program)
      ast_destroy_node(program);
    if (ir_program)
      ir_program_destroy(ir_program);
    type_checker_destroy(type_checker);
    symbol_table_destroy(symbol_table);
  }
  free(ir_error_message);
  code_generator_destroy(code_generator);
  register_allocator_destroy(register_allocator);
  parser_destroy(parser);
  lexer_destroy(lexer);
  if (debug_info)
    debug_info_destroy(debug_info);
  error_reporter_destroy(error_reporter);
  free(source);
  compiler_profile_add(&profile, PROFILE_PHASE_CLEANUP, phase_start);
  compiler_profile_print_compile(&profile, input_filename, result);

  return result;
}

void print_usage(const char *program_name) {
  printf("Usage: %s [options] <input.mettle>\n", program_name);
  printf("       %s help [topic]\n", program_name);
  printf("       %s docs [topic]\n", program_name);
  printf("       %s explain <CODE>   Explain a code: a diagnostic (E0004, M0103) or an --explain\n"
         "                            decision (dot-shape-address); 'list' for the index\n",
         program_name);
  printf("       %s test <file> [--filter=S]   Run @test functions in the compile-time\n"
         "                           interpreter (instant; no codegen or linking)\n",
         program_name);
  printf("       %s trace <file> <fn> [args...] Interpret a function and print a\n"
         "                           line-by-line value trace\n",
         program_name);
  printf("Options:\n");
  printf("  --error-format=F    Diagnostic output format: human (default) or json\n"
         "                      (one JSON object per diagnostic on stderr, for tooling)\n");
  printf("  --pgo               Zero-run profile-guided optimization: interpret main()\n"
         "                      at compile time (deterministic, sandboxed) and feed the\n"
         "                      measured call frequencies to the optimizer - a hot\n"
         "                      callee bypasses the inliner's static size budget like an\n"
         "                      explicit @inline. No instrumented build, no training\n"
         "                      run. Implies -O. METTLE_PGO_HOT sets the threshold.\n");
  printf("  --verify            Translation validation: after every optimization pass,\n"
         "                      execute each changed function's before/after IR on\n"
         "                      generated inputs and compare behavior. A diverging pass\n"
         "                      is reported with a concrete counterexample, quarantined\n"
         "                      for that function, and the build continues from the\n"
         "                      validated IR. Implies -O.\n");
  printf("  -i <file>           Input file\n");
  printf("  -o <file>           Output file (default: output.obj/output.o, or "
         "executable path with --build)\n");
  printf("  -I <dir>            Add import search directory (repeatable)\n");
  printf("  --stdlib <dir>      Set stdlib root directory (default: auto-detect "
         "bundled stdlib, then ./stdlib)\n");
  printf("  --build             Compile and link to an executable (COFF/PE on "
         "Windows, ELF on Linux)\n");
  printf("  --emit-obj          Emit a native object directly (default)\n");
  printf("  --target <triple>   Compile for another machine: x86_64-windows,\n"
         "                      x86_64-linux, x86_64-none, aarch64-linux,\n"
         "                      aarch64-none, i386-none, i686-none,\n"
         "                      i8086-none. The 16- and 32-bit targets emit a\n"
         "                      flat image only (--emit-flat)\n");
  printf("  --image-base <addr> Load address of the linked image, replacing "
         "the\n"
         "                      format's default (e.g. 0x7c00 for a boot "
         "sector)\n");
  printf("  --emit-flat <file>  Write a raw image with no object or executable\n"
         "                      container, laid out at --image-base. At 0x7c00\n"
         "                      it is padded to 512 bytes and signed 0x55AA\n");
  printf("  --emit-arm64        Emit a self-contained AArch64 Linux "
         "executable\n");
  printf("  --emit-arm64-obj    Emit an AArch64 relocatable object (link it "
         "on an\n"
         "                      ARM machine); the default output on an ARM "
         "host\n");
  printf("  --emit-ptx          Emit declared kernels as NVIDIA PTX (targets the\n"
         "                      local GPU, and the PTX ISA its driver can load,\n"
         "                      when one is visible; otherwise DGX Spark GB10,\n"
         "                      PTX 8.8 / sm_121a)\n");
  printf("  --gpu-info          Report the local GPUs, the driver, ptxas, and\n"
         "                      the target --emit-ptx would pick; no input file\n");
  printf("  --emit-kernel-decls[=F]\n"
         "                      With --emit-ptx, also write each kernel's\n"
         "                      host-side `extern kernel` declaration, so an\n"
         "                      importing host cannot drift from the module\n"
         "                      it launches (default: <output>.mettle)\n");
  printf("  --gpu-arch=A        PTX profile: native (this machine, and fail if\n"
         "                      there is none), gb10, portable (compute_75),\n"
         "                      sm_NN, or compute_NN\n");
  printf("  --ptx-version=M.m   Override the emitted PTX ISA version\n");
  printf("  --report-occupancy  With --emit-ptx: run ptxas -v on the emitted\n"
         "                      module and print each kernel's registers per\n"
         "                      thread plus the occupancy ceiling they imply,\n"
         "                      with whole-card fill thresholds when the SM\n"
         "                      count is known\n");
  printf("  --sms=N             SM count for those fill thresholds (default:\n"
         "                      ask the local driver; omitted when neither\n"
         "                      answers)\n");
  printf("  --gpu-checks        Emit the trap for each kernel-side\n"
         "                      gpu_assert; without it they cost nothing\n");
  printf("  --report-launches   List every dispatch site with the grid and\n"
         "                      block the compiler can fold, and the kernel\n"
         "                      each names\n");
  printf("  --gpu-tensor-tuple-budget=N\n"
         "                      PTX resident-fragment ceiling (0=architecture\n"
         "                      default); enables measured resident/replay variants\n"
         "                      without changing source or shared IR\n");
  printf("  --emit-spirv        Emit declared kernels as OpenCL SPIR-V\n");
  printf("  --linker <mode>     Linker backend: auto, internal, gcc, or msvc "
         "(default: internal with --build, otherwise %s)\n",
         linker_mode_name(LINKER_MODE_AUTO));
  printf("  --subsystem <kind>  Windows subsystem: console or windows. A windows "
         "image gets no console window\n");
  printf("  --link-arg <arg>    Pass an extra linker argument (repeatable; "
         "use with --build)\n");
  printf("  -l<name>            Bind shared library lib<name>.so (ELF; "
         "repeatable, attached form only)\n");
  printf("  -L<dir>             Search <dir> for shared libraries "
         "(ELF; repeatable)\n");
  printf("  --rpath <dir>       Record <dir> in DT_RUNPATH (ELF; "
         "repeatable)\n");
  printf("  --shared            Emit a shared object instead of a program "
         "(ELF)\n");
  printf("  --soname <name>     DT_SONAME for --shared output\n");
  printf("  --export-dynamic    Publish the program's own symbols so a loaded "
         "library can bind them\n");
  printf("  --dynamic-linker <path>\n");
  printf("                      Program loader for PT_INTERP (default "
         "%s)\n",
         METTLE_DEFAULT_ELF_INTERPRETER);
  printf("  --tracy             Link std/tracy with the Tracy profiler "
         "(requires --build)\n");
  printf("  --tracy-dir <dir>   Tracy repo root (default: TRACY_DIR env, then "
         ".mettle\\tracy_dir)\n");
  printf("  -d, --debug         Enable debug output and symbols\n");
  printf("  --dump-ast          Write parsed AST sidecar (.ast)\n");
  printf("  --dump-ir           Write optimized IR sidecar (.ir) without debug metadata\n");
  printf("  --simd-report       Report what each @simd loop became (needs -O/--release)\n");
  printf("  --explain           Report every optimization decision in the input file --\n"
         "                      loop vectorization and call inlining, with the reason\n"
         "                      whenever the optimizer declined (needs -O/--release).\n"
         "                      Re-runs lead with what CHANGED since the last build,\n"
         "                      regressions first\n");
  printf("  --explain=SELECTOR  Narrow the report to one slice of it: missed,\n"
         "                      fixable, proven, loops, calls, a function name, or a\n"
         "                      decision code (the id in brackets after a verdict)\n");
  printf("  --explain-json      Also write <output-stem>.explain.json (machine-\n"
         "                      readable report; implies --explain)\n");
  printf("  --annotate-asm      Print the emitted assembly annotated with the codegen\n"
         "                      decision behind each instruction (spill, vectorized\n"
         "                      kernel, strength-reduced divide, ...), a per-op\n"
         "                      latency/throughput cost model, recovered loops with\n"
         "                      their port bottleneck, a register-lifetime map, and an\n"
         "                      instruction-mix summary; also writes a\n"
         "                      <output-stem>.annot.json sidecar. Implies -O and joins\n"
         "                      the --explain remarks. Pair with --asm-syntax=\n");
  printf("  --asm-syntax=S       Assembly syntax for --annotate-asm: intel, att, or\n"
         "                      both (default both)\n");
  printf("  --annotate-lines=A-B Focused codegen report for source lines A..B (or a\n"
         "                      single line A): the emitted asm, per-op cost, the loops\n"
         "                      covering the range, the registers live across it, and\n"
         "                      the optimizer's decisions. Compact, for tools/LLMs.\n");
  printf("  --annotate-fn=NAME   Restrict --annotate-lines/--annotate-asm to one\n"
         "                      function\n");
  printf("  --annotate-hot[=N]  Print the program's top N codegen hotspots (hottest\n"
         "                      loops by cycles/iteration and functions by weighted\n"
         "                      cost); default N=8. For tools/LLMs.\n");
  printf("  --ml-opt            Run the learned ML IR optimizer after the classical\n"
         "                      passes (experimental). A GNN flags redundancy/algebra\n"
         "                      classical missed; sound transforms realize each, and\n"
         "                      every applied rewrite is re-executed through the\n"
         "                      translation-validation interpreter and discarded on\n"
         "                      divergence. Enables -O. See docs/ml-opt.md; with\n"
         "                      --explain it reports each rewrite and its verdict.\n");
  printf("  --ml-opt-speculative  Also apply the model's unproven proposals (dead-\n"
         "                      code deletes). These stand ONLY when the validator\n"
         "                      can execute the function and finds no divergence.\n"
         "                      Implies --ml-opt.\n");
  printf("  -g, --debug-symbols Generate debug symbols\n");
  printf("  -l, --line-mapping  Generate source line mapping\n");
  printf("  -s, --stack-trace   Report a crash at the exact statement. Records\n"
         "                      a location per instruction, which the\n"
         "                      register-allocating backend cannot carry, so\n"
         "                      the affected functions use the baseline emitter\n");
  printf("  --no-crash-report   Drop the default crash report from a linked\n"
         "                      executable. By default a fault names itself,\n"
         "                      its address and the function it happened in,\n"
         "                      for about 8 KB and no change to codegen\n");
  printf("  --debug-format <fmt> Debug format: dwarf, stabs, or map (default: "
         "dwarf)\n");
  printf("  -O, --optimize      Enable optimizations\n");
  printf("  -r, --release       Optimize for size (enables -O, strips comments, "
         "and drops unreachable functions)\n");
  printf("  --prelude           Auto-import the standard prelude (std/io, "
         "std/net, etc.)\n");
  printf("  --profile           Print per-phase compilation timings\n");
  printf("  --profile-runtime   Emit function-level runtime timing report "
         "(disables inlining)\n");
  printf("  --profile-runtime-ops  Emit runtime op-class counters per function "
         "(after optimization)\n");
  printf("  --profile-blocks    Emit per-basic-block execution counters to a "
         ".mprof sidecar\n"
         "                      (path via METTLE_PROFILE_OUT); fuses with "
         "--annotate-asm for\n"
         "                      the VTune-style codegen profile view. Implies "
         "--profile-runtime\n");
  printf("  --debug-hooks       Instrument for the interactive source-level "
         "debugger (requires -O0; used by the editor's F5)\n");
  printf("  --safe              Check every memory access the compiler cannot "
         "prove in bounds (kept under --release)\n");
  printf("  --native-heap       Route new/malloc/calloc/realloc/free through "
         "the Mettle allocator (std/alloc)\n");
  printf("  --static            Accepted for compatibility; owned ELF builds are "
         "always static\n");
  printf("  --musl              Rejected; owned runtime builds never link musl\n");
  printf("  --debug-compiler    Track compiler context for internal error reports\n");
  printf("  -V, --version       Show version information\n");
  printf("  -h, --help          Show this help message\n");
  printf("\nExamples:\n");
  printf("  %s app.mettle -o app.obj\n", program_name);
  printf("      Compile to a native object file.\n");
  printf("  %s --build app.mettle -o app.exe\n", program_name);
  printf("      Self-contained build: COFF object + internal PE linker.\n");
  printf("  %s --build --release app.mettle -o app.exe\n", program_name);
  printf("      Optimized, comment-stripped release build.\n");
  printf("  %s --build --tracy app.mettle -o app.exe\n", program_name);
  printf("      Build with Tracy instrumentation (set TRACY_DIR or "
         "--tracy-dir).\n");
  printf("\nHelp:\n");
  printf("  %s help <topic>     Detail on a topic (" METTLE_HELP_TOPICS ")\n",
         program_name);
  printf("  %s help all         Print every topic\n", program_name);
  printf("  %s docs [topic]     Show the matching documentation file path\n",
         program_name);
}

char *read_file(const char *filename) {
  FILE *file = fopen(filename, "r");
  if (!file) {
    return NULL;
  }

  // Get file size
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }

  // Allocate buffer and read file
  char *buffer = malloc(size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(buffer, 1, size, file);
  if (bytes_read < (size_t)size && ferror(file)) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[bytes_read] = '\0';

  fclose(file);
  return buffer;
}
