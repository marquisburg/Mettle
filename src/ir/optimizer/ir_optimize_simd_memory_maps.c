#include "ir_optimize_internal.h"

int ir_find_while_loop_bounds(IRFunction *function, size_t header_index,
                                     IRWhileLoopBounds *out) {
  size_t compare_index = 0;
  size_t branch_index = 0;

  if (!function || !out || header_index + 4 >= function->instruction_count) {
    return 0;
  }

  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 0;
  }

  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 0;
  }

  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 0;
  }

  out->compare_index = compare_index;
  out->branch_index = branch_index;
  out->loop_label = header->text;
  out->exit_label = branch->text;
  out->jump_index = (size_t)-1;

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, out->loop_label) == 0) {
      out->jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, out->exit_label) == 0) {
      break;
    }
  }

  return out->jump_index != (size_t)-1 &&
         /* threaded exit: the nop-install would delete the exit edge */
         ir_fused_loop_exit_is_adjacent(function, out->jump_index,
                                        out->exit_label);
}

int ir_symbol_contains(const char *symbol, const char *needle) {
  return symbol && needle && strstr(symbol, needle) != NULL;
}

/* The base a walking pointer was initialized from. The fused kernels replay
 * the walk from base[0], so the init must be the pointer's ONLY write before
 * the loop: with multiple writes (a reassignment, a previous loop's advance,
 * an if/else init) the entering value is not provably the base. */
const char *ir_find_ptr_init_base(const IRFunction *function, size_t before,
                                         const char *ptr_symbol) {
  const char *base = NULL;
  size_t i = 0;
  if (!function || !ptr_symbol) {
    return NULL;
  }
  for (i = 0; i < before; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!ir_instruction_writes_destination(ins) ||
        !ir_operand_is_symbol_named(&ins->dest, ptr_symbol)) {
      continue;
    }
    if (base || ins->op != IR_OP_ASSIGN ||
        ins->lhs.kind != IR_OPERAND_SYMBOL || !ins->lhs.name) {
      return NULL;
    }
    base = ins->lhs.name;
  }
  return base;
}

int ir_find_ptr_loop_len_operand(const IRFunction *function,
                                        size_t header_index,
                                        const char *end_ptr, const char *base,
                                        IROperand *out_len) {
  size_t i = 0;
  if (!function || !end_ptr || !base || !out_len) {
    return 0;
  }
  for (i = 0; i < header_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    int anchored = 0;
    /* end = base + (n << 2): anchored to the SAME base the walk starts from,
     * so the kernel's n iterations equal the scalar walk's (end-base)/4. An
     * end computed off a different/offset pointer would make `n` a lie. The
     * add's lhs is the base directly, or the end pointer itself when the IR
     * staged it as `end <- base; end = end + t` -- then the staging assign
     * must be the only prior write and must read the base. */
    if (ins->op != IR_OP_BINARY || !ins->text || strcmp(ins->text, "+") != 0 ||
        !ir_operand_is_symbol_named(&ins->dest, end_ptr) ||
        ins->rhs.kind != IR_OPERAND_TEMP || !ins->rhs.name) {
      continue;
    }
    if (ir_operand_is_symbol_named(&ins->lhs, base)) {
      anchored = 1;
    } else if (ir_operand_is_symbol_named(&ins->lhs, end_ptr)) {
      const IRInstruction *stage = NULL;
      anchored = 1;
      for (size_t j = 0; j < i; j++) {
        const IRInstruction *prev = &function->instructions[j];
        if (!ir_instruction_writes_destination(prev) ||
            !ir_operand_is_symbol_named(&prev->dest, end_ptr)) {
          continue;
        }
        if (stage || prev->op != IR_OP_ASSIGN ||
            !ir_operand_is_symbol_named(&prev->lhs, base)) {
          anchored = 0;
          break;
        }
        stage = prev;
      }
      if (!stage) {
        anchored = 0;
      }
    }
    if (!anchored) {
      continue;
    }
    const IRInstruction *scale = ir_find_temp_producer_before(
        function, i, ins->rhs.name);
    if (!scale || scale->op != IR_OP_BINARY || !scale->text) {
      continue;
    }
    if ((strcmp(scale->text, "<<") == 0 &&
         ir_operand_is_int_value(&scale->rhs, 2)) ||
        (strcmp(scale->text, "*") == 0 &&
         ir_operand_is_int_value(&scale->rhs, 4))) {
      if (scale->lhs.kind == IR_OPERAND_SYMBOL && scale->lhs.name &&
          ir_symbol_is_sum_loop_bound(function, scale->lhs.name)) {
        *out_len = ir_operand_symbol(scale->lhs.name);
        return 1;
      }
    }
  }
  return 0;
}

static const char *ir_find_ptr_step_symbol(const IRFunction *function,
                                           size_t start, size_t end,
                                           long long step) {
  size_t i = 0;
  if (!function) {
    return NULL;
  }
  for (i = start; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ir_operand_is_int_value(&ins->rhs, step) &&
        ir_symbol_contains(ins->dest.name, "__ptr_")) {
      return ins->dest.name;
    }
  }
  return NULL;
}

int ir_symbol_is_i32_ptr_param(const IRFunction *function,
                                      const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }
  if (ir_function_symbol_is_parameter(function, symbol_name)) {
    const char *type = ir_function_local_declared_type(function, symbol_name);
    if (!type && function->parameter_types) {
      size_t i = 0;
      for (i = 0; i < function->parameter_count; i++) {
        if (function->parameter_names[i] &&
            strcmp(function->parameter_names[i], symbol_name) == 0 &&
            function->parameter_types[i]) {
          type = function->parameter_types[i];
          break;
        }
      }
    }
    return type && strcmp(type, "int32*") == 0;
  }
  /* A local nothing reassigns holds one value for the whole function, which
   * is all a walking base needs. The `_param_` name checks this replaces were
   * the inliner-era spelling of the same property. */
  if (ir_symbol_is_settled_local(function, symbol_name, "int32*")) {
    return 1;
  }
  return ir_function_symbol_is_inlined_param(function, symbol_name, "int32*",
                                             "_param_src") ||
         ir_function_symbol_is_inlined_param(function, symbol_name, "int32*",
                                             "_param_dst") ||
         ir_function_symbol_is_inlined_param(function, symbol_name, "int32*",
                                             "_param_data");
}

static int ir_param_name_is_lo(const char *name) {
  return name && (ir_symbol_contains(name, "_param_lo") ||
                  ir_symbol_contains(name, "_param_floor_bound"));
}

static int ir_param_name_is_hi(const char *name) {
  return name && (ir_symbol_contains(name, "_param_hi") ||
                  ir_symbol_contains(name, "_param_ceiling_bound"));
}

static int ir_param_name_is_v(const char *name) {
  return name && (ir_symbol_contains(name, "_param_v") ||
                  ir_symbol_contains(name, "_param_x"));
}

static int ir_cast_is_to_int64(const IRInstruction *ins) {
  return ins && ins->op == IR_OP_CAST && ins->text &&
         strstr(ins->text, "int64") != NULL;
}

int ir_fuse_while_loop_to_insn(IRFunction *function, size_t header_index,
                                      size_t jump_index, IRInstruction *fused,
                                      int *changed) {
  if (!function || !fused || jump_index < header_index) {
    ir_instruction_destroy_storage(fused);
    return 0;
  }
  ir_instruction_destroy_storage(&function->instructions[header_index]);
  function->instructions[header_index] = *fused;
  memset(fused, 0, sizeof(*fused));
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_make_simd_with_len_and_two_ints(IRInstruction *out,
                                              SourceLocation location,
                                              IROpcode op,
                                              const IROperand *dest,
                                              const char *lhs_symbol,
                                              const char *rhs_symbol,
                                              const IROperand *len_operand,
                                              int arg1, int arg2) {
  if (!out || !dest || !lhs_symbol || !rhs_symbol || !len_operand) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->op = op;
  out->location = location;
  if (!ir_operand_clone(dest, &out->dest)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->lhs = ir_operand_symbol(lhs_symbol);
  out->rhs = ir_operand_symbol(rhs_symbol);
  out->arguments = calloc(3, sizeof(IROperand));
  if (!out->arguments) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->argument_count = 3;
  if (!ir_operand_clone(len_operand, &out->arguments[0])) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->arguments[1] = ir_operand_int(arg1);
  out->arguments[2] = ir_operand_int(arg2);
  return 1;
}

int ir_make_simd_with_len(IRInstruction *out, SourceLocation location,
                                 IROpcode op, const IROperand *dest,
                                 const char *lhs_symbol, const char *rhs_symbol,
                                 const IROperand *len_operand) {
  if (!out || !dest || !lhs_symbol || !rhs_symbol || !len_operand) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->op = op;
  out->location = location;
  if (!ir_operand_clone(dest, &out->dest)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->lhs = ir_operand_symbol(lhs_symbol);
  out->rhs = ir_operand_symbol(rhs_symbol);
  out->arguments = calloc(1, sizeof(IROperand));
  if (!out->arguments) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->argument_count = 1;
  if (!ir_operand_clone(len_operand, &out->arguments[0])) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  return 1;
}

static int ir_try_parse_any_direct_unit_increment(const IRInstruction *ins,
                                                  const char **symbol_out) {
  if (!ins || !symbol_out || ins->op != IR_OP_BINARY || !ins->text ||
      strcmp(ins->text, "+") != 0 || ins->dest.kind != IR_OPERAND_SYMBOL ||
      !ins->dest.name || !ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) ||
      !ir_operand_is_int_value(&ins->rhs, 1)) {
    return 0;
  }
  *symbol_out = ins->dest.name;
  return 1;
}

static int ir_operand_same_symbol(const IROperand *a, const IROperand *b) {
  return a && b && a->kind == IR_OPERAND_SYMBOL && b->kind == IR_OPERAND_SYMBOL &&
         a->name && b->name && strcmp(a->name, b->name) == 0;
}

static int ir_resolve_reverse_i32_index_base(const IRFunction *function,
                                             size_t before, const char *iv,
                                             const IROperand *len,
                                             const char *addr_temp,
                                             const char **base_out) {
  const IRInstruction *addr = NULL;
  const IRInstruction *scale = NULL;
  const IRInstruction *sub_index = NULL;
  const IRInstruction *sub_last = NULL;

  if (!function || !iv || !len || !addr_temp || !base_out) {
    return 0;
  }

  addr = ir_find_temp_producer_before(function, before, addr_temp);
  if (!addr || addr->op != IR_OP_BINARY || !addr->text ||
      strcmp(addr->text, "+") != 0 ||
      addr->lhs.kind != IR_OPERAND_SYMBOL || !addr->lhs.name ||
      addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
    return 0;
  }

  scale = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!scale || scale->op != IR_OP_BINARY || !scale->text ||
      strcmp(scale->text, "<<") != 0 ||
      !ir_operand_is_int_value(&scale->rhs, 2) ||
      scale->lhs.kind != IR_OPERAND_TEMP || !scale->lhs.name) {
    return 0;
  }

  sub_index = ir_find_temp_producer_before(function, before, scale->lhs.name);
  if (!sub_index || sub_index->op != IR_OP_BINARY || !sub_index->text ||
      strcmp(sub_index->text, "-") != 0 ||
      sub_index->lhs.kind != IR_OPERAND_TEMP || !sub_index->lhs.name ||
      !ir_operand_is_symbol_named(&sub_index->rhs, iv)) {
    return 0;
  }

  sub_last = ir_find_temp_producer_before(function, before, sub_index->lhs.name);
  if (!sub_last || sub_last->op != IR_OP_BINARY || !sub_last->text ||
      strcmp(sub_last->text, "-") != 0 ||
      !ir_operand_is_int_value(&sub_last->rhs, 1)) {
    return 0;
  }
  if (!ir_operand_same_symbol(&sub_last->lhs, len)) {
    return 0;
  }

  *base_out = addr->lhs.name;
  return 1;
}

static int ir_try_vectorize_simd_scale_i32_at(IRFunction *function,
                                              size_t header_index,
                                              int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *src_p = NULL;
  const char *dst_p = NULL;
  const char *src_base = NULL;
  const char *dst_base = NULL;
  const char *sum_symbol = NULL;
  IROperand len = {0};
  IRInstruction fused = {0};
  int mul_val = 0;
  int add_val = 0;
  int have_mul = 0;
  int have_add = 0;

  if (!function || !ir_find_while_loop_bounds(function, header_index, &bounds)) {
    return 1;
  }
  if (ir_loop_body_is_unclaimable(function, bounds.branch_index + 1,
                                    bounds.jump_index)) {
    return 1;
  }

  compare = &function->instructions[bounds.compare_index];
  /* Pointer induction names its walkers `__ptr_<hdr>_<base>_p` against
   * `__ptr_<hdr>_<base>_end`. The base tag carries the source's parameter
   * NAME, so matching on `_src_p` only fired when the parameter was literally
   * called `src`; every semantic property (loads only through src_p, stores
   * only through dst_p, both stepped by 4, resolvable init bases) is checked
   * below, so the names only need to prove induction created the pair. */
  if (!ir_symbol_contains(compare->lhs.name, "__ptr_") ||
      !ir_symbol_contains(compare->rhs.name, "__ptr_") ||
      !ir_symbol_contains(compare->rhs.name, "_end")) {
    return 1;
  }
  src_p = compare->lhs.name;

  /* Exactly two pointers step by 4 in the body: the source, and the walker
   * that must be the destination. */
  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ir_operand_is_int_value(&ins->rhs, 4) &&
        ir_symbol_contains(ins->dest.name, "__ptr_")) {
      if (strcmp(ins->dest.name, src_p) == 0) {
        continue;
      }
      if (dst_p && strcmp(dst_p, ins->dest.name) != 0) {
        return 1;
      }
      dst_p = ins->dest.name;
    }
  }
  if (!dst_p) {
    return 1;
  }
  {
    const char *stepped_src =
        ir_find_ptr_step_with_suffix(function, bounds.branch_index + 1,
                                     bounds.jump_index, 4, src_p);
    if (!stepped_src || strcmp(stepped_src, src_p) != 0) {
      return 1;
    }
  }

  src_base = ir_find_ptr_init_base(function, bounds.compare_index, src_p);
  dst_base = ir_find_ptr_init_base(function, bounds.compare_index, dst_p);
  if (!src_base || !dst_base ||
      !ir_symbol_is_i32_ptr_param(function, src_base) ||
      !ir_symbol_is_i32_ptr_param(function, dst_base)) {
    return 1;
  }
  if (!ir_find_ptr_loop_len_operand(function, bounds.compare_index,
                                    compare->rhs.name, src_base, &len)) {
    return 1;
  }

  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_ASSIGN) {
      continue;
    }
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT) {
      return 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "*") == 0 &&
        !ins->is_float && ins->rhs.kind == IR_OPERAND_INT) {
      if (have_mul && mul_val != (int)ins->rhs.int_value) {
        return 1;
      }
      mul_val = (int)ins->rhs.int_value;
      have_mul = 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->rhs.kind == IR_OPERAND_INT &&
        ins->lhs.kind == IR_OPERAND_TEMP && ins->dest.kind == IR_OPERAND_TEMP) {
      if (have_add && add_val != (int)ins->rhs.int_value) {
        return 1;
      }
      add_val = (int)ins->rhs.int_value;
      have_add = 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name) {
      const IRInstruction *cast =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      if (cast && cast->op == IR_OP_CAST && cast->text &&
          strcmp(cast->text, "int64") == 0) {
        sum_symbol = ins->dest.name;
      }
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        !ir_operand_is_symbol_named(&ins->lhs, src_p)) {
      return 1;
    }
    if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_SYMBOL &&
        !ir_operand_is_symbol_named(&ins->dest, dst_p)) {
      return 1;
    }
  }

  if (!have_mul || !have_add || !sum_symbol) {
    return 1;
  }
  if (strcmp(sum_symbol, src_p) == 0 || strcmp(sum_symbol, dst_p) == 0) {
    return 1;
  }
  {
    const char *sum_type = ir_function_local_declared_type(function, sum_symbol);
    if (!sum_type || strcmp(sum_type, "int64") != 0) {
      return 1;
    }
  }

  {
    IROperand dest = ir_operand_symbol(sum_symbol);
    if (!ir_make_simd_with_len_and_two_ints(
            &fused, function->instructions[header_index].location,
            IR_OP_SIMD_SCALE_I32, &dest, src_base,
            dst_base, &len, mul_val, add_val)) {
      return 0;
    }
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

static int ir_try_vectorize_simd_reverse_copy_i32_at(IRFunction *function,
                                                     size_t header_index,
                                                     int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *src_p = NULL;
  const char *dst_p = NULL;
  const char *src_base = NULL;
  const char *dst_base = NULL;
  const char *sum_symbol = NULL;
  IROperand len = {0};
  IRInstruction fused = {0};

  if (!function || !ir_find_while_loop_bounds(function, header_index, &bounds)) {
    return 1;
  }
  if (ir_loop_body_is_unclaimable(function, bounds.branch_index + 1,
                                    bounds.jump_index)) {
    return 1;
  }

  compare = &function->instructions[bounds.compare_index];
  if (!ir_symbol_contains(compare->lhs.name, "_dst_p") ||
      !ir_symbol_contains(compare->rhs.name, "_dst_end")) {
    return 1;
  }
  dst_p = compare->lhs.name;

  src_p = ir_find_ptr_step_symbol(function, bounds.branch_index + 1,
                                  bounds.jump_index, -4);
  if (!src_p || !ir_symbol_contains(src_p, "_src_p")) {
    const char *iv_symbol = NULL;
    const char *loaded_temp = NULL;
    int found_store = 0;

    dst_base = ir_find_ptr_init_base(function, bounds.compare_index, dst_p);
    if (!dst_base || !ir_symbol_is_i32_ptr_param(function, dst_base)) {
      return 1;
    }
    if (!ir_find_ptr_loop_len_operand(function, bounds.compare_index,
                                      compare->rhs.name, dst_base, &len)) {
      return 1;
    }

    for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
      const IRInstruction *ins = &function->instructions[i];
      const char *candidate_iv = NULL;
      if (ir_try_parse_any_direct_unit_increment(ins, &candidate_iv)) {
        iv_symbol = candidate_iv;
        break;
      }
    }
    if (!iv_symbol) {
      return 1;
    }
    /* The kernel reads src[len-1-iv] for iv = 0..len-1: the counter must
     * provably start at 0 or the reversed indexes are shifted. */
    if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
      return 1;
    }

    for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
          ins->op == IR_OP_ASSIGN || ins->op == IR_OP_LABEL ||
          ins->op == IR_OP_JUMP) {
        continue;
      }
      if (ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT ||
          ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
        return 1;
      }
      if (ins->op == IR_OP_LOAD && ins->rhs.kind == IR_OPERAND_INT &&
          ins->rhs.int_value == 4 && ins->lhs.kind == IR_OPERAND_TEMP &&
          ins->lhs.name) {
        const char *base = NULL;
        if (ir_resolve_reverse_i32_index_base(function, i, iv_symbol, &len,
                                              ins->lhs.name, &base)) {
          src_base = base;
          if (ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
            loaded_temp = ins->dest.name;
          }
          continue;
        }
      }
      if (ins->op == IR_OP_STORE && ins->rhs.kind == IR_OPERAND_INT &&
          ins->rhs.int_value == 4 &&
          ir_operand_is_symbol_named(&ins->dest, dst_p)) {
        found_store = 1;
        continue;
      }
      if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
          !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name && ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name) {
        const IRInstruction *cast =
            ir_find_temp_producer_before(function, i, ins->rhs.name);
        int cast_uses_loaded_value =
            !loaded_temp || ir_operand_is_temp_named(&cast->lhs, loaded_temp);
        if (!cast_uses_loaded_value && cast && cast->lhs.kind == IR_OPERAND_SYMBOL &&
            cast->lhs.name && loaded_temp) {
          for (size_t a = bounds.branch_index + 1; a < i; a++) {
            const IRInstruction *assign = &function->instructions[a];
            if (assign->op == IR_OP_ASSIGN &&
                ir_operand_is_symbol_named(&assign->dest, cast->lhs.name) &&
                ir_operand_is_temp_named(&assign->lhs, loaded_temp)) {
              cast_uses_loaded_value = 1;
              break;
            }
          }
        }
        if (ir_cast_is_to_int64(cast) && cast_uses_loaded_value) {
          sum_symbol = ins->dest.name;
          continue;
        }
      }
    }

    if (!src_base || !found_store || !sum_symbol ||
        !ir_symbol_is_i32_ptr_param(function, src_base)) {
      return 1;
    }
    {
      const char *sum_type = ir_function_local_declared_type(function, sum_symbol);
      if (!sum_type || strcmp(sum_type, "int64") != 0) {
        return 1;
      }
    }
    {
      IROperand dest = ir_operand_symbol(sum_symbol);
      if (!ir_make_simd_with_len(
              &fused, function->instructions[header_index].location,
              IR_OP_SIMD_REVERSE_COPY_I32, &dest, src_base,
              dst_base, &len)) {
        return 0;
      }
    }
    return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                      &fused, changed);
  }
  if (!ir_find_ptr_step_symbol(function, bounds.branch_index + 1,
                               bounds.jump_index, 4) ||
      strcmp(ir_find_ptr_step_symbol(function, bounds.branch_index + 1,
                                     bounds.jump_index, 4),
             dst_p) != 0) {
    return 1;
  }

  src_base = ir_find_ptr_init_base(function, bounds.compare_index, src_p);
  dst_base = ir_find_ptr_init_base(function, bounds.compare_index, dst_p);
  if (!src_base || !dst_base ||
      !ir_symbol_is_i32_ptr_param(function, src_base) ||
      !ir_symbol_is_i32_ptr_param(function, dst_base)) {
    return 1;
  }
  if (!ir_find_ptr_loop_len_operand(function, bounds.compare_index,
                                    compare->rhs.name, dst_base, &len)) {
    return 1;
  }

  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_ASSIGN) {
      continue;
    }
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT) {
      return 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "*") == 0) {
      return 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->rhs.kind == IR_OPERAND_TEMP) {
      const IRInstruction *cast =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      if (cast && cast->op == IR_OP_CAST && cast->text &&
          strcmp(cast->text, "int64") == 0) {
        sum_symbol = ins->dest.name;
      }
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        !ir_operand_is_symbol_named(&ins->lhs, src_p)) {
      return 1;
    }
    if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_SYMBOL &&
        !ir_operand_is_symbol_named(&ins->dest, dst_p)) {
      return 1;
    }
  }

  if (!sum_symbol) {
    return 1;
  }
  {
    const char *sum_type = ir_function_local_declared_type(function, sum_symbol);
    if (!sum_type || strcmp(sum_type, "int64") != 0) {
      return 1;
    }
  }

  {
    IROperand dest = ir_operand_symbol(sum_symbol);
    if (!ir_make_simd_with_len(
            &fused, function->instructions[header_index].location,
          IR_OP_SIMD_REVERSE_COPY_I32, &dest, src_base,
          dst_base, &len)) {
      return 0;
    }
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

static int ir_resolve_i32_index_base(const IRFunction *function, size_t before,
                                     const char *iv, const char *addr_temp,
                                     const char **base_out) {
  const IRInstruction *addr = NULL;
  const IRInstruction *index = NULL;

  if (!function || !iv || !addr_temp || !base_out) {
    return 0;
  }

  addr = ir_find_temp_producer_before(function, before, addr_temp);
  if (!addr || addr->op != IR_OP_BINARY || !addr->text ||
      strcmp(addr->text, "+") != 0 ||
      addr->lhs.kind != IR_OPERAND_SYMBOL || !addr->lhs.name ||
      addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
    return 0;
  }

  index = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!index || !ir_match_forward_i32_index(index, iv)) {
    return 0;
  }

  *base_out = addr->lhs.name;
  return 1;
}

static int ir_try_vectorize_simd_clamp_i32_at(IRFunction *function,
                                              size_t header_index,
                                              int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *iv_symbol = NULL;
  const char *src_base = NULL;
  const char *dst_base = NULL;
  const char *sum_symbol = NULL;
  const char *value_temp = NULL;
  const char *result_temp = NULL;
  long long lo = 0;
  long long hi = 0;
  int have_lo = 0;
  int have_hi = 0;
  int have_cmp_lo = 0;
  int have_cmp_hi = 0;
  int got_lo_assign = 0;
  int got_hi_assign = 0;
  int got_id_assign = 0;
  IRInstruction fused = {0};
  size_t increment_index = 0;

  if (!function || !ir_find_while_loop_bounds(function, header_index, &bounds)) {
    return 1;
  }
  if (ir_loop_body_is_unclaimable(function, bounds.branch_index + 1,
                                    bounds.jump_index)) {
    return 1;
  }

  compare = &function->instructions[bounds.compare_index];
  iv_symbol = compare->lhs.name;
  if (!ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
    return 1;
  }

  increment_index = bounds.jump_index;
  while (increment_index > bounds.branch_index + 1) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(
          &function->instructions[increment_index], iv_symbol)) {
    return 1;
  }
  /* The kernel clamps src[0..len) into dst[0..len): the loop must provably
   * start at iv == 0. */
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
    return 1;
  }

  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP) {
      continue;
    }
    if (ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT) {
      return 1;
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->lhs.kind == IR_OPERAND_INT) {
      if (ir_param_name_is_lo(ins->dest.name)) {
        lo = ins->lhs.int_value;
        have_lo = 1;
      } else if (ir_param_name_is_hi(ins->dest.name)) {
        hi = ins->lhs.int_value;
        have_hi = 1;
      }
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ir_param_name_is_v(ins->dest.name) &&
        ins->lhs.kind == IR_OPERAND_TEMP && ins->lhs.name) {
      value_temp = ins->lhs.name;
    }
    if (ins->op == IR_OP_LOAD && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 4 && ins->lhs.kind == IR_OPERAND_TEMP &&
        ins->lhs.name) {
      const char *base = NULL;
      if (ir_resolve_i32_index_base(function, i, iv_symbol, ins->lhs.name,
                                    &base)) {
        src_base = base;
      }
    }
    if (ins->op == IR_OP_STORE && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 4 && ins->dest.kind == IR_OPERAND_TEMP &&
        ins->dest.name) {
      const char *base = NULL;
      if (ir_resolve_i32_index_base(function, i, iv_symbol, ins->dest.name,
                                    &base)) {
        dst_base = base;
        if (ins->lhs.kind == IR_OPERAND_TEMP && ins->lhs.name) {
          result_temp = ins->lhs.name;
        }
      }
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name) {
      const IRInstruction *cast =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      if (ir_cast_is_to_int64(cast)) {
        sum_symbol = ins->dest.name;
      }
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "<") == 0 &&
        !ins->is_float && ins->rhs.kind == IR_OPERAND_SYMBOL &&
        ir_param_name_is_lo(ins->rhs.name)) {
      have_cmp_lo = 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, ">") == 0 &&
        !ins->is_float && ins->rhs.kind == IR_OPERAND_SYMBOL &&
        ir_param_name_is_hi(ins->rhs.name)) {
      have_cmp_hi = 1;
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_TEMP &&
        ins->dest.name) {
      if (ins->lhs.kind == IR_OPERAND_INT && have_lo &&
          ins->lhs.int_value == lo) {
        got_lo_assign = 1;
        result_temp = ins->dest.name;
      } else if (ins->lhs.kind == IR_OPERAND_SYMBOL &&
                 ir_param_name_is_hi(ins->lhs.name)) {
        got_hi_assign = 1;
        result_temp = ins->dest.name;
      } else if ((ins->lhs.kind == IR_OPERAND_SYMBOL &&
                  ir_param_name_is_v(ins->lhs.name)) ||
                 (value_temp && ir_operand_is_temp_named(&ins->lhs, value_temp))) {
        got_id_assign = 1;
        result_temp = ins->dest.name;
      }
    }
  }

  if (!have_lo || !have_hi || hi <= lo || !have_cmp_lo || !have_cmp_hi ||
      !got_lo_assign || !got_hi_assign || !got_id_assign || !value_temp ||
      !result_temp || !src_base || !dst_base || !sum_symbol) {
    return 1;
  }
  if (!ir_symbol_is_i32_ptr_param(function, src_base) ||
      !ir_symbol_is_i32_ptr_param(function, dst_base)) {
    return 1;
  }
  {
    const char *sum_type = ir_function_local_declared_type(function, sum_symbol);
    if (!sum_type || strcmp(sum_type, "int64") != 0) {
      return 1;
    }
  }
  if (ir_symbol_read_after(function, bounds.jump_index + 1, iv_symbol)) {
    return 1;
  }

  {
    IROperand dest = ir_operand_symbol(sum_symbol);
    if (!ir_make_simd_with_len_and_two_ints(
            &fused, function->instructions[header_index].location,
            IR_OP_SIMD_CLAMP_I32, &dest, src_base,
            dst_base, &compare->rhs, (int)lo, (int)hi)) {
      return 0;
    }
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

static int ir_try_vectorize_simd_clamp_ptr_at(IRFunction *function,
                                              size_t header_index,
                                              int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *src_p = NULL;
  const char *dst_p = NULL;
  const char *src_base = NULL;
  const char *dst_base = NULL;
  const char *sum_symbol = NULL;
  const char *value_temp = NULL;
  const char *result_temp = NULL;
  long long lo = 0;
  long long hi = 0;
  int have_lo = 0;
  int have_hi = 0;
  int have_cmp_lo = 0;
  int have_cmp_hi = 0;
  int got_lo_assign = 0;
  int got_hi_assign = 0;
  int got_id_assign = 0;
  IROperand len = {0};
  IRInstruction fused = {0};

  if (!function || !ir_find_while_loop_bounds(function, header_index, &bounds)) {
    return 1;
  }
  if (ir_loop_body_is_unclaimable(function, bounds.branch_index + 1,
                                    bounds.jump_index)) {
    return 1;
  }

  compare = &function->instructions[bounds.compare_index];
  if (!ir_symbol_contains(compare->lhs.name, "_p") ||
      !ir_symbol_contains(compare->rhs.name, "_end")) {
    return 1;
  }
  src_p = compare->lhs.name;
  dst_p = ir_find_ptr_step_with_suffix(function, bounds.branch_index + 1,
                                       bounds.jump_index, 4, "_dst_p");
  if (!dst_p) {
    dst_p = ir_find_ptr_step_with_suffix(function, bounds.branch_index + 1,
                                         bounds.jump_index, 4, "_output");
  }
  if (!dst_p) {
    return 1;
  }

  src_base = ir_find_ptr_init_base(function, bounds.compare_index, src_p);
  dst_base = ir_find_ptr_init_base(function, bounds.compare_index, dst_p);
  if (!src_base || !dst_base ||
      !ir_symbol_is_i32_ptr_param(function, src_base) ||
      !ir_symbol_is_i32_ptr_param(function, dst_base)) {
    return 1;
  }
  /* No unanchored fallback here: a `<<2` of some bound param floating before
   * the loop proves nothing about (end - base)/4, which is the kernel's
   * actual trip count. */
  if (!ir_find_ptr_loop_len_operand(function, bounds.compare_index,
                                    compare->rhs.name, src_base, &len)) {
    return 1;
  }

  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_BRANCH_ZERO) {
      continue;
    }
    if (ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT) {
      return 1;
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->lhs.kind == IR_OPERAND_INT) {
      if (ir_param_name_is_lo(ins->dest.name)) {
        lo = ins->lhs.int_value;
        have_lo = 1;
      } else if (ir_param_name_is_hi(ins->dest.name)) {
        hi = ins->lhs.int_value;
        have_hi = 1;
      }
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ir_param_name_is_v(ins->dest.name) &&
        ins->lhs.kind == IR_OPERAND_TEMP && ins->lhs.name) {
      value_temp = ins->lhs.name;
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ir_operand_is_symbol_named(&ins->lhs, src_p)) {
      continue;
    }
    if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ir_operand_is_symbol_named(&ins->dest, dst_p)) {
      if (ins->lhs.kind == IR_OPERAND_TEMP && ins->lhs.name) {
        result_temp = ins->lhs.name;
      }
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name) {
      const IRInstruction *cast =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      if (ir_cast_is_to_int64(cast)) {
        sum_symbol = ins->dest.name;
      }
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "<") == 0 &&
        !ins->is_float && ins->rhs.kind == IR_OPERAND_SYMBOL &&
        ir_param_name_is_lo(ins->rhs.name)) {
      have_cmp_lo = 1;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, ">") == 0 &&
        !ins->is_float && ins->rhs.kind == IR_OPERAND_SYMBOL &&
        ir_param_name_is_hi(ins->rhs.name)) {
      have_cmp_hi = 1;
      continue;
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_TEMP &&
        ins->dest.name) {
      if (ins->lhs.kind == IR_OPERAND_INT && have_lo &&
          ins->lhs.int_value == lo) {
        got_lo_assign = 1;
        result_temp = ins->dest.name;
      } else if (ins->lhs.kind == IR_OPERAND_SYMBOL &&
                 ir_param_name_is_hi(ins->lhs.name)) {
        got_hi_assign = 1;
        result_temp = ins->dest.name;
      } else if ((ins->lhs.kind == IR_OPERAND_SYMBOL &&
                  ir_param_name_is_v(ins->lhs.name)) ||
                 (value_temp && ir_operand_is_temp_named(&ins->lhs, value_temp))) {
        got_id_assign = 1;
        result_temp = ins->dest.name;
      }
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        ins->dest.kind == IR_OPERAND_SYMBOL &&
        (ir_operand_is_symbol_named(&ins->dest, src_p) ||
         ir_operand_is_symbol_named(&ins->dest, dst_p)) &&
        ir_operand_is_int_value(&ins->rhs, 4)) {
      continue;
    }
    if (ins->op == IR_OP_CAST || ins->op == IR_OP_ASSIGN) {
      continue;
    }
    if (ins->op == IR_OP_BINARY) {
      continue;
    }
    return 1;
  }

  if (!have_lo || !have_hi || hi <= lo || !have_cmp_lo || !have_cmp_hi ||
      !got_lo_assign || !got_hi_assign || !got_id_assign || !value_temp ||
      !result_temp || !sum_symbol) {
    return 1;
  }
  {
    const char *sum_type = ir_function_local_declared_type(function, sum_symbol);
    if (!sum_type || strcmp(sum_type, "int64") != 0) {
      return 1;
    }
  }

  {
    IROperand dest = ir_operand_symbol(sum_symbol);
    if (!ir_make_simd_with_len_and_two_ints(
            &fused, function->instructions[header_index].location,
            IR_OP_SIMD_CLAMP_I32, &dest, src_base, dst_base, &len, (int)lo,
            (int)hi)) {
      return 0;
    }
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

int ir_simd_memory_map_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL ||
        !ir_label_is_while_header(function->instructions[i].text)) {
      continue;
    }
    if (!ir_try_vectorize_simd_scale_i32_at(function, i, changed) ||
        !ir_try_vectorize_simd_reverse_copy_i32_at(function, i, changed) ||
        !ir_try_vectorize_simd_clamp_ptr_at(function, i, changed) ||
        !ir_try_vectorize_simd_clamp_i32_at(function, i, changed)) {
      return 0;
    }
  }
  return 1;
}

