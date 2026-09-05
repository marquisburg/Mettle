/* mtlc_api.c - implementation of the public libmtlc entry points (include/mtlc).
 *
 * Part of libmtlc. This translation unit is the seam between the stable public
 * API and the backend's internal IR/optimizer entry points. It is frontend-free:
 * it includes only backend headers (ir.h, ir_optimize.h, ml_opt.h) and the public
 * mtlc/ headers. */
#include "mtlc/mtlc.h"

#include "codegen/binary/arm64_ir.h"
#include "codegen/binary/startup.h"
#include "codegen/binary_emitter.h"
#include "codegen/code_generator.h"
#include "codegen/ptx_emitter.h"
#include "codegen/spirv_emitter.h"
#include "ir/ir.h"
#include "ir/ir_optimize.h"
#include "ir/ml_opt.h"
#include "linker/pe_emitter.h"
#include "linker/symbol_resolve.h"
#include "runtime/owned.h"
#include "runtime/verify_owned.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ version */

const char *mtlc_version(void) { return "libmtlc 0.2.0"; }

/* ------------------------------------------------------------------- target */

MtlcObjectFormat mtlc_host_object_format(void) {
#ifdef _WIN32
  return MTLC_OBJ_COFF;
#else
  return MTLC_OBJ_ELF;
#endif
}

MtlcLinkTarget mtlc_host_link_target(void) {
#ifdef _WIN32
  return MTLC_LINK_PE;
#else
  return MTLC_LINK_ELF;
#endif
}

const char *mtlc_arch_name(MtlcArch arch) {
  switch (arch) {
  case MTLC_ARCH_X86_64:
    return "x86-64";
  case MTLC_ARCH_ARM64:
    return "arm64";
  case MTLC_ARCH_PTX:
    return "ptx";
  case MTLC_ARCH_SPIRV:
    return "spirv";
  }
  return "?";
}

/* ------------------------------------------------------------------ context */

struct MtlcContext {
  int opt_level;
  int ml_opt;
  int whole_program;
  int explain;
  const char *explain_focus_file;
  char ptx_target[24];
  int ptx_isa_major;
  int ptx_isa_minor;
  int ptx_tensor_tuple_budget;
  char runtime_directory[1024];
  MtlcDiagHandler diag_handler;
  void *diag_user_data;
  char last_error[512];
  int has_error;
};

/* --------------------------------------------------------------- diagnostics */

const char *mtlc_diag_severity_name(MtlcDiagSeverity severity) {
  switch (severity) {
  case MTLC_DIAG_ERROR:
    return "error";
  case MTLC_DIAG_WARNING:
    return "warning";
  case MTLC_DIAG_NOTE:
    return "note";
  }
  return "diagnostic";
}

/* Report one diagnostic through `ctx`. A NULL context (the documented
 * conservative-defaults path) and a context with no installed handler both fall
 * back to the historical stderr behavior, so nothing an existing consumer sees
 * changes until it opts in. Errors are recorded for mtlc_context_last_error
 * regardless of where they are delivered. */
static void mtlc_diag(MtlcContext *ctx, MtlcDiagSeverity severity,
                      const char *format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  if (ctx && severity == MTLC_DIAG_ERROR) {
    snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", message);
    ctx->has_error = 1;
  }
  if (ctx && ctx->diag_handler) {
    ctx->diag_handler(ctx->diag_user_data, severity, message);
    return;
  }
  fprintf(stderr, "mtlc: %s\n", message);
}

void mtlc_context_set_diagnostic_handler(MtlcContext *ctx,
                                         MtlcDiagHandler handler,
                                         void *user_data) {
  if (ctx) {
    ctx->diag_handler = handler;
    ctx->diag_user_data = user_data;
  }
}

const char *mtlc_context_last_error(const MtlcContext *ctx) {
  return (ctx && ctx->has_error) ? ctx->last_error : NULL;
}

void mtlc_context_clear_error(MtlcContext *ctx) {
  if (ctx) {
    ctx->last_error[0] = '\0';
    ctx->has_error = 0;
  }
}

int mtlc_context_set_runtime_directory(MtlcContext *ctx, const char *path) {
  size_t length = path ? strlen(path) : 0;
  if (!ctx || !path || length == 0 || length >= sizeof(ctx->runtime_directory)) {
    return 0;
  }
  memcpy(ctx->runtime_directory, path, length + 1);
  return 1;
}

const char *mtlc_context_runtime_directory(const MtlcContext *ctx) {
  return ctx && ctx->runtime_directory[0] ? ctx->runtime_directory : NULL;
}

MtlcContext *mtlc_context_create(void) {
  MtlcContext *ctx = (MtlcContext *)calloc(1, sizeof(MtlcContext));
  if (ctx) {
    memcpy(ctx->ptx_target, "sm_121a", sizeof("sm_121a"));
    ctx->ptx_isa_major = 8;
    ctx->ptx_isa_minor = 8;
  }
  return ctx; /* zero-initialized: no opt, no ml, not whole-program */
}

void mtlc_context_destroy(MtlcContext *ctx) { free(ctx); }

void mtlc_context_set_opt_level(MtlcContext *ctx, int level) {
  if (ctx) {
    ctx->opt_level = level;
  }
}
int mtlc_context_opt_level(const MtlcContext *ctx) {
  return ctx ? ctx->opt_level : 0;
}

void mtlc_context_set_ml_opt(MtlcContext *ctx, int enabled) {
  if (ctx) {
    ctx->ml_opt = enabled ? 1 : 0;
  }
}
int mtlc_context_ml_opt(const MtlcContext *ctx) {
  return ctx ? ctx->ml_opt : 0;
}

void mtlc_context_set_whole_program(MtlcContext *ctx, int enabled) {
  if (ctx) {
    ctx->whole_program = enabled ? 1 : 0;
  }
}
int mtlc_context_whole_program(const MtlcContext *ctx) {
  return ctx ? ctx->whole_program : 0;
}

void mtlc_context_set_explain(MtlcContext *ctx, int enabled,
                              const char *focus_file) {
  if (ctx) {
    ctx->explain = enabled ? 1 : 0;
    ctx->explain_focus_file = focus_file;
  }
}
int mtlc_context_explain(const MtlcContext *ctx) {
  return ctx ? ctx->explain : 0;
}
const char *mtlc_context_explain_focus_file(const MtlcContext *ctx) {
  return ctx ? ctx->explain_focus_file : NULL;
}

static int ptx_target_name_valid(const char *target) {
  if (!target) {
    return 0;
  }
  int is_sm = strncmp(target, "sm_", 3) == 0;
  const unsigned char *p = (const unsigned char *)target;
  if (is_sm) {
    p += 3;
  } else if (strncmp(target, "compute_", 8) == 0) {
    p += 8;
  } else {
    return 0;
  }
  if (!isdigit(*p)) {
    return 0;
  }
  while (isdigit(*p)) {
    p++;
  }
  if (is_sm && (*p == 'a' || *p == 'f')) {
    p++;
  }
  return *p == '\0';
}

int mtlc_context_set_ptx_target(MtlcContext *ctx, const char *target,
                                int isa_major, int isa_minor) {
  if (!ctx || !ptx_target_name_valid(target) ||
      strlen(target) >= sizeof(ctx->ptx_target) || isa_major < 1 ||
      isa_major > 99 || isa_minor < 0 || isa_minor > 9) {
    return 0;
  }
  memcpy(ctx->ptx_target, target, strlen(target) + 1);
  ctx->ptx_isa_major = isa_major;
  ctx->ptx_isa_minor = isa_minor;
  return 1;
}

const char *mtlc_context_ptx_target(const MtlcContext *ctx) {
  return ctx && ctx->ptx_target[0] ? ctx->ptx_target : "sm_121a";
}

int mtlc_context_ptx_isa_major(const MtlcContext *ctx) {
  return ctx && ctx->ptx_isa_major ? ctx->ptx_isa_major : 8;
}

int mtlc_context_ptx_isa_minor(const MtlcContext *ctx) {
  return ctx ? ctx->ptx_isa_minor : 8;
}

int mtlc_context_set_ptx_tensor_tuple_budget(MtlcContext *ctx,
                                             int tuple_budget) {
  if (!ctx || tuple_budget < 0 || tuple_budget > 4096) return 0;
  ctx->ptx_tensor_tuple_budget = tuple_budget;
  return 1;
}

int mtlc_context_ptx_tensor_tuple_budget(const MtlcContext *ctx) {
  return ctx ? ctx->ptx_tensor_tuple_budget : 0;
}

/* ------------------------------------------------------------------- module */

struct MtlcModule {
  IRProgram *ir; /* owned */
};

MtlcModule *mtlc_module_adopt_ir(void *ir_program) {
  MtlcModule *m = (MtlcModule *)calloc(1, sizeof(MtlcModule));
  if (!m) {
    return NULL;
  }
  m->ir = (IRProgram *)ir_program;
  return m;
}

void *mtlc_module_ir(MtlcModule *module) { return module ? module->ir : NULL; }

size_t mtlc_module_function_count(const MtlcModule *module) {
  return (module && module->ir) ? module->ir->function_count : 0;
}

void mtlc_module_destroy(MtlcModule *module) {
  if (!module) {
    return;
  }
  if (module->ir) {
    ir_program_destroy(module->ir);
  }
  free(module);
}

/* ----------------------------------------------------------------- pipeline */

static int mtlc_module_has_kernel(const MtlcModule *module) {
  if (!module || !module->ir) return 0;
  for (size_t i = 0; i < module->ir->function_count; i++) {
    if (module->ir->functions[i] && module->ir->functions[i]->is_kernel)
      return 1;
  }
  return 0;
}

static int mtlc_optimize_policy(MtlcContext *ctx, MtlcModule *module,
                                int target_neutral_only,
                                int gpu_device_only) {
  if (!module || !module->ir) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "mtlc_optimize: a module is required");
    return 0;
  }
  /* Honor the context's documented contract: opt level 0 = none. A NULL ctx
   * keeps the historical conservative default (optimize on). */
  if (ctx && ctx->opt_level <= 0) {
    return 1;
  }
  IROptimizeOptions options;
  /* Zero every field so future additions default off. */
  IROptimizeOptions zero = {0};
  options = zero;
  options.whole_program = ctx ? ctx->whole_program : 0;
  options.explain = ctx ? ctx->explain : 0;
  options.explain_focus_file = ctx ? ctx->explain_focus_file : NULL;
  options.target_neutral_only = target_neutral_only;
  options.gpu_device_only = gpu_device_only;
  if (!gpu_device_only && !ir_program_lower_gpu_launches(module->ir)) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "GPU launch host lowering failed");
    return 0;
  }
  if (!ir_optimize_program(module->ir, &options)) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "the optimizer rejected the module");
    return 0;
  }
  return 1;
}

int mtlc_optimize(MtlcContext *ctx, MtlcModule *module) {
  int gpu = mtlc_module_has_kernel(module);
  return mtlc_optimize_policy(ctx, module, gpu, gpu);
}

int mtlc_optimize_for(MtlcContext *ctx, MtlcModule *module, MtlcArch arch) {
  switch (arch) {
  case MTLC_ARCH_X86_64:
    return mtlc_optimize_policy(ctx, module, 0, 0);
  case MTLC_ARCH_ARM64:
    return mtlc_optimize_policy(ctx, module, 1, 0);
  case MTLC_ARCH_PTX:
  case MTLC_ARCH_SPIRV:
    return mtlc_optimize_policy(ctx, module, 1, 1);
  default:
    mtlc_diag(ctx, MTLC_DIAG_ERROR,
              "mtlc_optimize_for: unknown architecture %d", (int)arch);
    return 0;
  }
}

int mtlc_apply_ml_opt(MtlcContext *ctx, MtlcModule *module,
                      MtlcMlOptStats *stats) {
  /* The caller decides when to run the ML pass; ctx carries only diagnostics. */
  if (!module || !module->ir) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "mtlc_apply_ml_opt: a module is required");
    return 0;
  }
  if (!ir_program_lower_gpu_launches(module->ir)) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "GPU launch host lowering failed");
    return 0;
  }
  MLOptStats s = {0};
  int ok = ir_apply_ml_opt(module->ir, &s);
  if (!ok) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "the ML optimizer failed on this module");
  }
  if (ok) {
    ir_hoist_constants(module->ir);
  }
  if (stats) {
    stats->proposals = s.proposals;
    stats->validated = s.validated;
    stats->proven = s.proven;
    stats->rejected = s.rejected;
    stats->skipped = s.skipped;
  }
  return ok;
}

/* ------------------------------------------------------------- codegen + link */

int mtlc_emit_object(MtlcContext *ctx, MtlcModule *module, const char *path) {
  if (!module || !module->ir || !path) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR,
              "mtlc_emit_object: a module and an output path are required");
    return 0;
  }
  if (!ir_program_lower_gpu_launches(module->ir)) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "GPU launch host lowering failed");
    return 0;
  }
#if defined(__aarch64__) && defined(__linux__)
  {
    char error[512] = {0};
    int ok = arm64_ir_write_object(module->ir, path, error, sizeof(error));
    if (!ok) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "AArch64 object emission failed: %s",
                error[0] ? error : "unknown error");
    }
    return ok;
  }
#else
  CodeGenerator *gen = code_generator_create();
  if (!gen) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "could not create the code generator");
    return 0;
  }
  code_generator_set_ir_program(gen, module->ir);
  if (!code_generator_generate_program(gen)) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "code generation failed: %s",
              gen->error_message ? gen->error_message : "unknown error");
    code_generator_destroy(gen);
    return 0;
  }
  BinaryEmitter *be = code_generator_get_binary_emitter(gen);
  int ok = binary_emitter_write_object_file(be, path);
  if (!ok) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "could not write object '%s': %s", path,
              binary_emitter_get_error(be) ? binary_emitter_get_error(be)
                                           : "unknown error");
  }
  code_generator_destroy(gen);
  return ok;
#endif
}

int mtlc_emit(MtlcContext *ctx, MtlcModule *module, MtlcArch arch,
              const char *path) {
  if (!module || !module->ir || !path) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR,
              "mtlc_emit: a module and an output path are required");
    return 0;
  }
  switch (arch) {
  case MTLC_ARCH_X86_64:
    return mtlc_emit_object(ctx, module, path);

  case MTLC_ARCH_ARM64: {
    /* Cross-host AArch64 relocatable object. The CLI's explicit --emit-arm64
     * legacy smoke target remains a self-contained executable. */
    if (!ir_program_lower_gpu_launches(module->ir)) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "GPU launch host lowering failed");
      return 0;
    }
    char error[512] = {0};
    int ok = arm64_ir_write_object(module->ir, path, error, sizeof(error));
    if (!ok)
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "AArch64 object emission failed: %s",
                error[0] ? error : "unknown error");
    return ok;
  }

  case MTLC_ARCH_PTX: {
    FILE *out = fopen(path, "w");
    if (!out) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "could not open PTX output '%s'", path);
      return 0;
    }
    char *err = NULL;
    PtxEmitOptions ptx_options = {
        mtlc_context_ptx_target(ctx), mtlc_context_ptx_isa_major(ctx),
        mtlc_context_ptx_isa_minor(ctx),
        mtlc_context_ptx_tensor_tuple_budget(ctx)};
    int ok = ptx_emit_program(module->ir, NULL, out, &ptx_options, &err);
    fclose(out);
    if (!ok) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "PTX emission failed: %s",
                err ? err : "unknown error");
      free(err);
    }
    return ok;
  }

  case MTLC_ARCH_SPIRV: {
    FILE *out = fopen(path, "wb");
    if (!out) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "could not open SPIR-V output '%s'",
                path);
      return 0;
    }
    char *err = NULL;
    int ok = spirv_emit_program(module->ir, NULL, out, &err);
    fclose(out);
    if (!ok) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "SPIR-V emission failed: %s",
                err ? err : "unknown error");
      free(err);
    }
    return ok;
  }
  }
  mtlc_diag(ctx, MTLC_DIAG_ERROR, "unknown architecture %d", (int)arch);
  return 0;
}

/* Link `object_paths` + the C-runtime startup object into a PE executable using
 * libmtlc's internal linker. Imports are resolved by DLL name, so no Windows SDK
 * import libraries are needed. */
static int link_pe_internal(MtlcContext *ctx, const char **object_paths,
                            const unsigned char *object_is_runtime_default,
                            size_t object_count, const char *output_path) {
  static const char *const import_dlls[] = {
      "kernel32.dll", "ws2_32.dll", "user32.dll", "gdi32.dll",
      "advapi32.dll", "winmm.dll"};
  LinkResolutionOptions resolution_options = {"mettle_start", 16u, 1,
                                              object_is_runtime_default};
  LinkResolution *resolution = NULL;
  PeEmissionOptions emission = {0};
  char *error_message = NULL;
  int ok = 0;

  if (!link_resolution_build(object_paths, object_count, &resolution_options,
                             &resolution, &error_message)) {
    if (error_message &&
        strstr(error_message, "Unresolved external symbol") != NULL) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "%s", error_message);
    } else {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "internal linker resolution failed: %s",
                error_message ? error_message : "unknown error");
    }
    free(error_message);
    return 0;
  }
  emission.import_dll_names = (const char **)import_dlls;
  emission.import_dll_count = sizeof(import_dlls) / sizeof(import_dlls[0]);
  if (!pe_emit_executable(resolution, output_path, &emission, &error_message)) {
    if (error_message &&
        strstr(error_message, "Unresolved external symbol") != NULL) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "%s", error_message);
    } else {
      mtlc_diag(ctx, MTLC_DIAG_ERROR, "internal linker PE emission failed: %s",
                error_message ? error_message : "unknown error");
    }
    free(error_message);
    link_resolution_destroy(resolution);
    return 0;
  }
  ok = 1;
  link_resolution_destroy(resolution);
  return ok;
}

int mtlc_build_executable(MtlcContext *ctx, MtlcModule *module,
                          const char *output_path) {
  if (!module || !module->ir || !output_path) {
    mtlc_diag(
        ctx, MTLC_DIAG_ERROR,
        "mtlc_build_executable: a module and an output path are required");
    return 0;
  }
  int wants_argc_argv = module->ir->main_wants_argc_argv;
  const char *runtime_directory = mtlc_context_runtime_directory(ctx);
  if (!runtime_directory) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR,
              "mtlc_build_executable: set the libmtlc runtime directory first");
    return 0;
  }

  /* temp paths derived from the output path */
  size_t base_len = strlen(output_path);
  char *obj_path = (char *)malloc(base_len + 16);
  char *startup_path = (char *)malloc(base_len + 24);
  size_t runtime_len = strlen(runtime_directory);
  char *runtime_path = (char *)malloc(runtime_len + 32);
  if (!obj_path || !startup_path || !runtime_path) {
    free(obj_path);
    free(startup_path);
    free(runtime_path);
    mtlc_diag(ctx, MTLC_DIAG_ERROR, "out of memory building temporary paths");
    return 0;
  }
#ifdef _WIN32
  snprintf(obj_path, base_len + 16, "%s.mtlcobj.obj", output_path);
  snprintf(startup_path, base_len + 24, "%s.mtlcstart.obj", output_path);
  snprintf(runtime_path, runtime_len + 32, "%s/freestanding.obj",
           runtime_directory);
#else
  snprintf(obj_path, base_len + 16, "%s.mtlcobj.o", output_path);
  snprintf(startup_path, base_len + 24, "%s.mtlcstart.o", output_path);
  snprintf(runtime_path, runtime_len + 32, "%s/freestanding.o",
           runtime_directory);
#endif

  int result = 0;
  if (!mtlc_emit_object(ctx, module, obj_path)) {
    goto done;
  }

  {
    FILE *runtime_file = fopen(runtime_path, "rb");
    if (!runtime_file) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR,
                "required freestanding runtime object not found at '%s'",
                runtime_path);
      goto done;
    }
    fclose(runtime_file);
  }

  if (binary_write_program_startup_object(startup_path, 0, 0,
                                           wants_argc_argv) != 0) {
    mtlc_diag(ctx, MTLC_DIAG_ERROR,
              "could not write the freestanding startup object");
    goto done;
  }

#ifdef _WIN32
  {
    const char *objects[3] = {startup_path, runtime_path, obj_path};
    /* Only the freestanding runtime contributes overridable defaults. */
    static const unsigned char objects_are_default[3] = {0u, 1u, 0u};
    result = link_pe_internal(ctx, objects, objects_are_default, 3, output_path);
  }
#else
  {
    const char *link_arguments[] = {
        "ld",        "--gc-sections", "-z",     "noseparate-code",
        "-e",        "_start",        "-o",     output_path,
        startup_path, runtime_path,     obj_path,  NULL,
    };
    result = mettle_run_process("ld", link_arguments) == 0;
    if (!result) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR,
                "the system linker failed to link '%s'", output_path);
    }
  }
#endif

  if (result) {
    char ownership_error[256];
    if (!mettle_verify_owned_executable(output_path, ownership_error,
                                         sizeof(ownership_error))) {
      mtlc_diag(ctx, MTLC_DIAG_ERROR,
                "refusing linked output '%s': %s", output_path,
                ownership_error);
      remove(output_path);
      result = 0;
    }
  }

done:
  remove(obj_path);
  remove(startup_path);
  free(obj_path);
  free(startup_path);
  free(runtime_path);
  return result;
}
