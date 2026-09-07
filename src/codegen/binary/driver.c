#include "codegen/binary/internal.h"
#include "codegen/target.h"
#include "codegen/binary/mir.h"
#include "codegen/binary/mir_annotate.h"
#include "ir/ir_pgo.h"
#include <limits.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#define CG_TIME_MAX 48
static struct {
  const char *name;
  double ticks;
  unsigned long long runs;
} g_cg_times[CG_TIME_MAX];
static size_t g_cg_time_count;

int cg_time_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_TIME_CODEGEN") ? 1 : 0;
  }
  return cached;
}

double cg_time_begin(void) {
  return cg_time_enabled() ? (double)clock() : 0.0;
}

void cg_time_end(const char *name, double started) {
  double spent;
  if (!cg_time_enabled() || !name) {
    return;
  }
  spent = (double)clock() - started;
  for (size_t i = 0; i < g_cg_time_count; i++) {
    if (g_cg_times[i].name == name ||
        strcmp(g_cg_times[i].name, name) == 0) {
      g_cg_times[i].ticks += spent;
      g_cg_times[i].runs++;
      return;
    }
  }
  if (g_cg_time_count < CG_TIME_MAX) {
    g_cg_times[g_cg_time_count].name = name;
    g_cg_times[g_cg_time_count].ticks = spent;
    g_cg_times[g_cg_time_count].runs = 1;
    g_cg_time_count++;
  }
}

void cg_time_report(void) {
  if (!cg_time_enabled()) {
    return;
  }
  fprintf(stderr, "-- codegen stage times (cumulative clock() ticks) --\n");
  for (size_t dumped = 0; dumped < g_cg_time_count; dumped++) {
    double best = -1.0;
    size_t pick = g_cg_time_count;
    for (size_t i = 0; i < g_cg_time_count; i++) {
      if (g_cg_times[i].ticks > best) {
        best = g_cg_times[i].ticks;
        pick = i;
      }
    }
    if (pick == g_cg_time_count) {
      break;
    }
    fprintf(stderr, "  %-34s %12.0f  (%llu runs)\n", g_cg_times[pick].name,
            g_cg_times[pick].ticks, g_cg_times[pick].runs);
    g_cg_times[pick].ticks = -2.0;
  }
}

static int code_generator_binary_track_debug_range(
    CodeGenerator *generator, IRFunction *ir_function,
    BinaryFunctionContext *context) {
  if (!generator->debug_info || (!generator->generate_stack_trace_support &&
                                 !generator->generate_crash_report)) {
    return 1;
  }
  context->runtime_end_label =
      code_generator_generate_label(generator, "mettledbg_func_end");
  if (!context->runtime_end_label) {
    code_generator_set_error(generator,
                             "Out of memory while tracking function debug "
                             "range in '%s'",
                             ir_function->name);
    return 0;
  }
  code_generator_add_runtime_function_mapping(
      generator, ir_function->name, ir_function->name,
      context->runtime_end_label,
      ir_function->location.line, ir_function->location.column,
      code_generator_runtime_filename(generator,
                                      ir_function->location.filename));
  return 1;
}

static int binary_bind_current_function(CodeGenerator *generator,
                                        const IRFunction *ir_function) {
  free(generator->current_function_name);
  generator->current_function_name = NULL;
  if (ir_function->name) {
    generator->current_function_name = strdup(ir_function->name);
    if (!generator->current_function_name) {
      code_generator_set_error(generator,
                               "Out of memory while tracking function name");
      return 0;
    }
  }
  generator->last_runtime_location_line = 0;
  generator->last_runtime_location_column = 0;
  return 1;
}

static int binary_emit_function_code(CodeGenerator *generator,
                                     IRFunction *ir_function,
                                     BinaryFunctionContext *context) {
  double started;

  if (ir_function->is_naked) {
    return code_generator_emit_binary_naked_function(generator, ir_function,
                                                     context);
  }
  if (mtlc_target()->arch == MTLC_TARGET_ARCH_X86_16 ||
      mtlc_target()->arch == MTLC_TARGET_ARCH_X86_32) {
    return code_generator_emit_binary_function_x86_16(generator, ir_function,
                                                      context);
  }
  started = cg_time_begin();
  if (!code_generator_binary_emit_function_via_mir(generator, ir_function,
                                                   context)) {
    return 0;
  }
  cg_time_end("emit_function_via_mir", started);
  return 1;
}

static int binary_emitter_failed(CodeGenerator *generator,
                                 BinaryEmitter *emitter,
                                 const char *fallback_message) {
  const char *reported = binary_emitter_get_error(emitter);
  code_generator_set_error(generator, "%s",
                           reported ? reported : fallback_message);
  return 0;
}

static size_t binary_function_text_section(
    BinaryEmitter *emitter, const IRFunction *ir_function,
    const BinaryFunctionContext *context) {
  const char *text_name = ".text";
  char granular_name[512];
  uint32_t characteristics = 0;
  int coff = emitter->target_format == BINARY_TARGET_FORMAT_COFF_WIN64;
  int granular = coff ||
                 emitter->target_format == BINARY_TARGET_FORMAT_ELF_X64;

  if (granular && emitter->section_count < 60000u &&
      strlen(ir_function->name) + 7u <= sizeof(granular_name)) {
    snprintf(granular_name, sizeof(granular_name),
             coff ? ".text$%s" : ".text.%s", ir_function->name);
    text_name = granular_name;
    if (coff && context->wants_wide_loop_alignment) {
      characteristics = 0x60000020u | 0x00600000u;
    }
  }
  return binary_emitter_get_or_create_section(emitter, text_name,
                                              BINARY_SECTION_TEXT,
                                              characteristics,
                                              BINARY_TEXT_SECTION_ALIGNMENT);
}

static BinarySymbolBinding binary_function_symbol_binding(
    CodeGenerator *generator, const IRFunction *ir_function) {
  int reachable_by_name =
      !generator->whole_program ||
      code_generator_binary_function_is_abi_public(generator,
                                                   ir_function->name) ||
      ir_function->is_kernel || ir_function->is_swappable ||
      generator->generate_stack_trace_support || generator->debug_hooks;
  return reachable_by_name ? BINARY_SYMBOL_GLOBAL : BINARY_SYMBOL_LOCAL;
}

static int binary_record_function_relocations(
    CodeGenerator *generator, BinaryEmitter *emitter,
    const BinaryFunctionContext *context, size_t text_section,
    size_t function_offset) {
  for (size_t i = 0; i < context->asm_relocations.count; i++) {
    const BinaryAsmRelocation *relocation = &context->asm_relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section, function_offset + relocation->offset,
            (BinaryRelocationKind)relocation->kind, relocation->symbol_name,
            relocation->addend)) {
      return binary_emitter_failed(generator, emitter,
                                   "Failed to record an asm relocation");
    }
  }
  for (size_t i = 0; i < context->call_relocations.count; i++) {
    const BinaryCallRelocation *relocation =
        &context->call_relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section,
            function_offset + relocation->displacement_offset,
            BINARY_RELOCATION_REL32, relocation->symbol_name, 0)) {
      return binary_emitter_failed(generator, emitter,
                                   "Failed to record function relocation");
    }
  }
  return 1;
}

static int binary_append_function(CodeGenerator *generator,
                                  IRFunction *ir_function,
                                  BinaryFunctionContext *context) {
  BinaryEmitter *emitter = code_generator_get_binary_emitter(generator);
  BinarySection *section = NULL;
  size_t text_section = 0;
  size_t function_offset = 0;

  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }
  text_section = binary_function_text_section(emitter, ir_function, context);
  if (text_section == (size_t)-1) {
    return binary_emitter_failed(generator, emitter,
                                 "Failed to create .text section");
  }
  if (!binary_emitter_align_section(emitter, text_section,
                                    context->wants_wide_loop_alignment
                                        ? (int)BINARY_LOOP_ALIGN_BIG
                                        : BINARY_TEXT_SECTION_ALIGNMENT,
                                    0x90)) {
    return binary_emitter_failed(generator, emitter,
                                 "Failed to align .text section");
  }
  section = binary_emitter_get_section(emitter, text_section);
  if (!section) {
    code_generator_set_error(generator, "Failed to access .text section");
    return 0;
  }
  function_offset = section->size;
  if (!binary_emitter_define_symbol(
          emitter, ir_function->name,
          binary_function_symbol_binding(generator, ir_function), text_section,
          function_offset, context->code.size) ||
      !binary_emitter_append_bytes(emitter, text_section, context->code.data,
                                   context->code.size, NULL)) {
    return binary_emitter_failed(generator, emitter,
                                 "Failed to emit function machine code");
  }
  if (!binary_record_function_relocations(generator, emitter, context,
                                          text_section, function_offset)) {
    return 0;
  }
  return code_generator_binary_export_debug_symbols(
      generator, context, text_section, function_offset, context->code.size);
}

int code_generator_emit_binary_function(CodeGenerator *generator,
                                        IRFunction *ir_function) {
  BinaryFunctionContext context = {0};
  double started;
  int ok;

  if (!generator || !ir_function) {
    return 0;
  }
  if (!code_generator_binary_validate_signature(generator, ir_function)) {
    return 0;
  }
  if (!mir_rewrite_string_concat_calls(ir_function)) {
    code_generator_set_error(generator,
                             "Out of memory while lowering string concat in "
                             "'%s'",
                             ir_function->name);
    return 0;
  }

  started = cg_time_begin();
  if (!code_generator_binary_prepare_function_context(generator, ir_function,
                                                      &context)) {
    return 0;
  }
  cg_time_end("prepare_function_context", started);

  if (!binary_bind_current_function(generator, ir_function) ||
      !code_generator_binary_track_debug_range(generator, ir_function,
                                               &context) ||
      !binary_emit_function_code(generator, ir_function, &context)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  started = cg_time_begin();
  ok = binary_append_function(generator, ir_function, &context);
  cg_time_end("shared append", started);
  binary_function_context_destroy(&context);
  return ok;
}

static size_t *code_generator_binary_pgo_emit_order(
    CodeGenerator *generator, size_t function_count) {
  size_t *emit_order = NULL;

if (ir_pgo_enabled() && function_count > 1) {
  emit_order = (size_t *)malloc(function_count * sizeof(size_t));
  long long *heat = (long long *)malloc(function_count * sizeof(long long));
  if (emit_order && heat) {
    for (size_t i = 0; i < function_count; i++) {
      emit_order[i] = i;
      IRFunction *fn = generator->ir_program->functions[i];
      heat[i] = (fn && fn->name && strcmp(fn->name, "main") == 0)
                    ? LLONG_MAX
                    : (fn && fn->name ? ir_pgo_callee_calls(fn->name) : 0);
    }
    /* Stable insertion sort of the function indices by heat, descending:
     * ties (and cold/-1) keep declaration order. */
    for (size_t i = 1; i < function_count; i++) {
      size_t slot = emit_order[i];
      long long h = heat[i];
      size_t j = i;
      while (j > 0 && heat[j - 1] < h) {
        emit_order[j] = emit_order[j - 1];
        heat[j] = heat[j - 1];
        j--;
      }
      emit_order[j] = slot;
      heat[j] = h;
    }
  } else {
    free(emit_order);
    emit_order = NULL;
  }
  free(heat);
}
  return emit_order;
}

int code_generator_generate_program_binary_object(CodeGenerator *generator) {
  if (!generator) {
    return 0;
  }
  if (!generator->ir_program) {
    code_generator_set_error(generator,
                             "IR program not attached to code generator");
    return 0;
  }
  /* Pin the calling convention to the target object format before emitting any
   * code: COFF -> MS-x64, ELF -> SysV. */
  code_generator_binary_select_abi(generator->binary_emitter->target_format);

  binary_emitter_reset(generator->binary_emitter);

  binary_global_const_table_reset();
  binary_ir_function_index_reset();
  if (!code_generator_binary_collect_global_constants(generator)) {
    return 0;
  }

  if (!code_generator_declare_binary_externs(generator)) {
    return 0;
  }

  /* --pgo code layout: emit measured-hot functions first (main leading) so
   * the hot working set shares I-cache lines and iTLB pages, cold glue sinks
   * to the tail. Zero-run: the frequencies come from the compile-time
   * interpretation of main(), no training run. Without a profile the order is
   * untouched. */
  size_t function_count = generator->ir_program->function_count;
  size_t *emit_order =
      code_generator_binary_pgo_emit_order(generator, function_count);

  for (size_t i = 0; i < function_count; i++) {
    IRFunction *ir_function =
        generator->ir_program->functions[emit_order ? emit_order[i] : i];
    if (!ir_function || ir_function->is_rule) {
      continue;
    }
    if (!code_generator_emit_binary_function(generator, ir_function)) {
      return 0;
    }
  }
  free(emit_order);
  cg_time_report();

  /* Global variables: an integer `const` folds to a CG_SYM_CONSTANT at every
   * use site and carries no storage (IR_MODSYM_CONSTANT, not represented
   * here); a non-integer `const` (float/string/aggregate) is registered as an
   * immutable variable instead and DOES need storage, since the IR references
   * it via a RIP-relative load like any global (IR_MODSYM_VARIABLE). */
  for (size_t i = 0; i < generator->ir_program->module_symbol_count; i++) {
    const IRModuleSymbol *sym = &generator->ir_program->module_symbols[i];
    if (sym->kind != IR_MODSYM_VARIABLE || sym->is_extern) {
      continue;
    }
    if (!code_generator_emit_binary_global_variable(generator, sym)) {
      return 0;
    }
  }

  if ((generator->profile_runtime || generator->debug_hooks) &&
      !code_generator_binary_emit_profile_tables(generator)) {
    return 0;
  }

  if ((generator->generate_stack_trace_support ||
       generator->generate_crash_report) &&
      !code_generator_binary_emit_runtime_debug_tables(generator)) {
    return 0;
  }

  if ((generator->generate_stack_trace_support ||
       generator->generate_crash_report) &&
      !code_generator_binary_emit_crash_startup(generator)) {
    return 0;
  }

  if ((generator->generate_stack_trace_support ||
       generator->generate_crash_report || generator->profile_runtime) &&
      !code_generator_binary_emit_elf_runtime_hooks(generator)) {
    return 0;
  }

  if (generator->generate_debug_info &&
      !code_generator_binary_emit_dwarf_debug_sections(generator)) {
    return 0;
  }

  int ok = generator->has_error ? 0 : 1;
  binary_global_const_table_reset();
  binary_ir_function_index_reset();
  return ok;
}
