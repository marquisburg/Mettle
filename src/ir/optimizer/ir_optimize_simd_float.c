#include "ir_optimize_internal.h"
#include "../ir_explain_ledger.h"

/* -------------------------------------------------------------------------- */
/* float64/float32 horizontal sum -> IR_OP_SIMD_SUM_F64/F32                    */
/* float64/float32 dot product   -> IR_OP_SIMD_DOT_F64/F32                     */
/* -------------------------------------------------------------------------- */

/* Decode a temp that must be the value of `*(base + (iv << shift)) [size]`,
 * the canonical lowering of `base[iv]` for a float array. On success records
 * the array base symbol and the element width in bits: 64 for the float64
 * shape (iv<<3, load size 8) and 32 for the float32 shape (iv<<2, load size 4).
 * Any other shape is rejected so the recognizer cannot mistake an int32 index
 * expression (or a differently-strided access) for a float reduction. */
static int ir_decode_float_indexed_load(IRFunction *function, size_t before,
                                        const char *load_temp, const char *iv,
                                        const char **base_out, int *bits_out) {
  const IRInstruction *load = NULL;
  const IRInstruction *addr = NULL;
  const IRInstruction *shl = NULL;
  long long size = 0;
  long long shift = 0;

  if (!load_temp || !iv || !base_out || !bits_out) {
    return 0;
  }
  load = ir_find_temp_producer_before(function, before, load_temp);
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  size = load->rhs.int_value;
  addr = ir_find_temp_producer_before(function, before, load->lhs.name);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name || addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
    return 0;
  }
  shl = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
      strcmp(shl->text, "<<") != 0 ||
      !ir_operand_is_symbol_named(&shl->lhs, iv) ||
      shl->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  shift = shl->rhs.int_value;
  if (shift == 3 && size == 8) {
    *bits_out = 64;
  } else if (shift == 2 && size == 4) {
    *bits_out = 32;
  } else {
    return 0;
  }
  *base_out = addr->lhs.name;
  return 1;
}

static int ir_decode_float_indexed_address(IRFunction *function, size_t before,
                                           const char *addr_temp,
                                           const char *iv,
                                           const char **base_out,
                                           int *bits_out) {
  const IRInstruction *addr = NULL;
  const IRInstruction *shl = NULL;
  long long shift = 0;

  if (!addr_temp || !iv || !base_out || !bits_out) {
    return 0;
  }
  addr = ir_find_temp_producer_before(function, before, addr_temp);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name || addr->rhs.kind != IR_OPERAND_TEMP ||
      !addr->rhs.name) {
    return 0;
  }
  shl = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
      strcmp(shl->text, "<<") != 0 ||
      !ir_operand_is_symbol_named(&shl->lhs, iv) ||
      shl->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }

  shift = shl->rhs.int_value;
  if (shift == 3) {
    *bits_out = 64;
  } else if (shift == 2) {
    *bits_out = 32;
  } else {
    return 0;
  }
  *base_out = addr->lhs.name;
  return 1;
}

/* Byte twin of the two decoders above. A 1-byte element needs no scaling, so
 * the address is `base + iv` with no shift between them -- the shape the
 * decoders above are built to see through, and therefore the shape they
 * refuse. */
static int ir_decode_byte_indexed_address(IRFunction *function, size_t before,
                                          const char *addr_temp,
                                          const char *iv,
                                          const char **base_out) {
  const IRInstruction *addr = NULL;
  if (!addr_temp || !iv || !base_out) {
    return 0;
  }
  addr = ir_find_temp_producer_before(function, before, addr_temp);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0) {
    return 0;
  }
  if (addr->lhs.kind == IR_OPERAND_SYMBOL && addr->lhs.name &&
      ir_operand_is_symbol_named(&addr->rhs, iv)) {
    *base_out = addr->lhs.name;
    return 1;
  }
  if (addr->rhs.kind == IR_OPERAND_SYMBOL && addr->rhs.name &&
      ir_operand_is_symbol_named(&addr->lhs, iv)) {
    *base_out = addr->rhs.name;
    return 1;
  }
  return 0;
}

/* `%t <- *(base + iv) [1]`, with how the byte widens into the int32 lane. */
static int ir_decode_byte_indexed_load(IRFunction *function, size_t before,
                                       const char *load_temp, const char *iv,
                                       const char **base_out,
                                       int *unsigned_out) {
  const IRInstruction *load = NULL;
  if (!load_temp || !iv || !base_out || !unsigned_out) {
    return 0;
  }
  load = ir_find_temp_producer_before(function, before, load_temp);
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT ||
      load->rhs.int_value != 1) {
    return 0;
  }
  if (!ir_decode_byte_indexed_address(function, before, load->lhs.name, iv,
                                      base_out)) {
    return 0;
  }
  /* The lane widens the way the scalar load does, which is the way the element
   * type reads: a signed byte sign-extends, an unsigned one zero-extends. This
   * was pinned to zero-extension while the scalar backends widened every byte
   * with movzx, so an int8 -56 read back as 200 and the kernel had to match. */
  *unsigned_out = load->is_unsigned ? 1 : 0;
  return 1;
}

/* A symbol is an acceptable float-array base if it is a function parameter, a
 * declared local (covers inlined-callee parameter copies), or a GLOBAL the
 * function never writes and never takes the address of -- real programs (an
 * LLM engine's scratch buffers, a game's framebuffer pointer) keep their hot
 * arrays in global pointers, and rejecting those left every such loop
 * scalar. The strict load-shape decode above already pins element width and
 * float-ness. */
static int ir_symbol_is_float_array_base(IRFunction *function,
                                         const char *symbol_name) {
  if (ir_function_symbol_is_parameter(function, symbol_name) ||
      ir_function_local_declared_type(function, symbol_name) != NULL) {
    return 1;
  }
  /* Global: its VALUE must be stable across the loop. The recognizers'
   * bodies are store/call-free, so only a direct write inside this function
   * or an escaped address could change it mid-loop. */
  if (!symbol_name || ir_symbol_address_taken(function, symbol_name)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    /* A SIMD array op carries the output array's base symbol as its dest by
     * convention (`@a = simd_affine_map_f32(...)`, `@a = simd_fill(...)`): it
     * writes the array ELEMENTS through the base, it does NOT reassign the
     * base pointer's value. So such an op (typically a SIBLING loop already
     * vectorized on the same array) must not disqualify the base -- otherwise
     * vectorizing one `a[i]=...` loop would poison every later one on `a`. */
    if (ins->op >= IR_OP_SIMD_SUM_I32 && ins->op <= IR_OP_SIMD_LCG_U32) {
      continue;
    }
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, symbol_name) == 0) {
      return 0;
    }
  }
  return 1;
}

static int ir_float_sum_type_matches(const char *sum_type, int width_bits) {
  if (!sum_type) {
    return 0;
  }
  if (width_bits == 64) {
    return strcmp(sum_type, "float64") == 0;
  }
  return strcmp(sum_type, "float32") == 0;
}

/* Declared type of a function parameter by name, or NULL. (Locals come from
 * ir_function_local_declared_type, which does not see params -- they aren't
 * DECLARE_LOCAL'd.) */
static const char *ir_function_param_declared_type(const IRFunction *function,
                                                   const char *name) {
  if (!function || !name || !function->parameter_names ||
      !function->parameter_types) {
    return NULL;
  }
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return function->parameter_types[i];
    }
  }
  return NULL;
}

static int ir_float_scalar_operand_matches(IRFunction *function,
                                           const IROperand *operand,
                                           int width_bits) {
  if (!operand) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_FLOAT) {
    if (operand->float_bits == width_bits) {
      return 1;
    }
    /* A literal used in float32 context usually still carries the default
     * float64 tag (`2.5` lowers as a double). The kernel broadcasts the
     * constant at its own lane width, so accept the mismatch whenever that
     * narrowing is exact -- then the kernel's coefficient is bit-identical to
     * the one the scalar loop multiplies by. */
    if (width_bits == 32 &&
        (double)(float)operand->float_value == operand->float_value) {
      return 1;
    }
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    /* A scalar coefficient may be a local OR a parameter (e.g. saxpy's `a` in
     * `y[i] = a*x[i] + y[i]` when `a` is a function arg). The kernel reads it as
     * a symbol either way; only the float width must match. */
    const char *ty = ir_function_local_declared_type(function, operand->name);
    if (!ty) {
      ty = ir_function_param_declared_type(function, operand->name);
    }
    return ir_float_sum_type_matches(ty, width_bits);
  }
  return 0;
}

static int ir_try_clone_float_scalar_operand(IRFunction *function,
                                             size_t before_index,
                                             const IROperand *operand,
                                             int width_bits,
                                             IROperand *out) {
  const IRInstruction *producer = NULL;

  if (!out) {
    return 0;
  }
  *out = ir_operand_none();
  if (ir_float_scalar_operand_matches(function, operand, width_bits)) {
    if (operand->kind == IR_OPERAND_FLOAT) {
      /* Normalize the tag to the kernel's lane width (the match may have
       * accepted an exactly-narrowable float64-tagged literal). */
      *out = ir_operand_float_sized(operand->float_value, width_bits);
      return 1;
    }
    return ir_operand_clone(operand, out);
  }
  if (!operand || operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }

  producer = ir_find_temp_producer_before(function, before_index, operand->name);
  if (!producer || producer->op != IR_OP_CAST || !producer->text ||
      !ir_float_sum_type_matches(producer->text, width_bits)) {
    return 0;
  }
  if (producer->lhs.kind == IR_OPERAND_FLOAT) {
    *out = ir_operand_float_sized(producer->lhs.float_value, width_bits);
    return 1;
  }
  if (producer->lhs.kind == IR_OPERAND_INT) {
    *out = ir_operand_float_sized((double)producer->lhs.int_value, width_bits);
    return 1;
  }
  return 0;
}

/* Shared loop-frame matcher for the float reductions. Confirms `header_index`
 * begins a `while (iv < bound)` loop starting at iv == 0 with a unit increment
 * of `iv`, no nested while, and a back-jump, returning the body bounds and key
 * symbols. Returns 1
 * with *matched=1 on a clean frame; *matched=0 means "not this shape, skip". */
static int ir_float_reduction_frame(IRFunction *function, size_t header_index,
                                    const char **iv_out, size_t *branch_out,
                                    size_t *jump_out, IROperand *bound_compare,
                                    int *matched) {
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *loop_label = NULL;
  const char *exit_label = NULL;

  *matched = 0;
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  loop_label = header->text;

  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      (compare->rhs.kind != IR_OPERAND_SYMBOL &&
       compare->rhs.kind != IR_OPERAND_INT) ||
      (compare->rhs.kind == IR_OPERAND_SYMBOL && !compare->rhs.name) ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }
  exit_label = branch->text;

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, exit_label) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
  }
  if (ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }

  /* Bound: a parameter/inlined-param (always invariant), or any other
   * symbol -- a local or a GLOBAL (dimension globals like an LLM's D/HD are
   * the norm in real code) -- that the loop region never writes and whose
   * address never escapes. The kernel reads it once at entry; invariance
   * makes that identical to the scalar loop's per-iteration read. */
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
    if (ir_symbol_address_taken(function, compare->rhs.name)) {
      return 1;
    }
    for (size_t i = branch_index + 1; i < jump_index; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ir_instruction_writes_destination(ins) &&
          ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
          strcmp(ins->dest.name, compare->rhs.name) == 0) {
        return 1;
      }
    }
  }

  increment_index = jump_index;
  while (increment_index > branch_index + 1) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(
          &function->instructions[increment_index], compare->lhs.name)) {
    return 1;
  }
  /* Every fused kernel walks its array bases from element 0 and treats the
   * compare bound as the element COUNT, so the loop must provably start at
   * iv == 0. Catches `j = 3; while (j < n)` and `for i in 1..n` reductions
   * that previously vectorized as 0..n. */
  if (!ir_iv_zero_at_header(function, header_index, compare->lhs.name)) {
    return 1;
  }

  if (!ir_operand_clone(&compare->rhs, bound_compare)) {
    return 0;
  }
  *iv_out = compare->lhs.name;
  *branch_out = branch_index;
  *jump_out = jump_index;
  *matched = 1;
  return 1;
}

/* Reject body shapes that are not a pure read-only reduction (a store or call
 * would make the fused kernel unsound). */
static int ir_float_body_is_pure_reduction(IRFunction *function, size_t lo,
                                           size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_STORE || op == IR_OP_CALL || op == IR_OP_CALL_INDIRECT ||
        op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ || op == IR_OP_JUMP) {
      return 0;
    }
  }
  return 1;
}

static void ir_install_fused_reduction(IRFunction *function,
                                       size_t header_index, size_t jump_index,
                                       IRInstruction *fused, int *changed) {
  ir_instruction_destroy_storage(&function->instructions[header_index]);
  function->instructions[header_index] = *fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
}

static int ir_try_vectorize_sum_float_at(IRFunction *function,
                                         size_t header_index, int *changed) {
  const char *iv_symbol = NULL;
  const char *sum_symbol = NULL;
  const char *base_symbol = NULL;
  const char *sum_type = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  IRInstruction fused = {0};
  int matched = 0;
  int width_bits = 0;
  int found = 0;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name) {
      int bits = 0;
      const char *base = NULL;
      if (!ir_decode_float_indexed_load(function, i, ins->rhs.name, iv_symbol,
                                        &base, &bits)) {
        continue;
      }
      sum_symbol = ins->dest.name;
      base_symbol = base;
      width_bits = bits;
      found = 1;
    }
  }

  if (!found || !sum_symbol || !base_symbol ||
      strcmp(sum_symbol, iv_symbol) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }
  sum_type = ir_function_local_declared_type(function, sum_symbol);
  {
    /* Adding a run of floats four lanes at a time is a different sum from
     * adding them one at a time: floating-point addition does not associate,
     * and the two answers differ by rounding the compiler cannot bound without
     * knowing how many terms there are. Where the accumulator carries a
     * declared bound, that is a property the program asked the compiler to
     * keep, and it is refused here rather than quietly traded for speed. Where
     * it does not, the licence is real and goes on the belief ledger. */
    double bound_lo = 0.0;
    double bound_hi = 0.0;
    if (ir_lookup_float_bound(sum_type, &bound_lo, &bound_hi)) {
      if (ir_explain_enabled()) {
        char reason[256];
        snprintf(reason, sizeof(reason),
                 "the accumulator's declared type pins it to %g..%g, and "
                 "reassociating the sum into lanes moves the answer by a "
                 "rounding this loop's trip count does not bound; the "
                 "declared bound does not survive the rewrite",
                 bound_lo, bound_hi);
        ir_explain_remark(function->name, "loop body",
                          function->instructions[header_index].location, 0,
                          "NOT vectorized", reason, NULL, NULL);
        ir_explain_remark_code("float-bound-declared");
      }
      ir_operand_destroy(&bound);
      return 1;
    }
    ir_explain_belief(
        "floating-point reassociation",
        "a float sum was vectorized into lanes, which is a different order of "
        "addition from the one written; no accumulator in this build carried a "
        "declared bound to check the difference against");
  }
  if (!ir_float_sum_type_matches(sum_type, width_bits) ||
      !ir_symbol_is_float_array_base(function, base_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = (width_bits == 64) ? IR_OP_SIMD_SUM_F64 : IR_OP_SIMD_SUM_F32;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = width_bits;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(base_symbol);
  fused.rhs = bound;
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_simd_sum_float_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_sum_float_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

static int ir_try_vectorize_dot_float_at(IRFunction *function,
                                         size_t header_index, int *changed) {
  const char *iv_symbol = NULL;
  const char *sum_symbol = NULL;
  const char *a_symbol = NULL;
  const char *b_symbol = NULL;
  const char *sum_type = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  IRInstruction fused = {0};
  int matched = 0;
  int width_bits = 0;
  int found = 0;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IRInstruction *mul = NULL;
    int bits_a = 0;
    int bits_b = 0;
    const char *base_a = NULL;
    const char *base_b = NULL;
    if (!(ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
          ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name)) {
      continue;
    }
    mul = ir_find_temp_producer_before(function, i, ins->rhs.name);
    if (!mul || mul->op != IR_OP_BINARY || !mul->is_float || !mul->text ||
        strcmp(mul->text, "*") != 0 || mul->lhs.kind != IR_OPERAND_TEMP ||
        !mul->lhs.name || mul->rhs.kind != IR_OPERAND_TEMP || !mul->rhs.name) {
      continue;
    }
    if (!ir_decode_float_indexed_load(function, i, mul->lhs.name, iv_symbol,
                                      &base_a, &bits_a) ||
        !ir_decode_float_indexed_load(function, i, mul->rhs.name, iv_symbol,
                                      &base_b, &bits_b) ||
        bits_a != bits_b) {
      continue;
    }
    sum_symbol = ins->dest.name;
    a_symbol = base_a;
    b_symbol = base_b;
    width_bits = bits_a;
    found = 1;
  }

  if (!found || !sum_symbol || !a_symbol || !b_symbol ||
      strcmp(sum_symbol, iv_symbol) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }
  sum_type = ir_function_local_declared_type(function, sum_symbol);
  if (!ir_float_sum_type_matches(sum_type, width_bits) ||
      !ir_symbol_is_float_array_base(function, a_symbol) ||
      !ir_symbol_is_float_array_base(function, b_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = (width_bits == 64) ? IR_OP_SIMD_DOT_F64 : IR_OP_SIMD_DOT_F32;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = width_bits;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(a_symbol);
  fused.rhs = ir_operand_symbol(b_symbol);
  fused.arguments = calloc(1, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&bound);
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 1;
  fused.arguments[0] = bound;
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_simd_dot_float_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_dot_float_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

static IROperand ir_float_const_operand(double value, int width_bits) {
  return ir_operand_float_sized(value, width_bits == 32 ? 32 : 64);
}

static void ir_affine_map_terms_destroy(IRAffineMapTerms *terms) {
  if (!terms) {
    return;
  }
  if (terms->has_src_scale) {
    ir_operand_destroy(&terms->src_scale);
  }
  if (terms->has_dst_scale) {
    ir_operand_destroy(&terms->dst_scale);
  }
  if (terms->has_bias) {
    ir_operand_destroy(&terms->bias);
  }
  memset(terms, 0, sizeof(*terms));
}

/* An `if` in a loop body whose arms only choose a value. See the if-conversion
 * block below, which is where one is turned into a lane select. */
typedef struct VLoopDiamond {
  size_t branch_index; /* the branch_zero */
  size_t then_lo, then_hi;
  size_t else_lo, else_hi; /* else_lo == else_hi when there is no else arm */
  size_t end;              /* one past the closing label */
  const char *sym;         /* the one symbol the arms assign */
  int has_else;
} VLoopDiamond;

static int vloop_match_diamond(const IRFunction *function, size_t at,
                               size_t body_hi, VLoopDiamond *out);
static int vloop_region_is_pure(const IRFunction *function, size_t lo, size_t hi,
                                int depth);
static int vloop_region_writes_escape(IRFunction *function, size_t lo, size_t hi,
                                      size_t after);

/* `allow_diamonds` admits a body whose branches all bound value diamonds. The
 * DAG builder still has the last word: a diamond it cannot fold into a select
 * returns no root, and the loop stays scalar. */
static int ir_float_map_body_is_safe_ex(IRFunction *function, size_t lo,
                                        size_t hi, const char *iv_symbol,
                                        size_t *store_index_out,
                                        int allow_diamonds) {
  size_t store_count = 0;

  if (!function || !store_index_out) {
    return 0;
  }
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_STORE) {
      store_count++;
      *store_index_out = i;
      continue;
    }
    if (allow_diamonds) {
      VLoopDiamond dm;
      if (ins->op == IR_OP_BRANCH_ZERO &&
          vloop_match_diamond(function, i, hi, &dm)) {
        if (!vloop_region_is_pure(function, dm.then_lo, dm.then_hi, 1) ||
            !vloop_region_is_pure(function, dm.else_lo, dm.else_hi, 1) ||
            vloop_region_writes_escape(function, dm.then_lo, dm.else_hi,
                                       hi + 1)) {
          return 0;
        }
        i = dm.end - 1;
        continue;
      }
      if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP) {
        continue; /* the labels a matched diamond leaves behind */
      }
    }
    if (ir_instruction_writes_symbol(ins) &&
        !ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      /* A per-iteration local the DAG builder can substitute (`var x = a[i];
       * out[i] = x*x*x`) is fine, provided it does not outlive the loop -- the
       * fused kernel deletes the body, so a value read afterward would vanish. */
      if (ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
          ir_symbol_live_after_loop(function, hi + 1, ins->dest.name)) {
        return 0;
      }
    }
    if (ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_INLINE_ASM ||
        ins->op == IR_OP_MEMCPY_INLINE || ins->op == IR_OP_COUNT_WORD_STARTS) {
      return 0;
    }
  }

  return store_count == 1;
}

static int ir_float_map_body_is_safe(IRFunction *function, size_t lo, size_t hi,
                                     const char *iv_symbol,
                                     size_t *store_index_out) {
  return ir_float_map_body_is_safe_ex(function, lo, hi, iv_symbol,
                                      store_index_out, 0);
}

static int ir_affine_map_add_bias(IRAffineMapTerms *terms,
                                  const IROperand *bias) {
  if (!terms || !bias || terms->has_bias) {
    return 0;
  }
  if (!ir_operand_clone(bias, &terms->bias)) {
    return 0;
  }
  terms->has_bias = 1;
  return 1;
}

static int ir_affine_map_add_indexed_term(IRAffineMapTerms *terms,
                                          const char *base,
                                          const IROperand *scale) {
  if (!terms || !base || !scale) {
    return 0;
  }
  if (strcmp(base, terms->dst_base) == 0) {
    if (terms->has_dst_scale) {
      return 0;
    }
    if (!ir_operand_clone(scale, &terms->dst_scale)) {
      return 0;
    }
    terms->has_dst_scale = 1;
    return 1;
  }

  if (terms->src_base && strcmp(base, terms->src_base) != 0) {
    return 0;
  }
  terms->src_base = base;
  if (terms->has_src_scale) {
    return 0;
  }
  if (!ir_operand_clone(scale, &terms->src_scale)) {
    return 0;
  }
  terms->has_src_scale = 1;
  return 1;
}

static int ir_try_parse_affine_map_term(IRFunction *function, size_t before,
                                        const IROperand *operand,
                                        const char *iv_symbol,
                                        IRAffineMapTerms *terms) {
  const IRInstruction *producer = NULL;
  const char *base = NULL;
  IROperand scalar = {0};
  int bits = 0;

  if (!function || !operand || !iv_symbol || !terms) {
    return 0;
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name &&
      ir_decode_float_indexed_load(function, before, operand->name, iv_symbol,
                                   &base, &bits) &&
      bits == terms->width_bits) {
    IROperand one = ir_float_const_operand(1.0, terms->width_bits);
    int ok = ir_affine_map_add_indexed_term(terms, base, &one);
    ir_operand_destroy(&one);
    return ok;
  }

  if (ir_try_clone_float_scalar_operand(function, before, operand,
                                        terms->width_bits, &scalar)) {
    int ok = ir_affine_map_add_bias(terms, &scalar);
    ir_operand_destroy(&scalar);
    return ok;
  }

  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }

  producer = ir_find_temp_producer_before(function, before, operand->name);
  if (!producer || producer->op != IR_OP_BINARY || !producer->is_float ||
      !producer->text || strcmp(producer->text, "*") != 0) {
    return 0;
  }

  if (producer->lhs.kind == IR_OPERAND_TEMP && producer->lhs.name &&
      ir_decode_float_indexed_load(function, before, producer->lhs.name,
                                   iv_symbol, &base, &bits) &&
      bits == terms->width_bits &&
      ir_try_clone_float_scalar_operand(function, before, &producer->rhs,
                                        terms->width_bits, &scalar)) {
    int ok = ir_affine_map_add_indexed_term(terms, base, &scalar);
    ir_operand_destroy(&scalar);
    return ok;
  }
  if (producer->rhs.kind == IR_OPERAND_TEMP && producer->rhs.name &&
      ir_decode_float_indexed_load(function, before, producer->rhs.name,
                                   iv_symbol, &base, &bits) &&
      bits == terms->width_bits &&
      ir_try_clone_float_scalar_operand(function, before, &producer->lhs,
                                        terms->width_bits, &scalar)) {
    int ok = ir_affine_map_add_indexed_term(terms, base, &scalar);
    ir_operand_destroy(&scalar);
    return ok;
  }

  return 0;
}

static int ir_try_parse_affine_map_expr(IRFunction *function, size_t before,
                                        const IROperand *operand,
                                        const char *iv_symbol,
                                        IRAffineMapTerms *terms) {
  const IRInstruction *producer = NULL;

  if (!operand) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    producer = ir_find_temp_producer_before(function, before, operand->name);
    if (producer && producer->op == IR_OP_BINARY && producer->is_float &&
        producer->text && strcmp(producer->text, "+") == 0) {
      return ir_try_parse_affine_map_expr(function, before, &producer->lhs,
                                          iv_symbol, terms) &&
             ir_try_parse_affine_map_expr(function, before, &producer->rhs,
                                          iv_symbol, terms);
    }
  }

  return ir_try_parse_affine_map_term(function, before, operand, iv_symbol,
                                      terms);
}

static int ir_affine_map_terms_finalize(IRAffineMapTerms *terms) {
  if (!terms || !terms->dst_base) {
    return 0;
  }

  if (!terms->src_base) {
    terms->src_base = terms->dst_base;
    terms->src_scale = ir_float_const_operand(0.0, terms->width_bits);
    terms->has_src_scale = 1;
  }
  if (!terms->has_dst_scale) {
    terms->dst_scale = ir_float_const_operand(0.0, terms->width_bits);
    terms->has_dst_scale = 1;
  }
  if (!terms->has_bias) {
    terms->bias = ir_float_const_operand(0.0, terms->width_bits);
    terms->has_bias = 1;
  }
  return terms->has_src_scale && terms->has_dst_scale && terms->has_bias;
}

static int ir_try_vectorize_affine_map_float_at(IRFunction *function,
                                                size_t header_index,
                                                int *changed) {
  const char *iv_symbol = NULL;
  const char *dst_base = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t store_index = 0;
  IROperand bound = {0};
  IRAffineMapTerms terms = {0};
  IRInstruction fused = {0};
  int matched = 0;
  int store_bits = 0;
  const IRInstruction *store = NULL;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_map_body_is_safe(function, branch_index + 1, jump_index,
                                 iv_symbol, &store_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  store = &function->instructions[store_index];
  /* The indexed-address decode only proves a 4/8-byte unit-stride store; it
   * canNOT tell a float32 array from a uint32 one (both lower to base+(i<<2),
   * size 4). Without `store->is_float` an integer copy `out[i]=a[i]` matched
   * this FLOAT kernel, and `1.0*x` is not a bit-identity for integer data
   * whose bits form a float NaN (the multiply canonicalizes the payload). */
  if (!store->is_float || store->dest.kind != IR_OPERAND_TEMP ||
      !store->dest.name ||
      store->lhs.kind != IR_OPERAND_TEMP || !store->lhs.name ||
      store->rhs.kind != IR_OPERAND_INT ||
      (store->rhs.int_value != 4 && store->rhs.int_value != 8) ||
      !ir_decode_float_indexed_address(function, store_index, store->dest.name,
                                       iv_symbol, &dst_base, &store_bits) ||
      store_bits != store->rhs.int_value * 8) {
    ir_operand_destroy(&bound);
    return 1;
  }

  terms.dst_base = dst_base;
  terms.width_bits = store_bits;
  if (!ir_try_parse_affine_map_expr(function, store_index, &store->lhs,
                                    iv_symbol, &terms) ||
      !ir_affine_map_terms_finalize(&terms)) {
    ir_operand_destroy(&bound);
    ir_affine_map_terms_destroy(&terms);
    return 1;
  }

  if (!ir_symbol_is_float_array_base(function, terms.src_base) ||
      !ir_symbol_is_float_array_base(function, dst_base) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    ir_affine_map_terms_destroy(&terms);
    return 1;
  }

  fused.op = (store_bits == 64) ? IR_OP_SIMD_AFFINE_MAP_F64
                                : IR_OP_SIMD_AFFINE_MAP_F32;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = store_bits;
  fused.dest = ir_operand_symbol(dst_base);
  fused.lhs = ir_operand_symbol(terms.src_base);
  fused.rhs = ir_operand_symbol(dst_base);
  fused.arguments = calloc(4, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&bound);
    ir_affine_map_terms_destroy(&terms);
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 4;
  fused.arguments[0] = bound;
  fused.arguments[1] = terms.src_scale;
  fused.arguments[2] = terms.dst_scale;
  fused.arguments[3] = terms.bias;
  terms.has_src_scale = 0;
  terms.has_dst_scale = 0;
  terms.has_bias = 0;
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  ir_affine_map_terms_destroy(&terms);
  return 1;
}

int ir_simd_affine_map_float_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_affine_map_float_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* counter -> float64 chain -> (int64)trunc reduction -> IR_OP_SIMD_I2F_REDUCE  */
/* -------------------------------------------------------------------------- */

/* Op-codes for one chain step (stored as the INT operand of each argument pair;
 * the FLOAT operand is the step constant k). Applied to the running value x. */
#define I2F_STEP_MUL 0  /* x = x * k */
#define I2F_STEP_ADD 1  /* x = x + k */
#define I2F_STEP_SUBR 2 /* x = x - k */
#define I2F_STEP_SUBL 3 /* x = k - x */
#define I2F_STEP_DIVR 4 /* x = x / k */
#define I2F_MAX_STEPS 8

typedef struct {
  int op_code;
  double k;
} I2fChainStep;

/* Resolve the producer instruction of a temp (its definition) or a symbol (its
 * last write) before `before`. Returns NULL when none. */
static const IRInstruction *ir_i2f_resolve_producer(IRFunction *function,
                                                    size_t before,
                                                    const IROperand *op) {
  if (!op || !op->name) {
    return NULL;
  }
  if (op->kind == IR_OPERAND_TEMP) {
    return ir_find_temp_producer_before(function, before, op->name);
  }
  if (op->kind == IR_OPERAND_SYMBOL) {
    size_t wi = 0;
    if (ir_find_last_writer_before(function, before, IR_OPERAND_SYMBOL, op->name,
                                   &wi)) {
      return &function->instructions[wi];
    }
  }
  return NULL;
}

static int ir_i2f_operand_is_f64_const(const IROperand *op, double *value_out) {
  if (!op || op->kind != IR_OPERAND_FLOAT || op->float_bits != 64) {
    return 0;
  }
  *value_out = op->float_value;
  return 1;
}

/* Walk the straight-line float64 expression `op` down to the base `(float64)iv`,
 * pushing each binary-with-constant step into `steps` in base->outermost order.
 * Returns 1 on a fully-decoded affine/constant chain rooted at the counter. */
static int ir_i2f_extract_chain(IRFunction *function, size_t before,
                                const IROperand *op, const char *iv,
                                I2fChainStep *steps, int *nsteps) {
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return 0;
  }
  /* Base: x0 = (float64)i (an int->float cast of the loop counter). */
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      strcmp(p->text, "float64") == 0 &&
      ir_operand_is_symbol_named(&p->lhs, iv)) {
    return 1;
  }
  if (p->op != IR_OP_BINARY || !p->is_float || !p->text) {
    return 0;
  }

  double k = 0.0;
  int l_const = ir_i2f_operand_is_f64_const(&p->lhs, &k);
  double kr = 0.0;
  int r_const = ir_i2f_operand_is_f64_const(&p->rhs, &kr);
  const IROperand *inner = NULL;
  int code = -1;

  if (r_const && !l_const) {
    inner = &p->lhs;
    k = kr;
    if (strcmp(p->text, "+") == 0) {
      code = I2F_STEP_ADD;
    } else if (strcmp(p->text, "-") == 0) {
      code = I2F_STEP_SUBR;
    } else if (strcmp(p->text, "*") == 0) {
      code = I2F_STEP_MUL;
    } else if (strcmp(p->text, "/") == 0) {
      code = I2F_STEP_DIVR;
    } else {
      return 0;
    }
  } else if (l_const && !r_const) {
    inner = &p->rhs;
    /* k already holds the left constant. */
    if (strcmp(p->text, "+") == 0) {
      code = I2F_STEP_ADD;
    } else if (strcmp(p->text, "*") == 0) {
      code = I2F_STEP_MUL;
    } else if (strcmp(p->text, "-") == 0) {
      code = I2F_STEP_SUBL;
    } else {
      return 0; /* k / x is not affine in x; reject */
    }
  } else {
    return 0; /* both or neither constant: not a counter-affine step */
  }

  size_t pidx = (size_t)(p - function->instructions);
  if (!ir_i2f_extract_chain(function, pidx, inner, iv, steps, nsteps)) {
    return 0;
  }
  if (*nsteps >= I2F_MAX_STEPS) {
    return 0;
  }
  steps[*nsteps].op_code = code;
  steps[*nsteps].k = k;
  (*nsteps)++;
  return 1;
}

/* Evaluate the decoded chain at counter value i (host double, for range proof). */
static double ir_i2f_eval_chain(const I2fChainStep *steps, int nsteps,
                                double i) {
  double x = i;
  for (int s = 0; s < nsteps; s++) {
    double k = steps[s].k;
    switch (steps[s].op_code) {
    case I2F_STEP_MUL: x = x * k; break;
    case I2F_STEP_ADD: x = x + k; break;
    case I2F_STEP_SUBR: x = x - k; break;
    case I2F_STEP_SUBL: x = k - x; break;
    case I2F_STEP_DIVR: x = x / k; break;
    default: break;
    }
  }
  return x;
}

/* Resolve a compile-time-constant trip bound from the loop compare's rhs: either
 * a direct INT, or an (int*)cast of an INT constant. Returns 1 and *out on
 * success. */
static int ir_i2f_resolve_const_bound(IRFunction *function, size_t before,
                                      const IROperand *rhs, long long *out) {
  if (!rhs) {
    return 0;
  }
  if (rhs->kind == IR_OPERAND_INT) {
    *out = rhs->int_value;
    return 1;
  }
  if (rhs->kind == IR_OPERAND_TEMP && rhs->name) {
    const IRInstruction *p =
        ir_find_temp_producer_before(function, before, rhs->name);
    if (p && p->op == IR_OP_CAST && p->lhs.kind == IR_OPERAND_INT) {
      *out = p->lhs.int_value;
      return 1;
    }
  }
  return 0;
}

/* The loop body may only contain the reduction's straight-line float work: local
 * decls, casts, float/assign temps, nops, and the single counter increment. Any
 * store/call/branch/jump/nested-loop makes the fused kernel unsound. */
static int ir_i2f_body_is_safe(IRFunction *function, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    switch (function->instructions[i].op) {
    case IR_OP_STORE:
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_JUMP:
    case IR_OP_LABEL:
    case IR_OP_INLINE_ASM:
    case IR_OP_MEMCPY_INLINE:
    case IR_OP_NEW:
    case IR_OP_ADDRESS_OF:
    case IR_OP_RETURN:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

static int ir_try_vectorize_i2f_reduce_at(IRFunction *function,
                                          size_t header_index, int *changed) {
  size_t compare_index = 0;
  size_t branch_index = (size_t)-1;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *iv_symbol = NULL;
  const char *acc_symbol = NULL;
  const char *loop_label = NULL;
  const char *exit_label = NULL;
  long long bound = 0;
  I2fChainStep steps[I2F_MAX_STEPS];
  int nsteps = 0;
  int found = 0;
  IRInstruction fused = {0};

  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  loop_label = header->text;

  /* Find the loop's exit test. Unlike the array reductions, a constant trip
   * bound is materialized by an (int64)const cast between the header and the
   * compare, so locate the branch first, then its compare via the temp it
   * tests. */
  for (size_t i = header_index + 1; i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_BRANCH_ZERO) {
      branch_index = i;
      break;
    }
    if (op == IR_OP_JUMP || op == IR_OP_LABEL || op == IR_OP_BRANCH_EQ) {
      break;
    }
  }
  if (branch_index == (size_t)-1) {
    return 1;
  }
  const IRInstruction *branch = &function->instructions[branch_index];
  if (!branch->text || branch->lhs.kind != IR_OPERAND_TEMP || !branch->lhs.name) {
    return 1;
  }
  const IRInstruction *compare =
      ir_find_temp_producer_before(function, branch_index, branch->lhs.name);
  if (!compare || compare->op != IR_OP_BINARY || compare->is_float ||
      !compare->text || strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name) {
    return 1;
  }
  compare_index = (size_t)(compare - function->instructions);
  iv_symbol = compare->lhs.name;
  exit_label = branch->text;

  /* Trip count must be a compile-time constant so the range proof is sound. */
  if (!ir_i2f_resolve_const_bound(function, compare_index, &compare->rhs,
                                  &bound) ||
      bound < 1) {
    return 1;
  }

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, exit_label) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
  }
  if (ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_i2f_body_is_safe(function, branch_index + 1, jump_index)) {
    return 1;
  }

  /* Counter must step by +1 and be initialized to 0 before the loop (the kernel
   * walks i = 0..bound-1). */
  increment_index = jump_index;
  while (increment_index > branch_index + 1) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(
          &function->instructions[increment_index], iv_symbol)) {
    return 1;
  }
  /* ir_iv_zero_at_header refuses at the first control-flow join, unlike a
   * textual last-writer scan that an if/else init upstream could fool. */
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
    return 1;
  }

  /* Find the reduction: acc = acc + t, acc an int64 local, t = (int64)CHAIN. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IRInstruction *cast = NULL;
    const char *t = NULL;
    int local_nsteps = 0;
    if (!(ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
          ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name)) {
      continue;
    }
    t = ins->dest.name;
    if (strcmp(t, iv_symbol) == 0) {
      continue;
    }
    cast = ir_find_temp_producer_before(function, i, ins->rhs.name);
    if (!cast || cast->op != IR_OP_CAST || !cast->is_float || !cast->text ||
        strcmp(cast->text, "int64") != 0) {
      continue;
    }
    if (!ir_i2f_extract_chain(function, i, &cast->lhs, iv_symbol, steps,
                              &local_nsteps) ||
        local_nsteps < 1) {
      continue;
    }
    acc_symbol = ins->dest.name;
    nsteps = local_nsteps;
    found = 1;
  }

  if (!found || !acc_symbol) {
    return 1;
  }
  {
    const char *acc_type = ir_function_local_declared_type(function, acc_symbol);
    if (!acc_type || strcmp(acc_type, "int64") != 0) {
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    return 1;
  }

  /* Range proof: the chain is affine in i, so its extrema are at i=0 and
   * i=bound-1. Require every truncated value to fit a signed int32 (so the
   * packed cvttpd2dq is exact) and the integer sum to stay below 2^52 (so f64
   * accumulation of integer addends is exact and reassociation-safe). */
  {
    double v0 = ir_i2f_eval_chain(steps, nsteps, 0.0);
    double vN = ir_i2f_eval_chain(steps, nsteps, (double)(bound - 1));
    double vmax = v0 > vN ? v0 : vN;
    double vmin = v0 < vN ? v0 : vN;
    double abs_max = vmax > -vmin ? vmax : -vmin;
    if (!(vmin == vmin) || !(vmax == vmax)) {
      return 1; /* NaN (e.g. divide by zero in the chain) */
    }
    if (abs_max >= 2147483647.0) {
      return 1; /* per-element value would overflow int32 */
    }
    if (abs_max * (double)bound >= 4503599627370496.0 /* 2^52 */) {
      return 1; /* running integer sum could exceed exact f64 range */
    }
  }

  /* Build the fused instruction: dest = acc; arguments[0] = bound (int64),
   * then (op_code INT, constant FLOAT64) per chain step. */
  fused.op = IR_OP_SIMD_I2F_REDUCE_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 0;
  fused.dest = ir_operand_symbol(acc_symbol);
  fused.argument_count = (size_t)(1 + 2 * nsteps);
  fused.arguments = calloc(fused.argument_count, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.arguments[0] = ir_operand_int(bound);
  for (int s = 0; s < nsteps; s++) {
    fused.arguments[1 + 2 * s] = ir_operand_int(steps[s].op_code);
    fused.arguments[2 + 2 * s] = ir_operand_float_sized(steps[s].k, 64);
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_simd_i2f_reduce_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_i2f_reduce_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* General auto-vectorizer: float32/float64 straight-line-DAG counted loops    */
/*   -> IR_OP_SIMD_VLOOP_F64 (width carried in float_bits: 64 or 32)           */
/*                                                                             */
/* Runs AFTER the per-shape recognizers (sum/dot/affine/i2f) so it only claims */
/* loops they did not. Handles element-wise maps out[iv] = DAG(...) and '+'    */
/* reductions over either float width; the store/accumulator type pins it.     */
/* -------------------------------------------------------------------------- */

/* Node tags , must match the kernel decoder in simd_float.c. */
#define VLOOP_VN_LOAD 0  /* op0 = loaded-array index */
#define VLOOP_VN_IOTA 1  /* (float64)iv, or the raw iv for int lanes */
#define VLOOP_VN_CONST 2 /* op0 = constant index */
#define VLOOP_VN_ADD 3
#define VLOOP_VN_SUB 4
#define VLOOP_VN_MUL 5
#define VLOOP_VN_DIV 6
#define VLOOP_VN_SCALAR 7 /* op0 = runtime loop-invariant scalar index */
#define VLOOP_VN_AND 8    /* int lanes only */
#define VLOOP_VN_OR 9     /* int lanes only */
#define VLOOP_VN_XOR 10   /* int lanes only */
#define VLOOP_VN_SHL 11   /* int lanes only; op0 = node, op1 = literal count */
#define VLOOP_VN_SAR 12   /* int lanes only; arithmetic >> by a literal count */
#define VLOOP_VN_SHR 13   /* int lanes only; logical >> by a literal count */
#define VLOOP_VN_MIN 14   /* int lanes only; signed per-lane minimum */
#define VLOOP_VN_MAX 15   /* int lanes only; signed per-lane maximum */
/* If-conversion. CMPGT leaves an all-ones/all-zeros lane mask; SELECT consumes
 * that mask and a PAIR of values. PAIR evaluates to nothing of its own: it is
 * how a three-operand node fits a two-operand encoding, and the kernel steps
 * over it with both values already on its stack. */
#define VLOOP_VN_CMPGT 16 /* int lanes only; signed op0 > op1 */
#define VLOOP_VN_PAIR 17  /* op0 = then value, op1 = else value */
#define VLOOP_VN_SELECT 18 /* op0 = mask, op1 = pair */
#define VLOOP_VN_CMPEQ 19 /* int lanes only; op0 == op1 */

#define VLOOP_MAX_DIAMOND_DEPTH 4

#define VLOOP_MAX_NODES 48
#define VLOOP_MAX_ARRAYS 4 /* loaded bases; +dst must keep distinct bases <= 4 */
#define VLOOP_MAX_CONSTS 16
#define VLOOP_MAX_SCALARS 8
#define VLOOP_REG_BUDGET 4 /* ymm node-eval stack depth the kernel supports */
int code_generator_vloop_pool_size(int elem8, int has_iota);

typedef struct {
  int tag;
  int op0;
  int op1;
} VLoopNode;

typedef struct {
  VLoopNode nodes[VLOOP_MAX_NODES];
  int n_nodes;
  const char *arrays[VLOOP_MAX_ARRAYS]; /* loaded base symbols (deduped) */
  int n_arrays;
  double consts[VLOOP_MAX_CONSTS];      /* deduped (bit-compare); float DAGs */
  long long iconsts[VLOOP_MAX_CONSTS];  /* deduped; int DAGs */
  int n_consts;
  const char *scalars[VLOOP_MAX_SCALARS]; /* invariant scalar symbols (deduped) */
  int n_scalars;
  int width_bits;
  int is_int; /* 0 = float lanes (width_bits 32/64), 1 = int32 lanes */
  /* Element width in MEMORY, which int lanes decouple from lane width: 32 by
   * default, or 8 for a byte map, where the loads widen into int32 lanes and
   * the store truncates back. Every array in one DAG shares it. */
  int elem_bits;
  int elem_unsigned; /* byte elements: zero-extend (uint8) vs sign-extend */
  size_t body_lo; /* loop body region, for symbol-invariance checks */
  size_t body_hi;
  int has_iota;
  int overflow; /* a table limit was exceeded -> refuse */
  int resolve_depth; /* body-local substitution recursion guard */
  int iota_bound_known; /* the loop's trip count is a compile-time constant */
  long long iota_bound; /* that constant (iv ranges over [0, iota_bound)) */
  /* Enclosing if-converted arms, innermost last. A name read inside an arm
   * resolves within that arm first; failing that, to the value that reached the
   * arm's diamond. Without this, resolving a name in the ELSE arm walked
   * through the THEN arm and took its write. */
  struct {
    size_t lo;          /* first instruction of the arm */
    size_t hi;          /* one past its last */
    size_t incoming_hi; /* the diamond's branch: where the arm's input settled */
  } regions[VLOOP_MAX_DIAMOND_DEPTH + 1];
  int n_regions;
} VLoopDag;

#define VLOOP_MAX_RESOLVE_DEPTH 16

/* A map may go deeper when the kernel has ymm registers left over. Sized by the
 * kernel itself so the two cannot disagree about what is spendable. */
static int vloop_map_budget(const VLoopDag *d) {
  return code_generator_vloop_pool_size(d->elem_bits == 8, d->has_iota);
}

static int vloop_tag_is_leaf(int tag) {
  return tag == VLOOP_VN_LOAD || tag == VLOOP_VN_IOTA ||
         tag == VLOOP_VN_CONST || tag == VLOOP_VN_SCALAR;
}

static int vloop_intern_array(VLoopDag *d, const char *base) {
  for (int i = 0; i < d->n_arrays; i++) {
    if (strcmp(d->arrays[i], base) == 0) {
      return i;
    }
  }
  if (d->n_arrays >= VLOOP_MAX_ARRAYS) {
    d->overflow = 1;
    return -1;
  }
  d->arrays[d->n_arrays] = base;
  return d->n_arrays++;
}

static int vloop_intern_const(VLoopDag *d, double v) {
  for (int i = 0; i < d->n_consts; i++) {
    if (memcmp(&d->consts[i], &v, sizeof(double)) == 0) {
      return i;
    }
  }
  if (d->n_consts >= VLOOP_MAX_CONSTS) {
    d->overflow = 1;
    return -1;
  }
  d->consts[d->n_consts] = v;
  return d->n_consts++;
}

static int vloop_intern_iconst(VLoopDag *d, long long v) {
  for (int i = 0; i < d->n_consts; i++) {
    if (d->iconsts[i] == v) {
      return i;
    }
  }
  if (d->n_consts >= VLOOP_MAX_CONSTS) {
    d->overflow = 1;
    return -1;
  }
  d->iconsts[d->n_consts] = v;
  return d->n_consts++;
}

static int vloop_intern_scalar(VLoopDag *d, const char *name) {
  for (int i = 0; i < d->n_scalars; i++) {
    if (strcmp(d->scalars[i], name) == 0) {
      return i;
    }
  }
  if (d->n_scalars >= VLOOP_MAX_SCALARS) {
    d->overflow = 1;
    return -1;
  }
  d->scalars[d->n_scalars] = name;
  return d->n_scalars++;
}

/* A symbol written anywhere in the loop body is not a single value across
 * iterations (the accumulator, a rotating local): it can be neither broadcast
 * nor producer-chased (the chase would bake the loop-ENTRY value into every
 * lane). */
static int vloop_symbol_written_in_body(const IRFunction *function,
                                        const VLoopDag *d, const char *name) {
  for (size_t i = d->body_lo; i < d->body_hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int vloop_add_node(VLoopDag *d, int tag, int op0, int op1) {
  if (d->n_nodes >= VLOOP_MAX_NODES) {
    d->overflow = 1;
    return -1;
  }
  d->nodes[d->n_nodes].tag = tag;
  d->nodes[d->n_nodes].op0 = op0;
  d->nodes[d->n_nodes].op1 = op1;
  return d->n_nodes++;
}

static int vloop_text_is_float_width(const char *text, int width_bits) {
  return (width_bits == 64 && strcmp(text, "float64") == 0) ||
         (width_bits == 32 && strcmp(text, "float32") == 0);
}

/* A float64 literal is admissible in a float32 DAG only when it narrows to
 * float32 EXACTLY (round-trips). Mettle defaults float literals to float64, so
 * `a[i] * 2.0` carries a float64 `2.0`; the f32 kernel broadcasts `(float)2.0`,
 * which for an exactly-representable value is the IDENTICAL number the literal
 * denotes. The only residual difference from the scalar loop is then the same
 * f32-lane-vs-f64-intermediate rounding the runtime-scalar reduction (`k*a[i]`)
 * already ships with -- so this is exactly as faithful as that. A non-exact
 * literal (0.1) would make the f32 coefficient differ from the value the scalar
 * loop multiplies by, so it is refused. Mirrors the affine kernel's policy. */
static int vloop_f64_narrows_exactly(double v) {
  return (double)(float)v == v;
}

/* A compile-time float literal: a FLOAT operand of the right width (or an
 * exactly-narrowable float64 literal in a float32 DAG), or a temp that is a
 * cast of an int/float literal to that width. Crucially this does NOT match
 * loop-invariant scalar *symbols* (parameters) , those are a runtime broadcast
 * handled via VLOOP_VN_SCALAR, so leaving them here makes the pass cleanly
 * refuse rather than miscompile. */
static int vloop_operand_is_literal(IRFunction *function, size_t before,
                                    const IROperand *op, int width_bits,
                                    double *out) {
  if (op->kind == IR_OPERAND_FLOAT) {
    if (op->float_bits == width_bits) {
      *out = op->float_value;
      return 1;
    }
    if (width_bits == 32 && op->float_bits == 64 &&
        vloop_f64_narrows_exactly(op->float_value)) {
      *out = op->float_value;
      return 1;
    }
  }
  if (op->kind == IR_OPERAND_TEMP && op->name) {
    const IRInstruction *p =
        ir_find_temp_producer_before(function, before, op->name);
    if (p && p->op == IR_OP_CAST && p->text &&
        vloop_text_is_float_width(p->text, width_bits)) {
      if (p->lhs.kind == IR_OPERAND_FLOAT) {
        *out = p->lhs.float_value;
        return 1;
      }
      if (p->lhs.kind == IR_OPERAND_INT) {
        *out = (double)p->lhs.int_value;
        return 1;
      }
    }
  }
  return 0;
}

static int vloop_binop_tag(const char *text) {
  if (strcmp(text, "+") == 0) return VLOOP_VN_ADD;
  if (strcmp(text, "-") == 0) return VLOOP_VN_SUB;
  if (strcmp(text, "*") == 0) return VLOOP_VN_MUL;
  if (strcmp(text, "/") == 0) return VLOOP_VN_DIV;
  return -1;
}

static int vloop_resolve_body_local(IRFunction *function, const char *sym,
                                    const char *iv, VLoopDag *d);

/* Recursively lower a float operand into the DAG; returns the node index or -1
 * to refuse. Builds a TREE (shared subexpressions are re-evaluated) so a simple
 * stack-machine kernel can replay it. */
static int vloop_build(IRFunction *function, size_t before, const IROperand *op,
                       const char *iv, VLoopDag *d) {
  if (!op || d->overflow) {
    return -1;
  }
  double cv = 0.0;
  if (vloop_operand_is_literal(function, before, op, d->width_bits, &cv)) {
    int ci = vloop_intern_const(d, cv);
    return ci < 0 ? -1 : vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return -1;
  }
  /* array load a[iv] (only a TEMP names a load result) */
  if (op->kind == IR_OPERAND_TEMP) {
    const char *base = NULL;
    int bits = 0;
    if (ir_decode_float_indexed_load(function, before, op->name, iv, &base,
                                     &bits) &&
        bits == d->width_bits) {
      int ai = vloop_intern_array(d, base);
      return ai < 0 ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
  }
  if (op->kind == IR_OPERAND_SYMBOL) {
    if (vloop_symbol_written_in_body(function, d, op->name)) {
      /* A symbol written in the body is not a stable broadcast value, but if
       * it is a single-assignment per-iteration LOCAL (`var d = a[i]-b[i]`),
       * substitute its defining expression into the DAG -- this is what makes
       * SSD / variance / `var x=...; x*x*x` shapes vectorize. */
      return vloop_resolve_body_local(function, op->name, iv, d);
    }
    /* Loop-invariant float scalar of the lane width (a local or parameter,
     * e.g. saxpy's runtime `a`): read once at loop entry and broadcast.
     * Preferred over chasing its pre-loop producer -- one slot beats
     * re-evaluating an invariant expression per lane. */
    if (ir_float_scalar_operand_matches(function, op, d->width_bits) &&
        !ir_symbol_address_taken(function, op->name)) {
      int si = vloop_intern_scalar(d, op->name);
      return si < 0 ? -1 : vloop_add_node(d, VLOOP_VN_SCALAR, si, 0);
    }
  }
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return -1;
  }
  size_t pidx = (size_t)(p - function->instructions);
  /* (float64)iv */
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      vloop_text_is_float_width(p->text, d->width_bits) &&
      ir_operand_is_symbol_named(&p->lhs, iv)) {
    d->has_iota = 1;
    return vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
  }
  /* (float64)(C +/- iv) / (float64)(iv +/- C): an INTEGER affine function of the
   * induction var cast to float64. Sound to evaluate in the f64 domain as
   * `IOTA +/- (float64)C` because every int32 value (and the int32 sum/difference
   * when it does not overflow) is exactly representable in float64, so the f64
   * op is bit-identical to casting the integer result. Gated to float64 (the
   * equivalence fails for float32 once |value| >= 2^24) and to a compile-time
   * trip count, so the no-overflow range check is decidable. */
  if (p->op == IR_OP_CAST && !p->is_float && p->text && d->width_bits == 64 &&
      vloop_text_is_float_width(p->text, d->width_bits) && d->iota_bound_known &&
      d->iota_bound > 0 &&
      (p->lhs.kind == IR_OPERAND_TEMP || p->lhs.kind == IR_OPERAND_SYMBOL)) {
    const IRInstruction *q = ir_i2f_resolve_producer(function, pidx, &p->lhs);
    if (q && q->op == IR_OP_BINARY && !q->is_float && q->text &&
        (strcmp(q->text, "+") == 0 || strcmp(q->text, "-") == 0)) {
      int is_sub = strcmp(q->text, "-") == 0;
      int iv_left = ir_operand_is_symbol_named(&q->lhs, iv);
      int iv_right = ir_operand_is_symbol_named(&q->rhs, iv);
      long long C = 0;
      int have = 0, on_left = 0;
      if (iv_left && q->rhs.kind == IR_OPERAND_INT) {
        C = q->rhs.int_value; have = 1; on_left = 1;
      } else if (iv_right && q->lhs.kind == IR_OPERAND_INT) {
        C = q->lhs.int_value; have = 1; on_left = 0;
      }
      if (have) {
        long long hi = d->iota_bound - 1; /* max iv */
        long long rmin, rmax;
        if (on_left) { /* iv (+/-) C */
          rmin = is_sub ? -C : C;
          rmax = is_sub ? hi - C : hi + C;
        } else { /* C (+/-) iv */
          rmin = is_sub ? C - hi : C;
          rmax = is_sub ? C : C + hi;
        }
        if (rmin >= -2147483648LL && rmax <= 2147483647LL) {
          int tag = vloop_binop_tag(q->text);
          int ci = vloop_intern_const(d, (double)C);
          if (tag < 0 || ci < 0) return -1;
          /* The kernel is a postorder stack machine: a binary node pops the two
           * most-recently built results as (left, right) and computes left OP
           * right. So build the LEFT operand first to preserve subtraction order
           * (`iv - C` vs `C - iv`). */
          int a, b;
          if (on_left) { /* iv OP C : left = iota */
            d->has_iota = 1;
            a = vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
            b = vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
          } else { /* C OP iv : left = const */
            a = vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
            d->has_iota = 1;
            b = vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
          }
          if (a < 0 || b < 0) return -1;
          return vloop_add_node(d, tag, a, b);
        }
      }
    }
  }
  /* binary float op */
  if (p->op == IR_OP_BINARY && p->is_float && p->text) {
    int tag = vloop_binop_tag(p->text);
    if (tag < 0) {
      return -1;
    }
    int a = vloop_build(function, pidx, &p->lhs, iv, d);
    if (a < 0) {
      return -1;
    }
    int b = vloop_build(function, pidx, &p->rhs, iv, d);
    if (b < 0) {
      return -1;
    }
    return vloop_add_node(d, tag, a, b);
  }
  return -1;
}

/* Substitute a single-assignment loop-body local into the DAG by building from
 * its defining expression in place of the symbol. This is what lets
 * `var d = a[i] - b[i]; s = s + d*d` (sum-of-squared-differences / variance)
 * and `var x = a[i]; out[i] = x*x*x` vectorize: the local is not a broadcast
 * value, it is an alias for a per-iteration expression. Guards keep it sound:
 *   - exactly ONE in-body definition (else it could be a recurrence or a
 *     conditionally-set value, neither of which is a pure alias);
 *   - not live after the loop (the fused kernel deletes the body);
 *   - bounded substitution depth, so a cycle of mutually-referential locals
 *     (or the accumulator referring to itself) refuses instead of recursing
 *     without end.
 * Re-evaluating the aliased subexpression at each use is correct (the local
 * held exactly that value); it only costs redundant compute the kernel could
 * later CSE. */
static int vloop_resolve_body_local(IRFunction *function, const char *sym,
                                    const char *iv, VLoopDag *d) {
  if (!sym || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  const IRInstruction *def = NULL;
  size_t def_idx = 0;
  for (size_t i = d->body_lo; i < d->body_hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, sym) == 0) {
      if (def) {
        return -1; /* written more than once: not a simple per-iteration alias */
      }
      def = ins;
      def_idx = i;
    }
  }
  if (!def || ir_symbol_live_after_loop(function, d->body_hi + 1, sym)) {
    return -1;
  }
  d->resolve_depth++;
  int result = -1;
  if (def->op == IR_OP_BINARY && def->is_float && def->text) {
    int tag = vloop_binop_tag(def->text);
    if (tag >= 0) {
      int a = vloop_build(function, def_idx, &def->lhs, iv, d);
      int b = (a < 0) ? -1 : vloop_build(function, def_idx, &def->rhs, iv, d);
      if (a >= 0 && b >= 0) {
        result = vloop_add_node(d, tag, a, b);
      }
    }
  } else if (def->op == IR_OP_ASSIGN) {
    result = vloop_build(function, def_idx, &def->lhs, iv, d);
  } else if (def->op == IR_OP_LOAD && def->is_float &&
             def->lhs.kind == IR_OPERAND_TEMP && def->lhs.name &&
             def->rhs.kind == IR_OPERAND_INT &&
             def->rhs.int_value == d->width_bits / 8) {
    /* `var x = a[i]` lowers to a LOAD straight into the symbol; rebuild it as
     * an indexed array load by decoding the address. */
    const char *base = NULL;
    int bits = 0;
    if (ir_decode_float_indexed_address(function, def_idx, def->lhs.name, iv,
                                        &base, &bits) &&
        bits == d->width_bits) {
      int ai = vloop_intern_array(d, base);
      result = (ai < 0) ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
  }
  d->resolve_depth--;
  return result;
}

/* Stack-machine evaluation depth (= ymm registers the kernel needs). Matches
 * the kernel's naive left-then-right post-order: eval a, hold it while eval b,
 * then combine. */
static int vloop_eval_depth(const VLoopDag *d, int node) {
  const VLoopNode *n = &d->nodes[node];
  if (vloop_tag_is_leaf(n->tag)) {
    return 1; /* leaf: LOAD / IOTA / CONST / SCALAR */
  }
  if (n->tag == VLOOP_VN_SHL || n->tag == VLOOP_VN_SAR ||
      n->tag == VLOOP_VN_SHR) {
    return vloop_eval_depth(d, n->op0); /* unary, evaluated in place */
  }
  if (n->tag == VLOOP_VN_SELECT) {
    /* mask, then and else are all live at once, in that order. */
    const VLoopNode *pair = &d->nodes[n->op1];
    int dm = vloop_eval_depth(d, n->op0);
    int dt = 1 + vloop_eval_depth(d, pair->op0);
    int de = 2 + vloop_eval_depth(d, pair->op1);
    int best = dm > dt ? dm : dt;
    return best > de ? best : de;
  }
  int da = vloop_eval_depth(d, n->op0);
  int db = vloop_eval_depth(d, n->op1);
  int alt = 1 + db;
  return da > alt ? da : alt;
}

/* Count distinct base pointers the kernel must keep in GP registers: the loaded
 * arrays plus the destination if it is not already among them. */
static int vloop_distinct_bases(const VLoopDag *d, const char *dst_base) {
  int n = d->n_arrays;
  for (int i = 0; i < d->n_arrays; i++) {
    if (strcmp(d->arrays[i], dst_base) == 0) {
      return n; /* dst is a loaded array too */
    }
  }
  return n + 1;
}

static int vloop_serialize_into(IRInstruction *fused, const VLoopDag *d,
                                int reduce_op, int root, int depth) {
  size_t argc = (size_t)(7 + d->n_arrays + d->n_scalars + 3 * d->n_nodes +
                         d->n_consts);
  fused->arguments = calloc(argc, sizeof(IROperand));
  if (!fused->arguments) {
    return 0;
  }
  fused->argument_count = argc;
  size_t k = 0;
  fused->arguments[k++] = ir_operand_int(reduce_op);
  fused->arguments[k++] = ir_operand_int(d->n_arrays);
  fused->arguments[k++] = ir_operand_int(d->n_nodes);
  fused->arguments[k++] = ir_operand_int(root);
  fused->arguments[k++] = ir_operand_int(d->n_consts);
  fused->arguments[k++] = ir_operand_int(d->n_scalars);
  fused->arguments[k++] = ir_operand_int(depth);
  for (int i = 0; i < d->n_arrays; i++) {
    fused->arguments[k++] = ir_operand_symbol(d->arrays[i]);
  }
  for (int i = 0; i < d->n_scalars; i++) {
    fused->arguments[k++] = ir_operand_symbol(d->scalars[i]);
  }
  for (int i = 0; i < d->n_nodes; i++) {
    fused->arguments[k++] = ir_operand_int(d->nodes[i].tag);
    fused->arguments[k++] = ir_operand_int(d->nodes[i].op0);
    fused->arguments[k++] = ir_operand_int(d->nodes[i].op1);
  }
  for (int i = 0; i < d->n_consts; i++) {
    fused->arguments[k++] = d->is_int
                                ? ir_operand_int(d->iconsts[i])
                                : ir_operand_float_sized(d->consts[i], 64);
  }
  return 1;
}

static int ir_try_vectorize_map_at(IRFunction *function, size_t header_index,
                                   int *changed) {
  const char *iv_symbol = NULL;
  const char *dst_base = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t store_index = 0;
  IROperand bound = {0};
  int matched = 0;
  int store_bits = 0;
  VLoopDag d;
  int root = -1;
  int depth = 0;
  const IRInstruction *store = NULL;
  IRInstruction fused = {0};

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_map_body_is_safe(function, branch_index + 1, jump_index,
                                 iv_symbol, &store_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  store = &function->instructions[store_index];
  /* `store->is_float` gate: a uint32 store has the same base+(i<<2) shape as a
   * float32 one, so without this an integer map would build a float DAG. */
  if (!store->is_float || store->dest.kind != IR_OPERAND_TEMP ||
      !store->dest.name ||
      (store->lhs.kind != IR_OPERAND_TEMP && store->lhs.kind != IR_OPERAND_SYMBOL &&
       store->lhs.kind != IR_OPERAND_FLOAT) ||
      store->rhs.kind != IR_OPERAND_INT ||
      (store->rhs.int_value != 4 && store->rhs.int_value != 8) ||
      !ir_decode_float_indexed_address(function, store_index, store->dest.name,
                                       iv_symbol, &dst_base, &store_bits) ||
      store_bits != store->rhs.int_value * 8) {
    ir_operand_destroy(&bound);
    return 1;
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = store_bits; /* 64 (float64) or 32 (float32) */
  d.body_lo = branch_index + 1;
  d.body_hi = jump_index;
  if (bound.kind == IR_OPERAND_INT) {
    d.iota_bound_known = 1;
    d.iota_bound = bound.int_value;
  }
  root = vloop_build(function, store_index, &store->lhs, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }

  /* Gates. */
  if (!ir_symbol_is_float_array_base(function, dst_base)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > vloop_map_budget(&d) ||
      vloop_distinct_bases(&d, dst_base) > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = store_bits;
  fused.dest = ir_operand_symbol(dst_base);
  fused.lhs = bound; /* take ownership of the cloned bound operand */
  if (!vloop_serialize_into(&fused, &d, /*reduce_op=*/0, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

/* '+' reduction over a float64 DAG: acc = acc + DAG(a_k[iv], (float64)iv,
 * consts). Picks up reductions the sum/dot recognizers (which run earlier) did
 * not claim: sum-of-products, polynomial-in-iv, multi-array combinations. */
static int ir_try_vectorize_reduce_at(IRFunction *function, size_t header_index,
                                      int *changed) {
  const char *iv_symbol = NULL;
  const char *acc_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t reduce_index = 0;
  IROperand bound = {0};
  int matched = 0;
  int found = 0;
  const IROperand *addend = NULL;
  VLoopDag d;
  int root = -1;
  int depth = 0;
  int width_bits = 0;
  IRInstruction fused = {0};

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  /* The accumulation appears in one of two equivalent IR forms:
   *   direct       `acc = acc + X`              (dest is the acc symbol)
   *   temp+ASSIGN  `%t = acc + X; acc <- %t`    (dest is a temp, then copied)
   * The latter survives when X is a float64-tracked expression narrowed to a
   * float32 acc (e.g. `s += a[i] * 2.0`): copy-prop won't fold the temp across
   * the narrowing, so the direct form never forms. Both are the same reduction;
   * `assign_index` records the trailing ASSIGN so it is exempted from the
   * written-once check below. */
  size_t assign_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!(ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 &&
          (ins->rhs.kind == IR_OPERAND_TEMP ||
           ins->rhs.kind == IR_OPERAND_SYMBOL))) {
      continue;
    }
    if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name)) {
      acc_symbol = ins->dest.name;
      addend = &ins->rhs;
      reduce_index = i;
      assign_index = (size_t)-1;
      found++;
    } else if (ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
               ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name) {
      /* `%t = acc + X`: confirm the next non-NOP copies %t straight back into
       * the same symbol (`acc <- %t`). */
      size_t j = i + 1;
      while (j < jump_index && function->instructions[j].op == IR_OP_NOP) {
        j++;
      }
      if (j < jump_index) {
        const IRInstruction *asg = &function->instructions[j];
        if (asg->op == IR_OP_ASSIGN &&
            ir_operand_is_symbol_named(&asg->dest, ins->lhs.name) &&
            ir_operand_is_temp_named(&asg->lhs, ins->dest.name)) {
          acc_symbol = ins->lhs.name;
          addend = &ins->rhs;
          reduce_index = i;
          assign_index = j;
          found++;
        }
      }
    }
  }
  if (found != 1 || !acc_symbol || strcmp(acc_symbol, iv_symbol) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }
  {
    const char *acc_type = ir_function_local_declared_type(function, acc_symbol);
    if (acc_type && strcmp(acc_type, "float64") == 0) {
      width_bits = 64;
    } else if (acc_type && strcmp(acc_type, "float32") == 0) {
      width_bits = 32;
    } else {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  /* acc must be written only by the single reduction instruction, and no
   * OTHER symbol may be written in the body besides the iv increment -- a
   * rotating local would be lost when the loop is fused away. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i == reduce_index || i == assign_index) {
      continue; /* the accumulation itself (temp+ASSIGN spans two slots) */
    }
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name) {
      /* The accumulator may be written ONLY by the reduction. */
      if (strcmp(ins->dest.name, acc_symbol) == 0) {
        ir_operand_destroy(&bound);
        return 1;
      }
      /* A non-iv symbol write is tolerated only when the symbol is a
       * per-iteration LOCAL that does not outlive the loop: the DAG builder
       * substitutes it (`var d = a[i]-b[i]; s += d*d`), and the fused kernel
       * deletes the body, so a value live afterward would be lost. */
      if (strcmp(ins->dest.name, iv_symbol) != 0 &&
          ir_symbol_live_after_loop(function, jump_index + 1, ins->dest.name)) {
        ir_operand_destroy(&bound);
        return 1;
      }
    }
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = width_bits; /* 64 (float64) or 32 (float32) */
  d.body_lo = branch_index + 1;
  d.body_hi = jump_index;
  if (bound.kind == IR_OPERAND_INT) {
    d.iota_bound_known = 1;
    d.iota_bound = bound.int_value;
  }
  root = vloop_build(function, reduce_index, addend, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > VLOOP_REG_BUDGET - 1 /* ymm2 reserved as accumulator */ ||
      d.n_arrays > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = width_bits;
  fused.dest = ir_operand_symbol(acc_symbol);
  fused.lhs = bound; /* take ownership */
  if (!vloop_serialize_into(&fused, &d, /*reduce_op=*/1, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Multi-store map fission: a counted loop whose body is K>=2 independent       */
/* unit-stride float stores (e.g. saxpy init `x[i]=f(i); y[i]=g(i)`). Each      */
/* store is emitted as its own full-count IR_OP_SIMD_VLOOP_F64 -- semantically  */
/* loop fission (all of store_1, then all of store_2) -- reusing the proven     */
/* single-store kernel unchanged. SOUND ONLY when the destinations are disjoint */
/* (reordering writes across the lane window is otherwise observable), so it is */
/* gated on a conservative non-aliasing proof below.                            */

#define MULTISTORE_MAX 8

static int ir_msf_name_is_allocator(const char *n) {
  if (!n) return 0;
  static const char *const a[] = {"malloc",        "calloc",
                                  "aligned_alloc", "_aligned_malloc",
                                  "alloc_zeroed",  NULL};
  for (int i = 0; a[i]; i++)
    if (strcmp(n, a[i]) == 0) return 1;
  return 0;
}

/* The single instruction that defines symbol/temp `name`, or -1 if there is not
 * exactly one (a conservative "give up" for the disjointness proof). */
static int ir_msf_single_def(IRFunction *function, const char *name,
                             int is_symbol) {
  int found = -1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    /* A SIMD array op carries the output array's base symbol as its dest by
     * convention (`@a = simd_vloop_f64(...)`): it writes the array ELEMENTS, not
     * the base pointer's value, so it must not count as a (re)definition of the
     * pointer -- otherwise a sibling already-vectorized loop on the same array
     * (e.g. saxpy's hot loop on @y) would mask its fresh-allocation provenance. */
    if (ins->op >= IR_OP_SIMD_SUM_I32 && ins->op <= IR_OP_SIMD_LCG_U32) continue;
    if (!ir_instruction_writes_destination(ins)) continue;
    const IROperand *d = &ins->dest;
    int match = is_symbol
                    ? (d->kind == IR_OPERAND_SYMBOL && d->name && name &&
                       strcmp(d->name, name) == 0)
                    : (d->kind == IR_OPERAND_TEMP && d->name && name &&
                       strcmp(d->name, name) == 0);
    if (match) {
      if (found >= 0) return -1;
      found = (int)i;
    }
  }
  return found;
}

/* True if `v` provably holds the result of a fresh heap allocation, following
 * cast/assign hops through singly-defined temps (bounded depth). */
static int ir_msf_value_is_fresh_alloc(IRFunction *function, const IROperand *v,
                                       int depth) {
  if (depth <= 0 || !v ||
      (v->kind != IR_OPERAND_TEMP && v->kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  int di = ir_msf_single_def(function, v->name, v->kind == IR_OPERAND_SYMBOL);
  if (di < 0) return 0;
  const IRInstruction *def = &function->instructions[di];
  if (def->op == IR_OP_NEW) return 1;
  if (def->op == IR_OP_CALL && ir_msf_name_is_allocator(def->text)) return 1;
  if (def->op == IR_OP_CAST || def->op == IR_OP_ASSIGN) {
    return ir_msf_value_is_fresh_alloc(function, &def->lhs, depth - 1);
  }
  return 0;
}

/* Every destination is a distinct local pointer, address never taken, defined
 * exactly once from a fresh allocation -- two distinct allocations never alias,
 * so the per-store full-count rewrite preserves the scalar memory state. */
static int ir_msf_bases_disjoint(IRFunction *function, const char **bases,
                                 int k) {
  for (int i = 0; i < k; i++) {
    if (!bases[i]) return 0;
    for (int j = i + 1; j < k; j++)
      if (!bases[j] || strcmp(bases[i], bases[j]) == 0) return 0;
  }
  for (int i = 0; i < k; i++) {
    if (ir_function_local_declared_type(function, bases[i]) == NULL) return 0;
    if (ir_symbol_address_taken(function, bases[i])) return 0;
    int di = ir_msf_single_def(function, bases[i], 1);
    if (di < 0) return 0;
    const IRInstruction *def = &function->instructions[di];
    if (def->op == IR_OP_NEW) continue;
    if (def->op == IR_OP_CALL && ir_msf_name_is_allocator(def->text)) continue;
    if ((def->op == IR_OP_CAST || def->op == IR_OP_ASSIGN) &&
        ir_msf_value_is_fresh_alloc(function, &def->lhs, 4)) {
      continue;
    }
    return 0;
  }
  return 1;
}

/* Collect the store indices of a multi-store map body; require it otherwise
 * pure (no loads -- so the only memory dependence is between the stores -- no
 * calls/branches, and any per-iteration local does not outlive the loop). */
static int ir_msf_body_is_safe(IRFunction *function, size_t lo, size_t hi,
                               const char *iv_symbol, size_t *stores,
                               int *nstores) {
  int ns = 0;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_STORE) {
      if (ns >= MULTISTORE_MAX) return 0;
      stores[ns++] = i;
      continue;
    }
    if (ins->op == IR_OP_LOAD || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_INLINE_ASM || ins->op == IR_OP_MEMCPY_INLINE ||
        ins->op == IR_OP_COUNT_WORD_STARTS) {
      return 0;
    }
    if (ir_instruction_writes_symbol(ins) &&
        !ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      if (ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
          ir_symbol_live_after_loop(function, hi + 1, ins->dest.name)) {
        return 0;
      }
    }
  }
  *nstores = ns;
  return ns >= 2;
}

/* Build the IR_OP_SIMD_VLOOP_F64 fused op for one store of a multi-store loop,
 * mirroring the single-store path. Returns 1 (filling *fused and *dst_base) or 0
 * to reject the whole loop. `bound` is cloned into the fused op. */
static int ir_msf_build_store(IRFunction *function, size_t store_index,
                              const char *iv_symbol, size_t body_lo,
                              size_t body_hi, const IROperand *bound,
                              IRInstruction *fused, const char **dst_base_out) {
  const IRInstruction *store = &function->instructions[store_index];
  const char *dst_base = NULL;
  int store_bits = 0;
  VLoopDag d;
  int root, depth;

  if (!store->is_float || store->dest.kind != IR_OPERAND_TEMP ||
      !store->dest.name ||
      (store->lhs.kind != IR_OPERAND_TEMP && store->lhs.kind != IR_OPERAND_SYMBOL &&
       store->lhs.kind != IR_OPERAND_FLOAT) ||
      store->rhs.kind != IR_OPERAND_INT ||
      (store->rhs.int_value != 4 && store->rhs.int_value != 8) ||
      !ir_decode_float_indexed_address(function, store_index, store->dest.name,
                                       iv_symbol, &dst_base, &store_bits) ||
      store_bits != store->rhs.int_value * 8) {
    return 0;
  }
  memset(&d, 0, sizeof(d));
  d.width_bits = store_bits;
  d.body_lo = body_lo;
  d.body_hi = body_hi;
  if (bound->kind == IR_OPERAND_INT) {
    d.iota_bound_known = 1;
    d.iota_bound = bound->int_value;
  }
  root = vloop_build(function, store_index, &store->lhs, iv_symbol, &d);
  if (root < 0 || d.overflow) return 0;
  if (!ir_symbol_is_float_array_base(function, dst_base)) return 0;
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) return 0;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > vloop_map_budget(&d) ||
      vloop_distinct_bases(&d, dst_base) > VLOOP_MAX_ARRAYS) {
    return 0;
  }
  memset(fused, 0, sizeof(*fused));
  fused->op = IR_OP_SIMD_VLOOP_F64;
  fused->location = store->location;
  fused->is_float = 1;
  fused->float_bits = store_bits;
  fused->dest = ir_operand_symbol(dst_base);
  if (!ir_operand_clone(bound, &fused->lhs) ||
      !vloop_serialize_into(fused, &d, /*reduce_op=*/0, root, depth)) {
    ir_instruction_destroy_storage(fused);
    return 0;
  }
  *dst_base_out = dst_base;
  return 1;
}

static int ir_try_vectorize_multistore_map_at(IRFunction *function,
                                              size_t header_index,
                                              int *changed) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0, jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  size_t stores[MULTISTORE_MAX];
  int ns = 0;
  IRInstruction fused[MULTISTORE_MAX];
  const char *bases[MULTISTORE_MAX];
  int built = 0;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) return 1;
  if (!ir_msf_body_is_safe(function, branch_index + 1, jump_index, iv_symbol,
                           stores, &ns)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  /* Room to place ns fused ops in the loop's instruction range. */
  if ((size_t)ns > jump_index - header_index + 1) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (built = 0; built < ns; built++) {
    if (!ir_msf_build_store(function, stores[built], iv_symbol,
                            branch_index + 1, jump_index, &bound, &fused[built],
                            &bases[built])) {
      for (int k = 0; k < built; k++) ir_instruction_destroy_storage(&fused[k]);
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (!ir_msf_bases_disjoint(function, bases, ns)) {
    for (int k = 0; k < ns; k++) ir_instruction_destroy_storage(&fused[k]);
    ir_operand_destroy(&bound);
    return 1;
  }
  /* Commit: one full-count store-kernel per destination, then NOP the loop. */
  for (int k = 0; k < ns; k++) {
    ir_instruction_destroy_storage(&function->instructions[header_index + k]);
    function->instructions[header_index + k] = fused[k];
  }
  for (size_t i = header_index + ns; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  ir_operand_destroy(&bound);
  *changed = 1;
  return 1;
}

int ir_auto_vectorize_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    /* Multi-store map fission runs first: it claims K>=2 store loops the
     * single-store recognizer would reject, lowering each to its own kernel. */
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_multistore_map_at(function, i, changed)) {
        return 0;
      }
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_map_at(function, i, changed)) {
        return 0;
      }
    }
    /* map may have fused (and NOP'd) the loop; reduce re-checks the header and
     * no-ops if so. The two shapes are mutually exclusive (map stores, reduce
     * accumulates). */
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_reduce_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Running extrema: `if (v > best) { best = v; }` -> a min/max VLOOP reduction  */
/*                                                                             */
/* The one loop shape a writer cannot phrase without a branch. Every other      */
/* reduction is an expression (`s = s + x`), so the straight-line-body rule     */
/* costs nothing; a running max has no operator to write, so the same rule      */
/* used to send every peak-finder, extent scan and clamp bound down the scalar  */
/* path. The diamond IS the operator here, so this recognizer reads it as one   */
/* rather than asking the body to be branchless first.                          */
/*                                                                             */
/* Both accumulators of a min-and-max scan are claimed, each as its own kernel  */
/* (two passes over the array, still far ahead of one scalar pass).             */
/* -------------------------------------------------------------------------- */
#define IR_MINMAX_MAX_ACCUMULATORS 2

/* The integer DAG builder lives with the integer vectorizer below. */
static int vloop_build_int(IRFunction *function, size_t before,
                           const IROperand *op, const char *iv, VLoopDag *d);

typedef struct {
  const char *acc;    /* the accumulator symbol the diamond updates */
  int is_max;         /* 1: keeps the larger; 0: keeps the smaller */
  IROperand value;    /* the compared element, owned */
  size_t cmp_index;   /* where to build the value's DAG from */
  size_t end;         /* one past the diamond's last instruction */
} IRMinMaxDiamond;

/* Two operands naming the same value. The compare and the assignment often
 * disagree textually: `var v = a[i]; if (v < lo) { lo = v; }` compares the
 * local but assigns the load temp it was copied from, because copy propagation
 * reaches the assignment and not the compare. */
static int ir_minmax_same_operand(const IRFunction *function, size_t body_lo,
                                  size_t before, const IROperand *a,
                                  const IROperand *b) {
  if (!a || !b || !a->name || !b->name) {
    return 0;
  }
  if (a->kind == b->kind && strcmp(a->name, b->name) == 0) {
    return 1;
  }
  /* Chase a body-local copy in either direction. */
  for (size_t i = body_lo; i < before; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_ASSIGN || ins->dest.kind != IR_OPERAND_SYMBOL ||
        !ins->dest.name || !ins->lhs.name) {
      continue;
    }
    if ((ir_operand_is_symbol_named(&ins->dest, a->name) &&
         strcmp(ins->lhs.name, b->name) == 0) ||
        (ir_operand_is_symbol_named(&ins->dest, b->name) &&
         strcmp(ins->lhs.name, a->name) == 0)) {
      return 1;
    }
  }
  return 0;
}

/* Does the taken arm write back the very value the compare tested?
 *
 * Written plainly -- `if (a[i] > m) { m = a[i]; }` -- it does not look like it
 * in the IR: the arm is its own basic block, so nothing folds its `a[i]` into
 * the one the compare already loaded, and the arm re-derives the index, re-adds
 * the base and loads again. Both halves are still the same element, which is
 * what has to be proven: decode each side's address back to a base indexed by
 * the loop counter and require the pair to agree. */
static int ir_minmax_arm_restates_value(IRFunction *function, size_t body_lo,
                                        const IRInstruction *arm,
                                        size_t arm_index,
                                        const IROperand *tested,
                                        const char *iv) {
  if (arm->op == IR_OP_ASSIGN) {
    return ir_minmax_same_operand(function, body_lo, arm_index, tested,
                                  &arm->lhs);
  }
  if (arm->op == IR_OP_LOAD && arm->lhs.kind == IR_OPERAND_TEMP &&
      arm->lhs.name && tested->kind == IR_OPERAND_TEMP && tested->name) {
    const char *tested_base = NULL;
    const char *arm_base = NULL;
    int tested_bits = 0;
    int arm_bits = 0;
    return ir_decode_float_indexed_load(function, arm_index, tested->name, iv,
                                        &tested_base, &tested_bits) &&
           ir_decode_float_indexed_address(function, arm_index, arm->lhs.name,
                                           iv, &arm_base, &arm_bits) &&
           tested_bits == arm_bits && tested_base && arm_base &&
           strcmp(tested_base, arm_base) == 0;
  }
  return 0;
}

/* Match `if (v REL acc) { acc = v; }` starting at `at`, which must be the
 * compare. Fills `out` and returns 1; returns 0 if the shape is anything else.
 *
 * The lowered form is fixed: compare, branch-past, assign, jump-to-end, then
 * the two empty labels the `if` leaves behind. */
static int ir_match_minmax_diamond(IRFunction *function, size_t at,
                                   size_t body_lo, size_t body_hi,
                                   const char *iv, IRMinMaxDiamond *out) {
  const IRInstruction *cmp = &function->instructions[at];
  size_t branch = 0, assign = 0, jump = 0, next_label = 0, end_label = 0;
  const IROperand *acc_side = NULL;
  const IROperand *val_side = NULL;
  int acc_is_left = 0;

  if (cmp->op != IR_OP_BINARY || !cmp->text || cmp->dest.kind != IR_OPERAND_TEMP ||
      !cmp->dest.name ||
      (strcmp(cmp->text, "<") != 0 && strcmp(cmp->text, ">") != 0)) {
    return 0;
  }
  if (!ir_find_next_non_nop(function, at + 1, &branch) || branch >= body_hi) {
    return 0;
  }
  {
    const IRInstruction *br = &function->instructions[branch];
    if (br->op != IR_OP_BRANCH_ZERO || !br->text ||
        !ir_operand_is_temp_named(&br->lhs, cmp->dest.name)) {
      return 0;
    }
    /* The arm runs to its jump-to-end. It is a block, not one instruction: a
     * plainly written `m = a[i]` re-derives the address there. */
    for (jump = branch + 1; jump < body_hi; jump++) {
      IROpcode op = function->instructions[jump].op;
      if (op == IR_OP_JUMP) {
        break;
      }
      if (op == IR_OP_LABEL || op == IR_OP_BRANCH_ZERO ||
          op == IR_OP_BRANCH_EQ || op == IR_OP_STORE || op == IR_OP_CALL ||
          op == IR_OP_CALL_INDIRECT || op == IR_OP_RETURN) {
        return 0;
      }
    }
    if (jump >= body_hi) {
      return 0;
    }
    /* The accumulator's update is the arm's last act. */
    assign = jump;
    while (assign > branch + 1 &&
           function->instructions[assign - 1].op == IR_OP_NOP) {
      assign--;
    }
    if (assign == branch + 1) {
      return 0; /* empty arm */
    }
    assign--;
    if (!ir_find_next_non_nop(function, jump + 1, &next_label) ||
        next_label >= body_hi ||
        !ir_find_next_non_nop(function, next_label + 1, &end_label) ||
        end_label >= body_hi) {
      return 0;
    }
  }
  {
    const IRInstruction *br = &function->instructions[branch];
    const IRInstruction *as = &function->instructions[assign];
    const IRInstruction *jp = &function->instructions[jump];
    const IRInstruction *nl = &function->instructions[next_label];
    const IRInstruction *el = &function->instructions[end_label];
    if (!ir_instruction_writes_destination(as) ||
        as->dest.kind != IR_OPERAND_SYMBOL || !as->dest.name ||
        jp->op != IR_OP_JUMP || !jp->text ||
        nl->op != IR_OP_LABEL || !nl->text || strcmp(nl->text, br->text) != 0 ||
        el->op != IR_OP_LABEL || !el->text || strcmp(el->text, jp->text) != 0) {
      return 0;
    }
    /* Nothing else in the arm may write a symbol -- the kernel keeps only the
     * accumulator, so any other surviving effect would be dropped. */
    for (size_t i = branch + 1; i < jump; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (i != assign && ir_instruction_writes_destination(ins) &&
          ins->dest.kind == IR_OPERAND_SYMBOL) {
        return 0;
      }
    }
    /* The accumulator is whichever compare operand the arm writes back. */
    if (ir_operand_is_symbol_named(&cmp->lhs, as->dest.name)) {
      acc_side = &cmp->lhs;
      val_side = &cmp->rhs;
      acc_is_left = 1;
    } else if (ir_operand_is_symbol_named(&cmp->rhs, as->dest.name)) {
      acc_side = &cmp->rhs;
      val_side = &cmp->lhs;
    } else {
      return 0;
    }
    if (!ir_minmax_arm_restates_value(function, body_lo, as, assign, val_side,
                                      iv)) {
      return 0;
    }
    out->acc = acc_side->name;
    /* `acc < v` and `v > acc` both keep the larger; the operand order is what
     * flips the sense, not the operator. */
    out->is_max = acc_is_left ? (strcmp(cmp->text, "<") == 0)
                              : (strcmp(cmp->text, ">") == 0);
    out->cmp_index = at;
    out->end = end_label + 1;
    if (!ir_operand_clone(val_side, &out->value)) {
      return 0;
    }
  }
  return 1;
}

/* An accumulator whose lanes the kernel can carry: a float of the loop's width,
 * or a signed int32 (vpmaxsd/vpminsd are signed, so uint32 is refused). */
static int ir_minmax_acc_width(const IRFunction *function, const char *acc,
                               int *is_int_out) {
  const char *ty = ir_function_local_declared_type(function, acc);
  if (!ty) {
    ty = ir_function_param_declared_type(function, acc);
  }
  if (!ty) {
    return 0;
  }
  if (strcmp(ty, "float64") == 0) {
    *is_int_out = 0;
    return 64;
  }
  if (strcmp(ty, "float32") == 0) {
    *is_int_out = 0;
    return 32;
  }
  if (strcmp(ty, "int32") == 0) {
    *is_int_out = 1;
    return 32;
  }
  return 0;
}

static int ir_try_vectorize_minmax_at(IRFunction *function, size_t header_index,
                                      int *changed) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  IRMinMaxDiamond diamonds[IR_MINMAX_MAX_ACCUMULATORS];
  int n_diamonds = 0;
  IRInstruction fused[IR_MINMAX_MAX_ACCUMULATORS];
  int width_bits = 0;
  int is_int = 0;
  int ok = 1;

  memset(diamonds, 0, sizeof(diamonds));
  memset(fused, 0, sizeof(fused));

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }

  /* Walk the body once: collect the diamonds, and refuse anything else that
   * carries control flow or touches memory. Everything between them must be
   * the plain load/compute the value is built from. */
  for (size_t i = branch_index + 1; i < jump_index && ok;) {
    const IRInstruction *ins = &function->instructions[i];
    IRMinMaxDiamond d;
    memset(&d, 0, sizeof(d));
    if (ir_match_minmax_diamond(function, i, branch_index + 1, jump_index,
                                iv_symbol, &d)) {
      if (n_diamonds >= IR_MINMAX_MAX_ACCUMULATORS) {
        ir_operand_destroy(&d.value);
        ok = 0;
        break;
      }
      diamonds[n_diamonds++] = d;
      i = d.end;
      continue;
    }
    if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_LABEL ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_RETURN) {
      ok = 0;
      break;
    }
    i++;
  }
  if (!ok || n_diamonds == 0) {
    goto refuse;
  }

  /* One width for every accumulator: the kernels share the loop's element
   * layout, and a float32 extremum next to a float64 one would need two. */
  for (int k = 0; k < n_diamonds; k++) {
    int k_int = 0;
    int w = ir_minmax_acc_width(function, diamonds[k].acc, &k_int);
    if (!w || strcmp(diamonds[k].acc, iv_symbol) == 0) {
      goto refuse;
    }
    if (k == 0) {
      width_bits = w;
      is_int = k_int;
    } else if (w != width_bits || k_int != is_int) {
      goto refuse;
    }
    for (int j = 0; j < k; j++) {
      if (strcmp(diamonds[j].acc, diamonds[k].acc) == 0) {
        goto refuse; /* two diamonds racing on one accumulator */
      }
    }
  }
  /* An int32 scan tracking BOTH extrema has a dedicated kernel downstream that
   * folds them in one pass; two passes here would be the worse trade. */
  if (is_int && n_diamonds == 2) {
    goto refuse;
  }

  /* No symbol may be written in the body except the induction variable, the
   * accumulators (by their own arm), and per-iteration locals the DAG builder
   * will substitute away. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    int is_acc_write = 0;
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    for (int k = 0; k < n_diamonds; k++) {
      if (strcmp(ins->dest.name, diamonds[k].acc) == 0) {
        is_acc_write = 1;
        /* Only the diamond's own arm may write it. */
        if (i < diamonds[k].cmp_index || i >= diamonds[k].end) {
          goto refuse;
        }
      }
    }
    if (is_acc_write || strcmp(ins->dest.name, iv_symbol) == 0) {
      continue;
    }
    if (ir_symbol_live_after_loop(function, jump_index + 1, ins->dest.name)) {
      goto refuse;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    goto refuse;
  }

  for (int k = 0; k < n_diamonds; k++) {
    VLoopDag d;
    int root = -1;
    int depth = 0;
    memset(&d, 0, sizeof(d));
    d.width_bits = width_bits;
    d.is_int = is_int;
    d.elem_bits = 32; /* lane width == element width for these */
    d.body_lo = branch_index + 1;
    d.body_hi = jump_index;
    if (bound.kind == IR_OPERAND_INT) {
      d.iota_bound_known = 1;
      d.iota_bound = bound.int_value;
    }
    root = is_int ? vloop_build_int(function, diamonds[k].cmp_index,
                                    &diamonds[k].value, iv_symbol, &d)
                  : vloop_build(function, diamonds[k].cmp_index,
                                &diamonds[k].value, iv_symbol, &d);
    if (root < 0 || d.overflow) {
      goto refuse;
    }
    for (int a = 0; a < d.n_arrays; a++) {
      if (!ir_symbol_is_float_array_base(function, d.arrays[a])) {
        goto refuse;
      }
    }
    /* An accumulator reaching a DAG would make the kernels depend on each
     * other's running value, which separate passes cannot reproduce. */
    for (int s = 0; s < d.n_scalars; s++) {
      for (int j = 0; j < n_diamonds; j++) {
        if (d.scalars[s] && strcmp(d.scalars[s], diamonds[j].acc) == 0) {
          goto refuse;
        }
      }
    }
    depth = vloop_eval_depth(&d, root);
    if (depth > VLOOP_REG_BUDGET - 1 || d.n_arrays > VLOOP_MAX_ARRAYS) {
      goto refuse;
    }
    fused[k].op = is_int ? IR_OP_SIMD_VLOOP_I32 : IR_OP_SIMD_VLOOP_F64;
    fused[k].location = function->instructions[header_index].location;
    fused[k].is_float = !is_int;
    fused[k].float_bits = width_bits;
    fused[k].dest = ir_operand_symbol(diamonds[k].acc);
    if (!ir_operand_clone(&bound, &fused[k].lhs) ||
        !vloop_serialize_into(&fused[k], &d, diamonds[k].is_max ? 2 : 3, root,
                              depth)) {
      goto refuse;
    }
  }

  /* Install: one kernel per accumulator, in the slots the loop's header and
   * first body instruction occupied. */
  for (int k = 0; k < n_diamonds; k++) {
    ir_instruction_destroy_storage(&function->instructions[header_index + k]);
    function->instructions[header_index + k] = fused[k];
    memset(&fused[k], 0, sizeof(fused[k]));
  }
  for (size_t i = header_index + (size_t)n_diamonds; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  ir_operand_destroy(&bound);
  for (int k = 0; k < n_diamonds; k++) {
    ir_operand_destroy(&diamonds[k].value);
  }
  return 1;

refuse:
  ir_operand_destroy(&bound);
  for (int k = 0; k < n_diamonds; k++) {
    ir_operand_destroy(&diamonds[k].value);
  }
  for (int k = 0; k < IR_MINMAX_MAX_ACCUMULATORS; k++) {
    if (fused[k].op != IR_OP_NOP || fused[k].arguments) {
      ir_instruction_destroy_storage(&fused[k]);
      memset(&fused[k], 0, sizeof(fused[k]));
    }
  }
  return 1;
}

int ir_simd_minmax_reduce_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_minmax_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Integer twin of the general auto-vectorizer -> IR_OP_SIMD_VLOOP_I32         */
/*                                                                             */
/* int32/uint32 unit-stride maps and '+' reductions over + - * & | ^ and       */
/* <<literal DAGs. Every supported op is congruent mod 2^32, intermediates     */
/* are truncated to the 4-byte store / int32 accumulator anyway, and integer   */
/* '+' is associative -- so unlike the float form, maps AND reductions are     */
/* BIT-EXACT against the scalar loop. Division, %, and >> are never taken      */
/* (not congruent / trapping). Runs after auto_vectorize (a float32 store      */
/* shape that pass refused has float-tagged BINARYs and refuses here too).     */
/* -------------------------------------------------------------------------- */

static int vloop_int_scalar_type_ok(const char *ty) {
  /* Any plain integer type: the kernel uses only the low 32 bits and every
   * supported op is congruent mod 2^32, so width and signedness don't matter
   * (operand_load extends per the declared type; bits >= 32 are irrelevant). */
  return ty && (strcmp(ty, "int8") == 0 || strcmp(ty, "uint8") == 0 ||
                strcmp(ty, "int16") == 0 || strcmp(ty, "uint16") == 0 ||
                strcmp(ty, "int32") == 0 || strcmp(ty, "uint32") == 0 ||
                strcmp(ty, "int64") == 0 || strcmp(ty, "uint64") == 0 ||
                strcmp(ty, "int") == 0);
}

/* An int->int cast whose target keeps at least 32 bits only changes bits the
 * 32-bit lanes never see (trunc-to-32 / sign- or zero-extension), so it is
 * transparent to the DAG. Casts to int8/int16 fold sign back into the low 32
 * bits and are refused. */
static int vloop_int_cast_is_transparent(const char *ty) {
  return ty && (strcmp(ty, "int32") == 0 || strcmp(ty, "uint32") == 0 ||
                strcmp(ty, "int64") == 0 || strcmp(ty, "uint64") == 0 ||
                strcmp(ty, "int") == 0);
}

/* The value range a DAG node can take, or 0 when this cannot bound it.
 *
 * `+ - * & | ^ <<` are all congruent mod 2^32: the low 32 bits of a result
 * depend only on the low 32 bits of its inputs, so 32-bit lanes reproduce them
 * whatever width the scalar code used. `>>` is the exception -- it reads bits
 * back DOWN, so a lane that wrapped where the scalar did not shifts different
 * bits in. Bounding the shifted value inside int32 rules that out: no wrap can
 * have happened, so both sides shift the same number.
 *
 * That bound is exactly what an image kernel's weighted sum has -- byte
 * elements times constant weights, `(r*77 + g*150 + b*29) >> 8` reaching 65280
 * at the very most. A runtime weight is unbounded and refused, which is the
 * honest answer: nothing here can prove that one did not overflow. */
#define VLOOP_RANGE_LIMIT 1099511627776LL /* 2^40: past this, give up */

static int vloop_int_node_range(const VLoopDag *d, int node, long long *lo_out,
                                long long *hi_out, int depth) {
  const VLoopNode *n = NULL;
  long long alo = 0, ahi = 0, blo = 0, bhi = 0;
  int b_known = 0;

  if (node < 0 || node >= d->n_nodes || depth > VLOOP_MAX_RESOLVE_DEPTH) {
    return 0;
  }
  n = &d->nodes[node];
  switch (n->tag) {
  case VLOOP_VN_LOAD:
    if (d->elem_bits == 8) {
      *lo_out = d->elem_unsigned > 0 ? 0 : -128;
      *hi_out = d->elem_unsigned > 0 ? 255 : 127;
    } else {
      *lo_out = -2147483648LL;
      *hi_out = 2147483647LL;
    }
    return 1;
  case VLOOP_VN_CONST:
    if (n->op0 < 0 || n->op0 >= d->n_consts) {
      return 0;
    }
    *lo_out = d->iconsts[n->op0];
    *hi_out = d->iconsts[n->op0];
    return 1;
  case VLOOP_VN_IOTA:
    if (!d->iota_bound_known || d->iota_bound <= 0) {
      return 0;
    }
    *lo_out = 0;
    *hi_out = d->iota_bound - 1;
    return 1;
  case VLOOP_VN_SCALAR:
    return 0; /* a runtime invariant: nothing here bounds it */
  default:
    break;
  }
  {
    /* A mask bounds its result however unbounded the other side was, so the
     * operands' ranges are looked up but not required up front. */
    int a_known = vloop_int_node_range(d, n->op0, &alo, &ahi, depth + 1);
    b_known = vloop_int_node_range(d, n->op1, &blo, &bhi, depth + 1);
    if (n->tag == VLOOP_VN_AND) {
      if (b_known && blo == bhi && blo >= 0) {
        *lo_out = 0;
        *hi_out = blo;
      } else if (a_known && alo == ahi && alo >= 0) {
        *lo_out = 0;
        *hi_out = alo;
      } else if (a_known && b_known && alo >= 0 && blo >= 0) {
        *lo_out = 0;
        *hi_out = ahi < bhi ? ahi : bhi;
      } else {
        return 0;
      }
      return 1;
    }
    if (!a_known) {
      return 0;
    }
  }
  switch (n->tag) {
  case VLOOP_VN_SHL:
    if (n->op1 < 0 || n->op1 > 30) {
      return 0;
    }
    *lo_out = alo << n->op1;
    *hi_out = ahi << n->op1;
    break;
  case VLOOP_VN_SAR:
  case VLOOP_VN_SHR:
    if (n->op1 < 0 || n->op1 > 31) {
      return 0;
    }
    /* A shift only shrinks magnitude; the bounds hold either way round. */
    *lo_out = alo >> n->op1 < 0 ? alo : alo >> n->op1;
    *hi_out = ahi >> n->op1;
    if (*lo_out > *hi_out) {
      return 0;
    }
    break;
  case VLOOP_VN_ADD:
  case VLOOP_VN_SUB:
  case VLOOP_VN_MUL: {
    if (!b_known) {
      return 0;
    }
    if (n->tag == VLOOP_VN_ADD) {
      *lo_out = alo + blo;
      *hi_out = ahi + bhi;
    } else if (n->tag == VLOOP_VN_SUB) {
      *lo_out = alo - bhi;
      *hi_out = ahi - blo;
    } else {
      long long c[4];
      c[0] = alo * blo;
      c[1] = alo * bhi;
      c[2] = ahi * blo;
      c[3] = ahi * bhi;
      *lo_out = c[0];
      *hi_out = c[0];
      for (int k = 1; k < 4; k++) {
        if (c[k] < *lo_out) *lo_out = c[k];
        if (c[k] > *hi_out) *hi_out = c[k];
      }
    }
    break;
  }
  case VLOOP_VN_OR:
  case VLOOP_VN_XOR: {
    /* Non-negative operands keep the result below the next power of two above
     * either bound; anything with a sign bit in play is not worth modelling. */
    if (!b_known || alo < 0 || blo < 0) {
      return 0;
    }
    {
      long long m = ahi > bhi ? ahi : bhi;
      long long p = 1;
      while (p <= m && p < VLOOP_RANGE_LIMIT) {
        p <<= 1;
      }
      *lo_out = 0;
      *hi_out = p - 1;
    }
    break;
  }
  default:
    return 0;
  }
  if (*lo_out < -VLOOP_RANGE_LIMIT || *hi_out > VLOOP_RANGE_LIMIT) {
    return 0;
  }
  return 1;
}

/* Is this node's value provably inside int32, so a right shift of it reads the
 * same bits in a lane as it does in the scalar loop? */
static int vloop_int_fits_int32(const VLoopDag *d, int node) {
  long long lo = 0, hi = 0;
  if (!vloop_int_node_range(d, node, &lo, &hi, 0)) {
    return 0;
  }
  return lo >= -2147483648LL && hi <= 2147483647LL;
}

/* Which right shift a `>>` becomes: an unsigned expression shifts zeros in
 * (vpsrld), a signed one replicates the sign (vpsrad). The lowerer records that
 * on the instruction so its constant folder does not fold a shift with the
 * wrong signedness, and the same bit answers it here. */
static int vloop_int_shift_right_kind(const IRInstruction *ins) {
  return ins->is_unsigned ? VLOOP_VN_SHR : VLOOP_VN_SAR;
}

static int vloop_int_binop_tag(const char *text) {
  if (strcmp(text, "+") == 0) return VLOOP_VN_ADD;
  if (strcmp(text, "-") == 0) return VLOOP_VN_SUB;
  if (strcmp(text, "*") == 0) return VLOOP_VN_MUL;
  if (strcmp(text, "&") == 0) return VLOOP_VN_AND;
  if (strcmp(text, "|") == 0) return VLOOP_VN_OR;
  if (strcmp(text, "^") == 0) return VLOOP_VN_XOR;
  return -1;
}

static int vloop_int_is_comparison(const char *text) {
  return strcmp(text, "<") == 0 || strcmp(text, ">") == 0 ||
         strcmp(text, "<=") == 0 || strcmp(text, ">=") == 0 ||
         strcmp(text, "==") == 0 || strcmp(text, "!=") == 0;
}

/* A comparison read as a value: `c = c + (a[i] > t)` counts, and `x * (a[i] !=
 * 0)` masks. The lanes hold the compare's own answer, which is all-ones or
 * zero, so `& 1` narrows it to the 0 or 1 the source means and `^ 1` negates
 * it. Both extra nodes take a broadcast constant, no register. */
static int vloop_compare_value_node(IRFunction *function, size_t before,
                                    const IRInstruction *cmp, const char *iv,
                                    VLoopDag *d) {
  int equality = strcmp(cmp->text, "==") == 0 || strcmp(cmp->text, "!=") == 0;
  int negate = strcmp(cmp->text, "!=") == 0 || strcmp(cmp->text, "<=") == 0 ||
               strcmp(cmp->text, ">=") == 0;
  /* Only `>` and `==` exist in the lanes. `<` is `>` with the operands the
   * other way round; `<=` and `>=` are the strict compare negated. */
  int gt_left = strcmp(cmp->text, ">") == 0 || strcmp(cmp->text, "<=") == 0;
  const IROperand *first = (equality || gt_left) ? &cmp->lhs : &cmp->rhs;
  const IROperand *second = (equality || gt_left) ? &cmp->rhs : &cmp->lhs;
  int a, b, mask, one, value;

  if (cmp->is_unsigned) {
    return -1; /* the lane compares are signed */
  }
  a = vloop_build_int(function, before, first, iv, d);
  if (a < 0) {
    return -1;
  }
  b = vloop_build_int(function, before, second, iv, d);
  if (b < 0) {
    return -1;
  }
  mask = vloop_add_node(d, equality ? VLOOP_VN_CMPEQ : VLOOP_VN_CMPGT, a, b);
  if (mask < 0) {
    return -1;
  }
  one = vloop_intern_iconst(d, 1);
  if (one < 0) {
    return -1;
  }
  one = vloop_add_node(d, VLOOP_VN_CONST, one, 0);
  if (one < 0) {
    return -1;
  }
  value = vloop_add_node(d, VLOOP_VN_AND, mask, one);
  if (value < 0 || !negate) {
    return value;
  }
  one = vloop_intern_iconst(d, 1);
  if (one < 0) {
    return -1;
  }
  one = vloop_add_node(d, VLOOP_VN_CONST, one, 0);
  return one < 0 ? -1 : vloop_add_node(d, VLOOP_VN_XOR, value, one);
}

/* ---- if-conversion: a conditional assignment becomes a lane select --------
 *
 * `if (v < lo) { v = lo; }` is a value, not control flow: every lane computes
 * both sides and keeps one. Deciding that here rather than in a per-kernel
 * recognizer is what lets a clamp, a saturation, a ReLU and a running extremum
 * share one path, whatever order the source writes them in.
 *
 * The lowered form of an `if` is fixed:
 *
 *     %t = <condition>
 *     branch_zero %t -> Lelse
 *     <then arm>
 *     jump Lend
 *   Lelse:
 *     <else arm, possibly empty>
 *   Lend:
 *
 * An arm may hold another diamond. `if (x < lo) return lo; if (x > hi) return
 * hi; return x;` is what a clamp helper looks like once it is inlined, and its
 * second test sits inside the first one's else arm, so arms resolve
 * recursively. What refuses is an effect a masked lane must not have: a store,
 * a call, or anything the kernel cannot replay for every lane. */

static int vloop_region_is_pure(const IRFunction *function, size_t lo, size_t hi,
                                int depth);

/* Match a value diamond whose branch sits at `at`. Structure only: which name
 * the arms choose between is the resolver's question, not this one's. */
static int vloop_match_diamond(const IRFunction *function, size_t at,
                               size_t body_hi, VLoopDiamond *out) {
  const IRInstruction *br = &function->instructions[at];
  size_t jump = 0, else_label = 0, end_label = 0, probe = 0;
  int nesting = 0;

  if (br->op != IR_OP_BRANCH_ZERO || !br->text ||
      br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name) {
    return 0;
  }
  /* The then arm ends at its jump to the join. A nested diamond has a jump of
   * its own, so count the branches it opens and skip their jumps. */
  for (jump = at + 1; jump < body_hi; jump++) {
    IROpcode op = function->instructions[jump].op;
    if (op == IR_OP_BRANCH_ZERO) {
      nesting++;
      continue;
    }
    if (op == IR_OP_JUMP) {
      if (nesting == 0) {
        break;
      }
      nesting--;
    }
  }
  if (jump >= body_hi || !function->instructions[jump].text) {
    return 0;
  }
  if (!ir_find_next_non_nop(function, jump + 1, &else_label) ||
      else_label >= body_hi) {
    return 0;
  }
  {
    const IRInstruction *el = &function->instructions[else_label];
    if (el->op != IR_OP_LABEL || !el->text || strcmp(el->text, br->text) != 0) {
      return 0;
    }
  }
  /* Everything from the else label to the label the then arm jumps to is the
   * else arm. With no else, the two labels are adjacent. */
  end_label = else_label;
  for (probe = else_label + 1; probe < body_hi; probe++) {
    const IRInstruction *ins = &function->instructions[probe];
    if (ins->op == IR_OP_LABEL && ins->text &&
        strcmp(ins->text, function->instructions[jump].text) == 0) {
      end_label = probe;
      break;
    }
  }
  if (end_label == else_label) {
    /* An else-if chain has no join of its own: each arm jumps to the one join
     * the whole chain shares, which sits just past this region. Accept that
     * label as the closing one so a nested test resolves like a lone `if`. */
    const IRInstruction *outer = body_hi < function->instruction_count
                                     ? &function->instructions[body_hi]
                                     : NULL;
    if (!outer || outer->op != IR_OP_LABEL || !outer->text ||
        strcmp(outer->text, function->instructions[jump].text) != 0) {
      return 0;
    }
    end_label = body_hi;
  }
  memset(out, 0, sizeof(*out));
  out->branch_index = at;
  out->then_lo = at + 1;
  out->then_hi = jump;
  out->else_lo = else_label + 1;
  out->else_hi = end_label;
  out->end = end_label + 1;
  for (probe = out->else_lo; probe < out->else_hi; probe++) {
    if (function->instructions[probe].op != IR_OP_NOP) {
      out->has_else = 1;
      break;
    }
  }
  return 1;
}

/* True if [lo,hi) is a well-formed nest of value diamonds over straight-line
 * arithmetic: nothing in it escapes a lane. */
static int vloop_region_is_pure(const IRFunction *function, size_t lo, size_t hi,
                                int depth) {
  if (depth > VLOOP_MAX_DIAMOND_DEPTH) {
    return 0;
  }
  for (size_t i = lo; i < hi;) {
    const IRInstruction *ins = &function->instructions[i];
    VLoopDiamond dm;
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      i++;
      continue;
    }
    if (ins->op == IR_OP_BRANCH_ZERO) {
      if (!vloop_match_diamond(function, i, hi, &dm) ||
          !vloop_region_is_pure(function, dm.then_lo, dm.then_hi, depth + 1) ||
          !vloop_region_is_pure(function, dm.else_lo, dm.else_hi, depth + 1)) {
        return 0;
      }
      i = dm.end;
      continue;
    }
    if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_LABEL ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_EQ ||
        ins->op == IR_OP_RETURN || ins->op == IR_OP_INLINE_ASM ||
        ins->op == IR_OP_ADDRESS_OF || ins->op == IR_OP_NEW) {
      return 0;
    }
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind != IR_OPERAND_SYMBOL &&
        ins->dest.kind != IR_OPERAND_TEMP) {
      return 0;
    }
    i++;
  }
  return 1;
}

/* Every symbol a region writes, checked dead after the loop: the kernel deletes
 * the body, so a value chosen inside it and read later would vanish with it. */
static int vloop_region_writes_escape(IRFunction *function, size_t lo, size_t hi,
                                      size_t after) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    if (ir_symbol_live_after_loop(function, after, ins->dest.name)) {
      return 1;
    }
  }
  return 0;
}

static int vloop_build_int(IRFunction *function, size_t before,
                           const IROperand *op, const char *iv, VLoopDag *d);
static int vloop_resolve_def_int(IRFunction *function, const IRInstruction *def,
                                 size_t def_idx, const char *iv, VLoopDag *d);
static int vloop_resolve_region_int(IRFunction *function, const char *name,
                                    size_t lo, size_t hi, const char *iv,
                                    VLoopDag *d);

static int vloop_instruction_writes_name(const IRInstruction *ins,
                                         const char *name) {
  return ir_instruction_writes_destination(ins) &&
         (ins->dest.kind == IR_OPERAND_SYMBOL ||
          ins->dest.kind == IR_OPERAND_TEMP) &&
         ins->dest.name && strcmp(ins->dest.name, name) == 0;
}

/* Does [lo,hi) assign `name` on some path, nested diamonds included? */
static int vloop_region_assigns(const IRFunction *function, size_t lo, size_t hi,
                                const char *name) {
  for (size_t i = lo; i < hi; i++) {
    if (vloop_instruction_writes_name(&function->instructions[i], name)) {
      return 1;
    }
  }
  return 0;
}

/* Two operands naming the same value. Enough to tell `v = lo` from `v = hi`
 * when both sit beside the compare that chose between them. */
static int vloop_operand_same(const IROperand *a, const IROperand *b) {
  if (!a || !b || a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case IR_OPERAND_INT:
    return a->int_value == b->int_value;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return a->name && b->name && strcmp(a->name, b->name) == 0;
  default:
    return 0;
  }
}

/* The single instruction an arm uses to hand `name` back, when the arm is just
 * that: `v = lo`. NULL when the arm computes something less direct, or nothing
 * at all. */
static const IRInstruction *vloop_arm_simple_assign(const IRFunction *function,
                                                    size_t lo, size_t hi,
                                                    const char *name) {
  const IRInstruction *found = NULL;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (!vloop_instruction_writes_name(ins, name)) {
      if (ir_instruction_writes_destination(ins)) {
        return NULL; /* the arm computes; leave it to the region resolver */
      }
      continue;
    }
    if (found || (ins->op != IR_OP_ASSIGN && ins->op != IR_OP_CAST)) {
      return NULL;
    }
    found = ins;
  }
  return found;
}

/* Which compare operand an arm hands back: 0 = the compare's left, 1 = its
 * right, -1 = neither. An arm that assigns nothing hands back the value that
 * reached the diamond, which is what the compare's own read of `name` is. */
static int vloop_arm_side(const IRFunction *function, const IRInstruction *cmp,
                          const VLoopDiamond *dm, int is_then,
                          const char *name) {
  size_t lo = is_then ? dm->then_lo : dm->else_lo;
  size_t hi = is_then ? dm->then_hi : dm->else_hi;
  const IRInstruction *as = NULL;
  if (!vloop_region_assigns(function, lo, hi, name)) {
    if (ir_operand_is_symbol_named(&cmp->lhs, name) ||
        ir_operand_is_temp_named(&cmp->lhs, name)) {
      return 0;
    }
    if (ir_operand_is_symbol_named(&cmp->rhs, name) ||
        ir_operand_is_temp_named(&cmp->rhs, name)) {
      return 1;
    }
    return -1;
  }
  as = vloop_arm_simple_assign(function, lo, hi, name);
  if (!as) {
    return -1;
  }
  if (vloop_operand_same(&as->lhs, &cmp->lhs)) {
    return 0;
  }
  if (vloop_operand_same(&as->lhs, &cmp->rhs)) {
    return 1;
  }
  return -1;
}

/* The value of `name` where the diamond is entered. */
static int vloop_incoming_node(IRFunction *function, const VLoopDiamond *dm,
                               const char *name, const char *iv, VLoopDag *d) {
  return vloop_resolve_region_int(function, name, d->body_lo, dm->branch_index,
                                  iv, d);
}

/* The value one arm hands back: what it assigns, or, when it assigns nothing,
 * the value that reached the diamond. */
static int vloop_arm_value(IRFunction *function, const VLoopDiamond *dm,
                           int is_then, const char *name, const char *iv,
                           VLoopDag *d) {
  size_t lo = is_then ? dm->then_lo : dm->else_lo;
  size_t hi = is_then ? dm->then_hi : dm->else_hi;
  const IRInstruction *as = NULL;
  int result;
  if (!vloop_region_assigns(function, lo, hi, name)) {
    return vloop_incoming_node(function, dm, name, iv, d);
  }
  as = vloop_arm_simple_assign(function, lo, hi, name);
  if (as) {
    return vloop_build_int(function, dm->branch_index, &as->lhs, iv, d);
  }
  /* The arm computes. Anything it reads resolves inside the arm, or from
   * before the diamond; never from the other arm. */
  if (d->n_regions >= VLOOP_MAX_DIAMOND_DEPTH) {
    return -1;
  }
  d->regions[d->n_regions].lo = lo;
  d->regions[d->n_regions].hi = hi;
  d->regions[d->n_regions].incoming_hi = dm->branch_index;
  d->n_regions++;
  result = vloop_resolve_region_int(function, name, lo, hi, iv, d);
  d->n_regions--;
  return result;
}

/* Fold one value diamond into the DAG.
 *
 * A select whose arms are the two compared values IS a minimum or a maximum,
 * which is one instruction instead of a compare and a blend, and two ymm
 * registers shallower. Take that shape first, decided off the compare's
 * operands rather than off the spelling of the source, so a clamp, a floor and
 * a running extremum in either operand order all reach the same vpmaxsd.
 * Anything else becomes a general lane select. */
static int vloop_diamond_node(IRFunction *function, const VLoopDiamond *dm,
                              const char *name, const char *iv, VLoopDag *d) {
  const IRInstruction *br = &function->instructions[dm->branch_index];
  const IRInstruction *cmp = NULL;
  int then_side, else_side, lt, gt, mask, then_node, else_node, pair;

  if (br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name) {
    return -1;
  }
  cmp = ir_find_temp_producer_before(function, dm->branch_index, br->lhs.name);
  if (!cmp || cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
      cmp->is_unsigned) {
    return -1; /* a signed int32 compare is what the lanes carry */
  }
  lt = strcmp(cmp->text, "<") == 0 || strcmp(cmp->text, "<=") == 0;
  gt = strcmp(cmp->text, ">") == 0 || strcmp(cmp->text, ">=") == 0;
  if (!lt && !gt) {
    return -1;
  }

  then_side = vloop_arm_side(function, cmp, dm, 1, name);
  else_side = vloop_arm_side(function, cmp, dm, 0, name);
  if (then_side >= 0 && else_side >= 0 && then_side != else_side) {
    /* `a < b ? a : b` keeps the smaller; flipping the operator or the arm
     * order flips which side survives. */
    int keeps_min = (then_side == 0) ? lt : gt;
    int a = vloop_build_int(function, dm->branch_index, &cmp->lhs, iv, d);
    int b = (a < 0) ? -1
                    : vloop_build_int(function, dm->branch_index, &cmp->rhs, iv,
                                      d);
    if (a < 0 || b < 0) {
      return -1;
    }
    return vloop_add_node(d, keeps_min ? VLOOP_VN_MIN : VLOOP_VN_MAX, a, b);
  }

  /* General select. The mask is `left > right`, so `<` swaps the operands and
   * `<=` / `>=` are the negation of the strict compare the other way round,
   * which is the same select with its arms exchanged. */
  {
    int strict = strcmp(cmp->text, "<") == 0 || strcmp(cmp->text, ">") == 0;
    int gt_left = gt; /* mask operand order: 1 = lhs > rhs */
    int ma, mb;
    if (!strict) {
      gt_left = !gt_left; /* `a <= b` is `!(a > b)`; `a >= b` is `!(b > a)` */
    }
    ma = vloop_build_int(function, dm->branch_index,
                         gt_left ? &cmp->lhs : &cmp->rhs, iv, d);
    if (ma < 0) {
      return -1;
    }
    mb = vloop_build_int(function, dm->branch_index,
                         gt_left ? &cmp->rhs : &cmp->lhs, iv, d);
    if (mb < 0) {
      return -1;
    }
    mask = vloop_add_node(d, VLOOP_VN_CMPGT, ma, mb);
    if (mask < 0) {
      return -1;
    }
  }
  {
    /* The kernel reads its three operands off the stack in the order they were
     * built, so a negated mask has to swap which ARM is built first, not just
     * which field of the pair it lands in. */
    int swap = strcmp(cmp->text, "<=") == 0 || strcmp(cmp->text, ">=") == 0;
    then_node = vloop_arm_value(function, dm, swap ? 0 : 1, name, iv, d);
    if (then_node < 0) {
      return -1;
    }
    else_node = vloop_arm_value(function, dm, swap ? 1 : 0, name, iv, d);
    if (else_node < 0) {
      return -1;
    }
    pair = vloop_add_node(d, VLOOP_VN_PAIR, then_node, else_node);
    if (pair < 0) {
      return -1;
    }
    return vloop_add_node(d, VLOOP_VN_SELECT, mask, pair);
  }
}

/* Resolve `name` over [lo,hi): find the last construct that assigns it and
 * build from there. The kernel replays the node list as a tree, so this builds
 * only the nodes the value actually needs. */
static int vloop_resolve_region_int(IRFunction *function, const char *name,
                                    size_t lo, size_t hi, const char *iv,
                                    VLoopDag *d) {
  VLoopDiamond last_dm;
  const IRInstruction *last_def = NULL;
  size_t last_def_idx = 0;
  int have_dm = 0;
  int result;

  memset(&last_dm, 0, sizeof(last_dm));
  if (!name || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  for (size_t i = lo; i < hi;) {
    const IRInstruction *ins = &function->instructions[i];
    VLoopDiamond dm;
    if (ins->op == IR_OP_BRANCH_ZERO) {
      if (!vloop_match_diamond(function, i, hi, &dm)) {
        /* A diamond that does not close inside this region means the region
         * ends mid-arm. Walking on would read the other arm's writes as if
         * they had happened. */
        return -1;
      }
      if (vloop_region_assigns(function, dm.then_lo, dm.else_hi, name)) {
        last_dm = dm;
        have_dm = 1;
        last_def = NULL;
      }
      i = dm.end;
      continue;
    }
    if (vloop_instruction_writes_name(ins, name)) {
      last_def = ins;
      last_def_idx = i;
      have_dm = 0;
    }
    i++;
  }
  d->resolve_depth++;
  if (have_dm) {
    result = vloop_diamond_node(function, &last_dm, name, iv, d);
  } else if (last_def) {
    result = vloop_resolve_def_int(function, last_def, last_def_idx, iv, d);
  } else {
    result = -1;
  }
  d->resolve_depth--;
  return result;
}

/* Fold one definition of a per-iteration local into the DAG. */
static int vloop_resolve_def_int(IRFunction *function, const IRInstruction *def,
                                 size_t def_idx, const char *iv, VLoopDag *d) {
  int result = -1;

  if (!def || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  d->resolve_depth++;
  if (def->op == IR_OP_BINARY && !def->is_float && def->text) {
    int tag = vloop_int_binop_tag(def->text);
    if (vloop_int_is_comparison(def->text)) {
      result = vloop_compare_value_node(function, def_idx, def, iv, d);
    } else if (tag >= 0) {
      int a = vloop_build_int(function, def_idx, &def->lhs, iv, d);
      int b = (a < 0) ? -1 : vloop_build_int(function, def_idx, &def->rhs, iv, d);
      if (a >= 0 && b >= 0) {
        result = vloop_add_node(d, tag, a, b);
      }
    } else if (strcmp(def->text, "<<") == 0 &&
               def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value >= 0 &&
               def->rhs.int_value < 32) {
      int a = vloop_build_int(function, def_idx, &def->lhs, iv, d);
      result = (a < 0) ? -1
                       : vloop_add_node(d, VLOOP_VN_SHL, a,
                                        (int)def->rhs.int_value);
    } else if (strcmp(def->text, ">>") == 0 &&
               def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value >= 0 &&
               def->rhs.int_value < 32) {
      int a = vloop_build_int(function, def_idx, &def->lhs, iv, d);
      result = (a < 0 || !vloop_int_fits_int32(d, a))
                   ? -1
                   : vloop_add_node(d, vloop_int_shift_right_kind(def), a,
                                    (int)def->rhs.int_value);
    }
  } else if (def->op == IR_OP_ASSIGN || def->op == IR_OP_CAST) {
    /* A cast into the local is transparent when it keeps at least the 32 bits
     * the lanes carry; narrower ones fold sign back in and are refused. */
    if (def->op == IR_OP_ASSIGN ||
        (def->text && vloop_int_cast_is_transparent(def->text))) {
      result = vloop_build_int(function, def_idx, &def->lhs, iv, d);
    }
  } else if (def->op == IR_OP_LOAD && def->lhs.kind == IR_OPERAND_TEMP &&
             def->lhs.name && def->rhs.kind == IR_OPERAND_INT) {
    /* `var x = a[i]` lowers to a LOAD straight into the symbol; rebuild it as
     * an indexed array load by decoding the address. */
    const char *base = NULL;
    int bits = 0;
    int ai = -1;
    if (d->elem_bits == 8 && def->rhs.int_value == 1 &&
        ir_decode_byte_indexed_address(function, def_idx, def->lhs.name, iv,
                                       &base)) {
      /* Byte lanes are zero-extended regardless of declared signedness (the
       * movzx convention; see ir_decode_byte_indexed_load). */
      int is_unsigned = 1;
      if (d->elem_unsigned < 0) {
        d->elem_unsigned = is_unsigned;
      }
      if (is_unsigned == d->elem_unsigned) {
        ai = vloop_intern_array(d, base);
      }
    } else if (d->elem_bits != 8 && def->rhs.int_value == 4 &&
               ir_decode_float_indexed_address(function, def_idx, def->lhs.name,
                                               iv, &base, &bits) &&
               bits == 32) {
      ai = vloop_intern_array(d, base);
    }
    if (ai >= 0) {
      result = vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
  }
  d->resolve_depth--;
  return result;
}

/* Integer twin of vloop_resolve_body_local: fold a per-iteration local's
 * defining expression into the DAG in place of the symbol. The local must be
 * dead after the loop, since the fused kernel deletes the body. The region
 * resolver takes the last write to reach the point of the read, which is what
 * lets several guarded writes to one local compose. */
static int vloop_resolve_body_local_int(IRFunction *function, const char *sym,
                                        const char *iv, size_t read_at,
                                        VLoopDag *d) {
  if (!sym || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  if (ir_symbol_live_after_loop(function, d->body_hi + 1, sym)) {
    return -1;
  }
  if (read_at > d->body_hi) {
    read_at = d->body_hi;
  }
  /* Innermost arm outwards: what this arm assigned, else what reached its
   * diamond, and so on out to the loop body. */
  for (int k = d->n_regions - 1; k >= 0; k--) {
    size_t stop = read_at < d->regions[k].hi ? read_at : d->regions[k].hi;
    if (stop > d->regions[k].lo) {
      int found = vloop_resolve_region_int(function, sym, d->regions[k].lo, stop,
                                           iv, d);
      if (found >= 0) {
        return found;
      }
    }
    read_at = d->regions[k].incoming_hi;
  }
  return vloop_resolve_region_int(function, sym, d->body_lo, read_at, iv, d);
}

static int vloop_build_int(IRFunction *function, size_t before,
                           const IROperand *op, const char *iv, VLoopDag *d) {
  if (!op || d->overflow) {
    return -1;
  }
  if (op->kind == IR_OPERAND_INT) {
    int ci = vloop_intern_iconst(d, op->int_value);
    return ci < 0 ? -1 : vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return -1;
  }
  if (op->kind == IR_OPERAND_SYMBOL) {
    /* the counter used directly as a value: out[i] = a[i] + i */
    if (strcmp(op->name, iv) == 0) {
      d->has_iota = 1;
      return vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
    }
    if (vloop_symbol_written_in_body(function, d, op->name)) {
      /* Same substitution the float builder does: a per-iteration local
       * (`var v: int32 = (int32)src[i];`) is not a broadcast value, but its
       * defining expression can be folded into the DAG. Hoisting the
       * declaration out of the body -- which the compiler now does -- leaves
       * the WRITE inside it, so without this the named-intermediate form of
       * every int map stays scalar. */
      return vloop_resolve_body_local_int(function, op->name, iv, before, d);
    }
    {
      const char *ty = ir_function_local_declared_type(function, op->name);
      if (!ty) {
        ty = ir_function_param_declared_type(function, op->name);
      }
      if (vloop_int_scalar_type_ok(ty) &&
          !ir_symbol_address_taken(function, op->name)) {
        int si = vloop_intern_scalar(d, op->name);
        return si < 0 ? -1 : vloop_add_node(d, VLOOP_VN_SCALAR, si, 0);
      }
    }
  }
  /* array load a[iv]. Whether that is a 4-byte element read straight into the
   * lane or a byte widened into it is fixed for the whole DAG by the store the
   * matcher found: one loop cannot mix the two, because the kernel walks every
   * base by the same element stride. */
  if (op->kind == IR_OPERAND_TEMP && d->elem_bits == 8) {
    const char *base = NULL;
    int is_unsigned = 0;
    if (ir_decode_byte_indexed_load(function, before, op->name, iv, &base,
                                    &is_unsigned)) {
      /* One widening rule per kernel: a uint8 array beside an int8 one would
       * need two, and the lanes carry only the one. */
      if (d->elem_unsigned < 0) {
        d->elem_unsigned = is_unsigned;
      } else if (is_unsigned != d->elem_unsigned) {
        return -1;
      }
      {
        int ai = vloop_intern_array(d, base);
        return ai < 0 ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
      }
    }
    {
      /* Any OTHER load in a byte kernel is an element the lanes cannot walk
       * alongside the rest. Arithmetic temps fall through to the DAG below. */
      const IRInstruction *from =
          ir_find_temp_producer_before(function, before, op->name);
      if (from && from->op == IR_OP_LOAD) {
        return -1;
      }
    }
  }
  if (op->kind == IR_OPERAND_TEMP) {
    const char *base = NULL;
    int bits = 0;
    if (ir_decode_float_indexed_load(function, before, op->name, iv, &base,
                                     &bits) &&
        bits == 32) {
      int ai = vloop_intern_array(d, base);
      return ai < 0 ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
    /* A temp written on more than one path is an inlined callee's return
     * value: `return lo` and `return x` land in the same temp from different
     * arms. Resolving it by its nearest producer would take one arm's value
     * for all lanes, so hand it to the region resolver instead. */
    {
      int defs = 0;
      for (size_t k = d->body_lo; k < d->body_hi && defs < 2; k++) {
        if (vloop_instruction_writes_name(&function->instructions[k],
                                          op->name)) {
          defs++;
        }
      }
      if (defs > 1) {
        size_t stop = before > d->body_hi ? d->body_hi : before;
        return vloop_resolve_region_int(function, op->name, d->body_lo, stop, iv,
                                        d);
      }
    }
  }
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return -1;
  }
  size_t pidx = (size_t)(p - function->instructions);
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      vloop_int_cast_is_transparent(p->text)) {
    return vloop_build_int(function, pidx, &p->lhs, iv, d);
  }
  if (p->op == IR_OP_BINARY && !p->is_float && p->text) {
    if (strcmp(p->text, "<<") == 0 && p->rhs.kind == IR_OPERAND_INT &&
        p->rhs.int_value >= 0 && p->rhs.int_value <= 31) {
      int a = vloop_build_int(function, pidx, &p->lhs, iv, d);
      return a < 0 ? -1
                   : vloop_add_node(d, VLOOP_VN_SHL, a, (int)p->rhs.int_value);
    }
    if (strcmp(p->text, ">>") == 0 && p->rhs.kind == IR_OPERAND_INT &&
        p->rhs.int_value >= 0 && p->rhs.int_value <= 31) {
      int a = vloop_build_int(function, pidx, &p->lhs, iv, d);
      if (a < 0 || !vloop_int_fits_int32(d, a)) {
        return -1;
      }
      return vloop_add_node(d, vloop_int_shift_right_kind(p), a,
                            (int)p->rhs.int_value);
    }
    if (vloop_int_is_comparison(p->text)) {
      return vloop_compare_value_node(function, pidx, p, iv, d);
    }
    int tag = vloop_int_binop_tag(p->text);
    if (tag < 0) {
      return -1;
    }
    int a = vloop_build_int(function, pidx, &p->lhs, iv, d);
    if (a < 0) {
      return -1;
    }
    int b = vloop_build_int(function, pidx, &p->rhs, iv, d);
    if (b < 0) {
      return -1;
    }
    return vloop_add_node(d, tag, a, b);
  }
  return -1;
}

/* Shared matcher for the int32 map shape. Fills the DAG and the loop facts;
 * *claim_out = 1 when every gate passes (the loop WOULD be fused). The bound
 * operand is cloned into *bound on a successful frame match regardless and
 * must be destroyed by the caller unless ownership is taken. Returns 0 only
 * on allocation failure. */
static int ir_match_int_map_at(IRFunction *function, size_t header_index,
                               VLoopDag *d, IROperand *bound,
                               size_t *jump_index_out, const char **dst_base_out,
                               int *root_out, int *depth_out, int *claim_out) {
  const char *iv_symbol = NULL;
  const char *dst_base = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t store_index = 0;
  int matched = 0;
  int store_bits = 0;
  int root = -1;
  int depth = 0;
  const IRInstruction *store = NULL;

  *claim_out = 0;
  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_map_body_is_safe_ex(function, branch_index + 1, jump_index,
                                    iv_symbol, &store_index, 1)) {
    return 1;
  }

  store = &function->instructions[store_index];
  if (store->dest.kind != IR_OPERAND_TEMP || !store->dest.name ||
      (store->lhs.kind != IR_OPERAND_TEMP &&
       store->lhs.kind != IR_OPERAND_SYMBOL &&
       store->lhs.kind != IR_OPERAND_INT) ||
      store->rhs.kind != IR_OPERAND_INT) {
    return 1;
  }

  memset(d, 0, sizeof(*d));
  d->width_bits = 32;
  d->is_int = 1;
  d->elem_bits = 32;
  d->body_lo = branch_index + 1;
  d->body_hi = jump_index;

  if (store->rhs.int_value == 1) {
    /* A byte map: the lanes are still int32 (every op congruent mod 2^32, so
     * the arithmetic is the scalar loop's exactly), and only the traffic at
     * either end narrows -- widen on load, truncate on store. That truncation
     * is what the store does anyway, so a `(uint8)` cast in front of it is an
     * identity and must not be read as a narrowing the lanes have to model. */
    const IROperand *value = &store->lhs;
    d->elem_bits = 8;
    d->elem_unsigned = -1; /* the first byte load fixes it for the kernel */
    if (!ir_decode_byte_indexed_address(function, store_index,
                                        store->dest.name, iv_symbol,
                                        &dst_base)) {
      return 1;
    }
    if (value->kind == IR_OPERAND_TEMP && value->name) {
      const IRInstruction *cast =
          ir_find_temp_producer_before(function, store_index, value->name);
      if (cast && cast->op == IR_OP_CAST && cast->text &&
          (strcmp(cast->text, "uint8") == 0 ||
           strcmp(cast->text, "int8") == 0)) {
        value = &cast->lhs;
      }
    }
    root = vloop_build_int(function, store_index, value, iv_symbol, d);
    if (d->elem_unsigned < 0) {
      return 1; /* nothing widened: a fill, which has its own kernel */
    }
  } else if (store->rhs.int_value == 4 &&
             ir_decode_float_indexed_address(function, store_index,
                                             store->dest.name, iv_symbol,
                                             &dst_base, &store_bits) &&
             store_bits == 32) {
    root = vloop_build_int(function, store_index, &store->lhs, iv_symbol, d);
  } else {
    return 1;
  }
  if (root < 0 || d->overflow) {
    return 1;
  }
  /* Trivial bodies (a plain copy or a constant splat) belong to the tuned
   * memory-map/fill kernels; claiming them here would only swap one kernel
   * for a slower one. */
  if (d->n_nodes < 2) {
    return 1;
  }

  if (!ir_symbol_is_float_array_base(function, dst_base)) {
    return 1;
  }
  for (int i = 0; i < d->n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d->arrays[i])) {
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    return 1;
  }
  depth = vloop_eval_depth(d, root);
  if (depth > vloop_map_budget(d) ||
      vloop_distinct_bases(d, dst_base) > VLOOP_MAX_ARRAYS) {
    return 1;
  }

  *jump_index_out = jump_index;
  *dst_base_out = dst_base;
  *root_out = root;
  *depth_out = depth;
  *claim_out = 1;
  return 1;
}

/* Read-only probe for other passes (pointer-induction must not convert a loop
 * this vectorizer would claim -- the kernel needs the indexed form). */
int ir_auto_vectorize_int_claimable(IRFunction *function, size_t header_index) {
  VLoopDag d;
  IROperand bound = {0};
  size_t jump_index = 0;
  const char *dst_base = NULL;
  int root = -1;
  int depth = 0;
  int claim = 0;
  if (!ir_match_int_map_at(function, header_index, &d, &bound, &jump_index,
                           &dst_base, &root, &depth, &claim)) {
    return 0;
  }
  ir_operand_destroy(&bound);
  return claim;
}

static int ir_try_vectorize_int_map_at(IRFunction *function,
                                       size_t header_index, int *changed) {
  VLoopDag d;
  IROperand bound = {0};
  size_t jump_index = 0;
  const char *dst_base = NULL;
  int root = -1;
  int depth = 0;
  int claim = 0;
  IRInstruction fused = {0};

  if (!ir_match_int_map_at(function, header_index, &d, &bound, &jump_index,
                           &dst_base, &root, &depth, &claim)) {
    return 0;
  }
  if (!claim) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_I32;
  fused.location = function->instructions[header_index].location;
  /* float_bits carries the ELEMENT width for this opcode: 32 for int32
   * elements, 8 for a byte map (int32 lanes either way). is_unsigned then says
   * how the byte widens into the lane. */
  fused.float_bits = d.elem_bits;
  fused.is_unsigned = (d.elem_bits == 8) ? (d.elem_unsigned != 0) : 0;
  fused.dest = ir_operand_symbol(dst_base);
  fused.lhs = bound; /* take ownership of the cloned bound operand */
  if (!vloop_serialize_into(&fused, &d, /*reduce_op=*/0, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

static int ir_try_vectorize_int_reduce_at(IRFunction *function,
                                          size_t header_index, int *changed) {
  const char *iv_symbol = NULL;
  const char *acc_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t reduce_index = 0;
  IROperand bound = {0};
  int matched = 0;
  int found = 0;
  const IROperand *addend = NULL;
  VLoopDag d;
  int root = -1;
  int depth = 0;
  IRInstruction fused = {0};

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && strcmp(ins->dest.name, iv_symbol) != 0 &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        (ins->rhs.kind == IR_OPERAND_TEMP ||
         ins->rhs.kind == IR_OPERAND_SYMBOL)) {
      acc_symbol = ins->dest.name;
      addend = &ins->rhs;
      reduce_index = i;
      found++;
    }
  }
  if (found != 1 || !acc_symbol) {
    ir_operand_destroy(&bound);
    return 1;
  }
  {
    /* The kernel accumulates in 32-bit lanes and stores 32 bits back, which
     * is only congruent when the accumulator itself is 32-bit. A widening
     * int64 += int32 sum keeps high bits the lanes never compute -- refuse
     * (the dedicated sum_i32 kernel handles the bare-load form of that). */
    const char *acc_type = ir_function_local_declared_type(function, acc_symbol);
    if (!acc_type) {
      acc_type = ir_function_param_declared_type(function, acc_symbol);
    }
    if (!acc_type || (strcmp(acc_type, "int32") != 0 &&
                      strcmp(acc_type, "uint32") != 0)) {
      ir_operand_destroy(&bound);
      return 1;
    }
    /* The kernel re-extends the folded 32-bit sum into the accumulator's
     * 8-byte stack home, and the extension must match the declared
     * signedness (homes hold canonically-extended values). */
    fused.is_unsigned = (strcmp(acc_type, "uint32") == 0);
  }
  /* acc written only by the reduction; no other symbol writes besides iv. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i != reduce_index && ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        (strcmp(ins->dest.name, acc_symbol) == 0 ||
         strcmp(ins->dest.name, iv_symbol) != 0)) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = 32;
  d.is_int = 1;
  d.elem_bits = 32; /* byte reductions belong to the vpsadbw sum kernel */
  d.body_lo = branch_index + 1;
  d.body_hi = jump_index;
  root = vloop_build_int(function, reduce_index, addend, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }
  if (d.n_nodes < 2) { /* a bare-load sum belongs to the tuned sum kernels */
    ir_operand_destroy(&bound);
    return 1;
  }
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > VLOOP_REG_BUDGET - 1 /* ymm2 reserved as accumulator */ ||
      d.n_arrays > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_I32;
  fused.location = function->instructions[header_index].location;
  fused.float_bits = 32;
  fused.dest = ir_operand_symbol(acc_symbol);
  fused.lhs = bound; /* take ownership */
  if (!vloop_serialize_into(&fused, &d, /*reduce_op=*/1, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_auto_vectorize_int_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_int_map_at(function, i, changed)) {
        return 0;
      }
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_int_reduce_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Early-exit search skip-ahead -> IR_OP_SIMD_FIND                             */
/*                                                                             */
/* Vectorizes find / memchr / mismatch loops WITHOUT touching their control    */
/* flow: only the counter's zero-init is replaced by a kernel that computes    */
/* the exact first index where the exit predicate holds (else n). The scalar   */
/* loop survives and re-runs from that index, so it executes at most the hit   */
/* iteration plus the sub-block tail, and every exit path (break, return, a    */
/* flag store) replays natively. Soundness needs only two facts, both proved   */
/* here: iterations BEFORE the first hit are observably pure (address math +   */
/* the decoded loads + the compare + the increment, nothing trapping), and     */
/* the kernel's predicate is exactly the loop's exit predicate.                */
/*                                                                             */
/* Two source shapes:                                                          */
/*   Form A: while (i < n) { if (a[i] PRED rhs) { <anything that returns or   */
/*           breaks> } i++; }      -- hit when the condition is TRUE.          */
/*   Form B: while (i < n) { <pure> if (!(...)) -> exits via the condition    */
/*           branch jumping OUT of the body (e.g. `while (i < n && a[i] !=    */
/*           key)`) -- hit when the condition is FALSE (predicate inverted).   */
/* rhs forms: int literal, loop-invariant scalar symbol, or b[i] (mismatch).   */
/* Elements: int32 (8 lanes) or bytes (32 lanes; == / != only). Ordered        */
/* predicates are signed-gated; literals/scalars are width/signedness-gated    */
/* so the 32-bit lane compare agrees with the scalar 64-bit compare.           */
/* -------------------------------------------------------------------------- */

/* Predicate codes -- must match the kernel decoder in simd_int.c. */
#define VFIND_P_EQ 0
#define VFIND_P_NE 1
#define VFIND_P_LT 2
#define VFIND_P_GT 3
#define VFIND_P_LE 4
#define VFIND_P_GE 5

static int vfind_pred_from_text(const char *text) {
  if (strcmp(text, "==") == 0) return VFIND_P_EQ;
  if (strcmp(text, "!=") == 0) return VFIND_P_NE;
  if (strcmp(text, "<") == 0) return VFIND_P_LT;
  if (strcmp(text, ">") == 0) return VFIND_P_GT;
  if (strcmp(text, "<=") == 0) return VFIND_P_LE;
  if (strcmp(text, ">=") == 0) return VFIND_P_GE;
  return -1;
}

static int vfind_pred_invert(int p) {
  switch (p) {
  case VFIND_P_EQ: return VFIND_P_NE;
  case VFIND_P_NE: return VFIND_P_EQ;
  case VFIND_P_LT: return VFIND_P_GE;
  case VFIND_P_GT: return VFIND_P_LE;
  case VFIND_P_LE: return VFIND_P_GT;
  default: return VFIND_P_LT;
  }
}

static int vfind_pred_mirror(int p) { /* a P b == b P' a */
  switch (p) {
  case VFIND_P_LT: return VFIND_P_GT;
  case VFIND_P_GT: return VFIND_P_LT;
  case VFIND_P_LE: return VFIND_P_GE;
  case VFIND_P_GE: return VFIND_P_LE;
  default: return p; /* EQ / NE symmetric */
  }
}

/* Decode `temp` as the canonical a[iv] load for int32 (addr = base + (iv<<2),
 * size 4) or byte (addr = base + iv, size 1) elements. Returns the LOAD
 * instruction so callers can pin identity and signedness. */
static int vfind_decode_indexed_load(IRFunction *function, size_t before,
                                     const char *temp, const char *iv,
                                     const char **base_out, int *u8_out,
                                     const IRInstruction **load_out) {
  const IRInstruction *load = NULL;
  const IRInstruction *addr = NULL;

  if (!temp || !iv) {
    return 0;
  }
  load = ir_find_temp_producer_before(function, before, temp);
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  addr = ir_find_temp_producer_before(
      function, (size_t)(load - function->instructions), load->lhs.name);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name) {
    return 0;
  }
  if (load->rhs.int_value == 4) { /* int32: index temp = iv << 2 */
    const IRInstruction *shl = NULL;
    if (addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
      return 0;
    }
    shl = ir_find_temp_producer_before(
        function, (size_t)(addr - function->instructions), addr->rhs.name);
    if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
        strcmp(shl->text, "<<") != 0 ||
        !ir_operand_is_symbol_named(&shl->lhs, iv) ||
        shl->rhs.kind != IR_OPERAND_INT || shl->rhs.int_value != 2) {
      return 0;
    }
    *u8_out = 0;
  } else if (load->rhs.int_value == 1) { /* byte: addr = base + iv */
    if (!ir_operand_is_symbol_named(&addr->rhs, iv)) {
      return 0;
    }
    *u8_out = 1;
  } else {
    return 0;
  }
  *base_out = addr->lhs.name;
  *load_out = load;
  return 1;
}

static int vfind_symbol_written_in(const IRFunction *function, size_t lo,
                                   size_t hi, const char *name) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* The lexer style `alpha || digit || '_'` loop has several short circuit
 * branches, so the scalar predicate matcher below cannot claim it. After the
 * ASCII range fold it has one stable, exact shape. Recognize that shape and
 * plant a class search before the loop. The loop stays intact and checks the
 * stop byte itself, just like every other find skip ahead. */
#define VFIND_P_ASCII_IDENT_END 6

static int vfind_same_operand(const IROperand *a, const IROperand *b) {
  if (!a || !b || a->kind != b->kind) return 0;
  if (a->kind == IR_OPERAND_INT) return a->int_value == b->int_value;
  if (a->kind == IR_OPERAND_SYMBOL || a->kind == IR_OPERAND_TEMP) {
    return a->name && b->name && strcmp(a->name, b->name) == 0;
  }
  return 0;
}

static int vfind_temp_from(const IROperand *operand,
                           const IRInstruction *producer) {
  return operand && producer && producer->dest.kind == IR_OPERAND_TEMP &&
         producer->dest.name && operand->kind == IR_OPERAND_TEMP &&
         operand->name && strcmp(operand->name, producer->dest.name) == 0;
}

static int vfind_binary_const(const IRInstruction *ins, const char *op,
                              const IROperand *lhs, long long rhs) {
  return ins && ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
         strcmp(ins->text, op) == 0 && vfind_same_operand(&ins->lhs, lhs) &&
         ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == rhs;
}

static int vfind_label_target(const IRInstruction *branch,
                              const IRInstruction *label) {
  return branch && label && branch->text && label->op == IR_OP_LABEL &&
         label->text && strcmp(branch->text, label->text) == 0;
}

static int vfind_instruction_reads_symbol(const IRInstruction *ins,
                                          const char *name) {
  if (!ins || !name) return 0;
  if (ir_operand_is_symbol_named(&ins->lhs, name) ||
      ir_operand_is_symbol_named(&ins->rhs, name)) {
    return 1;
  }
  for (size_t i = 0; i < ins->argument_count; i++) {
    if (ir_operand_is_symbol_named(&ins->arguments[i], name)) return 1;
  }
  return 0;
}

static int vfind_symbol_is_signed_i32(const IRFunction *function,
                                      const char *name) {
  if (!function || !name) return 0;
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names && function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return function->parameter_types && function->parameter_types[i] &&
             strcmp(function->parameter_types[i], "int32") == 0;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol_named(&ins->dest, name)) {
      return ins->text && strcmp(ins->text, "int32") == 0;
    }
  }
  return 0;
}

static int vfind_operand_fits_signed_i32(const IRFunction *function,
                                         size_t at,
                                         const IROperand *operand) {
  IRValueRangeCtx ranges;
  IRIntRange value;
  ir_value_range_ctx_init(&ranges, function);
  ir_value_range_of(&ranges, at, operand, &value);
  ir_value_range_ctx_destroy(&ranges);
  return value.lo >= INT32_MIN && value.hi <= INT32_MAX;
}

static int ir_try_vectorize_ascii_ident_find_at(IRFunction *function,
                                                 size_t header_index,
                                                 int *changed) {
  size_t cmp_index = 0, branch_index = 0, jump_index = (size_t)-1;
  size_t real[24];
  size_t real_count = 0;
  const IROperand *base = NULL;
  IRInstruction fused = {0};

  if (!function || header_index >= function->instruction_count ||
      function->instructions[header_index].op != IR_OP_LABEL ||
      !ir_label_is_while_header(function->instructions[header_index].text)) {
    return 1;
  }
  if (!ir_find_next_non_nop(function, header_index + 1, &cmp_index) ||
      !ir_find_next_non_nop(function, cmp_index + 1, &branch_index)) {
    return 1;
  }

  IRInstruction *header = &function->instructions[header_index];
  IRInstruction *bound_cmp = &function->instructions[cmp_index];
  IRInstruction *bound_branch = &function->instructions[branch_index];
  if (bound_cmp->op != IR_OP_BINARY || bound_cmp->is_float ||
      !bound_cmp->text || strcmp(bound_cmp->text, "<") != 0 ||
      bound_cmp->lhs.kind != IR_OPERAND_SYMBOL || !bound_cmp->lhs.name ||
      (bound_cmp->rhs.kind != IR_OPERAND_SYMBOL &&
       bound_cmp->rhs.kind != IR_OPERAND_TEMP &&
       bound_cmp->rhs.kind != IR_OPERAND_INT) ||
      bound_branch->op != IR_OP_BRANCH_ZERO || !bound_branch->text ||
      !vfind_temp_from(&bound_branch->lhs, bound_cmp)) {
    return 1;
  }
  const char *iv = bound_cmp->lhs.name;

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_JUMP && ins->text &&
        strcmp(ins->text, header->text) == 0) {
      jump_index = i;
      break;
    }
    if (ins->op == IR_OP_LABEL && ins->text &&
        strcmp(ins->text, bound_branch->text) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      !ir_fused_loop_exit_is_adjacent(function, jump_index,
                                       bound_branch->text)) {
    return 1;
  }
  for (size_t i = branch_index + 1; i <= jump_index; i++) {
    if (function->instructions[i].op == IR_OP_NOP) continue;
    if (real_count >= sizeof(real) / sizeof(real[0])) return 1;
    real[real_count++] = i;
  }
  if (real_count != 21) return 1;

  IRInstruction *addr = &function->instructions[real[0]];
  IRInstruction *load = &function->instructions[real[1]];
  IRInstruction *assign = &function->instructions[real[2]];
  IRInstruction *fold = &function->instructions[real[3]];
  IRInstruction *sub = &function->instructions[real[4]];
  IRInstruction *alpha = &function->instructions[real[5]];
  IRInstruction *alpha_branch = &function->instructions[real[6]];
  IRInstruction *alpha_jump = &function->instructions[real[7]];
  IRInstruction *digit_label = &function->instructions[real[8]];
  IRInstruction *digit_lo = &function->instructions[real[9]];
  IRInstruction *digit_lo_branch = &function->instructions[real[10]];
  IRInstruction *digit_hi = &function->instructions[real[11]];
  IRInstruction *digit_hi_branch = &function->instructions[real[12]];
  IRInstruction *digit_jump = &function->instructions[real[13]];
  IRInstruction *digit_mid_label = &function->instructions[real[14]];
  IRInstruction *underscore_label = &function->instructions[real[15]];
  IRInstruction *underscore = &function->instructions[real[16]];
  IRInstruction *underscore_branch = &function->instructions[real[17]];
  IRInstruction *continue_label = &function->instructions[real[18]];
  IRInstruction *increment = &function->instructions[real[19]];
  IRInstruction *backedge = &function->instructions[real[20]];

  if (addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->dest.kind != IR_OPERAND_TEMP ||
      !addr->dest.name || load->op != IR_OP_LOAD || !load->is_unsigned ||
      !vfind_temp_from(&load->lhs, addr) ||
      load->rhs.kind != IR_OPERAND_INT || load->rhs.int_value != 1 ||
      assign->op != IR_OP_ASSIGN || assign->dest.kind != IR_OPERAND_SYMBOL ||
      !assign->dest.name || !vfind_temp_from(&assign->lhs, load)) {
    return 1;
  }
  if (ir_operand_is_symbol_named(&addr->lhs, iv)) base = &addr->rhs;
  else if (ir_operand_is_symbol_named(&addr->rhs, iv)) base = &addr->lhs;
  if (!base || (base->kind != IR_OPERAND_SYMBOL &&
                base->kind != IR_OPERAND_TEMP) || !base->name ||
      (base->kind == IR_OPERAND_TEMP &&
       !ir_find_temp_producer_before(function, header_index, base->name)) ||
      (bound_cmp->rhs.kind == IR_OPERAND_TEMP &&
       (!bound_cmp->rhs.name ||
        !ir_find_temp_producer_before(function, header_index,
                                      bound_cmp->rhs.name))) ||
      ir_symbol_address_taken(function, assign->dest.name)) {
    return 1;
  }
  if (strcmp(assign->dest.name, iv) == 0 ||
      (base->kind == IR_OPERAND_SYMBOL &&
       (strcmp(base->name, iv) == 0 ||
        strcmp(base->name, assign->dest.name) == 0)) ||
      (bound_cmp->rhs.kind == IR_OPERAND_SYMBOL &&
       (strcmp(bound_cmp->rhs.name, iv) == 0 ||
        strcmp(bound_cmp->rhs.name, assign->dest.name) == 0))) {
    return 1;
  }

  if (!vfind_binary_const(fold, "|", &assign->dest, 32) ||
      !vfind_binary_const(sub, "-", &fold->dest, 97) ||
      !vfind_binary_const(alpha, "<=", &sub->dest, 25) ||
      !alpha->is_unsigned || alpha_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&alpha_branch->lhs, alpha) ||
      alpha_jump->op != IR_OP_JUMP ||
      !vfind_label_target(alpha_branch, digit_label) ||
      !vfind_label_target(alpha_jump, continue_label) ||
      !vfind_binary_const(digit_lo, ">=", &assign->dest, 48) ||
      digit_lo_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&digit_lo_branch->lhs, digit_lo) ||
      !vfind_label_target(digit_lo_branch, underscore_label) ||
      !vfind_binary_const(digit_hi, "<=", &assign->dest, 57) ||
      digit_hi_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&digit_hi_branch->lhs, digit_hi) ||
      !vfind_label_target(digit_hi_branch, digit_mid_label) ||
      digit_jump->op != IR_OP_JUMP ||
      !vfind_label_target(digit_jump, continue_label) ||
      !vfind_binary_const(underscore, "==", &assign->dest, 95) ||
      underscore_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&underscore_branch->lhs, underscore) ||
      strcmp(underscore_branch->text, bound_branch->text) != 0 ||
      !ir_try_parse_direct_unit_increment(increment, iv) ||
      backedge->op != IR_OP_JUMP || !backedge->text ||
      strcmp(backedge->text, header->text) != 0) {
    return 1;
  }

  /* The kernel advances in 64 bits. Signed int32 endpoints prove that this is
   * exact even for a negative start: a unit step cannot wrap before reaching
   * an int32 bound. */
  if (!vfind_symbol_is_signed_i32(function, iv) ||
      !vfind_operand_fits_signed_i32(function, header_index,
                                     &bound_cmp->rhs)) {
    return 1;
  }

  /* Skipping assignments to the source local is safe only when its value does
   * not escape the loop. The stop iteration still replays and writes it on the
   * hit path; this check also covers the no-hit path. */
  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    if (vfind_instruction_reads_symbol(&function->instructions[i],
                                       assign->dest.name)) {
      return 1;
    }
  }

  /* A second pass over the inserted loop must not plant a second kernel. */
  for (size_t i = header_index; i-- > 0;) {
    IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) break;
    if (ins->op == IR_OP_SIMD_FIND &&
        ir_operand_is_symbol_named(&ins->dest, iv)) {
      return 1;
    }
  }

  fused.op = IR_OP_SIMD_FIND;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(iv);
  if (!ir_operand_clone(&bound_cmp->rhs, &fused.lhs) ||
      !ir_operand_clone(base, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.arguments = calloc(5, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 5;
  fused.arguments[0] = ir_operand_int(VFIND_P_ASCII_IDENT_END);
  fused.arguments[1] = ir_operand_int(1);
  fused.arguments[2] = ir_operand_int(0);
  fused.arguments[3] = ir_operand_int(0);
  fused.arguments[4] = ir_operand_symbol(iv);
  if (!ir_function_insert_instruction(function, header_index, &fused)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_instruction_destroy_storage(&fused);
  if (changed) *changed = 1;
  return 1;
}

static int ir_try_vectorize_find_at(IRFunction *function, size_t header_index,
                                    int *changed, int *claimed_out,
                                    int install) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  size_t cb = 0;            /* the single conditional branch in the body */
  int n_cond = 0;
  size_t exit_lo = 0, exit_hi = 0; /* Form A exit region (cb+1 .. L_idx) */
  size_t l_idx = 0;
  int form_b = 0;
  const IRInstruction *br = NULL;
  const IRInstruction *cmp = NULL;
  int pred = -1;
  const char *a_base = NULL;
  int a_u8 = 0;
  const IRInstruction *a_load = NULL;
  const char *b_base = NULL;
  const IRInstruction *b_load = NULL;
  const IROperand *other = NULL;
  int rhs_kind = -1;
  IROperand rhs_arg = {0};
  size_t init_index = (size_t)-1;
  IRInstruction fused = {0};

  if (install &&
      !ir_try_vectorize_ascii_ident_find_at(function, header_index, changed)) {
    return 0;
  }

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }

  /* exactly one conditional branch in the body */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_BRANCH_EQ) {
      ir_operand_destroy(&bound);
      return 1;
    }
    if (op == IR_OP_BRANCH_ZERO) {
      cb = i;
      n_cond++;
    }
  }
  if (n_cond != 1) {
    ir_operand_destroy(&bound);
    return 1;
  }
  br = &function->instructions[cb];
  if (br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name || !br->text ||
      strcmp(br->text, function->instructions[header_index].text) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }

  /* locate the branch target inside the body (Form A) or not (Form B), and
   * refuse any other label in the body. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL) {
      continue;
    }
    if (ins->text && strcmp(ins->text, br->text) == 0 && i > cb && !l_idx) {
      l_idx = i;
      continue;
    }
    ir_operand_destroy(&bound);
    return 1;
  }
  form_b = (l_idx == 0);
  if (!form_b) {
    exit_lo = cb + 1;
    exit_hi = l_idx;
    /* The exit region runs ONLY on a hit (the kernel never skips a hit), so
     * its contents are unconstrained -- but it must actually EXIT: exactly
     * one terminator (RETURN, or JUMP out of the loop) as its last real
     * instruction, no other control flow, so a hit can never fall through
     * into the increment as if nothing happened with iterations skipped. */
    int saw_term = 0;
    for (size_t i = exit_lo; i < exit_hi; i++) {
      const IRInstruction *e = &function->instructions[i];
      if (e->op == IR_OP_NOP) {
        continue;
      }
      if (saw_term) {
        ir_operand_destroy(&bound);
        return 1;
      }
      if (e->op == IR_OP_RETURN) {
        saw_term = 1;
        continue;
      }
      if (e->op == IR_OP_JUMP) {
        /* must leave the loop: target label not within [header, jump] */
        int inside = 0;
        for (size_t k = header_index; k <= jump_index; k++) {
          const IRInstruction *lab = &function->instructions[k];
          if (lab->op == IR_OP_LABEL && lab->text && e->text &&
              strcmp(lab->text, e->text) == 0) {
            inside = 1;
            break;
          }
        }
        if (inside) {
          ir_operand_destroy(&bound);
          return 1;
        }
        saw_term = 1;
        continue;
      }
      if (e->op == IR_OP_BRANCH_ZERO || e->op == IR_OP_BRANCH_EQ ||
          e->op == IR_OP_LABEL) {
        ir_operand_destroy(&bound);
        return 1;
      }
      /* anything else (stores, calls, math) is fine: it only runs on a hit */
    }
    if (!saw_term) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }

  /* decode the condition: load CMP rhs, or the folded `load != 0` form
   * (x != 0 lowers to branching on the raw loaded value -- the strlen shape) */
  if (vfind_decode_indexed_load(function, cb, br->lhs.name, iv_symbol, &a_base,
                                &a_u8, &a_load)) {
    pred = VFIND_P_NE; /* branch condition == the value: "value != 0" */
    other = NULL;      /* implicit literal 0 */
  } else {
    cmp = ir_find_temp_producer_before(function, cb, br->lhs.name);
    if (!cmp || cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text) {
      ir_operand_destroy(&bound);
      return 1;
    }
    pred = vfind_pred_from_text(cmp->text);
    if (pred < 0) {
      ir_operand_destroy(&bound);
      return 1;
    }
    if (cmp->lhs.kind == IR_OPERAND_TEMP && cmp->lhs.name &&
        vfind_decode_indexed_load(function, cb, cmp->lhs.name, iv_symbol,
                                  &a_base, &a_u8, &a_load)) {
      other = &cmp->rhs;
    } else if (cmp->rhs.kind == IR_OPERAND_TEMP && cmp->rhs.name &&
               vfind_decode_indexed_load(function, cb, cmp->rhs.name,
                                         iv_symbol, &a_base, &a_u8, &a_load)) {
      other = &cmp->lhs;
      pred = vfind_pred_mirror(pred);
    } else {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  /* the load must be the loop's own (in-body), not a stale pre-loop value */
  if ((size_t)(a_load - function->instructions) <= branch_index) {
    ir_operand_destroy(&bound);
    return 1;
  }

  /* classify the other side */
  if (!other) { /* implicit `!= 0` */
    rhs_kind = 0;
    rhs_arg = ir_operand_int(0);
  } else if (other->kind == IR_OPERAND_INT) {
    long long v = other->int_value;
    int ok = a_u8 ? (v >= 0 && v <= 255)
                  : (a_load->is_unsigned ? (v >= 0 && v <= 4294967295LL)
                                         : (v >= -2147483648LL &&
                                            v <= 2147483647LL));
    if (!ok) {
      ir_operand_destroy(&bound);
      return 1;
    }
    rhs_kind = 0;
    rhs_arg = ir_operand_int(v);
  } else if (other->kind == IR_OPERAND_TEMP && other->name) {
    int b_u8 = 0;
    if (!vfind_decode_indexed_load(function, cb, other->name, iv_symbol,
                                   &b_base, &b_u8, &b_load) ||
        b_u8 != a_u8 ||
        (size_t)(b_load - function->instructions) <= branch_index ||
        a_load->is_unsigned != b_load->is_unsigned ||
        !ir_symbol_is_float_array_base(function, b_base)) {
      ir_operand_destroy(&bound);
      return 1;
    }
    rhs_kind = 2;
    rhs_arg = ir_operand_symbol(b_base);
  } else if (other->kind == IR_OPERAND_SYMBOL && other->name) {
    const char *ty = ir_function_local_declared_type(function, other->name);
    if (!ty) {
      ty = ir_function_param_declared_type(function, other->name);
    }
    int ok = 0;
    if (a_u8) {
      ok = ty && (strcmp(ty, "int8") == 0 || strcmp(ty, "uint8") == 0);
    } else if (a_load->is_unsigned) {
      ok = ty && strcmp(ty, "uint32") == 0;
    } else {
      ok = ty && strcmp(ty, "int32") == 0;
    }
    if (!ok || strcmp(other->name, iv_symbol) == 0 ||
        ir_symbol_address_taken(function, other->name) ||
        vfind_symbol_written_in(function, branch_index + 1, jump_index,
                                other->name)) {
      ir_operand_destroy(&bound);
      return 1;
    }
    rhs_kind = 1;
    if (!ir_operand_clone(other, &rhs_arg)) {
      ir_operand_destroy(&bound);
      return 0;
    }
  } else {
    ir_operand_destroy(&bound);
    return 1;
  }

  /* ordered predicates: signed 32-bit only (vpcmpgtd is signed) */
  if (pred != VFIND_P_EQ && pred != VFIND_P_NE &&
      (a_u8 || a_load->is_unsigned ||
       (rhs_kind == 2 && b_load->is_unsigned))) {
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  if (!ir_symbol_is_float_array_base(function, a_base)) {
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  /* continue-path purity: outside the exit region, only the decoded loads,
   * non-trapping address math (+, <<), temp copies, the compare, the branch,
   * the increment, and the back-jump may appear. A stray load (a page the
   * kernel never touches) or a trapping op (/) in a SKIPPED iteration would
   * be an observable difference, so anything else refuses. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!form_b && i >= exit_lo && i < exit_hi) {
      continue; /* exit region: runs only on a hit, checked above */
    }
    if (ins->op == IR_OP_NOP || i == cb || (!form_b && i == l_idx)) {
      continue;
    }
    if (ins == a_load || (b_load && ins == b_load)) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        ins->dest.kind == IR_OPERAND_TEMP &&
        (strcmp(ins->text, "+") == 0 || strcmp(ins->text, "<<") == 0 ||
         ins == cmp)) {
      continue;
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_TEMP) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol) &&
        ir_operand_is_symbol_named(&ins->lhs, iv_symbol) &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == 1) {
      continue;
    }
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  /* Form B: the branch exits when the condition is FALSE */
  if (form_b) {
    pred = vfind_pred_invert(pred);
  }

  /* locate the zero-init to replace (the frame already proved it exists on
   * the straight-line path into the header) */
  for (size_t i = header_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      break;
    }
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      init_index = i;
      break;
    }
  }
  if (init_index == (size_t)-1) {
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  /* The kernel replaces the zero-init, so everything it reads must already
   * hold its loop-entry value THERE. `run = 0; a = src + cand; while (run <
   * limit && a[run] == b[run])` puts the bases after the init, and the kernel
   * would scan the previous iteration's buffers -- or uninitialized ones on
   * the first pass. */
  {
    const char *reads[4];
    size_t read_count = 0;
    reads[read_count++] = a_base;
    if (b_base) {
      reads[read_count++] = b_base;
    }
    if (rhs_arg.kind == IR_OPERAND_SYMBOL && rhs_arg.name) {
      reads[read_count++] = rhs_arg.name;
    }
    if (bound.kind == IR_OPERAND_SYMBOL && bound.name) {
      reads[read_count++] = bound.name;
    }
    for (size_t r = 0; r < read_count; r++) {
      if (reads[r] && vfind_symbol_written_in(function, init_index + 1,
                                              header_index, reads[r])) {
        ir_operand_destroy(&bound);
        ir_operand_destroy(&rhs_arg);
        return 1;
      }
    }
  }

  if (claimed_out) {
    *claimed_out = 1;
  }
  if (!install) { /* read-only probe (pointer-induction asks before converting) */
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  fused.op = IR_OP_SIMD_FIND;
  fused.location = function->instructions[header_index].location;
  fused.dest = ir_operand_symbol(iv_symbol);
  fused.lhs = bound; /* take ownership */
  fused.rhs = ir_operand_symbol(a_base);
  fused.arguments = calloc(4, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    ir_operand_destroy(&rhs_arg);
    return 0;
  }
  fused.argument_count = 4;
  fused.arguments[0] = ir_operand_int(pred);
  fused.arguments[1] = ir_operand_int(a_u8);
  fused.arguments[2] = ir_operand_int(rhs_kind);
  fused.arguments[3] = rhs_arg; /* take ownership */

  /* Replace ONLY the init; the loop itself is untouched. */
  ir_instruction_destroy_storage(&function->instructions[init_index]);
  function->instructions[init_index] = fused;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

/* Read-only probe for pointer-induction: converting a claimable find loop to
 * a pointer walk would hide the indexed shape and leave it scalar. */
int ir_auto_vectorize_find_claimable(IRFunction *function, size_t header_index) {
  int claimed = 0;
  if (!ir_try_vectorize_find_at(function, header_index, NULL, &claimed, 0)) {
    return 0;
  }
  return claimed;
}

int ir_auto_vectorize_find_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_find_at(function, i, changed, NULL, 1)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Phase B: outer-loop lane vectorizer -> IR_OP_SIMD_OUTER_LANE_F64            */
/*                                                                             */
/* Recognizes `while(p<P){ <inner counted loop carrying float64 iacc>;         */
/*   total += iacc; p++ }` where the inner loop is outer-IV-invariant and its  */
/*   body is a serial recurrence iacc = CHAIN(iacc, uniform-of-i terms). Runs  */
/*   4 outer iterations in lockstep f64x4 lanes to hide the recurrence latency.*/
/* -------------------------------------------------------------------------- */

/* uniform-of-i linear micro-program ops (applied left to right, starting at i
 * in the integer domain; OL_U_CVT switches to the float domain). */
#define OL_U_AND 1
#define OL_U_OR 2
#define OL_U_XOR 3
#define OL_U_ADD 4
#define OL_U_SUB 5
#define OL_U_MUL 6
#define OL_U_SHL 7
#define OL_U_SHR 8
#define OL_U_CVT 9
#define OL_U_FADD 10
#define OL_U_FSUB 11
#define OL_U_FMUL 12
#define OL_U_FDIV 13
/* inner-recurrence chain ops */
#define OL_C_ADD 0
#define OL_C_SUB 1
#define OL_C_MUL 2
#define OL_C_DIV 3

#define OL_MAX_CHAIN 8
#define OL_MAX_UNIF 8
#define OL_MAX_MICRO 16
#define OL_MAX_FCONST 16

typedef struct {
  int op;
  long long imm; /* int literal for int ops; fconst index for float ops */
} OlMicro;
typedef struct {
  OlMicro micro[OL_MAX_MICRO];
  int n_micro;
} OlUniform;
typedef struct {
  int op;        /* OL_C_* */
  int side;      /* 0: iacc OP term ; 1: term OP iacc */
  int term_kind; /* 0: const (fconst idx) ; 1: uniform (unif idx) */
  int term_idx;
} OlChainStep;
typedef struct {
  OlChainStep chain[OL_MAX_CHAIN];
  int n_chain;
  OlUniform unif[OL_MAX_UNIF];
  int n_unif;
  double fconst[OL_MAX_FCONST];
  int n_fconst;
  /* init_mode 0: the inner accumulator starts at a compile-time float const
   * (iacc_init) -> all outer iterations identical (lane0 fast path, bit-exact).
   * init_mode 1: the seed is a function of the outer index p (a uniform program
   * over p in init_prog) -> outer iterations differ; lanes diverge and are
   * summed by per-lane extraction in p order (still bit-exact). */
  int init_mode;
  double iacc_init;
  OlUniform init_prog;
  int overflow;
} OlDag;

static int ol_intern_fconst(OlDag *d, double v) {
  for (int i = 0; i < d->n_fconst; i++) {
    if (memcmp(&d->fconst[i], &v, sizeof(double)) == 0) {
      return i;
    }
  }
  if (d->n_fconst >= OL_MAX_FCONST) {
    d->overflow = 1;
    return -1;
  }
  d->fconst[d->n_fconst] = v;
  return d->n_fconst++;
}

/* float64 literal (direct or cast-of-literal), like vloop_operand_is_literal. */
static int ol_operand_is_fconst(IRFunction *fn, size_t before,
                                const IROperand *op, double *out) {
  if (op->kind == IR_OPERAND_FLOAT && op->float_bits == 64) {
    *out = op->float_value;
    return 1;
  }
  if (op->kind == IR_OPERAND_TEMP && op->name) {
    const IRInstruction *p = ir_find_temp_producer_before(fn, before, op->name);
    if (p && p->op == IR_OP_CAST && p->text && strcmp(p->text, "float64") == 0) {
      if (p->lhs.kind == IR_OPERAND_FLOAT) { *out = p->lhs.float_value; return 1; }
      if (p->lhs.kind == IR_OPERAND_INT) { *out = (double)p->lhs.int_value; return 1; }
    }
  }
  return 0;
}

/* True if operand's expression references symbol `sym` (bounded walk). */
static int ol_contains_symbol(IRFunction *fn, size_t before, const IROperand *op,
                              const char *sym, int depth) {
  if (!op || depth > 24) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && sym &&
      strcmp(op->name, sym) == 0) {
    return 1;
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  const IRInstruction *p = ir_i2f_resolve_producer(fn, before, op);
  if (!p) {
    return 0;
  }
  size_t pidx = (size_t)(p - fn->instructions);
  if (p->op == IR_OP_BINARY || p->op == IR_OP_CAST) {
    if (ol_contains_symbol(fn, pidx, &p->lhs, sym, depth + 1)) return 1;
    if (p->op == IR_OP_BINARY &&
        ol_contains_symbol(fn, pidx, &p->rhs, sym, depth + 1))
      return 1;
  }
  return 0;
}

/* Build a linear uniform-of-i program for `op` (a value derived from the inner
 * counter `iv` and constants only). Emits micro-ops in i-first apply order. */
static int ol_build_uniform(IRFunction *fn, size_t before, const IROperand *op,
                            const char *iv, OlDag *d, OlUniform *prog) {
  if (!op || d->overflow) {
    return 0;
  }
  /* base: the inner counter i */
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, iv) == 0) {
    return 1; /* program starts implicitly at i (int domain) */
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  const IRInstruction *p = ir_i2f_resolve_producer(fn, before, op);
  if (!p) {
    return 0;
  }
  size_t pidx = (size_t)(p - fn->instructions);
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      strcmp(p->text, "float64") == 0) {
    /* int -> float64 cast */
    if (!ol_build_uniform(fn, pidx, &p->lhs, iv, d, prog)) return 0;
    if (prog->n_micro >= OL_MAX_MICRO) { d->overflow = 1; return 0; }
    prog->micro[prog->n_micro].op = OL_U_CVT;
    prog->micro[prog->n_micro].imm = 0;
    prog->n_micro++;
    return 1;
  }
  if (p->op != IR_OP_BINARY || !p->text) {
    return 0;
  }
  /* Identify the i-bearing operand and the constant operand. */
  const IROperand *L = &p->lhs;
  const IROperand *R = &p->rhs;
  int l_has = ol_contains_symbol(fn, pidx, L, iv, 0);
  int r_has = ol_contains_symbol(fn, pidx, R, iv, 0);
  const IROperand *inner = NULL;
  const IROperand *cst = NULL;
  int cst_on_right = 1;
  if (l_has && !r_has) { inner = L; cst = R; cst_on_right = 1; }
  else if (r_has && !l_has) { inner = R; cst = L; cst_on_right = 0; }
  else { return 0; }

  if (p->is_float) {
    double cv = 0.0;
    if (!ol_operand_is_fconst(fn, pidx, cst, &cv)) return 0;
    int op_code;
    if (strcmp(p->text, "+") == 0) { op_code = OL_U_FADD; }
    else if (strcmp(p->text, "*") == 0) { op_code = OL_U_FMUL; }
    else if (strcmp(p->text, "-") == 0) {
      if (!cst_on_right) return 0; /* c - x not supported */
      op_code = OL_U_FSUB;
    } else if (strcmp(p->text, "/") == 0) {
      if (!cst_on_right) return 0;
      op_code = OL_U_FDIV;
    } else { return 0; }
    int ci = ol_intern_fconst(d, cv);
    if (ci < 0) return 0;
    if (!ol_build_uniform(fn, pidx, inner, iv, d, prog)) return 0;
    if (prog->n_micro >= OL_MAX_MICRO) { d->overflow = 1; return 0; }
    prog->micro[prog->n_micro].op = op_code;
    prog->micro[prog->n_micro].imm = ci;
    prog->n_micro++;
    return 1;
  }
  /* integer op with an integer-literal constant */
  if (cst->kind != IR_OPERAND_INT) return 0;
  long long imm = cst->int_value;
  int op_code;
  if (strcmp(p->text, "&") == 0) { op_code = OL_U_AND; }
  else if (strcmp(p->text, "|") == 0) { op_code = OL_U_OR; }
  else if (strcmp(p->text, "^") == 0) { op_code = OL_U_XOR; }
  else if (strcmp(p->text, "+") == 0) { op_code = OL_U_ADD; }
  else if (strcmp(p->text, "*") == 0) { op_code = OL_U_MUL; }
  else if (strcmp(p->text, "-") == 0) {
    if (!cst_on_right) return 0;
    op_code = OL_U_SUB;
  } else if (strcmp(p->text, "<<") == 0) {
    if (!cst_on_right) return 0;
    op_code = OL_U_SHL;
  } else if (strcmp(p->text, ">>") == 0) {
    if (!cst_on_right) return 0;
    op_code = OL_U_SHR;
  } else { return 0; }
  if (!ol_build_uniform(fn, pidx, inner, iv, d, prog)) return 0;
  if (prog->n_micro >= OL_MAX_MICRO) { d->overflow = 1; return 0; }
  prog->micro[prog->n_micro].op = op_code;
  prog->micro[prog->n_micro].imm = imm;
  prog->n_micro++;
  return 1;
}

/* Extract one chain term (const or uniform-of-i) into the dag; sets *kind/*idx. */
static int ol_extract_term(IRFunction *fn, size_t before, const IROperand *op,
                           const char *iv, OlDag *d, int *kind, int *idx) {
  double cv = 0.0;
  if (ol_operand_is_fconst(fn, before, op, &cv)) {
    int ci = ol_intern_fconst(d, cv);
    if (ci < 0) return 0;
    *kind = 0;
    *idx = ci;
    return 1;
  }
  if (d->n_unif >= OL_MAX_UNIF) { d->overflow = 1; return 0; }
  OlUniform *prog = &d->unif[d->n_unif];
  prog->n_micro = 0;
  if (!ol_build_uniform(fn, before, op, iv, d, prog)) return 0;
  *kind = 1;
  *idx = d->n_unif;
  d->n_unif++;
  return 1;
}

/* Walk the inner accumulator update expression into the recurrence chain
 * (base-first). Exactly one operand at each binary leads to iacc; the other is a
 * uniform term. */
static int ol_build_chain(IRFunction *fn, size_t before, const IROperand *op,
                          const char *iacc, const char *iv, OlDag *d) {
  if (!op || d->overflow) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, iacc) == 0) {
    return 1; /* base: the carried accumulator */
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  const IRInstruction *p = ir_i2f_resolve_producer(fn, before, op);
  if (!p || p->op != IR_OP_BINARY || !p->is_float || !p->text) {
    return 0;
  }
  size_t pidx = (size_t)(p - fn->instructions);
  int l_has = ol_contains_symbol(fn, pidx, &p->lhs, iacc, 0);
  int r_has = ol_contains_symbol(fn, pidx, &p->rhs, iacc, 0);
  const IROperand *inner = NULL;
  const IROperand *term = NULL;
  int side;
  if (l_has && !r_has) { inner = &p->lhs; term = &p->rhs; side = 0; }
  else if (r_has && !l_has) { inner = &p->rhs; term = &p->lhs; side = 1; }
  else { return 0; }

  int op_code;
  if (strcmp(p->text, "+") == 0) { op_code = OL_C_ADD; }
  else if (strcmp(p->text, "-") == 0) { op_code = OL_C_SUB; }
  else if (strcmp(p->text, "*") == 0) { op_code = OL_C_MUL; }
  else if (strcmp(p->text, "/") == 0) { op_code = OL_C_DIV; }
  else { return 0; }

  int kind = 0, idx = 0;
  if (!ol_extract_term(fn, pidx, term, iv, d, &kind, &idx)) {
    return 0;
  }
  if (!ol_build_chain(fn, pidx, inner, iacc, iv, d)) { /* recurse base-side first */
    return 0;
  }
  if (d->n_chain >= OL_MAX_CHAIN) { d->overflow = 1; return 0; }
  d->chain[d->n_chain].op = op_code;
  d->chain[d->n_chain].side = side;
  d->chain[d->n_chain].term_kind = kind;
  d->chain[d->n_chain].term_idx = idx;
  d->n_chain++;
  return 1;
}

/* The inner loop body must be pure straight-line float/int work (the recurrence
 * + the uniform computations + the counter increment). No memory, calls, or
 * control flow. */
static int ol_inner_body_pure(IRFunction *fn, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    switch (fn->instructions[i].op) {
    case IR_OP_STORE:
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_LABEL:
    case IR_OP_INLINE_ASM:
    case IR_OP_MEMCPY_INLINE:
    case IR_OP_NEW:
    case IR_OP_ADDRESS_OF:
    case IR_OP_RETURN:
    case IR_OP_LOAD:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

/* Scan [lo,hi) for the first BRANCH_ZERO, returning its index or -1 (stops at a
 * jump/second label so it stays within the loop header region). */
static long long ol_find_branch_zero(IRFunction *fn, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    IROpcode op = fn->instructions[i].op;
    if (op == IR_OP_BRANCH_ZERO) return (long long)i;
    if (op == IR_OP_JUMP) return -1;
  }
  return -1;
}

/* Find a JUMP to `label` in [lo,hi); returns index or -1. */
static long long ol_find_jump_to(IRFunction *fn, size_t lo, size_t hi,
                                 const char *label) {
  for (size_t i = lo; i < hi; i++) {
    if (fn->instructions[i].op == IR_OP_JUMP && fn->instructions[i].text &&
        label && strcmp(fn->instructions[i].text, label) == 0) {
      return (long long)i;
    }
  }
  return -1;
}

/* Decode a `iv <cmp> N` loop compare feeding the branch at branch_index. Returns
 * 1 with *iv_out/*bound_out/*cmp_out (0:<, 1:<=) on success. */
static int ol_decode_loop_compare(IRFunction *fn, size_t branch_index,
                                  const char **iv_out, IROperand *bound_out,
                                  int *cmp_out) {
  const IRInstruction *br = &fn->instructions[branch_index];
  if (br->op != IR_OP_BRANCH_ZERO || br->lhs.kind != IR_OPERAND_TEMP ||
      !br->lhs.name) {
    return 0;
  }
  const IRInstruction *c =
      ir_find_temp_producer_before(fn, branch_index, br->lhs.name);
  if (!c || c->op != IR_OP_BINARY || c->is_float || !c->text ||
      c->lhs.kind != IR_OPERAND_SYMBOL || !c->lhs.name) {
    return 0;
  }
  if (strcmp(c->text, "<") == 0) { *cmp_out = 0; }
  else if (strcmp(c->text, "<=") == 0) { *cmp_out = 1; }
  else { return 0; }
  if (c->rhs.kind != IR_OPERAND_SYMBOL && c->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  *iv_out = c->lhs.name;
  return ir_operand_clone(&c->rhs, bound_out);
}

static int ir_try_vectorize_outer_lane_at(IRFunction *function,
                                           size_t header_index, int *changed) {
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  const char *outer_label = header->text;
  size_t n = function->instruction_count;
/* Diagnostic hook (no-op in production); set to an fprintf during bring-up. */
#define OL_DBG(msg) ((void)0)

  long long ob = ol_find_branch_zero(function, header_index + 1, n);
  if (ob < 0) {
    OL_DBG("no outer branch_zero");
    return 1;
  }
  size_t outer_branch = (size_t)ob;
  const char *p_sym = NULL;
  IROperand outerP = {0};
  int outer_cmp = 0;
  if (!ol_decode_loop_compare(function, outer_branch, &p_sym, &outerP,
                              &outer_cmp)) {
    OL_DBG("outer compare decode failed");
    return 1;
  }
  if (outer_cmp != 0) {
    OL_DBG("outer compare is not '<'");
    ir_operand_destroy(&outerP);
    return 1;
  }
  long long oj = ol_find_jump_to(function, outer_branch + 1, n, outer_label);
  if (oj < 0) {
    OL_DBG("no outer back-jump");
    ir_operand_destroy(&outerP);
    return 1;
  }
  size_t outer_jump = (size_t)oj;

  /* Find the (single) inner while header in the outer body. ir_label_is_while_header
   * also matches the loop's *end* label (it contains "_lbl_ir_while_"), so skip
   * any label naming a while-end marker , only true headers count. */
  long long inner_hdr = -1;
  for (size_t i = outer_branch + 1; i < outer_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text &&
        ir_label_is_while_header(ins->text) &&
        !strstr(ins->text, "while_end")) {
      if (inner_hdr >= 0) {
        OL_DBG(">1 inner while header");
        ir_operand_destroy(&outerP);
        return 1;
      }
      inner_hdr = (long long)i;
    }
  }
  if (inner_hdr < 0) {
    OL_DBG("no inner while header");
    ir_operand_destroy(&outerP);
    return 1;
  }
  size_t inner_header = (size_t)inner_hdr;
  const char *inner_label = function->instructions[inner_header].text;

  long long ib = ol_find_branch_zero(function, inner_header + 1, outer_jump);
  if (ib < 0) { OL_DBG("no inner branch_zero"); ir_operand_destroy(&outerP); return 1; }
  size_t inner_branch = (size_t)ib;
  const char *i_sym = NULL;
  IROperand innerN = {0};
  int inner_cmp = 0;
  if (!ol_decode_loop_compare(function, inner_branch, &i_sym, &innerN,
                              &inner_cmp)) {
    OL_DBG("inner compare decode failed");
    ir_operand_destroy(&outerP);
    return 1;
  }
  long long ij = ol_find_jump_to(function, inner_branch + 1, outer_jump,
                                 inner_label);
  if (ij < 0) { OL_DBG("no inner back-jump"); ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
  size_t inner_jump = (size_t)ij;

  /* Inner increment: i = i + istep (unit). */
  {
    size_t inc = inner_jump;
    while (inc > inner_branch + 1) {
      inc--;
      if (function->instructions[inc].op != IR_OP_NOP) break;
    }
    if (!ir_try_parse_direct_unit_increment(&function->instructions[inc],
                                            i_sym)) {
      OL_DBG("inner increment not unit");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
  }
  /* Outer increment: p = p + 1 (unit), just before the outer back-jump. */
  {
    size_t inc = outer_jump;
    while (inc > inner_jump) {
      inc--;
      if (function->instructions[inc].op != IR_OP_NOP) break;
    }
    if (!ir_try_parse_direct_unit_increment(&function->instructions[inc],
                                            p_sym)) {
      OL_DBG("outer increment not unit");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
  }
  if (ir_loop_body_is_unclaimable(function, inner_branch + 1, inner_jump)) {
    OL_DBG("inner body has nested while");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* The kernel replays the outer iterations as p = 0..P-1, so the outer iv
   * must provably start at 0. */
  if (!ir_iv_zero_at_header(function, header_index, p_sym)) {
    OL_DBG("outer iv does not start at 0");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }
  /* The per-outer-iteration init region (outer branch -> inner header) must be
   * straight-line: the i0 and iacc-seed scans below take the textually-last
   * writer, which is only the executed value when no label/jump/branch can
   * reorder control flow through the region. */
  for (size_t i = outer_branch + 1; i < inner_header; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
        op == IR_OP_BRANCH_EQ) {
      OL_DBG("control flow in outer init region");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
  }

  /* The outer reduction total = total + iacc, after the inner loop. */
  const char *total_sym = NULL;
  const char *iacc_sym = NULL;
  for (size_t i = inner_jump + 1; i < outer_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ins->rhs.kind == IR_OPERAND_SYMBOL && ins->rhs.name) {
      total_sym = ins->dest.name;
      iacc_sym = ins->rhs.name;
      break;
    }
  }
  if (!total_sym || !iacc_sym || strcmp(total_sym, iacc_sym) == 0) {
    OL_DBG("no outer reduction total+=iacc");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }
  if (!ir_float_sum_type_matches(
          ir_function_local_declared_type(function, total_sym), 64) ||
      !ir_float_sum_type_matches(
          ir_function_local_declared_type(function, iacc_sym), 64)) {
    OL_DBG("total/iacc not float64");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* i init (i0) before the inner header: the LAST write to i in the init
   * region must be a constant assign (a later non-constant write would make
   * the recorded i0 stale). */
  long long i0 = 0;
  int found_i0 = 0;
  for (size_t i = outer_branch + 1; i < inner_header; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, i_sym)) {
      if (ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_INT) {
        i0 = ins->lhs.int_value;
        found_i0 = 1;
      } else {
        found_i0 = 0;
      }
    }
  }
  if (!found_i0) {
    OL_DBG("no inner i0");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* The inner accumulator seed: either a compile-time float const (all outer
   * iterations identical -> lane0 fast path) or a function of the outer index p
   * (iterations differ -> divergent lanes). */
  OlDag d;
  memset(&d, 0, sizeof(d));
  {
    size_t init_idx = 0;
    if (!ir_find_last_writer_before(function, inner_header, IR_OPERAND_SYMBOL,
                                    iacc_sym, &init_idx) ||
        init_idx <= outer_branch) {
      /* The seed must be written INSIDE the per-outer-iteration init region;
       * a writer before the outer loop would mean iacc carries across outer
       * iterations (a serial dependence the per-lane reseed would break). */
      OL_DBG("no iacc init writer in the outer init region");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
    const IRInstruction *init_ins = &function->instructions[init_idx];
    if (init_ins->op == IR_OP_ASSIGN && init_ins->lhs.kind == IR_OPERAND_FLOAT) {
      d.init_mode = 0;
      d.iacc_init = init_ins->lhs.float_value;
    } else {
      /* Build a uniform program over the OUTER index p for the seed value. For
       * `iacc <- value` walk the value; for `iacc = a OP b` walk the binary. */
      const IROperand *seed_op = NULL;
      IROperand iacc_op = ir_operand_symbol(iacc_sym);
      if (init_ins->op == IR_OP_ASSIGN) {
        seed_op = &init_ins->lhs;
      } else {
        seed_op = &iacc_op; /* ol_build_uniform resolves iacc's producer */
      }
      d.init_prog.n_micro = 0;
      if (!ol_build_uniform(function, inner_header, seed_op, p_sym, &d,
                            &d.init_prog) ||
          d.overflow) {
        OL_DBG("iacc seed neither const nor uniform-of-p");
        ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
      }
      d.init_mode = 1;
    }
  }

  /* The single inner recurrence write: iacc = <expr>. */
  long long iacc_upd = -1;
  for (size_t i = inner_branch + 1; i < inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float &&
        ir_operand_is_symbol_named(&ins->dest, iacc_sym)) {
      if (iacc_upd >= 0) { iacc_upd = -2; break; }
      iacc_upd = (long long)i;
    } else if (ir_operand_is_symbol_named(&ins->dest, iacc_sym)) {
      iacc_upd = -2; /* iacc written by a non-float-binary -> reject */
      break;
    }
  }
  if (iacc_upd < 0) {
    OL_DBG("no single iacc recurrence update");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* Body purity: no stores/calls/branches/etc. in the inner body. */
  if (!ol_inner_body_pure(function, inner_branch + 1, inner_jump)) {
    OL_DBG("inner body not pure");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* Build the recurrence chain from the iacc update RHS (which is the full
   * expression; reconstruct it from dest = lhs OP rhs of the update). */
  {
    /* The update is `iacc = A OP B` (a float binary). Treat its result as the
     * chain root expression by walking from a synthetic temp == the update. */
    const IRInstruction *upd = &function->instructions[iacc_upd];
    /* Re-express: build chain over the binary `upd`. We emulate ol_build_chain
     * on the update by handling its top node directly. */
    int l_has = ol_contains_symbol(function, (size_t)iacc_upd, &upd->lhs,
                                   iacc_sym, 0);
    int r_has = ol_contains_symbol(function, (size_t)iacc_upd, &upd->rhs,
                                   iacc_sym, 0);
    const IROperand *inner_op = NULL;
    const IROperand *term_op = NULL;
    int side;
    if (l_has && !r_has) { inner_op = &upd->lhs; term_op = &upd->rhs; side = 0; }
    else if (r_has && !l_has) { inner_op = &upd->rhs; term_op = &upd->lhs; side = 1; }
    else { OL_DBG("update: both/neither operand carries iacc"); ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
    int op_code;
    if (strcmp(upd->text, "+") == 0) op_code = OL_C_ADD;
    else if (strcmp(upd->text, "-") == 0) op_code = OL_C_SUB;
    else if (strcmp(upd->text, "*") == 0) op_code = OL_C_MUL;
    else if (strcmp(upd->text, "/") == 0) op_code = OL_C_DIV;
    else { OL_DBG("update: top op not +-*/"); ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
    int kind = 0, idx = 0;
    if (!ol_extract_term(function, (size_t)iacc_upd, term_op, i_sym, &d, &kind,
                         &idx) ||
        !ol_build_chain(function, (size_t)iacc_upd, inner_op, iacc_sym, i_sym,
                        &d)) {
      OL_DBG("chain/term build failed");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
    if (d.n_chain >= OL_MAX_CHAIN) { ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
    d.chain[d.n_chain].op = op_code;
    d.chain[d.n_chain].side = side;
    d.chain[d.n_chain].term_kind = kind;
    d.chain[d.n_chain].term_idx = idx;
    d.n_chain++;
  }
  if (d.overflow || d.n_chain == 0) {
    OL_DBG("dag overflow or empty chain");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* The INNER loop must be invariant in p (p may only feed the seed, in the init
   * region before inner_header): reject any p reference in [inner_header,
   * inner_jump]. And `total` must be touched only by the reduction (after
   * inner_jump): reject it anywhere in [outer_branch+1, inner_jump]. */
  for (size_t i = inner_header; i <= inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *ops[3] = {&ins->lhs, &ins->rhs, &ins->dest};
    for (int k = 0; k < 3; k++) {
      if (ops[k]->kind == IR_OPERAND_SYMBOL && ops[k]->name &&
          strcmp(ops[k]->name, p_sym) == 0) {
        OL_DBG("inner loop references p (not p-invariant)");
        ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
      }
    }
  }
  for (size_t i = outer_branch + 1; i <= inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *ops[3] = {&ins->lhs, &ins->rhs, &ins->dest};
    for (int k = 0; k < 3; k++) {
      if (ops[k]->kind == IR_OPERAND_SYMBOL && ops[k]->name &&
          strcmp(ops[k]->name, total_sym) == 0) {
        OL_DBG("total referenced in inner region");
        ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
      }
    }
  }

  /* Serialize and install. Layout (mirror in the kernel):
   * header: [0]inner_cmp [1]istep [2]n_chain [3]n_unif [4]n_fconst [5]i0
   *         [6]init_mode [7]iacc_init(FLOAT)
   * then n_chain*4 INT chain steps; then n_unif chain-uniform programs
   * (n_micro INT + n_micro*2 INT); then IF init_mode==1 the seed program
   * (same shape); then n_fconst FLOAT. dest=total, lhs=P, rhs=N. */
  IRInstruction fused = {0};
  size_t argc = 8 + (size_t)(4 * d.n_chain);
  for (int u = 0; u < d.n_unif; u++) {
    argc += 1 + (size_t)(2 * d.unif[u].n_micro);
  }
  if (d.init_mode == 1) {
    argc += 1 + (size_t)(2 * d.init_prog.n_micro);
  }
  argc += (size_t)d.n_fconst;
  fused.arguments = calloc(argc, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 0;
  }
  fused.argument_count = argc;
  size_t k = 0;
  fused.arguments[k++] = ir_operand_int(inner_cmp);
  fused.arguments[k++] = ir_operand_int(1); /* istep */
  fused.arguments[k++] = ir_operand_int(d.n_chain);
  fused.arguments[k++] = ir_operand_int(d.n_unif);
  fused.arguments[k++] = ir_operand_int(d.n_fconst);
  fused.arguments[k++] = ir_operand_int(i0);
  fused.arguments[k++] = ir_operand_int(d.init_mode);
  fused.arguments[k++] = ir_operand_float_sized(d.iacc_init, 64);
  for (int s = 0; s < d.n_chain; s++) {
    fused.arguments[k++] = ir_operand_int(d.chain[s].op);
    fused.arguments[k++] = ir_operand_int(d.chain[s].side);
    fused.arguments[k++] = ir_operand_int(d.chain[s].term_kind);
    fused.arguments[k++] = ir_operand_int(d.chain[s].term_idx);
  }
  for (int u = 0; u < d.n_unif; u++) {
    fused.arguments[k++] = ir_operand_int(d.unif[u].n_micro);
    for (int m = 0; m < d.unif[u].n_micro; m++) {
      fused.arguments[k++] = ir_operand_int(d.unif[u].micro[m].op);
      fused.arguments[k++] = ir_operand_int(d.unif[u].micro[m].imm);
    }
  }
  if (d.init_mode == 1) {
    fused.arguments[k++] = ir_operand_int(d.init_prog.n_micro);
    for (int m = 0; m < d.init_prog.n_micro; m++) {
      fused.arguments[k++] = ir_operand_int(d.init_prog.micro[m].op);
      fused.arguments[k++] = ir_operand_int(d.init_prog.micro[m].imm);
    }
  }
  for (int c = 0; c < d.n_fconst; c++) {
    fused.arguments[k++] = ir_operand_float_sized(d.fconst[c], 64);
  }
  fused.op = IR_OP_SIMD_OUTER_LANE_F64;
  fused.location = header->location;
  fused.is_float = 1;
  fused.float_bits = 64;
  fused.dest = ir_operand_symbol(total_sym);
  fused.lhs = outerP; /* take ownership */
  fused.rhs = innerN; /* take ownership */
  OL_DBG("INSTALLED outer-lane fusion");
  ir_install_fused_reduction(function, header_index, outer_jump, &fused,
                             changed);
  return 1;
}

int ir_outer_vectorize_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_outer_lane_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}
