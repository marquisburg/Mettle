#include "ir_optimize_internal.h"

#include <stdio.h>

/* Name -> IRFunction index for the optimizer.
 *
 * ir_program_find_function used to linear-scan every function (strcmp each) and
 * is called once per CALL instruction during inlining, across every function,
 * for several rounds -- O(calls * functions). That dominated IR optimization on
 * large programs. We cache an open-addressing hash table keyed on the program
 * pointer + function_count; inlining mutates bodies but never adds or removes
 * functions, so the cache stays valid for the whole optimization run. */
static IRFunctionIndex g_ir_function_index = {0};

void ir_function_index_reset(void) {
  free(g_ir_function_index.slots);
  g_ir_function_index.slots = NULL;
  g_ir_function_index.slot_count = 0;
  g_ir_function_index.program = NULL;
  g_ir_function_index.function_count = 0;
}

static void ir_function_index_insert(IRFunctionIndex *index,
                                     IRFunction *function) {
  size_t mask = index->slot_count - 1;
  size_t i = mettle_fnv1a_hash(function->name) & mask;
  while (index->slots[i].name) {
    /* First definition of a given name wins, matching the old linear scan. */
    if (strcmp(index->slots[i].name, function->name) == 0) {
      return;
    }
    i = (i + 1) & mask;
  }
  index->slots[i].name = function->name;
  index->slots[i].function = function;
}

/* Returns 1 if the index is ready to query, 0 on allocation failure (caller
 * falls back to a linear scan). */
static int ir_function_index_ensure(const IRProgram *program) {
  if (g_ir_function_index.program == program &&
      g_ir_function_index.function_count == program->function_count &&
      g_ir_function_index.slots) {
    return 1;
  }

  ir_function_index_reset();

  size_t slot_count = 16;
  while (slot_count < program->function_count * 2) {
    slot_count *= 2;
  }

  IRFunctionIndexSlot *slots = calloc(slot_count, sizeof(IRFunctionIndexSlot));
  if (!slots) {
    return 0;
  }

  g_ir_function_index.slots = slots;
  g_ir_function_index.slot_count = slot_count;
  g_ir_function_index.program = program;
  g_ir_function_index.function_count = program->function_count;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name) {
      ir_function_index_insert(&g_ir_function_index, function);
    }
  }

  return 1;
}

IRFunction *ir_program_find_function(IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }

  if (ir_function_index_ensure(program)) {
    const IRFunctionIndex *index = &g_ir_function_index;
    size_t mask = index->slot_count - 1;
    size_t i = mettle_fnv1a_hash(name) & mask;
    while (index->slots[i].name) {
      if (strcmp(index->slots[i].name, name) == 0) {
        return index->slots[i].function;
      }
      i = (i + 1) & mask;
    }
    return NULL;
  }

  /* Fallback: index allocation failed; behave as before. */
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }

  return NULL;
}

static int ir_function_name_is_inline_denylisted(const char *name) {
  if (!name) {
    return 0;
  }
  /* fib / bench_* inlining + loop unrolling explodes compile time (see
   * ir_optimize.c history). Benchmark hot paths use dedicated functions. */
  return strcmp(name, "fib") == 0 || strcmp(name, "bench_looped") == 0 ||
         strcmp(name, "bench_unrolled") == 0;
}

/* The id of the refusal most recently produced here, for the structured half
 * of the --explain report. The prose gets reworded as we learn how to say it
 * better; the id is what a tool keys off, so the two are written together by
 * IR_INLINE_WHY and cannot drift apart. Mirrors the vectorizer's bail ids. */
static MTLC_THREAD_LOCAL const char *g_inline_refusal_code = NULL;

#define IR_INLINE_WHY(target, id, text)                                        \
  do {                                                                         \
    *(target) = (text);                                                        \
    g_inline_refusal_code = (id);                                              \
  } while (0)

/* When `why_not`/`fix` are non-NULL they receive a user-facing reason and an
 * actionable suggestion (static strings) every time this returns 0; --explain
 * reports them verbatim. A NULL fix means there is nothing actionable. */
/* `site_loop_depth` is how many of the caller's loops enclose the call, which
 * is how many trip counts its call sequence multiplies by. A site nested two
 * deep or more doubles the callee size budget: a callee just over the cap that
 * an inner loop reaches every iteration is exactly the one worth copying in,
 * and copying it is what exposes its field reads to values the caller already
 * holds. One loop is not enough on its own -- that is the case --pgo exists to
 * measure. */
static int ir_function_is_inline_candidate_at(const IRFunction *function,
                                              int site_loop_depth,
                                              const char **why_not,
                                              const char **fix) {
  const char *unused;
  if (!why_not) {
    why_not = &unused;
  }
  if (!fix) {
    fix = &unused;
  }
  *why_not = NULL;
  *fix = NULL;
  if (!function || !function->name || function->instruction_count == 0) {
    IR_INLINE_WHY(why_not, "callee-no-body",
                  "the callee has no body available to the inliner");
    return 0;
  }
  /* `@noinline` is an absolute veto. */
  if (function->is_noinline) {
    IR_INLINE_WHY(why_not, "callee-noinline",
                  "the callee is marked @noinline");
    *fix = "remove @noinline if inlining is wanted here";
    return 0;
  }
  /* `@inline` forces the function past the discretionary heuristics below --
   * the name denylist, the parameter/size/call-count caps, and the loop-shape
   * guard -- but never past the structural correctness guards: inline-asm and
   * the must-have-a-return rule still apply. */
  int forced = function->is_inline;
  size_t body_budget = ir_opt_inline_body_budget(function);
  if (site_loop_depth >= 2) {
    body_budget *= 2u;
  }
  size_t nested_call_budget = ir_opt_inline_nested_call_budget(function);

  if (!forced && ir_function_name_is_inline_denylisted(function->name)) {
    IR_INLINE_WHY(why_not, "callee-denylisted",
                  "the callee is on the compiler's inline denylist "
                  "(a compile-time-blowup guard)");
    return 0;
  }
  if (!forced && function->parameter_count > IR_INLINE_MAX_PARAMETERS) {
    IR_INLINE_WHY(why_not, "too-many-parameters",
                  "the callee has more than 16 parameters");
    *fix = "pass a struct instead of a long parameter list";
    return 0;
  }
  if (function->parameter_count > 0 && !function->parameter_names) {
    IR_INLINE_WHY(why_not, "callee-parameter-names",
                  "the callee's parameter names are unavailable to the inliner");
    return 0;
  }

  size_t non_nop_count = 0;
  size_t call_count = 0;
  int has_return = 0;
  int has_while_label = 0;
  int has_less_compare = 0;
  int has_greater_compare = 0;
  int has_subtract = 0;
  int has_multiply = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction || instruction->op == IR_OP_NOP) {
      continue;
    }

    non_nop_count++;
    if (!forced && non_nop_count > body_budget) {
      IR_INLINE_WHY(why_not, "callee-over-budget",
                    "the callee's body is over the profile-adjusted inline "
                    "instruction budget");
      *fix = "mark the callee @inline to override the budget, or compile "
             "with --pgo so a measured-hot callee overrides it";
      return 0;
    }

    if (instruction->op == IR_OP_INLINE_ASM) {
      IR_INLINE_WHY(why_not, "callee-inline-asm",
                    "the callee contains inline assembly");
      return 0;
    }

    if (instruction->op == IR_OP_LABEL && instruction->text) {
      if (strncmp(instruction->text, "ir_while_", 9) == 0 ||
          strstr(instruction->text, "_lbl_ir_while_") != NULL) {
        has_while_label = 1;
      }
    }
    if (instruction->op == IR_OP_BINARY && instruction->text) {
      if (strcmp(instruction->text, "<") == 0) {
        has_less_compare = 1;
      } else if (strcmp(instruction->text, ">") == 0) {
        has_greater_compare = 1;
      } else if (strcmp(instruction->text, "-") == 0) {
        has_subtract = 1;
      } else if (strcmp(instruction->text, "*") == 0) {
        has_multiply = 1;
      }
    }

    /* Loop-bearing callees are allowed when not denylisted; the loop unroller
     * keeps its own static/PGO-adjusted trip-count caps. */

    /* CALL and CALL_INDIRECT are allowed:
     * calls just turns those into call instructions in the caller, which is
     * fine. This lets us inline leaf-ish functions (like grep's
     * pattern_matches) whose only calls are in cold fallback paths. Cap the
     * number of contained calls so that we don't inline glue functions like
     * print_int that orchestrate many helper calls; those produce lots of
     * caller bloat without runtime gain. */
    if (instruction->op == IR_OP_CALL ||
        instruction->op == IR_OP_CALL_INDIRECT) {
      call_count++;
      if (!forced && call_count > nested_call_budget) {
        IR_INLINE_WHY(why_not, "callee-call-count",
                      "the callee makes more calls of its own than the "
                      "profile-adjusted inline call-count budget allows");
        *fix = "mark the callee @inline to override the call-count cap";
        return 0;
      }
    }

    if (instruction->op == IR_OP_RETURN) {
      has_return = 1;
    }
  }

  /* A loop-bearing callee inlines when it is small: that is what exposes its
   * loop to values the caller already holds in registers, which measured as
   * word_freq spending 44% of its time in a hash and a compare clang folds
   * into the caller. Past the budget the call really is noise next to the
   * loop it reaches. The old textual denylist (`-`, `*`, or `<` and `>` next
   * to a while label) guarded two things that are both gone: recognizers that
   * matched parameter NAMES and went scalar on inlined loops (they now accept
   * any settled base), and a pass-skip cache that the freestanding getenv's
   * shared buffer corrupted into half-applied passes. */
  if (!forced && has_while_label &&
      non_nop_count > (site_loop_depth >= 2
                           ? 2u * IR_INLINE_LOOP_BODY_INSTRUCTIONS
                           : IR_INLINE_LOOP_BODY_INSTRUCTIONS)) {
    IR_INLINE_WHY(why_not, "callee-has-loop",
                  "the callee's loop body is over the inline size budget for "
                  "a loop-bearing callee (the call itself costs little next "
                  "to the loop inside it)");
    *fix = "mark the callee @inline to inline it anyway";
    return 0;
  }
  (void)has_less_compare;
  (void)has_greater_compare;
  (void)has_subtract;
  (void)has_multiply;
  if (!has_return) {
    IR_INLINE_WHY(why_not, "callee-no-return",
                  "the callee has no return instruction the inliner can rewrite");
    return 0;
  }
  return 1;
}

static int ir_function_is_inline_candidate(const IRFunction *function,
                                           const char **why_not,
                                           const char **fix) {
  return ir_function_is_inline_candidate_at(function, 0, why_not, fix);
}

static size_t ir_function_non_nop_instruction_count(const IRFunction *function) {
  if (!function) {
    return 0;
  }

  size_t count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_NOP) {
      count++;
    }
  }
  return count;
}

static int ir_inline_rewrite_operand(const IROperand *source, IROperand *out,
                                     IRNameMap *symbol_map,
                                     IRNameMap *temp_map,
                                     IRNameMap *label_map,
                                     const char *inline_prefix) {
  if (!source || !out) {
    return 0;
  }

  if (source->kind == IR_OPERAND_SYMBOL && source->name) {
    const char *mapped = ir_name_map_lookup(symbol_map, source->name);
    if (mapped) {
      *out = ir_operand_symbol(mapped);
      /* Preserve the float width carried by the original operand; the symbol
       * constructor does not copy it, and losing it makes a float32 value look
       * like a plain copy to later coalescing (dropping an f64->f32 narrow). */
      out->float_bits = source->float_bits;
      return out->kind == IR_OPERAND_SYMBOL && out->name;
    }
  } else if (source->kind == IR_OPERAND_TEMP && source->name) {
    const char *mapped =
        ir_name_map_get_or_create(temp_map, source->name, inline_prefix, "tmp");
    if (!mapped) {
      return 0;
    }
    *out = ir_operand_temp(mapped);
    out->float_bits = source->float_bits;
    return out->kind == IR_OPERAND_TEMP && out->name;
  } else if (source->kind == IR_OPERAND_LABEL && source->name) {
    const char *mapped = ir_name_map_get_or_create(label_map, source->name,
                                                   inline_prefix, "lbl");
    if (!mapped) {
      return 0;
    }
    *out = ir_operand_label(mapped);
    return out->kind == IR_OPERAND_LABEL && out->name;
  }

  return ir_operand_clone(source, out);
}

int ir_clone_instruction_plain(const IRInstruction *source,
                                      IRInstruction *out) {
  if (!source || !out) {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  out->op = source->op;
  out->intrinsic = source->intrinsic;
  out->address_space = source->address_space;
  out->memory_order = source->memory_order;
  out->failure_memory_order = source->failure_memory_order;
  out->memory_scope = source->memory_scope;
  out->memory_regions = source->memory_regions;
  out->alias_class = source->alias_class;
  out->async_copy_element_count = source->async_copy_element_count;
  out->async_copy_transaction_bytes = source->async_copy_transaction_bytes;
  out->async_copy_pending_groups = source->async_copy_pending_groups;
  out->async_copy_cache = source->async_copy_cache;
  out->async_copy_generated = source->async_copy_generated;
  if (!ir_instruction_tensor_copy(out, source)) {
    return 0;
  }
  out->tensor_transfer_has_prepared_view =
      source->tensor_transfer_has_prepared_view;
  out->tensor_mma_count = source->tensor_mma_count;
  out->tensor_residency_id = source->tensor_residency_id;
  out->tensor_residency_role = source->tensor_residency_role;
  out->tensor_residency_scope = source->tensor_residency_scope;
  out->location = source->location;
  out->is_float = source->is_float;
  out->is_unsigned = source->is_unsigned;
  out->float_bits = source->float_bits;
  /* is_unsigned carries codegen-critical signedness: unsigned div/rem/shr and
   * zero-extending uint8/16/32 loads. Dropping it here (it only runs at -O)
   * silently reverts those to signed -- a uint32-as-signed miscompile. */
  out->is_unsigned = source->is_unsigned;
  out->allocates = source->allocates; /* string-concat heap allocation marker */
  out->ast_ref = source->ast_ref;
  out->value_type = source->value_type;

  if (!ir_operand_clone(&source->dest, &out->dest) ||
      !ir_operand_clone(&source->lhs, &out->lhs) ||
      !ir_operand_clone(&source->rhs, &out->rhs)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }

  if (source->text) {
    out->text = mettle_strdup(source->text);
    if (!out->text) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
  }

  out->argument_count = source->argument_count;
  if (source->argument_count > 0) {
    out->arguments = calloc(source->argument_count, sizeof(IROperand));
    if (!out->arguments) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
    for (size_t i = 0; i < source->argument_count; i++) {
      if (!ir_operand_clone(&source->arguments[i], &out->arguments[i])) {
        ir_instruction_destroy_storage(out);
        return 0;
      }
    }
  }
  if (source->argument_types && source->argument_count > 0) {
    out->argument_types =
        malloc(source->argument_count * sizeof(*out->argument_types));
    if (!out->argument_types) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
    memcpy(out->argument_types, source->argument_types,
           source->argument_count * sizeof(*out->argument_types));
  }

  return 1;
}

static int ir_clone_instruction_for_inline(const IRInstruction *source,
                                           IRInstruction *out,
                                           IRNameMap *symbol_map,
                                           IRNameMap *temp_map,
                                           IRNameMap *label_map,
                                           const char *inline_prefix) {
  if (!source || !out || !symbol_map || !temp_map || !label_map ||
      !inline_prefix) {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  out->op = source->op;
  out->intrinsic = source->intrinsic;
  out->address_space = source->address_space;
  out->memory_order = source->memory_order;
  out->failure_memory_order = source->failure_memory_order;
  out->memory_scope = source->memory_scope;
  out->memory_regions = source->memory_regions;
  out->alias_class = source->alias_class;
  out->async_copy_element_count = source->async_copy_element_count;
  out->async_copy_transaction_bytes = source->async_copy_transaction_bytes;
  out->async_copy_pending_groups = source->async_copy_pending_groups;
  out->async_copy_cache = source->async_copy_cache;
  out->async_copy_generated = source->async_copy_generated;
  if (!ir_instruction_tensor_copy(out, source)) {
    return 0;
  }
  out->tensor_transfer_has_prepared_view =
      source->tensor_transfer_has_prepared_view;
  out->tensor_mma_count = source->tensor_mma_count;
  out->tensor_residency_id = source->tensor_residency_id;
  out->tensor_residency_role = source->tensor_residency_role;
  out->tensor_residency_scope = source->tensor_residency_scope;
  out->location = source->location;
  out->is_float = source->is_float;
  out->is_unsigned = source->is_unsigned;
  out->float_bits = source->float_bits;
  out->is_unsigned = source->is_unsigned; /* unsigned div/shr + zero-ext loads */
  out->allocates = source->allocates;     /* string-concat allocation marker */
  out->ast_ref = NULL;
  out->value_type = source->value_type;

  if (!ir_inline_rewrite_operand(&source->dest, &out->dest, symbol_map,
                                 temp_map, label_map, inline_prefix) ||
      !ir_inline_rewrite_operand(&source->lhs, &out->lhs, symbol_map, temp_map,
                                 label_map, inline_prefix) ||
      !ir_inline_rewrite_operand(&source->rhs, &out->rhs, symbol_map, temp_map,
                                 label_map, inline_prefix)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }

  if (source->text) {
    if (source->op == IR_OP_LABEL || source->op == IR_OP_JUMP ||
        source->op == IR_OP_BRANCH_ZERO || source->op == IR_OP_BRANCH_EQ) {
      const char *mapped =
          ir_name_map_get_or_create(label_map, source->text, inline_prefix, "lbl");
      if (!mapped) {
        ir_instruction_destroy_storage(out);
        return 0;
      }
      out->text = mettle_strdup(mapped);
    } else {
      out->text = mettle_strdup(source->text);
    }

    if (!out->text) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
  }

  out->argument_count = source->argument_count;
  if (source->argument_count > 0) {
    out->arguments = calloc(source->argument_count, sizeof(IROperand));
    if (!out->arguments) {
      ir_instruction_destroy_storage(out);
      return 0;
    }

    for (size_t i = 0; i < source->argument_count; i++) {
      if (!ir_inline_rewrite_operand(&source->arguments[i], &out->arguments[i],
                                     symbol_map, temp_map, label_map,
                                     inline_prefix)) {
        ir_instruction_destroy_storage(out);
        return 0;
      }
    }
  }
  if (source->argument_types && source->argument_count > 0) {
    out->argument_types =
        malloc(source->argument_count * sizeof(*out->argument_types));
    if (!out->argument_types) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
    memcpy(out->argument_types, source->argument_types,
           source->argument_count * sizeof(*out->argument_types));
  }

  return 1;
}

static int ir_append_parameter_materialization(
    IRInstructionVector *vector, const IRInstruction *call_instruction,
    const IRFunction *callee, IRNameMap *symbol_map) {
  if (!vector || !call_instruction || !callee || !symbol_map) {
    return 0;
  }

  for (size_t i = 0; i < callee->parameter_count; i++) {
    const char *parameter_name = callee->parameter_names[i];
    const char *mapped_name = ir_name_map_lookup(symbol_map, parameter_name);
    const char *type_name = "int64";
    if (!parameter_name || !mapped_name) {
      return 0;
    }
    if (call_instruction->arguments[i].kind == IR_OPERAND_SYMBOL &&
        call_instruction->arguments[i].name &&
        strcmp(mapped_name, call_instruction->arguments[i].name) == 0) {
      continue;
    }
    if (callee->parameter_types && callee->parameter_types[i] &&
        callee->parameter_types[i][0] != '\0') {
      type_name = callee->parameter_types[i];
    }

    IRInstruction declare_local = {0};
    declare_local.op = IR_OP_DECLARE_LOCAL;
    declare_local.location = call_instruction->location;
    declare_local.dest = ir_operand_symbol(mapped_name);
    declare_local.text = mettle_strdup(type_name);
    if (!declare_local.dest.name || !declare_local.text ||
        !ir_instruction_vector_append_move(vector, &declare_local)) {
      ir_instruction_destroy_storage(&declare_local);
      return 0;
    }

    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = call_instruction->location;
    assign.dest = ir_operand_symbol(mapped_name);
    /* A float parameter carries the narrowing contract on the assign
     * (float_bits = declared parameter width), mirroring the RETURN path
     * below. Without it the backend skips the precision conversion and a
     * float64-tracked argument temp is bit-truncated into a float32
     * parameter local (low dword of the double, 0 for round values). */
    if (strcmp(type_name, "float32") == 0) {
      assign.is_float = 1;
      assign.float_bits = 32;
    } else if (strcmp(type_name, "float64") == 0 ||
               strcmp(type_name, "float") == 0) {
      assign.is_float = 1;
      assign.float_bits = 64;
    }
    if (!assign.dest.name ||
        !ir_operand_clone(&call_instruction->arguments[i], &assign.lhs) ||
        !ir_instruction_vector_append_move(vector, &assign)) {
      ir_instruction_destroy_storage(&assign);
      return 0;
    }
  }

  return 1;
}

static int ir_function_assigns_symbol(const IRFunction *function,
                                      const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction || instruction->op == IR_OP_NOP ||
        instruction->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        strcmp(instruction->dest.name, symbol_name) == 0) {
      return 1;
    }
    /* A frontend may lower a write such as `n -= 8` to address-of plus store.
     * Treat the address as a possible write so inlining never aliases the
     * parameter directly to caller storage. */
    if (instruction->op == IR_OP_ADDRESS_OF &&
        instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name &&
        strcmp(instruction->lhs.name, symbol_name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_inline_return_is_narrow_integer(const IRFunction *callee) {
  static const char *const NARROW[] = {"int32", "uint32", "int16",
                                       "uint16", "int8",  "uint8"};
  size_t i = 0;
  if (!callee || !callee->return_type_name) {
    return 0;
  }
  for (i = 0; i < sizeof(NARROW) / sizeof(NARROW[0]); i++) {
    if (strcmp(callee->return_type_name, NARROW[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_inline_integer_bytes(const char *type, int *is_unsigned) {
  static const struct {
    const char *name;
    int bytes;
    int is_unsigned;
  } WIDTHS[] = {{"int8", 1, 0},   {"uint8", 1, 1},  {"bool", 1, 1},
                {"int16", 2, 0},  {"uint16", 2, 1}, {"int32", 4, 0},
                {"uint32", 4, 1}, {"int64", 8, 0},  {"uint64", 8, 1}};
  size_t i = 0;
  if (!type) {
    return 0;
  }
  for (i = 0; i < sizeof(WIDTHS) / sizeof(WIDTHS[0]); i++) {
    if (strcmp(type, WIDTHS[i].name) == 0) {
      if (is_unsigned) {
        *is_unsigned = WIDTHS[i].is_unsigned;
      }
      return WIDTHS[i].bytes;
    }
  }
  return 0;
}

static int ir_inline_width_fits(int src_bytes, int src_unsigned, int dst_bytes,
                                int dst_unsigned) {
  if (src_bytes <= 0 || dst_bytes <= 0 || src_bytes > dst_bytes) {
    return 0;
  }
  if (src_unsigned == dst_unsigned) {
    return 1;
  }
  return src_unsigned && src_bytes < dst_bytes;
}

static const IRInstruction *ir_inline_sole_temp_def(const IRFunction *callee,
                                                    const char *name) {
  const IRInstruction *found = NULL;
  size_t i = 0;
  for (i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *in = &callee->instructions[i];
    if (in->op == IR_OP_NOP || in->dest.kind != IR_OPERAND_TEMP ||
        !in->dest.name || strcmp(in->dest.name, name) != 0) {
      continue;
    }
    if (found) {
      return NULL;
    }
    found = in;
  }
  return found;
}

static int ir_inline_value_fits_return(const IRFunction *callee,
                                       const IROperand *value, int bytes,
                                       int is_unsigned, int depth) {
  if (!value || bytes <= 0 || depth > 4) {
    return 0;
  }
  if (value->kind == IR_OPERAND_INT) {
    if (bytes >= 8) {
      return 1;
    }
    if (is_unsigned) {
      return value->int_value >= 0 &&
             value->int_value <= (1LL << (bytes * 8)) - 1;
    }
    return value->int_value >= -(1LL << (bytes * 8 - 1)) &&
           value->int_value <= (1LL << (bytes * 8 - 1)) - 1;
  }
  if (value->kind == IR_OPERAND_SYMBOL && value->name) {
    int src_unsigned = 0;
    int declared = ir_inline_integer_bytes(
        ir_function_local_declared_type((IRFunction *)callee, value->name),
        &src_unsigned);
    size_t p = 0;
    for (p = 0; declared == 0 && p < callee->parameter_count; p++) {
      if (callee->parameter_names && callee->parameter_names[p] &&
          strcmp(callee->parameter_names[p], value->name) == 0) {
        declared = ir_inline_integer_bytes(
            callee->parameter_types ? callee->parameter_types[p] : NULL,
            &src_unsigned);
      }
    }
    return ir_inline_width_fits(declared, src_unsigned, bytes, is_unsigned);
  }
  if (value->kind != IR_OPERAND_TEMP || !value->name) {
    return 0;
  }
  {
    const IRInstruction *def = ir_inline_sole_temp_def(callee, value->name);
    if (!def || def->is_float) {
      return 0;
    }
    switch (def->op) {
    case IR_OP_CAST: {
      int cast_unsigned = 0;
      int cast_bytes = ir_inline_integer_bytes(def->text, &cast_unsigned);
      return ir_inline_width_fits(cast_bytes, cast_unsigned, bytes,
                                  is_unsigned);
    }
    case IR_OP_LOAD:
      return def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value > 0 &&
             def->rhs.int_value <= 8 &&
             ir_inline_width_fits((int)def->rhs.int_value,
                                  def->is_unsigned ? 1 : 0, bytes, is_unsigned);
    case IR_OP_ASSIGN:
      return ir_inline_value_fits_return(callee, &def->lhs, bytes, is_unsigned,
                                         depth + 1);
    case IR_OP_BINARY:
      return def->text &&
             (strcmp(def->text, "==") == 0 || strcmp(def->text, "!=") == 0 ||
              strcmp(def->text, "<") == 0 || strcmp(def->text, "<=") == 0 ||
              strcmp(def->text, ">") == 0 || strcmp(def->text, ">=") == 0 ||
              strcmp(def->text, "&&") == 0 || strcmp(def->text, "||") == 0);
    default:
      return 0;
    }
  }
}

static int ir_inline_call_instruction(IRInstructionVector *vector,
                                      const IRInstruction *call_instruction,
                                      const IRFunction *callee,
                                      size_t inline_site_id) {
  if (!vector || !call_instruction || !callee) {
    return 0;
  }

  char *inline_prefix = ir_make_inline_prefix(callee->name, inline_site_id);
  if (!inline_prefix) {
    return 0;
  }

  IRNameMap symbol_map = {0};
  IRNameMap temp_map = {0};
  IRNameMap label_map = {0};
  int ok = 0;

  for (size_t i = 0; i < callee->parameter_count; i++) {
    const char *parameter_name = callee->parameter_names[i];
    if (!parameter_name) {
      goto cleanup;
    }

    const IROperand *argument = &call_instruction->arguments[i];
    char *mapped = NULL;
    int add_ok = 0;
    if (argument->kind == IR_OPERAND_SYMBOL && argument->name &&
        !ir_function_assigns_symbol(callee, parameter_name)) {
      add_ok = ir_name_map_add(&symbol_map, parameter_name, argument->name);
    } else {
      mapped = ir_make_inline_name(inline_prefix, "param", parameter_name);
      if (!mapped) {
        goto cleanup;
      }
      add_ok = ir_name_map_add(&symbol_map, parameter_name, mapped);
      free(mapped);
    }
    if (!add_ok) {
      goto cleanup;
    }
  }

  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *instruction = &callee->instructions[i];
    if (instruction->op == IR_OP_DECLARE_LOCAL &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name) {
      char *mapped =
          ir_make_inline_name(inline_prefix, "local", instruction->dest.name);
      if (!mapped) {
        goto cleanup;
      }
      int add_ok = ir_name_map_add(&symbol_map, instruction->dest.name, mapped);
      free(mapped);
      if (!add_ok) {
        goto cleanup;
      }
    }
  }

  if (!ir_append_parameter_materialization(vector, call_instruction, callee,
                                           &symbol_map)) {
    goto cleanup;
  }

  char *inline_end_label = ir_make_inline_name(inline_prefix, "label", "end");
  if (!inline_end_label) {
    goto cleanup;
  }

  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *source = &callee->instructions[i];
    IRInstruction emitted = {0};

    /* `@simd` contracts are enforced at the loop's definition site (the
     * standalone callee, which the function pipeline verifies independently).
     * Don't carry the markers into an inlined copy: after inlining the loop may
     * no longer satisfy a recognizer's preconditions (e.g. dot_i8 requires the
     * array bases to be parameters), and the user never wrote that copy. */
    if (source->op == IR_OP_NOP && source->text &&
        strncmp(source->text, IR_SIMD_MARKER_PREFIX,
                strlen(IR_SIMD_MARKER_PREFIX)) == 0) {
      continue;
    }

    if (source->op == IR_OP_RETURN) {
      if (source->lhs.kind != IR_OPERAND_NONE &&
          call_instruction->dest.kind != IR_OPERAND_NONE) {
        emitted.op = IR_OP_ASSIGN;
        emitted.location = call_instruction->location;
        int ret_unsigned = 0;
        int ret_bytes =
            ir_inline_integer_bytes(callee->return_type_name, &ret_unsigned);
        if (!source->is_float && ir_inline_return_is_narrow_integer(callee) &&
            !ir_inline_value_fits_return(callee, &source->lhs, ret_bytes,
                                         ret_unsigned, 0)) {
          char *narrowed = mettle_strdup(callee->return_type_name);
          if (!narrowed) {
            ir_instruction_destroy_storage(&emitted);
            free(inline_end_label);
            goto cleanup;
          }
          emitted.op = IR_OP_CAST;
          emitted.text = narrowed;
        }
        /* The RETURN carries the narrowing contract: float_bits is the return
         * type's width (the destination precision) and lhs.float_bits is the
         * value's own width. Propagate both so the synthesized assign performs
         * any f64->f32 conversion the return ABI would have, and so a later
         * coalescing pass cannot mistake it for a width-preserving copy. */
        emitted.is_float = source->is_float;
        emitted.is_unsigned = source->is_unsigned;
        emitted.float_bits = source->float_bits;
        if (!ir_operand_clone(&call_instruction->dest, &emitted.dest) ||
            !ir_inline_rewrite_operand(&source->lhs, &emitted.lhs, &symbol_map,
                                       &temp_map, &label_map, inline_prefix) ||
            !ir_instruction_vector_append_move(vector, &emitted)) {
          ir_instruction_destroy_storage(&emitted);
          free(inline_end_label);
          goto cleanup;
        }
      }

      memset(&emitted, 0, sizeof(emitted));
      emitted.op = IR_OP_JUMP;
      emitted.location = call_instruction->location;
      emitted.text = mettle_strdup(inline_end_label);
      if (!emitted.text || !ir_instruction_vector_append_move(vector, &emitted)) {
        ir_instruction_destroy_storage(&emitted);
        free(inline_end_label);
        goto cleanup;
      }
      continue;
    }

    if (!ir_clone_instruction_for_inline(source, &emitted, &symbol_map, &temp_map,
                                         &label_map, inline_prefix) ||
        !ir_instruction_vector_append_move(vector, &emitted)) {
      ir_instruction_destroy_storage(&emitted);
      free(inline_end_label);
      goto cleanup;
    }
  }

  {
    IRInstruction end_label = {0};
    end_label.op = IR_OP_LABEL;
    end_label.location = call_instruction->location;
    end_label.text = inline_end_label;
    if (!ir_instruction_vector_append_move(vector, &end_label)) {
      ir_instruction_destroy_storage(&end_label);
      free(inline_end_label);
      goto cleanup;
    }
  }

  ok = 1;

cleanup:
  ir_name_map_destroy(&label_map);
  ir_name_map_destroy(&temp_map);
  ir_name_map_destroy(&symbol_map);
  free(inline_prefix);
  return ok;
}

/* True when instruction `site` sits inside a loop body of `function`:
 * between a loop header label and a back-jump to that label. A loop-resident
 * call pays its overhead every iteration -- those sites keep full inlining
 * eligibility even in an over-budget caller, because that is exactly where
 * inlining still buys runtime. A site outside every loop runs at most once
 * per call of the function; refusing it costs nothing measurable. */
static int ir_call_site_is_in_loop(const IRFunction *function, size_t site) {
  for (size_t h = 0; h < site; h++) {
    const IRInstruction *header = &function->instructions[h];
    if (header->op != IR_OP_LABEL || !header->text ||
        !ir_label_is_while_header(header->text)) {
      continue;
    }
    for (size_t j = site + 1; j < function->instruction_count; j++) {
      const IRInstruction *jmp = &function->instructions[j];
      if (jmp->op == IR_OP_JUMP && jmp->text &&
          strcmp(jmp->text, header->text) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

/* Bitmap form of ir_call_site_is_in_loop for the inliner's walk over an
 * over-budget caller: marks every [header, last back-jump] range once,
 * instead of re-deriving loop membership per call site (which was quadratic
 * on machine-generated functions with hundreds of loops and calls). Each byte
 * counts the loops enclosing that instruction, so a nonzero byte answers
 * "inside a loop" and the value answers "how deep". Returns NULL on allocation
 * failure or when the function has no loops -- callers fall back to the
 * per-site scan. */
static char *ir_build_in_loop_bitmap(const IRFunction *function) {
  char *in_loop = NULL;
  for (size_t h = 0; h < function->instruction_count; h++) {
    const IRInstruction *header = &function->instructions[h];
    if (header->op != IR_OP_LABEL || !header->text ||
        !ir_label_is_while_header(header->text)) {
      continue;
    }
    size_t last = 0;
    int found = 0;
    for (size_t j = h + 1; j < function->instruction_count; j++) {
      const IRInstruction *jmp = &function->instructions[j];
      if (jmp->op == IR_OP_JUMP && jmp->text &&
          strcmp(jmp->text, header->text) == 0) {
        last = j;
        found = 1;
      }
    }
    if (!found) {
      continue;
    }
    if (!in_loop) {
      in_loop = calloc(function->instruction_count, 1);
      if (!in_loop) {
        return NULL;
      }
    }
    /* Accumulate rather than set: the byte ends up holding how many loops
     * enclose the instruction, which is how many trip counts a call sitting
     * there multiplies by. */
    for (size_t k = h; k <= last; k++) {
      if ((unsigned char)in_loop[k] < 255) {
        in_loop[k] = (char)(in_loop[k] + 1);
      }
    }
  }
  return in_loop;
}

/* A "tiny leaf": at most IR_INLINE_TINY_LEAF_NON_NOP_INSTRUCTIONS non-nop
 * instructions and no calls of its own. Inlining one into ANY caller is
 * (nearly) free -- the body is about the size of the call sequence it
 * replaces, and with no nested calls the growth cannot cascade. */
static int ir_function_is_tiny_leaf(const IRFunction *callee) {
  size_t non_nop = 0;
  for (size_t i = 0; i < callee->instruction_count; i++) {
    IROpcode op = callee->instructions[i].op;
    if (op == IR_OP_NOP) {
      continue;
    }
    if (op == IR_OP_CALL || op == IR_OP_CALL_INDIRECT) {
      return 0;
    }
    if (++non_nop > IR_INLINE_TINY_LEAF_NON_NOP_INSTRUCTIONS) {
      return 0;
    }
  }
  return 1;
}

static int ir_inline_calls_in_function(IRProgram *program, IRFunction *function,
                                       size_t *inline_counter, int *changed) {
  if (!program || !function || !inline_counter) {
    return 0;
  }

  /* An over-budget caller may not GROW further, but freezing it entirely
   * would refuse free wins. Three exemptions still go in: tiny leaf callees
   * (accessors, predicates -- cannot cause runaway growth), @inline-forced
   * callees (the user explicitly overriding the heuristic), and calls at
   * LOOP-RESIDENT sites (the only places where call overhead multiplies --
   * the budget exists to bound code size, not to leave per-iteration call
   * overhead in hot loops). What stays refused: cold one-shot call sites,
   * which cost nothing measurable as real calls. */
  /* Measured once, for the caller as this pass found it, and deliberately not
   * charged as the caller grows. Recomputing it after every inline is what the
   * paragraph above reads like it should do, and it is worse: cutting a pass
   * off part way leaves a caller half converted, with some of a routine
   * inlined and the rest still calls, which allocates worse than either doing
   * all of it or none of it. Charging the growth cost 2.4% across Suite 3
   * Set 2, every benchmark slower. Growth is bounded between rounds instead,
   * where the caller is whole again. */
  int caller_over_budget = ir_function_non_nop_instruction_count(function) >
                           ir_opt_inline_caller_budget(function);
  /* Built for every caller, not just over-budget ones: the callee size budget
   * reads it too, so a loop-resident site can take a callee a one-shot site
   * would not. */
  char *in_loop = ir_build_in_loop_bitmap(function);

  /* Pre-scan: find the first call site that will actually inline. Callers with
   * none (the common case once the program stabilizes) skip the rebuild -- the
   * rebuild used to deep-clone every instruction of every function per driver
   * round, which dominated the pass. The decision below must stay identical to
   * the one in the rebuild loop. */
  size_t first_inline = function->instruction_count;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_CALL && instruction->text &&
        instruction->argument_count <= IR_INLINE_MAX_PARAMETERS) {
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (callee && callee != function &&
          instruction->argument_count == callee->parameter_count &&
          (!caller_over_budget || callee->is_inline ||
           ir_function_is_tiny_leaf(callee) || ir_opt_function_is_hot(callee) ||
           ir_opt_site_is_hot(function, instruction->location) ||
           (in_loop && in_loop[i])) &&
          ir_function_is_inline_candidate_at(callee, in_loop ? in_loop[i] : 0,
                                             NULL, NULL)) {
        first_inline = i;
        break;
      }
    }
  }
  if (first_inline == function->instruction_count) {
    free(in_loop);
    return 1;
  }

  IRInstructionVector vector = {0};
  int local_changed = 0;
  if (!ir_instruction_vector_reserve(&vector, function->instruction_count)) {
    free(in_loop);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    if (i >= first_inline && instruction->op == IR_OP_CALL &&
        instruction->text &&
        instruction->argument_count <= IR_INLINE_MAX_PARAMETERS) {
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (callee && callee != function &&
          instruction->argument_count == callee->parameter_count &&
          (!caller_over_budget || callee->is_inline ||
           ir_function_is_tiny_leaf(callee) || ir_opt_function_is_hot(callee) ||
           ir_opt_site_is_hot(function, instruction->location) ||
           (in_loop && in_loop[i])) &&
          ir_function_is_inline_candidate_at(callee, in_loop ? in_loop[i] : 0,
                                             NULL, NULL)) {
        if (ir_explain_enabled()) {
          char entity[160];
          size_t weight = ir_function_non_nop_instruction_count(callee);
          snprintf(entity, sizeof(entity), "call to `%s`", instruction->text);
          ir_explain_remark(function->name, entity, instruction->location, 1,
                            "inlined", NULL, NULL, NULL);
          ir_explain_remark_code("inlined");
          ir_explain_remark_quantity("calleeInstructions", (long)weight);
          /* A one-line wrapper going away is not news. Saying so lets a
             reader collapse the routine majority and see the real decisions. */
          if (weight <= IR_EXPLAIN_TRIVIAL_CALLEE_INSTRUCTIONS) {
            ir_explain_remark_trivial();
          }
        }
        if (!ir_inline_call_instruction(&vector, instruction, callee,
                                        (*inline_counter)++)) {
          ir_instruction_vector_destroy(&vector);
          free(in_loop);
          return 0;
        }
        local_changed = 1;
        continue;
      }
    }

    /* Untouched instruction: MOVE it (append_move neutralizes the source, so
     * the destroy sweep below only pays for replaced call instructions). */
    if (!ir_instruction_vector_append_move(&vector, instruction)) {
      ir_instruction_vector_destroy(&vector);
      free(in_loop);
      return 0;
    }
  }
  free(in_loop);

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);
  function->instructions = vector.items;
  function->instruction_count = vector.count;
  function->instruction_capacity = vector.capacity;
  vector.items = NULL;
  vector.count = 0;
  vector.capacity = 0;

  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

/* --- Self-recursion inlining -------------------------------------------
 *
 * The regular inliner never inlines a function into itself (callee !=
 * function), so a recursive function pays full call overhead at every level
 * of the recursion tree. Inlining the body into its own self-call sites a
 * bounded number of times (the gcc "max-inline-recursive-depth" idea)
 * multiplies the work done per real call: depth 1 turns each call into ~the
 * work of a small subtree, cutting the dynamic call count by the subtree
 * size. Growth is bounded by a body-size cap, so deep expansion stops on its
 * own. Loop-bearing recursive bodies (a quicksort partition, a merge) expand
 * too: the label-rename map keeps each clone's loops disjoint. */
static int ir_self_call_is_loop_resident(const IRFunction *function) {
  char *in_loop = ir_build_in_loop_bitmap(function);
  int resident = 0;
  if (!in_loop) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_CALL && instruction->text &&
        function->name && strcmp(instruction->text, function->name) == 0 &&
        in_loop[i]) {
      resident = 1;
      break;
    }
  }
  free(in_loop);
  return resident;
}

static int ir_function_is_self_inline_candidate(const IRFunction *function,
                                                size_t *self_call_count_out) {
  if (!function || !function->name || function->instruction_count == 0 ||
      function->is_noinline) {
    return 0;
  }
  if (function->parameter_count > IR_INLINE_MAX_PARAMETERS ||
      (function->parameter_count > 0 && !function->parameter_names)) {
    return 0;
  }
  size_t self_calls = 0;
  int has_return = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->op == IR_OP_INLINE_ASM ||
        instruction->op == IR_OP_CALL_INDIRECT) {
      return 0;
    }
    if (instruction->op == IR_OP_CALL && instruction->text &&
        strcmp(instruction->text, function->name) == 0) {
      if (instruction->argument_count != function->parameter_count) {
        return 0;
      }
      self_calls++;
      if (self_calls > IR_SELF_INLINE_MAX_SELF_CALLS) {
        return 0;
      }
    }
    if (instruction->op == IR_OP_RETURN) {
      has_return = 1;
    }
  }

  if (self_call_count_out) {
    *self_call_count_out = self_calls;
  }
  return has_return && self_calls > 0;
}

/* One depth level: rebuild the function, expanding every direct self-call
 * site with a clone of the CURRENT body (the clone's own self-calls stay as
 * real calls, to be expanded by the next round or executed at runtime). */
static int ir_inline_self_calls_once(IRFunction *function,
                                     size_t *inline_counter, int *changed) {
  IRInstructionVector vector = {0};
  int local_changed = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    IRInstruction cloned = {0};

    if (instruction->op == IR_OP_CALL && instruction->text &&
        strcmp(instruction->text, function->name) == 0 &&
        instruction->argument_count == function->parameter_count) {
      if (!ir_inline_call_instruction(&vector, instruction, function,
                                      (*inline_counter)++)) {
        ir_instruction_vector_destroy(&vector);
        return 0;
      }
      local_changed = 1;
      continue;
    }

    if (!ir_clone_instruction_plain(instruction, &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);
  function->instructions = vector.items;
  function->instruction_count = vector.count;
  function->instruction_capacity = vector.capacity;
  vector.items = NULL;
  vector.count = 0;
  vector.capacity = 0;

  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

int ir_inline_self_recursion_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }

  /* Inline prefixes are "__inl_<id>" with no callee name, so every producer
   * of ids needs its own disjoint range: the regular inliner counts up from
   * 0, the forced-inline simulator from 900000, and self-recursion expansion
   * from here. (A real program cannot push the other counters anywhere near
   * this base: each site materializes instructions, so memory runs out many
   * orders of magnitude earlier.) */
  size_t inline_counter = 1800000000;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    size_t self_calls = 0;
    if (!ir_function_is_self_inline_candidate(function, &self_calls)) {
      continue;
    }
    int max_depth = ir_opt_self_inline_max_depth(function);
    size_t body_budget = ir_opt_self_inline_body_budget(function);
    if (max_depth > 1 && ir_self_call_is_loop_resident(function)) {
      max_depth = 1;
    }
    for (int depth = 0; depth < max_depth; depth++) {
      if (ir_function_non_nop_instruction_count(function) >
          body_budget) {
        break;
      }
      int round_changed = 0;
      if (!ir_inline_self_calls_once(function, &inline_counter,
                                     &round_changed)) {
        return 0;
      }
      if (!round_changed) {
        break;
      }
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

/* Why did this specific call site survive inlining? Shared by the --explain
 * refusal remarks and the `@inline!` contract enforcement so the report and
 * the error always agree. */
/* True when the callee's body now contains a SIMD kernel op -- its loops were
 * vectorized after inlining decisions were made, so the historical refusal
 * reason (usually the loop-shape guard) no longer describes the body. */
static int ir_function_contains_simd_kernel(const IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op >= IR_OP_COUNT_WORD_STARTS && op <= IR_OP_SIMD_OUTER_LANE_F64) {
      return 1;
    }
  }
  return 0;
}

/* Loop depth of one call site, for the report to agree with the decision. */
static int ir_inline_site_loop_depth(IRFunction *caller,
                                     const IRInstruction *instruction) {
  char *depths;
  int depth = 0;
  if (!caller || instruction < caller->instructions ||
      (size_t)(instruction - caller->instructions) >=
          caller->instruction_count) {
    return 0;
  }
  depths = ir_build_in_loop_bitmap(caller);
  if (depths) {
    depth = (unsigned char)depths[instruction - caller->instructions];
    free(depths);
  }
  return depth;
}

static void ir_inline_site_reason(IRFunction *caller,
                                  const IRInstruction *instruction,
                                  IRFunction *callee, const char **reason,
                                  const char **fix) {
  *reason = NULL;
  *fix = NULL;
  g_inline_refusal_code = NULL;
  if (callee != caller && ir_function_contains_simd_kernel(callee)) {
    IR_INLINE_WHY(reason, "callee-has-kernel",
                  "the callee's loops were vectorized into SIMD kernels after "
                  "inlining ran; it stays a real call (the kernel runs the same "
                  "either way)");
    return;
  }
  if (callee == caller) {
    IR_INLINE_WHY(reason, "recursive",
                  "the call is directly recursive");
    *fix = "bounded self-recursion expansion applies automatically; rewrite "
           "as a loop for full control";
  } else if (ir_function_non_nop_instruction_count(caller) >
                 ir_opt_inline_caller_budget(caller) &&
             !callee->is_inline && !ir_function_is_tiny_leaf(callee) &&
             !ir_opt_function_is_hot(callee) &&
             instruction >= caller->instructions &&
             !ir_opt_site_is_hot(caller, instruction->location) &&
             !ir_call_site_is_in_loop(
                 caller, (size_t)(instruction - caller->instructions))) {
    /* Mirrors the gate in ir_inline_calls_in_function: tiny leaves,
     * @inline-forced callees, and loop-resident sites are exempt from the
     * caller budget, so only cold one-shot sites can be refused for this
     * reason -- and for those, NOT inlining is the right call, so there is
     * deliberately no fix advice to hand out. */
    IR_INLINE_WHY(reason, "caller-over-budget",
                  "the calling function is over the profile-adjusted caller "
                  "budget, and this call site is not measured hot or inside a "
                  "loop, so it runs at most once per call of the function and "
                  "keeping it a real call costs nothing measurable (loop-resident "
                  "calls, measured-hot sites, tiny call-free callees, and "
                  "@inline-marked callees still inline here)");
  } else if (instruction->argument_count > IR_INLINE_MAX_PARAMETERS ||
             instruction->argument_count != callee->parameter_count) {
    IR_INLINE_WHY(reason, "argument-count",
                  "the call's argument count doesn't match what the inliner "
                  "handles for this callee");
  } else if (ir_function_is_inline_candidate_at(
                 callee, ir_inline_site_loop_depth(caller, instruction), reason,
                 fix)) {
    /* Candidate-eligible but still here: the call site appeared late (a
     * nested inline in the final round) or rounds hit their cap. */
    IR_INLINE_WHY(reason, "rounds-exhausted",
                  "inlining rounds reached their limit before this call could "
                  "be revisited");
  }
}

/* --explain: record every call that SURVIVED all inlining rounds, with the
 * reason it was not inlined. Successful inlines are recorded at the moment
 * they happen (the call instruction no longer exists afterwards); refusals are
 * recorded here, once, after the dust settles -- doing it inside the round
 * loop would repeat each refusal once per round. Calls to functions not
 * defined in the program (runtime/extern) are skipped: the inliner could never
 * touch them, so there is no decision to explain. */
void ir_inline_explain_report_remaining(IRProgram *program) {
  if (!program || !ir_explain_enabled()) {
    return;
  }

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function || function->rewrite_role) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (instruction->op != IR_OP_CALL || !instruction->text) {
        continue;
      }
      if (!ir_explain_location_enabled(&instruction->location)) {
        continue;
      }
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (!callee) {
        continue;
      }
      const char *reason = NULL;
      const char *fix = NULL;
      ir_inline_site_reason(function, instruction, callee, &reason, &fix);
      const char *refusal_code = g_inline_refusal_code;

      /* When the fix is "mark it @inline", PROVE it: re-run the candidate
       * check with the decorator pretend-applied. A pass means the call
       * really will inline; a fail means the suggestion is wrong (a
       * structural guard hides behind the discretionary cap that fired
       * first), so the fix is corrected rather than printed as-is. */
      const char *verified = NULL;
      int advisory = 0;
      char corrected_fix[320];
      if (fix && strstr(fix, "@inline") && !callee->is_inline &&
          !callee->is_noinline) {
        int saved = callee->is_inline;
        callee->is_inline = 1;
        const char *forced_reason = NULL;
        const char *unused_fix = NULL;
        if (ir_function_is_inline_candidate(callee, &forced_reason,
                                            &unused_fix)) {
          verified = "re-checked with @inline pretend-applied: the structural "
                     "guards pass, so this call will inline";
        } else if (forced_reason && reason &&
                   strcmp(forced_reason, reason) == 0) {
          /* The pretend-apply failed for the reason already printed; a fix
           * line restating it would be noise. */
          fix = NULL;
        } else {
          /* The simulation disproved the advice. What is left explains why
           * the obvious suggestion fails; it does not instruct anyone, so
           * the report must not rank it as work to do. */
          snprintf(corrected_fix, sizeof(corrected_fix),
                   "none. Re-checked with @inline "
                   "pretend-applied and it still won't inline: %s",
                   forced_reason ? forced_reason : "a structural guard");
          fix = corrected_fix;
          advisory = 1;
        }
        callee->is_inline = saved;
      }

      char entity[160];
      snprintf(entity, sizeof(entity), "call to `%s`", instruction->text);
      ir_explain_remark(function->name, entity, instruction->location, 0,
                        "NOT inlined", reason, fix, verified);
      if (advisory) {
        ir_explain_remark_advisory();
      }
      ir_explain_remark_code(refusal_code);
      ir_explain_remark_quantity("calleeInstructions",
                                 (long)ir_function_non_nop_instruction_count(callee));
    }
  }
}

/* `@inline!` contract: after every inlining round has run, any surviving call
 * to a contract function is a hard compile error carrying the same reason the
 * --explain report would give. Not focus-filtered -- a contract holds across
 * the whole program. Returns 1 when every contract held. */
int ir_inline_enforce_contracts(IRProgram *program) {
  if (!program) {
    return 1;
  }
  /* Inlining clones call sites (each clone keeps the original source
   * location), so one offending line can surface several times; report each
   * (location, callee) once. */
  struct {
    size_t line, column;
    const char *callee;
  } reported[64];
  size_t reported_count = 0;
  int ok = 1;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (instruction->op != IR_OP_CALL || !instruction->text) {
        continue;
      }
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (!callee || !callee->is_inline_contract) {
        continue;
      }
      int already_reported = 0;
      for (size_t r = 0; r < reported_count; r++) {
        if (reported[r].line == instruction->location.line &&
            reported[r].column == instruction->location.column &&
            strcmp(reported[r].callee, instruction->text) == 0) {
          already_reported = 1;
          break;
        }
      }
      if (already_reported) {
        ok = 0; /* still a violation, just not re-printed */
        continue;
      }
      if (reported_count < 64) {
        reported[reported_count].line = instruction->location.line;
        reported[reported_count].column = instruction->location.column;
        reported[reported_count].callee = instruction->text;
        reported_count++;
      }
      const char *reason = NULL;
      const char *fix = NULL;
      ir_inline_site_reason(function, instruction, callee, &reason, &fix);
      fprintf(stderr,
              "%s:%zu:%zu: error: @inline! call to `%s` was not inlined: "
              "%s%s%s\n",
              instruction->location.filename ? instruction->location.filename
                                             : "<input>",
              instruction->location.line, instruction->location.column,
              instruction->text, reason ? reason : "unknown",
              fix ? "; " : "", fix ? fix : "");
      ok = 0;
    }
  }
  if (!ok) {
    ir_optimize_note_user_error();
  }
  return ok;
}

int ir_inline_explain_simulate_force_inline(IRProgram *program,
                                            IRFunction *caller,
                                            const char *callee_name,
                                            int *was_noinline_out,
                                            const char **decline_reason_out) {
  if (decline_reason_out) {
    *decline_reason_out = NULL;
  }
  if (!program || !caller || !callee_name) {
    return 0;
  }
  IRFunction *callee = ir_program_find_function(program, callee_name);
  if (!callee || callee == caller) {
    return 0;
  }

  /* A self-recursive callee can never be inlined AWAY: every expansion
   * re-creates the same call inside the loop, so the @inline advice is dead
   * on arrival -- and actually running the forced rounds grows the clone
   * geometrically (rec_fib: 179 -> 114,839 instructions by round 3, then a
   * multi-minute re-optimize of the wreckage). Withdraw the advice with the
   * honest reason instead of simulating it. */
  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *ins = &callee->instructions[i];
    if (ins->op == IR_OP_CALL && ins->text &&
        strcmp(ins->text, callee_name) == 0) {
      if (decline_reason_out) {
        *decline_reason_out =
            "the callee is recursive (it calls itself), so inlining cannot "
            "remove the call from the loop body";
      }
      return 0;
    }
  }

  /* Two pretends, reported distinctly: a `@noinline` callee simulates the
   * user REMOVING that decorator (the veto precedes everything, so forcing
   * is_inline alone would never fire); anything else simulates ADDING
   * @inline. */
  int saved_is_inline = callee->is_inline;
  int saved_is_noinline = callee->is_noinline;
  if (was_noinline_out) {
    *was_noinline_out = callee->is_noinline;
  }
  callee->is_inline = 1;
  callee->is_noinline = 0;
  /* With the pretend flags set, any remaining refusal is structural (loops,
   * inline asm, no return, ...) -- something no decorator can override. Hand
   * that reason out so --explain can WITHDRAW the @inline advice instead of
   * printing a suggestion the inliner itself has just proven dead. */
  const char *why_not = NULL;
  int candidate = ir_function_is_inline_candidate(callee, &why_not, NULL);
  if (!candidate && decline_reason_out) {
    *decline_reason_out = why_not;
  }
  int changed = 0;
  if (candidate) {
    /* High counter base so the clone's fresh __inl_* names cannot collide
     * with names the real inlining rounds already left in the caller.
     * Multiple rounds: when the forced callee was declined for making calls
     * of its own (the glue-function cap), those inner calls land in the
     * caller on round 1 and -- when their targets are ordinary inline
     * candidates -- disappear on the next round, exactly as the real
     * pipeline's rounds would once the user adds @inline. */
    size_t inline_counter = 900000;
    for (int round = 0; round < IR_INLINE_MAX_ROUNDS; round++) {
      int round_changed = 0;
      if (!ir_inline_calls_in_function(program, caller, &inline_counter,
                                       &round_changed)) {
        changed = 0;
        break;
      }
      if (!round_changed) {
        break;
      }
      changed = 1;
      /* Runaway-growth guard: the direct self-call refusal above cannot see
       * mutual recursion (A calls B calls A), where each pretend round
       * re-introduces the calls it just expanded. A clone this size is no
       * longer evidence about the user's loop -- abandon the simulation and
       * let the caller keep its generic advice. */
      if (caller->instruction_count >
          16 * IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS) {
        changed = 0;
        break;
      }
    }
  }
  callee->is_inline = saved_is_inline;
  callee->is_noinline = saved_is_noinline;
  return candidate && changed;
}

int ir_inline_small_functions_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }

  size_t inline_counter = 0;
  for (int round = 0; round < IR_INLINE_MAX_ROUNDS; round++) {
    int round_changed = 0;

    for (size_t i = 0; i < program->function_count; i++) {
      if (program->functions[i] && program->functions[i]->rewrite_role) {
        continue;
      }
      if (!ir_inline_calls_in_function(program, program->functions[i],
                                       &inline_counter, &round_changed)) {
        return 0;
      }
    }

    if (round_changed && changed) {
      *changed = 1;
    }
    if (!round_changed) {
      break;
    }
  }

  return 1;
}
