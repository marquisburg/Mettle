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

/* Index of the first instruction after each position that emits bytes, or the
 * instruction count when there is none.
 *
 * The optimizer retires an instruction by turning it into a NOP rather than
 * removing it, so an optimized function carries long runs of them. Walking such
 * a run per jump to decide fall-through is quadratic in a function with many
 * jumps. One backward pass answers it for every position. */
static size_t *code_generator_binary_build_next_emitting(
    const IRFunction *function) {
  size_t count = function->instruction_count;
  size_t *next = (size_t *)malloc((count ? count : 1) * sizeof(size_t));

  if (!next) {
    return NULL;
  }
  for (size_t i = count; i-- > 0;) {
    if (i + 1 >= count) {
      next[i] = count;
      continue;
    }
    IROpcode op = function->instructions[i + 1].op;
    next[i] = (op == IR_OP_LABEL || op == IR_OP_NOP ||
               op == IR_OP_DECLARE_LOCAL)
                  ? next[i + 1]
                  : i + 1;
  }
  return next;
}

/* Does the IR_OP_JUMP at `index` target the code that immediately follows it?
 *
 * Only instructions that emit no bytes may sit in between. Labels qualify (a
 * label is a name for a position); so do NOPs and local declarations, which the
 * emitter drops. Any other opcode means the jump really does skip something.
 *
 * Which is to say: the target's label sits after the jump and before the next
 * instruction that emits anything. */
static int code_generator_binary_jump_is_fallthrough(
    const IRFunction *function, size_t index, const BinaryLabelIndex *labels,
    const size_t *next_emitting) {
  const IRInstruction *jmp = &function->instructions[index];
  size_t target;

  if (!jmp->text || !jmp->text[0] || !labels || !next_emitting) {
    return 0;
  }
  target = binary_label_index_find(labels, jmp->text);
  return target != (size_t)-1 && target > index &&
         target < next_emitting[index];
}

typedef int (*BinaryPeepholeEmitter)(CodeGenerator *, BinaryFunctionContext *,
                                     const IRFunction *, size_t, size_t *);

static const BinaryPeepholeEmitter BINARY_PEEPHOLES[] = {
    code_generator_binary_try_emit_binary_compare_branch_chain,
    code_generator_binary_try_emit_compare_assign_diamond,
    code_generator_binary_try_emit_offset_scaled_address_load,
    code_generator_binary_try_emit_offset_scaled_address_store,
    code_generator_binary_try_emit_address_add_load,
    code_generator_binary_try_emit_address_add_store,
    code_generator_binary_try_emit_scaled_address_load,
    code_generator_binary_try_emit_scaled_address_store,
    code_generator_binary_try_emit_binary_cast_chain,
    code_generator_binary_try_emit_float_cast_binary_chain,
    code_generator_binary_try_emit_float_binary_expression_chain,
    code_generator_binary_try_emit_binary_expression_chain,
    code_generator_binary_try_emit_compare_branch_zero,
};

/* Loop-top alignment, matching what the MIR backend does for the functions it
 * takes: an IR label that some LATER branch jumps back to is a loop header, so
 * pad it onto a boundary. Without this a loop's speed depends on where its
 * function happened to land -- an unrelated change upstream that shifts the
 * function by a few bytes moves a hot loop across an instruction-fetch window
 * and swings it by tens of percent.
 *
 * The pad sits before the label, so the back-edge jumps past it and only a
 * fall-through into the loop decodes it, once. */
static char *binary_scan_loop_alignment(const IRFunction *ir_function,
                                        const BinaryLabelIndex *labels) {
  char *align_label = NULL;
  if (ir_function->instruction_count == 0) {
    return NULL;
  }
  align_label = (char *)calloc(ir_function->instruction_count, 1);
  if (!align_label) {
    return NULL;
  }
  for (size_t b = 0; b < ir_function->instruction_count; b++) {
    const IRInstruction *br = &ir_function->instructions[b];
    size_t d;
    if (br->op != IR_OP_JUMP && br->op != IR_OP_BRANCH_ZERO &&
        br->op != IR_OP_BRANCH_EQ) {
      continue;
    }
    if (!br->text || !br->text[0]) {
      continue;
    }
    /* Scanning the prefix for the target label costs the whole function for
     * every forward branch, which a body built out of if/else is made of. */
    d = binary_label_index_find(labels, br->text);
    if (d != (size_t)-1 && d < b) {
      /* 1 = align, 2 = align wider: the body from here to this back-edge is
       * big enough that the extra padding costs nothing next to it. The
       * FURTHEST back-edge decides, so a nested loop sharing a header is
       * measured at its full extent. */
      align_label[d] =
          (b - d >= BINARY_LOOP_BIG_IR_INSTRUCTIONS || align_label[d] == 2) ? 2
                                                                            : 1;
    }
  }
  return align_label;
}

static int binary_emit_aligned_label(CodeGenerator *generator,
                                     BinaryFunctionContext *context,
                                     char wanted) {
  if (wanted == 2) {
    context->wants_wide_loop_alignment = 1;
  }
  if (!binary_emit_align_code(
          &context->code,
          wanted == 2 ? BINARY_LOOP_ALIGN_BIG : BINARY_LOOP_ALIGN,
          wanted == 2 ? BINARY_LOOP_ALIGN_BIG_MAX_PAD
                      : BINARY_LOOP_ALIGN_MAX_PAD)) {
    code_generator_set_error(generator,
                             "Out of memory while aligning a loop header");
    return 0;
  }
  return 1;
}

static int binary_emit_location_marker(CodeGenerator *generator,
                                       BinaryFunctionContext *context,
                                       const IRInstruction *instruction) {
  if (!generator->debug_info || !generator->generate_stack_trace_support ||
      instruction->location.line <= 0) {
    return 1;
  }
  return code_generator_binary_emit_runtime_location_marker(
      generator, context, instruction->location.line,
      instruction->location.column,
      code_generator_runtime_filename(generator,
                                      instruction->location.filename));
}

static int binary_emit_function_body(CodeGenerator *generator,
                                     BinaryFunctionContext *context,
                                     const IRFunction *ir_function,
                                     const BinaryLabelIndex *labels,
                                     const size_t *next_emitting,
                                     const char *align_label, int annot,
                                     size_t annot_base) {
  size_t annot_prev_off = context->code.size;
  int annot_prev_idx = -1;

  for (size_t i = 0; i < ir_function->instruction_count;) {
    size_t consumed = 0;
    size_t peephole;
    int taken = 0;

    /* A jump to the code that immediately follows it is the fall-through it
     * would have taken anyway. The lowering emits one wherever a structured
     * statement ends by branching to its own exit label -- if/else arms, loop
     * bodies, short-circuits -- and each costs five bytes of instruction fetch
     * for nothing. Only zero-byte instructions may sit in between; anything
     * that emits code means the jump really is a jump. */
    if (ir_function->instructions[i].op == IR_OP_JUMP &&
        code_generator_binary_jump_is_fallthrough(ir_function, i, labels,
                                                  next_emitting)) {
      i++;
      continue;
    }
    if (align_label && align_label[i] &&
        !binary_emit_aligned_label(generator, context, align_label[i])) {
      if (annot) mir_annotate_end_function();
      return 0;
    }
    /* Lazily record the previous instruction's span now that it is fully
     * emitted; this is robust to the cascade's many `continue` paths because
     * every one returns here. */
    if (annot && annot_prev_idx >= 0 && context->code.size > annot_prev_off) {
      mir_annotate_record_ir(ir_function, annot_prev_idx,
                             annot_prev_off - annot_base,
                             context->code.size - annot_prev_off,
                             context->code.data + annot_prev_off);
    }
    annot_prev_off = context->code.size;
    annot_prev_idx = (int)i;
    /* Labels emit no bytes, so the lazy span recorder never sees them; record
     * a zero-byte marker so loop recovery can resolve backward-branch
     * targets. */
    if (annot && ir_function->instructions[i].op == IR_OP_LABEL) {
      mir_annotate_record_ir_label(ir_function->instructions[i].text,
                                   context->code.size - annot_base);
    }

    if (code_generator_binary_try_skip_scaled_address_shift(ir_function, i,
                                                            &consumed)) {
      i += consumed;
      continue;
    }
    for (peephole = 0;
         peephole < sizeof(BINARY_PEEPHOLES) / sizeof(BINARY_PEEPHOLES[0]);
         peephole++) {
      if (BINARY_PEEPHOLES[peephole](generator, context, ir_function, i,
                                     &consumed)) {
        taken = 1;
        break;
      }
    }
    if (taken) {
      i += consumed;
      continue;
    }

    if (!binary_emit_location_marker(generator, context,
                                     &ir_function->instructions[i])) {
      return 0;
    }
    if (!code_generator_binary_emit_instruction(
            generator, context, &ir_function->instructions[i])) {
      if (annot) mir_annotate_end_function();
      return 0;
    }
    i++;
  }

  /* Record the last instruction's span (no further loop top runs). */
  if (annot && annot_prev_idx >= 0 && context->code.size > annot_prev_off) {
    mir_annotate_record_ir(ir_function, annot_prev_idx,
                           annot_prev_off - annot_base,
                           context->code.size - annot_prev_off,
                           context->code.data + annot_prev_off);
  }
  if (annot) {
    mir_annotate_end_function();
  }
  return 1;
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

int code_generator_emit_binary_function(CodeGenerator *generator,
                                               IRFunction *ir_function) {
  BinaryEmitter *emitter = NULL;
  BinaryFunctionContext context = {0};
  size_t text_section = 0;
  BinarySection *section = NULL;
  size_t function_offset = 0;
  size_t return_offset = 0;

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

  double cg_t = cg_time_begin();
  if (!code_generator_binary_prepare_function_context(generator, ir_function,
                                                      &context)) {
    return 0;
  }
  cg_time_end("prepare_function_context", cg_t);

  free(generator->current_function_name);
  if (ir_function->name) {
    generator->current_function_name = strdup(ir_function->name);
    if (!generator->current_function_name) {
      code_generator_set_error(generator,
                               "Out of memory while tracking function name");
      binary_function_context_destroy(&context);
      return 0;
    }
  } else {
    generator->current_function_name = NULL;
  }
  generator->last_runtime_location_line = 0;
  generator->last_runtime_location_column = 0;

  if (!code_generator_binary_track_debug_range(generator, ir_function,
                                              &context)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  if (ir_function->is_naked) {
    if (!code_generator_emit_binary_naked_function(generator, ir_function,
                                                   &context)) {
      binary_function_context_destroy(&context);
      return 0;
    }
    return_offset = context.code.size;
    goto mir_shared_append;
  }

  /* Real mode and 32-bit protected mode have no 64-bit frame, no rip-relative
   * addressing and none of the register conventions the rest of this backend
   * assumes, so they get their own emitter rather than a flag threaded through
   * this one. */
  if (mtlc_target()->arch == MTLC_TARGET_ARCH_X86_16 ||
      mtlc_target()->arch == MTLC_TARGET_ARCH_X86_32) {
    if (!code_generator_emit_binary_function_x86_16(generator, ir_function,
                                                    &context)) {
      binary_function_context_destroy(&context);
      return 0;
    }
    return_offset = context.code.size;
    goto mir_shared_append;
  }

  /* Route fully-supported leaf integer functions through the MIR + linear-scan
   * register allocator The MIR path fills context.code
   * with a complete prologue..epilogue and resolves its own label fixups; all
   * downstream emission (.text append, relocations, debug symbols) is shared. */
  if (mir_function_is_eligible(generator, ir_function)) {
    cg_t = cg_time_begin();
    if (!code_generator_binary_emit_function_via_mir(generator,
                                                     ir_function, &context)) {
      binary_function_context_destroy(&context);
      return 0;
    }
    cg_time_end("emit_function_via_mir", cg_t);
    return_offset = context.code.size;
    goto mir_shared_append;
  }

  /* --annotate-asm: this function uses the BASELINE (fallback) backend, which
   * works at IR granularity. Open a capture context and record each IR
   * instruction's emitted byte span. The optimized idioms the MIR gate rejects
   * (vectorized kernels, the Fibonacci rotate) land here. */
  int annot = mir_annotate_enabled();
  size_t annot_base = context.code.size;
  if (annot) {
    mir_annotate_begin_function(
        ir_function->name, ir_function,
        ir_function->location.filename, ir_function->location.line);
    mir_annotate_note_backend("baseline (fallback)", NULL);
  }

  if (ir_function->is_interrupt &&
      !code_generator_binary_emit_interrupt_entry(generator, ir_function,
                                                  &context)) {
    if (annot) mir_annotate_end_function();
    binary_function_context_destroy(&context);
    return 0;
  }

  if (!code_generator_binary_emit_prologue(generator, &context)) {
    if (annot) mir_annotate_end_function();
    binary_function_context_destroy(&context);
    return 0;
  }
  if (annot && context.code.size > annot_base) {
    mir_annotate_record_synthetic("prologue", "frame", 0,
                                  context.code.size - annot_base,
                                  context.code.data + annot_base);
  }

  /* Loop-top alignment, matching what the MIR backend does for the functions it
   * takes: an IR label that some LATER branch jumps back to is a loop header,
   * so pad it onto a boundary. Without this a loop's speed depends on where its
   * function happened to land -- an unrelated change upstream that shifts the
   * function by a few bytes moves a hot loop across an instruction-fetch window
   * and swings it by tens of percent.
   *
   * The pad sits before the label, so the back-edge jumps past it and only a
   * fall-through into the loop decodes it, once. */
  cg_t = cg_time_begin();
  /* Both the alignment scan and the fall-through test below ask where a label
   * is; one index serves both, and lives until the emit loop is done. */
  BinaryLabelIndex labels;
  size_t *next_emitting = NULL;
  if (!binary_label_index_build(ir_function, &labels)) {
    if (annot) mir_annotate_end_function();
    binary_function_context_destroy(&context);
    return 0;
  }
  next_emitting = code_generator_binary_build_next_emitting(ir_function);
  if (!next_emitting) {
    binary_label_index_destroy(&labels);
    if (annot) mir_annotate_end_function();
    binary_function_context_destroy(&context);
    return 0;
  }
  char *align_label = NULL;
  align_label = binary_scan_loop_alignment(ir_function, &labels);

  cg_time_end("baseline align scan", cg_t);
  cg_t = cg_time_begin();
  if (!binary_emit_function_body(generator, &context, ir_function, &labels,
                                 next_emitting, align_label, annot,
                                 annot_base)) {
    free(align_label);
    binary_function_context_destroy(&context);
    return 0;
  }
  free(align_label);
  align_label = NULL;


  return_offset = context.code.size;
  if (context.runtime_end_label &&
      !binary_label_table_define(&context.labels, context.runtime_end_label,
                                 return_offset)) {
    code_generator_set_error(
        generator,
        "Failed to define runtime function end label in function '%s'",
        context.function_name);
    binary_function_context_destroy(&context);
    return 0;
  }

  if (!code_generator_binary_emit_promoted_global_stores(generator, &context)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  for (size_t i = context.saved_register_count; i > 0; i--) {
    size_t slot = i - 1;
    if (!binary_emit_mov_reg_mem(&context.code, context.saved_registers[slot],
                                 BINARY_GP_RBP,
                                 -context.saved_register_offsets[slot])) {
      code_generator_set_error(generator,
                               "Out of memory while restoring callee registers");
      binary_function_context_destroy(&context);
      return 0;
    }
  }

  if ((context.return_float_bits == 32 &&
       !binary_emit_movd_xmm_reg(&context.code, BINARY_XMM0,
                                 BINARY_GP_RAX)) ||
      (context.return_float_bits == 64 &&
       !binary_emit_movq_xmm_reg(&context.code, BINARY_XMM0,
                                 BINARY_GP_RAX)) ||
      !binary_emit_mov_reg_reg(&context.code, BINARY_GP_RSP, BINARY_GP_RBP) ||
      !binary_emit_pop_reg(&context.code, BINARY_GP_RBP) ||
      (!ir_function->is_interrupt && !binary_emit_ret(&context.code))) {
    code_generator_set_error(generator,
                             "Out of memory while emitting function epilogue");
    binary_function_context_destroy(&context);
    return 0;
  }

  if (ir_function->is_interrupt &&
      !code_generator_binary_emit_interrupt_exit(generator, ir_function,
                                                 &context)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  if (!code_generator_binary_resolve_fixups(generator, &context, return_offset)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  binary_label_index_destroy(&labels);
  free(next_emitting);
  cg_time_end("baseline emit loop", cg_t);
mir_shared_append:
  cg_t = cg_time_begin();
  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    binary_function_context_destroy(&context);
    return 0;
  }

  /* Each function gets its own text section (`.text$name` on COFF,
   * `.text.name` on ELF) so section GC, internal or ld's, can drop the ones
   * nothing reachable calls; the merged result lays out identically to one
   * shared .text. A COFF section count is a uint16, so enormous programs
   * fall back to the shared section. */
  {
    const char *text_name = ".text";
    char granular_name[512];
    uint32_t text_characteristics = 0;
    int granular_format =
        emitter->target_format == BINARY_TARGET_FORMAT_COFF_WIN64 ||
        emitter->target_format == BINARY_TARGET_FORMAT_ELF_X64;

    if (granular_format && emitter->section_count < 60000u &&
        strlen(ir_function->name) + 7u <= sizeof(granular_name)) {
      snprintf(granular_name, sizeof(granular_name),
               emitter->target_format == BINARY_TARGET_FORMAT_COFF_WIN64
                   ? ".text$%s"
                   : ".text.%s",
               ir_function->name);
      text_name = granular_name;
      if (emitter->target_format == BINARY_TARGET_FORMAT_COFF_WIN64 &&
          context.wants_wide_loop_alignment) {
        text_characteristics = 0x60000020u | 0x00600000u;
      }
    }
    text_section = binary_emitter_get_or_create_section(
        emitter, text_name, BINARY_SECTION_TEXT, text_characteristics,
        BINARY_TEXT_SECTION_ALIGNMENT);
  }
  if (text_section == (size_t)-1) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create .text section");
    binary_function_context_destroy(&context);
    return 0;
  }

  /* Aligning only the functions that asked for it, rather than all of them:
   * making every entry 32-byte aligned measured worse across the suite, because
   * the extra inter-function padding reshuffles everything downstream for the
   * benefit of functions that had no wide-aligned loop to protect. */
  if (!binary_emitter_align_section(emitter, text_section,
                                    context.wants_wide_loop_alignment
                                        ? (int)BINARY_LOOP_ALIGN_BIG
                                        : BINARY_TEXT_SECTION_ALIGNMENT,
                                    0x90)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to align .text section");
    binary_function_context_destroy(&context);
    return 0;
  }

  section = binary_emitter_get_section(emitter, text_section);
  if (!section) {
    code_generator_set_error(generator, "Failed to access .text section");
    binary_function_context_destroy(&context);
    return 0;
  }

  /* Only a function the outside can name gets a global symbol. An internal
   * one is local, so an ordinary name like `close` or `read` no longer
   * collides with the identically-named service the owned runtime exports for
   * the standard library to bind. Kernels and swappable functions are reached
   * by name from outside the object, so they stay global. */
  function_offset = section->size;
  BinarySymbolBinding function_binding =
      (!generator->whole_program ||
       code_generator_binary_function_is_abi_public(generator,
                                                    ir_function->name) ||
       ir_function->is_kernel || ir_function->is_swappable ||
       generator->generate_stack_trace_support || generator->debug_hooks)
          ? BINARY_SYMBOL_GLOBAL
          : BINARY_SYMBOL_LOCAL;
  if (!binary_emitter_define_symbol(emitter, ir_function->name,
                                    function_binding, text_section,
                                    function_offset, context.code.size) ||
      !binary_emitter_append_bytes(emitter, text_section, context.code.data,
                                   context.code.size, NULL)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to emit function machine code");
    binary_function_context_destroy(&context);
    return 0;
  }

  for (size_t i = 0; i < context.asm_relocations.count; i++) {
    BinaryAsmRelocation *relocation = &context.asm_relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section, function_offset + relocation->offset,
            (BinaryRelocationKind)relocation->kind, relocation->symbol_name,
            relocation->addend)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to record an asm relocation");
      binary_function_context_destroy(&context);
      return 0;
    }
  }

  for (size_t i = 0; i < context.call_relocations.count; i++) {
    BinaryCallRelocation *relocation = &context.call_relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section,
            function_offset + relocation->displacement_offset,
            BINARY_RELOCATION_REL32, relocation->symbol_name, 0)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to record function relocation");
      binary_function_context_destroy(&context);
      return 0;
    }
  }

  if (!code_generator_binary_export_debug_symbols(generator, &context,
                                                  text_section, function_offset,
                                                  return_offset)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  binary_function_context_destroy(&context);
  cg_time_end("shared append", cg_t);
  return 1;
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
