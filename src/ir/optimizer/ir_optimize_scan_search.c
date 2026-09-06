#include "ir_optimize_internal.h"

static int ir_make_simd_minmax_i32(IRInstruction *out, SourceLocation location,
                                   const char *minv_symbol,
                                   const char *maxv_symbol,
                                   const char *arr_symbol,
                                   const IROperand *len_operand) {
  if (!out || !minv_symbol || !maxv_symbol || !arr_symbol || !len_operand) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->op = IR_OP_SIMD_MINMAX_I32;
  out->location = location;
  out->dest = ir_operand_symbol(minv_symbol);
  out->lhs = ir_operand_symbol(arr_symbol);
  if (!ir_operand_clone(len_operand, &out->rhs)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->arguments = calloc(1, sizeof(IROperand));
  if (!out->arguments) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->argument_count = 1;
  out->arguments[0] = ir_operand_symbol(maxv_symbol);
  if (!out->arguments[0].name) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  return 1;
}

static int ir_make_lower_bound_i32(IRInstruction *out, SourceLocation location,
                                   const char *lo_symbol,
                                   const char *arr_symbol,
                                   const IROperand *n_operand,
                                   const IROperand *key_operand) {
  if (!out || !lo_symbol || !arr_symbol || !n_operand || !key_operand) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->op = IR_OP_LOWER_BOUND_I32;
  out->location = location;
  out->dest = ir_operand_symbol(lo_symbol);
  out->lhs = ir_operand_symbol(arr_symbol);
  if (!out->dest.name || !out->lhs.name || !ir_operand_clone(n_operand, &out->rhs)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->arguments = calloc(1, sizeof(IROperand));
  if (!out->arguments) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->argument_count = 1;
  if (!ir_operand_clone(key_operand, &out->arguments[0])) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  return 1;
}

static int ir_try_fuse_lower_bound_i32_at(IRFunction *function,
                                          size_t header_index, int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  size_t exit_label_index = (size_t)-1;
  const char *lo_symbol = NULL;
  const char *hi_symbol = NULL;
  const char *mid_symbol = NULL;
  const char *arr_symbol = NULL;
  const char *delta_temp = NULL;
  const char *half_temp = NULL;
  const char *scaled_temp = NULL;
  const char *addr_temp = NULL;
  const char *loaded_temp = NULL;
  const char *cmp_temp = NULL;
  const char *false_label = NULL;
  IROperand key_operand = {0};
  IRInstruction fused = {0};
  int saw_lo_update = 0;
  int saw_hi_update = 0;

  if (!function || !ir_find_while_loop_bounds(function, header_index, &bounds)) {
    return 1;
  }
  if (!ir_find_label_index(function, bounds.exit_label, &exit_label_index) ||
      exit_label_index <= bounds.branch_index) {
    return 1;
  }
  if (ir_loop_body_is_unclaimable(function, bounds.branch_index + 1,
                                    exit_label_index)) {
    return 1;
  }

  compare = &function->instructions[bounds.compare_index];
  if (!compare->text || strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name) {
    return 1;
  }
  lo_symbol = compare->lhs.name;
  hi_symbol = compare->rhs.name;

  for (size_t i = bounds.branch_index + 1; i < exit_label_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_LABEL) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "-") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
        ir_operand_is_symbol_named(&ins->lhs, hi_symbol) &&
        ir_operand_is_symbol_named(&ins->rhs, lo_symbol)) {
      delta_temp = ins->dest.name;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && !ins->is_float &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name && delta_temp &&
        ir_operand_is_temp_named(&ins->lhs, delta_temp) &&
        ((strcmp(ins->text, "/") == 0 && ir_operand_is_int_value(&ins->rhs, 2)) ||
         (strcmp(ins->text, ">>") == 0 && ir_operand_is_int_value(&ins->rhs, 1)))) {
      half_temp = ins->dest.name;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        half_temp && ir_operand_is_symbol_named(&ins->lhs, lo_symbol) &&
        ir_operand_is_temp_named(&ins->rhs, half_temp)) {
      mid_symbol = ins->dest.name;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && !ins->is_float &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name && mid_symbol &&
        ir_operand_is_symbol_named(&ins->lhs, mid_symbol) &&
        ((strcmp(ins->text, "<<") == 0 && ir_operand_is_int_value(&ins->rhs, 2)) ||
         (strcmp(ins->text, "*") == 0 && ir_operand_is_int_value(&ins->rhs, 4)))) {
      scaled_temp = ins->dest.name;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
        scaled_temp && ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
        ir_operand_is_temp_named(&ins->rhs, scaled_temp)) {
      arr_symbol = ins->lhs.name;
      addr_temp = ins->dest.name;
      continue;
    }
    if (ins->op == IR_OP_LOAD && addr_temp &&
        ir_operand_is_temp_named(&ins->lhs, addr_temp) &&
        ir_operand_is_int_value(&ins->rhs, 4) &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
      loaded_temp = ins->dest.name;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "<") == 0 &&
        !ins->is_float && loaded_temp &&
        ir_operand_is_temp_named(&ins->lhs, loaded_temp) &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
      cmp_temp = ins->dest.name;
      key_operand = ins->rhs;
      continue;
    }
    if (ins->op == IR_OP_BRANCH_ZERO && cmp_temp &&
        ir_operand_is_temp_named(&ins->lhs, cmp_temp) && ins->text) {
      false_label = ins->text;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && mid_symbol &&
        ir_operand_is_symbol_named(&ins->dest, lo_symbol) &&
        ir_operand_is_symbol_named(&ins->lhs, mid_symbol) &&
        ir_operand_is_int_value(&ins->rhs, 1)) {
      saw_lo_update = 1;
      continue;
    }
    if (ins->op == IR_OP_ASSIGN && mid_symbol &&
        ir_operand_is_symbol_named(&ins->dest, hi_symbol) &&
        ir_operand_is_symbol_named(&ins->lhs, mid_symbol)) {
      saw_hi_update = 1;
      continue;
    }
    if (ins->op == IR_OP_CAST) {
      continue;
    }
    return 1;
  }

  if (!delta_temp || !half_temp || !mid_symbol || !arr_symbol || !addr_temp ||
      !loaded_temp || !cmp_temp || !false_label || !saw_lo_update ||
      !saw_hi_update || key_operand.kind == IR_OPERAND_NONE) {
    return 1;
  }
  if (!ir_make_lower_bound_i32(&fused, function->instructions[header_index].location,
                               lo_symbol, arr_symbol, &compare->rhs,
                               &key_operand)) {
    return 0;
  }
  return ir_fuse_while_loop_to_insn(function, header_index, exit_label_index - 1,
                                    &fused, changed);
}

int ir_lower_bound_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_fuse_lower_bound_i32_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

static int ir_verify_minmax_preloop_init(const IRFunction *function,
                                         size_t header_index, const char *iv,
                                         const char **arr_base_out,
                                         const char **minv_out,
                                         const char **maxv_out) {
  const char *arr_base = NULL;
  const char *minv = NULL;
  const char *maxv = NULL;
  int saw_i = 0;

  if (!function || !iv || !arr_base_out || !minv_out || !maxv_out) {
    return 0;
  }

  for (size_t i = header_index; i > 0;) {
    i--;
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      break;
    }
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ir_instruction_is_safety_scaffolding(ins)) {
      continue;
    }
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, iv) &&
        ir_operand_is_int_value(&ins->lhs, 1)) {
      saw_i = 1;
      continue;
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_symbol_contains(ins->dest.name, "_param_")) {
      continue;
    }
    if (ins->op == IR_OP_LOAD && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ins->lhs.name && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 4) {
      if (!maxv) {
        maxv = ins->dest.name;
        arr_base = ins->lhs.name;
      } else if (!minv && strcmp(ins->dest.name, maxv) != 0) {
        minv = ins->dest.name;
      } else if (!minv) {
        minv = ins->dest.name;
      }
      if (ins->lhs.name) {
        if (!arr_base) {
          arr_base = ins->lhs.name;
        } else if (strcmp(arr_base, ins->lhs.name) != 0) {
          return 0;
        }
      }
      continue;
    }
    /* A pure write to a temp cannot disturb the seeds, the base, or the
     * counter. Inlining leaves the argument's address math here (`%t <-
     * &@arr` ahead of the parameter copy), which used to fail the walk. */
    if (ins->dest.kind == IR_OPERAND_TEMP &&
        (ins->op == IR_OP_ASSIGN || ins->op == IR_OP_ADDRESS_OF ||
         ins->op == IR_OP_BINARY || ins->op == IR_OP_UNARY ||
         ins->op == IR_OP_CAST)) {
      continue;
    }
    if (ir_instruction_writes_destination(ins) &&
        !ir_operand_is_symbol_named(&ins->dest, iv)) {
      return 0;
    }
  }

  if (!saw_i || !arr_base || !minv || !maxv) {
    return 0;
  }
  *arr_base_out = arr_base;
  *minv_out = minv;
  *maxv_out = maxv;
  return 1;
}

static int ir_body_is_minmax_scan_loop(const IRFunction *function,
                                       size_t branch_index, size_t jump_index,
                                       const char *iv, const char *arr_base,
                                       const char *minv, const char *maxv) {
  int saw_load = 0;
  int saw_min_cmp = 0;
  int saw_max_cmp = 0;

  if (!function || !iv || !arr_base || !minv || !maxv) {
    return 0;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_ASSIGN) {
      continue;
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_TEMP &&
        ins->lhs.name) {
      const char *base = NULL;
      int elem_size = 0;
      int step = 0;
      if (!ir_resolve_indexed_address_temp(function, i, iv, NULL,
                                           ins->lhs.name, &base, &elem_size,
                                           &step)) {
        return 0;
      }
      if (strcmp(base, arr_base) != 0) {
        int alias_match = 0;
        for (size_t a = 0; a < branch_index; a++) {
          const IRInstruction *alias = &function->instructions[a];
          if (alias->op == IR_OP_ASSIGN &&
              ir_operand_is_symbol_named(&alias->dest, base) &&
              ir_operand_is_symbol_named(&alias->lhs, arr_base)) {
            alias_match = 1;
            break;
          }
        }
        if (!alias_match) {
          return 0;
        }
      }
      if (elem_size != 4 || step != 4) {
        return 0;
      }
      saw_load = 1;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && !ins->is_float &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
      if (strcmp(ins->text, "<") == 0 &&
          ((ins->lhs.kind == IR_OPERAND_SYMBOL &&
            (ir_operand_is_symbol_named(&ins->lhs, minv) ||
             ir_symbol_contains(ins->lhs.name, "local_v") ||
             ir_symbol_contains(ins->lhs.name, "_param_v"))) ||
           (ins->rhs.kind == IR_OPERAND_SYMBOL &&
            ir_operand_is_symbol_named(&ins->rhs, minv)))) {
        saw_min_cmp = 1;
        continue;
      }
      if (strcmp(ins->text, ">") == 0 &&
          ((ins->lhs.kind == IR_OPERAND_SYMBOL &&
            (ir_operand_is_symbol_named(&ins->lhs, maxv) ||
             ir_symbol_contains(ins->lhs.name, "local_v") ||
             ir_symbol_contains(ins->lhs.name, "_param_v"))) ||
           (ins->rhs.kind == IR_OPERAND_SYMBOL &&
            ir_operand_is_symbol_named(&ins->rhs, maxv)))) {
        saw_max_cmp = 1;
        continue;
      }
      if (ir_binary_is_unit_increment_of_iv(ins, iv)) {
        continue;
      }
    }
    if (ins->op == IR_OP_BINARY && ins->text && !ins->is_float &&
        (strcmp(ins->text, "<<") == 0 || strcmp(ins->text, "+") == 0)) {
      continue;
    }
    if (ins->op == IR_OP_CAST) {
      continue;
    }
    return 0;
  }

  return saw_load && saw_min_cmp && saw_max_cmp;
}

static int ir_try_vectorize_simd_minmax_i32_at(IRFunction *function,
                                               size_t header_index,
                                               int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *iv_symbol = NULL;
  const char *arr_base = NULL;
  const char *minv_symbol = NULL;
  const char *maxv_symbol = NULL;
  long long iv_start = 0;
  IRInstruction fused = {0};

  {
    IRAffineLoop loop;
    if (!function || !ir_affine_model_loop(function, header_index, &loop) ||
        ir_affine_body_unclaimable(&loop)) {
      return 1;
    }
    bounds = loop.bounds;
  }

  compare = &function->instructions[bounds.compare_index];
  iv_symbol = compare->lhs.name;
  if (!iv_symbol || !compare->rhs.name ||
      !ir_symbol_is_loop_bound(function, compare->rhs.name, header_index,
                               bounds.jump_index)) {
    return 1;
  }
  if (!ir_ptr_induction_iv_start_value(function, header_index, iv_symbol,
                                       &iv_start) ||
      iv_start != 1) {
    return 1;
  }

  if (!ir_verify_minmax_preloop_init(function, header_index, iv_symbol,
                                     &arr_base, &minv_symbol, &maxv_symbol)) {
    return 1;
  }
  if (!ir_body_is_minmax_scan_loop(function, bounds.branch_index,
                                   bounds.jump_index, iv_symbol, arr_base,
                                   minv_symbol, maxv_symbol)) {
    return 1;
  }

  if (!ir_make_simd_minmax_i32(
          &fused, function->instructions[header_index].location, minv_symbol,
          maxv_symbol, arr_base, &compare->rhs)) {
    return 0;
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

static int ir_prefix_sum_symbol_is_body_local(const IRFunction *function,
                                              size_t header_index,
                                              size_t body_start,
                                              size_t body_end,
                                              const char *name) {
  if (!function || !name) {
    return 0;
  }
  for (size_t i = header_index; i < function->instruction_count; i++) {
    const IRInstruction *ins = NULL;
    if (i >= body_start && i < body_end) {
      continue;
    }
    ins = &function->instructions[i];
    if (ir_operand_is_symbol_named(&ins->lhs, name) ||
        ir_operand_is_symbol_named(&ins->rhs, name)) {
      return 0;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_symbol_named(&ins->arguments[a], name)) {
        return 0;
      }
    }
    if (ins->op == IR_OP_STORE &&
        ir_operand_is_symbol_named(&ins->dest, name)) {
      return 0;
    }
  }
  return 1;
}

static const char *ir_prefix_sum_stored_source(const IRFunction *function,
                                              size_t store_index,
                                              const IROperand *value) {
  if (!function || !value) {
    return NULL;
  }
  if (value->kind == IR_OPERAND_SYMBOL && value->name) {
    return value->name;
  }
  if (value->kind != IR_OPERAND_TEMP || !value->name) {
    return NULL;
  }
  for (size_t i = store_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->dest.kind != IR_OPERAND_TEMP || !ins->dest.name ||
        strcmp(ins->dest.name, value->name) != 0) {
      continue;
    }
    if ((ins->op != IR_OP_CAST && ins->op != IR_OP_ASSIGN) ||
        ins->lhs.kind != IR_OPERAND_SYMBOL || !ins->lhs.name) {
      return NULL;
    }
    return ins->lhs.name;
  }
  return NULL;
}

static const char *ir_prefix_sum_body_accumulator(const IRFunction *function,
                                                  size_t branch_index,
                                                  size_t jump_index) {
  const char *accumulator = NULL;
  int accumulated = 0;

  if (!function) {
    return NULL;
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const char *stored = NULL;
    if (ins->op != IR_OP_STORE) {
      continue;
    }
    stored = ir_prefix_sum_stored_source(function, i, &ins->lhs);
    if (!stored || (accumulator && strcmp(accumulator, stored) != 0)) {
      return NULL;
    }
    accumulator = stored;
  }
  if (!accumulator) {
    return NULL;
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ir_operand_is_symbol_named(&ins->dest, accumulator) &&
        (ir_operand_is_symbol_named(&ins->lhs, accumulator) ||
         ir_operand_is_symbol_named(&ins->rhs, accumulator))) {
      accumulated = 1;
      continue;
    }
    if (ir_operand_is_symbol_named(&ins->dest, accumulator)) {
      return NULL;
    }
  }
  return accumulated ? accumulator : NULL;
}

static int ir_prefix_sum_accumulator_is_zeroed(const IRFunction *function,
                                               size_t limit,
                                               const char *accumulator) {
  const char *type = NULL;
  int zeroed = 0;

  if (!function || !accumulator) {
    return 0;
  }
  type = ir_function_local_declared_type(function, accumulator);
  if (!type || strcmp(type, "int64") != 0) {
    return 0;
  }
  for (size_t i = 0; i < limit; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL || ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
        strcmp(ins->dest.name, accumulator) != 0) {
      continue;
    }
    if (ins->op != IR_OP_ASSIGN || !ir_operand_is_int_value(&ins->lhs, 0)) {
      return 0;
    }
    zeroed = 1;
  }
  return zeroed;
}

static int ir_prefix_sum_stores_accumulator(const IRFunction *function,
                                            size_t store_index,
                                            const IROperand *value,
                                            const char *accumulator) {
  if (!function || !value || !accumulator) {
    return 0;
  }
  if (ir_operand_is_symbol_named(value, accumulator)) {
    return 1;
  }
  if (value->kind != IR_OPERAND_TEMP || !value->name) {
    return 0;
  }
  for (size_t i = store_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->dest.kind != IR_OPERAND_TEMP || !ins->dest.name ||
        strcmp(ins->dest.name, value->name) != 0) {
      continue;
    }
    if (ins->op != IR_OP_CAST && ins->op != IR_OP_ASSIGN) {
      return 0;
    }
    return ir_operand_is_symbol_named(&ins->lhs, accumulator);
  }
  return 0;
}

static int ir_body_is_prefix_sum_loop(const IRFunction *function,
                                      size_t header_index, size_t branch_index,
                                      size_t jump_index, const char *iv,
                                      const char *src_base,
                                      const char *dst_base,
                                      const char *sum_symbol) {
  int saw_load = 0;
  int saw_store = 0;
  int saw_sum_add = 0;

  if (!function || !iv || !src_base || !dst_base || !sum_symbol) {
    return 0;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_TEMP &&
        ins->lhs.name) {
      const char *base = NULL;
      int elem_size = 0;
      int step = 0;
      if (!ir_resolve_indexed_address_temp(function, i, iv, NULL,
                                           ins->lhs.name, &base, &elem_size,
                                           &step)) {
        return 0;
      }
      if (strcmp(base, src_base) != 0 || elem_size != 4 || step != 4) {
        return 0;
      }
      saw_load = 1;
      continue;
    }
    if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_TEMP &&
        ins->dest.name) {
      const char *base = NULL;
      int elem_size = 0;
      int step = 0;
      if (!ir_resolve_indexed_address_temp(function, i, iv, NULL,
                                           ins->dest.name, &base, &elem_size,
                                           &step)) {
        return 0;
      }
      if (strcmp(base, dst_base) != 0 || elem_size != 4 || step != 4) {
        return 0;
      }
      if (!ir_prefix_sum_stores_accumulator(function, i, &ins->lhs,
                                            sum_symbol)) {
        return 0;
      }
      saw_store = 1;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float &&
        ir_operand_is_symbol_named(&ins->dest, sum_symbol)) {
      saw_sum_add = 1;
      continue;
    }
    if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, iv) != 0 &&
        !ir_prefix_sum_symbol_is_body_local(function, header_index,
                                            branch_index + 1, jump_index,
                                            ins->dest.name)) {
      return 0;
    }
    if (ins->op == IR_OP_CAST || ins->op == IR_OP_BINARY) {
      continue;
    }
    if (ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_LABEL) {
      return 0;
    }
    return 0;
  }

  return saw_load && saw_store && saw_sum_add;
}

static int ir_try_fuse_prefix_sum_i32_at(IRFunction *function,
                                         size_t header_index, int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *iv_symbol = NULL;
  const char *src_base = NULL;
  const char *dst_base = NULL;
  const char *sum_symbol = NULL;
  long long iv_start = 0;
  IRInstruction fused = {0};

  {
    IRAffineLoop loop;
    if (!function || !ir_affine_model_loop(function, header_index, &loop) ||
        ir_affine_body_unclaimable(&loop)) {
      return 1;
    }
    bounds = loop.bounds;
  }

  compare = &function->instructions[bounds.compare_index];
  iv_symbol = compare->lhs.name;
  if (!iv_symbol || !compare->rhs.name ||
      !ir_symbol_is_loop_bound(function, compare->rhs.name, header_index,
                               bounds.jump_index)) {
    return 1;
  }
  if (!ir_ptr_induction_iv_start_value(function, header_index, iv_symbol,
                                       &iv_start) ||
      iv_start != 0) {
    return 1;
  }

  for (size_t i = 0; i < header_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol) &&
        ir_operand_is_int_value(&ins->lhs, 0)) {
      break;
    }
  }

  sum_symbol = ir_prefix_sum_body_accumulator(function, bounds.branch_index,
                                             bounds.jump_index);
  if (!sum_symbol || strcmp(sum_symbol, iv_symbol) == 0 ||
      !ir_prefix_sum_accumulator_is_zeroed(function, header_index,
                                           sum_symbol)) {
    return 1;
  }

  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_TEMP &&
        ins->lhs.name) {
      if (!ir_resolve_indexed_address_temp(function, i, iv_symbol, NULL,
                                           ins->lhs.name, &src_base, NULL,
                                           NULL)) {
        return 1;
      }
      break;
    }
  }
  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_TEMP &&
        ins->dest.name) {
      if (!ir_resolve_indexed_address_temp(function, i, iv_symbol, NULL,
                                           ins->dest.name, &dst_base, NULL,
                                           NULL)) {
        return 1;
      }
      break;
    }
  }
  if (!src_base || !dst_base ||
      !ir_symbol_is_i32_ptr_param(function, src_base) ||
      !ir_symbol_is_i32_ptr_param(function, dst_base)) {
    return 1;
  }
  if (!ir_body_is_prefix_sum_loop(function, header_index, bounds.branch_index,
                                  bounds.jump_index, iv_symbol, src_base,
                                  dst_base, sum_symbol)) {
    return 1;
  }

  {
    IROperand dest = ir_operand_symbol(sum_symbol);
    if (!ir_make_simd_with_len(
            &fused, function->instructions[header_index].location,
            IR_OP_PREFIX_SUM_I32, &dest, src_base, dst_base, &compare->rhs)) {
      return 0;
    }
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

const char *ir_find_ptr_step_with_suffix(const IRFunction *function,
                                                size_t start, size_t end,
                                                long long step,
                                                const char *suffix) {
  size_t i = 0;
  if (!function || !suffix) {
    return NULL;
  }
  for (i = start; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ir_operand_is_int_value(&ins->rhs, step) &&
        ir_symbol_contains(ins->dest.name, suffix)) {
      return ins->dest.name;
    }
  }
  return NULL;
}

static int ir_body_is_minmax_ptr_loop(const IRFunction *function,
                                      size_t branch_index, size_t jump_index,
                                      const char *walk_ptr, const char *minv,
                                      const char *maxv) {
  int saw_load = 0;
  int saw_min_cmp = 0;
  int saw_max_cmp = 0;

  if (!function || !walk_ptr || !minv || !maxv) {
    return 0;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_ASSIGN) {
      continue;
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ir_operand_is_symbol_named(&ins->lhs, walk_ptr)) {
      saw_load = 1;
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && !ins->is_float &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
      if (strcmp(ins->text, "<") == 0 &&
          ins->rhs.kind == IR_OPERAND_SYMBOL &&
          ir_operand_is_symbol_named(&ins->rhs, minv)) {
        saw_min_cmp = 1;
        continue;
      }
      if (strcmp(ins->text, ">") == 0 &&
          ins->rhs.kind == IR_OPERAND_SYMBOL &&
          ir_operand_is_symbol_named(&ins->rhs, maxv)) {
        saw_max_cmp = 1;
        continue;
      }
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        ins->dest.kind == IR_OPERAND_SYMBOL &&
        ir_operand_is_symbol_named(&ins->dest, walk_ptr) &&
        ir_operand_is_int_value(&ins->rhs, 4)) {
      continue;
    }
    if (ins->op == IR_OP_CAST) {
      continue;
    }
    return 0;
  }

  return saw_load && saw_min_cmp && saw_max_cmp;
}

static int ir_try_vectorize_simd_minmax_ptr_at(IRFunction *function,
                                               size_t header_index,
                                               int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *walk_ptr = NULL;
  const char *arr_base = NULL;
  const char *minv_symbol = NULL;
  const char *maxv_symbol = NULL;
  IRInstruction fused = {0};

  {
    IRAffineLoop loop;
    if (!function || !ir_affine_model_loop(function, header_index, &loop) ||
        ir_affine_body_unclaimable(&loop)) {
      return 1;
    }
    bounds = loop.bounds;
  }

  compare = &function->instructions[bounds.compare_index];
  if (!ir_symbol_contains(compare->lhs.name, "__ptr_") ||
      !ir_symbol_contains(compare->rhs.name, "__ptr_") ||
      !ir_symbol_contains(compare->lhs.name, "_p") ||
      !ir_symbol_contains(compare->rhs.name, "_end")) {
    return 1;
  }
  walk_ptr = compare->lhs.name;
  arr_base = ir_find_ptr_init_base(function, bounds.compare_index, walk_ptr);
  if (!arr_base || !ir_symbol_is_i32_ptr_param(function, arr_base)) {
    return 1;
  }
  {
    IROperand len = {0};
    if (!ir_find_ptr_loop_len_operand(function, bounds.compare_index,
                                      compare->rhs.name, arr_base, &len)) {
      return 1;
    }
    if (!ir_verify_minmax_preloop_init(function, header_index, "@unused_iv",
                                       &arr_base, &minv_symbol, &maxv_symbol)) {
      arr_base = ir_find_ptr_init_base(function, bounds.compare_index, walk_ptr);
      if (!arr_base ||
          !ir_verify_minmax_preloop_init(function, header_index, "@unused_iv",
                                         &arr_base, &minv_symbol, &maxv_symbol)) {
        return 1;
      }
    }
    if (!ir_body_is_minmax_ptr_loop(function, bounds.branch_index,
                                    bounds.jump_index, walk_ptr, minv_symbol,
                                    maxv_symbol)) {
      return 1;
    }

    if (!ir_make_simd_minmax_i32(
            &fused, function->instructions[header_index].location, minv_symbol,
            maxv_symbol, arr_base, &len)) {
      return 0;
    }
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

static int ir_try_fuse_prefix_sum_ptr_at(IRFunction *function,
                                         size_t header_index, int *changed) {
  IRWhileLoopBounds bounds = {0};
  IRInstruction *compare = NULL;
  const char *src_p = NULL;
  const char *dst_p = NULL;
  const char *src_base = NULL;
  const char *dst_base = NULL;
  const char *sum_symbol = NULL;
  IROperand len = {0};
  IRInstruction fused = {0};

  {
    IRAffineLoop loop;
    if (!function || !ir_affine_model_loop(function, header_index, &loop) ||
        ir_affine_body_unclaimable(&loop)) {
      return 1;
    }
    bounds = loop.bounds;
  }

  compare = &function->instructions[bounds.compare_index];
  if (!ir_symbol_contains(compare->lhs.name, "_src_p") ||
      !ir_symbol_contains(compare->rhs.name, "_src_end")) {
    return 1;
  }
  src_p = compare->lhs.name;
  dst_p = ir_find_ptr_step_with_suffix(function, bounds.branch_index + 1,
                                       bounds.jump_index, 4, "_dst_p");
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

  sum_symbol = ir_prefix_sum_body_accumulator(function, bounds.branch_index,
                                             bounds.jump_index);
  if (!sum_symbol || !ir_prefix_sum_accumulator_is_zeroed(function,
                                                          header_index,
                                                          sum_symbol)) {
    return 1;
  }

  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        !ir_operand_is_symbol_named(&ins->lhs, src_p)) {
      return 1;
    }
    if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_SYMBOL &&
        !ir_operand_is_symbol_named(&ins->dest, dst_p)) {
      return 1;
    }
    if (ins->op == IR_OP_STORE &&
        !ir_prefix_sum_stores_accumulator(function, i, &ins->lhs,
                                          sum_symbol)) {
      return 1;
    }
    if (ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ir_operand_is_symbol_named(&ins->dest, sum_symbol)) {
      continue;
    }
    if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, src_p) != 0 &&
        strcmp(ins->dest.name, dst_p) != 0 &&
        !ir_prefix_sum_symbol_is_body_local(function, header_index,
                                            bounds.branch_index + 1,
                                            bounds.jump_index,
                                            ins->dest.name)) {
      return 1;
    }
    if (ins->op == IR_OP_CAST || ins->op == IR_OP_BINARY) {
      continue;
    }
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_JUMP) {
      return 1;
    }
    return 1;
  }

  {
    IROperand dest = ir_operand_symbol(sum_symbol);
    if (!ir_make_simd_with_len(
            &fused, function->instructions[header_index].location,
            IR_OP_PREFIX_SUM_I32, &dest, src_base, dst_base, &len)) {
      return 0;
    }
  }
  return ir_fuse_while_loop_to_insn(function, header_index, bounds.jump_index,
                                    &fused, changed);
}

int ir_simd_minmax_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (int iteration = 0; iteration < 16; iteration++) {
    int any_changed = 0;

    for (size_t i = 0; i < function->instruction_count; i++) {
      if (function->instructions[i].op != IR_OP_LABEL ||
          !ir_label_is_while_header(function->instructions[i].text)) {
        continue;
      }

      int local_changed = 0;
      if (!ir_try_vectorize_simd_minmax_ptr_at(function, i, &local_changed)) {
        return 0;
      }
      if (!local_changed &&
          !ir_try_vectorize_simd_minmax_i32_at(function, i, &local_changed)) {
        return 0;
      }
      if (local_changed) {
        any_changed = 1;
        if (changed) {
          *changed = 1;
        }
        break;
      }
    }

    if (!any_changed) {
      return 1;
    }
  }

  return 1;
}

int ir_prefix_sum_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (int iteration = 0; iteration < 16; iteration++) {
    int any_changed = 0;

    for (size_t i = 0; i < function->instruction_count; i++) {
      if (function->instructions[i].op != IR_OP_LABEL ||
          !ir_label_is_while_header(function->instructions[i].text)) {
        continue;
      }

      int local_changed = 0;
      if (!ir_try_fuse_prefix_sum_ptr_at(function, i, &local_changed)) {
        return 0;
      }
      if (!local_changed &&
          !ir_try_fuse_prefix_sum_i32_at(function, i, &local_changed)) {
        return 0;
      }
      if (local_changed) {
        any_changed = 1;
        if (changed) {
          *changed = 1;
        }
        break;
      }
    }

    if (!any_changed) {
      return 1;
    }
  }

  return 1;
}


typedef struct {
  IRInstruction *header;
  size_t branch_index;
  size_t jump_index;
  const char *iv_symbol;
  IROperand len;
} IRCountedLoop;

static int ir_match_counted_loop(IRFunction *function, size_t header_index,
                                 IRCountedLoop *loop) {
  IRInstruction *header;
  IRInstruction *compare;
  IRInstruction *branch;
  const char *loop_label;
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t jump_index = (size_t)-1;
  size_t increment_index;

  memset(loop, 0, sizeof(*loop));
  if (!function || header_index + 4 >= function->instruction_count) {
    return -1;
  }

  header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return -1;
  }
  loop_label = header->text;

  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return -1;
  }

  compare = &function->instructions[compare_index];
  branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return -1;
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
        strcmp(function->instructions[i].text, branch->text) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      !ir_symbol_is_loop_bound(function, compare->rhs.name, header_index,
                               jump_index)) {
    return -1;
  }
  /* A threaded exit would lose its edge when the loop is fused away. */
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, branch->text) ||
      ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return -1;
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
    return -1;
  }
  if (!ir_iv_zero_at_header(function, header_index, compare->lhs.name) ||
      ir_symbol_live_after_loop(function, jump_index + 1, compare->lhs.name)) {
    return -1;
  }

  if (!ir_operand_clone(&compare->rhs, &loop->len)) {
    return 0;
  }
  loop->header = header;
  loop->branch_index = branch_index;
  loop->jump_index = jump_index;
  loop->iv_symbol = compare->lhs.name;
  return 1;
}

static int ir_fuse_counted_loop(IRFunction *function, size_t header_index,
                                IRCountedLoop *loop, IRInstruction *fused,
                                int *changed) {
  fused->location = loop->header->location;
  fused->arguments = calloc(1, sizeof(IROperand));
  if (!fused->arguments) {
    ir_operand_destroy(&loop->len);
    ir_instruction_destroy_storage(fused);
    return 0;
  }
  fused->argument_count = 1;
  fused->arguments[0] = loop->len;

  ir_instruction_destroy_storage(loop->header);
  *loop->header = *fused;
  for (size_t i = header_index + 1; i <= loop->jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_dot_scan_body(const IRFunction *function,
                            const IRCountedLoop *loop, long long width,
                            int skip_casts, const char **a_symbol,
                            const char **b_symbol, int *a_unsigned,
                            int *b_unsigned, const char **sum_symbol) {
  int has_mul_add = 0;
  for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ins->op == IR_OP_ASSIGN ||
        (skip_casts && ins->op == IR_OP_CAST)) {
      continue;
    }
    if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_TEMP &&
        ins->lhs.name && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == width) {
      const char *base = NULL;
      if (ir_resolve_indexed_address_temp(function, i, loop->iv_symbol, NULL,
                                          ins->lhs.name, &base, NULL, NULL)) {
        if (!*a_symbol) {
          *a_symbol = base;
          *a_unsigned = ins->is_unsigned;
        } else if (!*b_symbol && strcmp(base, *a_symbol) != 0) {
          *b_symbol = base;
          *b_unsigned = ins->is_unsigned;
        }
      }
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name)) {
      const IRInstruction *mul =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      if (mul && mul->op == IR_OP_BINARY && mul->text &&
          strcmp(mul->text, "*") == 0 && !mul->is_float) {
        has_mul_add = 1;
        *sum_symbol = ins->dest.name;
      }
    }
  }
  return has_mul_add;
}

static int ir_dot_bases_are_arrays(const IRFunction *function,
                                   const char *sum_symbol,
                                   const char *sum_wanted,
                                   const char *a_symbol,
                                   const char *b_symbol) {
  const char *sum_type = ir_function_local_declared_type(function, sum_symbol);
  return sum_type && strcmp(sum_type, sum_wanted) == 0 &&
         ir_symbol_is_sum_array_base(function, a_symbol) &&
         ir_symbol_is_sum_array_base(function, b_symbol);
}

static int ir_try_vectorize_dot_i32_at(IRFunction *function, size_t header_index,
                                       int *changed) {
  IRCountedLoop loop;
  IRInstruction fused = {0};
  const char *sum_symbol = NULL;
  const char *a_symbol = NULL;
  const char *b_symbol = NULL;
  int a_unsigned = 0;
  int b_unsigned = 0;
  int matched = ir_match_counted_loop(function, header_index, &loop);

  if (matched <= 0) {
    return matched == 0 ? 0 : 1;
  }

  /* Require FOUR-BYTE loads: the kernel reads and strides int32 elements, so a
   * loop over byte arrays with an int64 accumulator matched here and was
   * replayed four bytes at a time over the wrong memory. The byte dot is a
   * separate recognizer, keyed on width 1. */
  if (!ir_dot_scan_body(function, &loop, 4, 0, &a_symbol, &b_symbol,
                        &a_unsigned, &b_unsigned, &sum_symbol) ||
      !sum_symbol || !a_symbol || !b_symbol ||
      !ir_dot_bases_are_arrays(function, sum_symbol, "int64", a_symbol,
                               b_symbol)) {
    ir_operand_destroy(&loop.len);
    return 1;
  }

  fused.op = IR_OP_SIMD_DOT_I32;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(a_symbol);
  fused.rhs = ir_operand_symbol(b_symbol);
  return ir_fuse_counted_loop(function, header_index, &loop, &fused, changed);
}

/* int8 x int8 -> int32 dot product: the quantized-GEMM inner loop
 *   sum(int32) += (int32)a[i] * (int32)b[i]
 * over a unit-stride counted loop, where a and b are int8 arrays. Recognized by
 * the same shape as the int32 dot but with BYTE loads (load width 1) and an
 * int32 accumulator; emits IR_OP_SIMD_DOT_I8. Matched by instruction pattern
 * (byte loads feeding a multiply-accumulate reduction), not by name. */
static int ir_try_vectorize_dot_i8_at(IRFunction *function, size_t header_index,
                                      int *changed) {
  IRCountedLoop loop;
  IRInstruction fused = {0};
  const char *sum_symbol = NULL;
  const char *a_symbol = NULL;
  const char *b_symbol = NULL;
  /* Whether the byte loads widen zero-extended. A uint8 array and an int8 array
   * reach here in the same shape, and the two dot products differ: the kernel
   * has to be told which widening the source asked for. Both sides have to
   * widen the same way; a mixed int8/uint8 dot is not a shape this kernel
   * has. */
  int a_unsigned = 0;
  int b_unsigned = 0;
  int matched = ir_match_counted_loop(function, header_index, &loop);

  if (matched <= 0) {
    return matched == 0 ? 0 : 1;
  }

  if (!ir_dot_scan_body(function, &loop, 1, 1, &a_symbol, &b_symbol,
                        &a_unsigned, &b_unsigned, &sum_symbol) ||
      !sum_symbol || !a_symbol || !b_symbol ||
      !ir_dot_bases_are_arrays(function, sum_symbol, "int32", a_symbol,
                               b_symbol) ||
      a_unsigned != b_unsigned) {
    ir_operand_destroy(&loop.len);
    return 1;
  }

  fused.op = IR_OP_SIMD_DOT_I8;
  fused.is_unsigned = a_unsigned;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(a_symbol);
  fused.rhs = ir_operand_symbol(b_symbol);
  return ir_fuse_counted_loop(function, header_index, &loop, &fused, changed);
}

static int ir_memcmp_byte_loop_is_indexed_load(const IRFunction *function,
                                               size_t load_index,
                                               const IRInstruction *load,
                                               const char *base_symbol,
                                               const char *iv_symbol) {
  const IRInstruction *addr = NULL;
  if (!function || !load || !base_symbol || !iv_symbol ||
      load->op != IR_OP_LOAD || load->rhs.kind != IR_OPERAND_INT ||
      load->rhs.int_value != 1 || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name) {
    return 0;
  }

  addr = ir_find_temp_producer_before(function, load_index, load->lhs.name);
  if (!addr || addr->op != IR_OP_BINARY || !addr->text ||
      strcmp(addr->text, "+") != 0) {
    return 0;
  }

  return (ir_operand_is_symbol_named(&addr->lhs, base_symbol) &&
          ir_operand_is_symbol_named(&addr->rhs, iv_symbol)) ||
         (ir_operand_is_symbol_named(&addr->rhs, base_symbol) &&
          ir_operand_is_symbol_named(&addr->lhs, iv_symbol));
}

static int ir_memcmp_byte_loop_value_symbol(const IRFunction *function,
                                             size_t before_index,
                                             const IROperand *operand,
                                             const char **out_symbol) {
  const IRInstruction *producer = NULL;
  if (!operand || !out_symbol) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    *out_symbol = operand->name;
    return 1;
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }
  producer = ir_find_temp_producer_before(function, before_index, operand->name);
  if (producer && producer->op == IR_OP_CAST &&
      producer->lhs.kind == IR_OPERAND_SYMBOL && producer->lhs.name) {
    *out_symbol = producer->lhs.name;
    return 1;
  }
  return 0;
}

static int ir_memcmp_byte_loop_tail_is_zero_return(const IRFunction *function,
                                                   size_t exit_label_index) {
  int saw_exit_label = 0;
  int saw_return = 0;

  if (!function || exit_label_index >= function->instruction_count) {
    return 0;
  }

  for (size_t i = exit_label_index; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (!saw_exit_label) {
      if (ins->op != IR_OP_LABEL) {
        return 0;
      }
      saw_exit_label = 1;
      continue;
    }
    if (ins->op == IR_OP_LABEL) {
      return 0;
    }
    if (ins->op == IR_OP_RETURN) {
      if (saw_return || !ir_operand_is_int_value(&ins->lhs, 0)) {
        return 0;
      }
      saw_return = 1;
      continue;
    }
    return 0;
  }

  return saw_exit_label && saw_return;
}

static int ir_try_memcmp_byte_loop_function(IRFunction *function,
                                            int *changed) {
  const char *a_symbol = NULL;
  const char *b_symbol = NULL;
  const char *len_symbol = NULL;
  const char *iv_symbol = NULL;
  const char *lhs_byte = NULL;
  const char *rhs_byte = NULL;
  size_t header_index = (size_t)-1;
  size_t exit_label_index = (size_t)-1;
  IRWhileLoopBounds bounds = {0};
  int saw_a_load = 0;
  int saw_b_load = 0;
  int last_byte_load_base = 0;
  int saw_neq = 0;
  int saw_diff_return = 0;
  int saw_zero_return = 0;

  if (!function || function->parameter_count != 3 || !function->parameter_names ||
      !function->parameter_types ||
      strcmp(function->parameter_types[0], "cstring") != 0 ||
      strcmp(function->parameter_types[1], "cstring") != 0 ||
      strcmp(function->parameter_types[2], "int64") != 0) {
    return 1;
  }

  a_symbol = function->parameter_names[0];
  b_symbol = function->parameter_names[1];
  len_symbol = function->parameter_names[2];
  if (!a_symbol || !b_symbol || !len_symbol) {
    return 1;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRWhileLoopBounds candidate = {0};
    if (ir_find_while_loop_bounds(function, i, &candidate)) {
      IRInstruction *compare = &function->instructions[candidate.compare_index];
      if (ir_operand_is_symbol_named(&compare->rhs, len_symbol)) {
        bounds = candidate;
        header_index = i;
        iv_symbol = compare->lhs.name;
        break;
      }
    }
  }

  if (header_index == (size_t)-1 || !iv_symbol ||
      !ir_find_label_index(function, bounds.exit_label, &exit_label_index)) {
    return 1;
  }
  /* memcmp compares bytes 0..len: the loop must start at iv == 0 (a loop from
   * iv == 1 that skips byte 0 is NOT memcmp). */
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
    return 1;
  }

  for (size_t i = bounds.branch_index + 1; i < bounds.jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LOAD && ins->dest.kind == IR_OPERAND_TEMP &&
        ins->dest.name) {
      if (ir_memcmp_byte_loop_is_indexed_load(function, i, ins, a_symbol,
                                             iv_symbol)) {
        saw_a_load = 1;
        last_byte_load_base = 1;
      } else if (ir_memcmp_byte_loop_is_indexed_load(function, i, ins, b_symbol,
                                                    iv_symbol)) {
        saw_b_load = 1;
        last_byte_load_base = 2;
      } else {
        last_byte_load_base = 0;
      }
    } else if (ins->op == IR_OP_CAST && ins->text &&
               strcmp(ins->text, "uint8") == 0 &&
               ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name) {
      if (!lhs_byte && last_byte_load_base == 1) {
        lhs_byte = ins->dest.name;
      } else if (!rhs_byte && last_byte_load_base == 2 &&
                 (!lhs_byte || strcmp(ins->dest.name, lhs_byte) != 0)) {
        rhs_byte = ins->dest.name;
      }
      last_byte_load_base = 0;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "!=") == 0 && lhs_byte && rhs_byte) {
      saw_neq =
          (ir_operand_is_symbol_named(&ins->lhs, lhs_byte) &&
           ir_operand_is_symbol_named(&ins->rhs, rhs_byte)) ||
          (ir_operand_is_symbol_named(&ins->lhs, rhs_byte) &&
           ir_operand_is_symbol_named(&ins->rhs, lhs_byte));
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "-") == 0 && lhs_byte && rhs_byte) {
      const char *left = NULL;
      const char *right = NULL;
      if (ir_memcmp_byte_loop_value_symbol(function, i, &ins->lhs, &left) &&
          ir_memcmp_byte_loop_value_symbol(function, i, &ins->rhs, &right) &&
          left && right && strcmp(left, lhs_byte) == 0 &&
          strcmp(right, rhs_byte) == 0) {
        for (size_t j = i + 1; j < bounds.jump_index; j++) {
          const IRInstruction *ret = &function->instructions[j];
          if (ret->op == IR_OP_RETURN &&
              ir_operand_is_temp_named(&ret->lhs, ins->dest.name)) {
            saw_diff_return = 1;
            break;
          }
          if (ret->op == IR_OP_LABEL) {
            break;
          }
        }
      }
    }
  }

  saw_zero_return =
      ir_memcmp_byte_loop_tail_is_zero_return(function, exit_label_index);

  if (!saw_a_load || !saw_b_load || !saw_neq || !saw_diff_return ||
      !saw_zero_return) {
    return 1;
  }

  {
    IRInstruction call = {0};
    IRInstruction cast = {0};
    IRInstruction ret = {0};
    call.op = IR_OP_CALL;
    call.location = function->instructions[header_index].location;
    call.dest = ir_operand_temp("__memcmp_result_i32");
    call.text = mettle_strdup("memcmp");
    call.arguments = calloc(3, sizeof(IROperand));
    if (!call.text || !call.arguments) {
      ir_instruction_destroy_storage(&call);
      return 0;
    }
    call.argument_count = 3;
    call.arguments[0] = ir_operand_symbol(a_symbol);
    call.arguments[1] = ir_operand_symbol(b_symbol);
    call.arguments[2] = ir_operand_symbol(len_symbol);

    cast.op = IR_OP_CAST;
    cast.location = call.location;
    cast.dest = ir_operand_temp("__memcmp_result_i64");
    cast.lhs = ir_operand_temp("__memcmp_result_i32");
    cast.text = mettle_strdup("int64");
    if (!cast.text) {
      ir_instruction_destroy_storage(&call);
      ir_instruction_destroy_storage(&cast);
      return 0;
    }

    ret.op = IR_OP_RETURN;
    ret.location = call.location;
    ret.lhs = ir_operand_temp("__memcmp_result_i64");

    ir_instruction_destroy_storage(&function->instructions[header_index]);
    function->instructions[header_index] = call;
    ir_instruction_destroy_storage(&function->instructions[header_index + 1]);
    function->instructions[header_index + 1] = cast;
    ir_instruction_destroy_storage(&function->instructions[header_index + 2]);
    function->instructions[header_index + 2] = ret;
    for (size_t i = header_index + 3; i < function->instruction_count; i++) {
      ir_instruction_make_nop(&function->instructions[i]);
    }
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_memcmp_byte_loop_pass(IRFunction *function, int *changed) {
  return ir_try_memcmp_byte_loop_function(function, changed);
}

/* For a LOAD at `load_index` whose address temp is `addr = base + (index << 2)`,
 * recover the base symbol and the index. The index is either a symbol directly
 * (then *lane_base = that symbol, *lane = 0) or a temp `sym + C` for a small
 * constant C (then *lane_base = sym, *lane = C). Returns 1 on a clean match. */
static int ir_slp_load_base_index(const IRFunction *function, size_t load_index,
                                  const char *addr_temp, const char **base_out,
                                  const char **lane_base_out, long long *lane_out) {
  const IRInstruction *addp = ir_find_temp_producer_before(function, load_index,
                                                           addr_temp);
  if (!addp || addp->op != IR_OP_BINARY || !addp->text ||
      strcmp(addp->text, "+") != 0 || addp->is_float ||
      addp->lhs.kind != IR_OPERAND_SYMBOL || !addp->lhs.name ||
      addp->rhs.kind != IR_OPERAND_TEMP || !addp->rhs.name) {
    return 0;
  }
  *base_out = addp->lhs.name;
  const IRInstruction *shl = ir_find_temp_producer_before(
      function, load_index, addp->rhs.name);
  if (!shl || shl->op != IR_OP_BINARY || !shl->text ||
      strcmp(shl->text, "<<") != 0 || shl->rhs.kind != IR_OPERAND_INT ||
      shl->rhs.int_value != 2) {
    return 0;
  }
  /* shl->lhs is the index: a symbol (lane 0) or a temp `sym + C`. */
  if (shl->lhs.kind == IR_OPERAND_SYMBOL && shl->lhs.name) {
    *lane_base_out = shl->lhs.name;
    *lane_out = 0;
    return 1;
  }
  if (shl->lhs.kind == IR_OPERAND_TEMP && shl->lhs.name) {
    const IRInstruction *off = ir_find_temp_producer_before(function, load_index,
                                                            shl->lhs.name);
    if (off && off->op == IR_OP_BINARY && off->text &&
        strcmp(off->text, "+") == 0 && !off->is_float &&
        off->lhs.kind == IR_OPERAND_SYMBOL && off->lhs.name &&
        off->rhs.kind == IR_OPERAND_INT) {
      *lane_base_out = off->lhs.name;
      *lane_out = off->rhs.int_value;
      return 1;
    }
  }
  return 0;
}

/* Initial value (a symbol or int) assigned to `sym` by the nearest ASSIGN before
 * `before_index`. Returns a cloned operand in *out, or 0 if not found/clean. */
static int ir_slp_find_init(const IRFunction *function, size_t before_index,
                            const char *sym, IROperand *out) {
  for (size_t i = before_index; i-- > 0;) {
    const IRInstruction *in = &function->instructions[i];
    if ((in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST) &&
        in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        strcmp(in->dest.name, sym) == 0) {
      /* Use the source symbol/value directly (a cast of a symbol just renames
       * its integer value for indexing). */
      if (in->lhs.kind == IR_OPERAND_SYMBOL || in->lhs.kind == IR_OPERAND_INT) {
        return ir_operand_clone(&in->lhs, out);
      }
      return 0;
    }
    /* A non-NOP redefinition we don't understand: stop. */
    if (in->op == IR_OP_BINARY && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, sym) == 0) {
      return 0;
    }
  }
  return 0;
}

/* SLP-vectorize a group of K parallel int32 multiply-accumulate reductions in a
 * counted loop: K isomorphic chains `sumJ = sumJ + (av * b[idxJ])` sharing one
 * broadcast scalar `av = a[a_idx]`, with contiguous loads (idxJ = b_base + J) and
 * K contiguous post-loop stores `c[out_idx + J] = sumJ`. Pattern-based: matches
 * the instruction-level parallelism, not any function or nest shape. */
#define IR_SLP_MAX_LANES 8

/* Find the K contiguous output stores `c[out_idx + lane] = sum_by_lane[lane]`
 * just after the loop-exit label. Fills c_base/out_idx_sym/store_idx, returns 1
 * if all K are found and consistent. */
static int ir_slp_find_stores(IRFunction *function, size_t exit_label_index,
                              int K, const char *const *sum_by_lane,
                              const char **c_base_out,
                              const char **out_idx_out, size_t *store_idx) {
  const char *c_base = NULL, *out_idx_sym = NULL;
  int found = 0;
  for (int wanted = 0; wanted < K; wanted++) {
    int got = 0;
    for (size_t i = exit_label_index + 1;
         i < function->instruction_count && i < exit_label_index + 80; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (in->op == IR_OP_LABEL || in->op == IR_OP_JUMP ||
          in->op == IR_OP_BRANCH_ZERO) {
        break;
      }
      if (in->op != IR_OP_STORE || in->dest.kind != IR_OPERAND_TEMP ||
          !in->dest.name ||
          !ir_operand_is_symbol_named(&in->lhs, sum_by_lane[wanted])) {
        continue;
      }
      const char *cb = NULL, *ci = NULL;
      long long lane = 0;
      int r = ir_slp_load_base_index(function, i, in->dest.name, &cb, &ci, &lane);
      if (!r || lane != wanted) {
        continue;
      }
      if (wanted == 0) {
        c_base = cb;
        out_idx_sym = ci;
      } else if (strcmp(cb, c_base) != 0 || strcmp(ci, out_idx_sym) != 0) {
        continue;
      }
      store_idx[wanted] = i;
      got = 1;
      found++;
      break;
    }
    if (!got) {
      return 0;
    }
  }
  *c_base_out = c_base;
  *out_idx_out = out_idx_sym;
  return found == K && c_base && out_idx_sym;
}

/* The SLP MAC kernels run exactly `compare->rhs` iterations from the recorded
 * a_off/b_off starting indexes, i.e. they replay the loop as iv = 0..bound-1.
 * That is only the scalar trip count when the iv provably starts at 0, steps
 * by exactly +1, and the bound symbol is loop-invariant -- none of which the
 * body scans below establish on their own. */
static int ir_slp_loop_frame_is_replayable(const IRFunction *function,
                                           size_t header_index,
                                           size_t branch_index,
                                           size_t jump_index,
                                           const IRInstruction *compare,
                                           const char *iv_symbol) {
  /* The bound is either a loop-invariant symbol or a compile-time constant
   * (the latter appears once a global bound like `N` is folded, or when the
   * source writes a literal `while (k < 32)`). A constant is trivially
   * invariant, so only a symbolic bound needs the body-write check below. */
  const char *bound_sym = NULL;
  if (compare->rhs.kind == IR_OPERAND_SYMBOL && compare->rhs.name) {
    bound_sym = compare->rhs.name;
  } else if (compare->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
    return 0;
  }
  int iv_inc_ok = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_try_parse_direct_unit_increment(ins, iv_symbol)) {
      iv_inc_ok = 1;
      continue;
    }
    /* Any other write to the iv (a second increment, a reset) breaks the
     * trip-count identity. */
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      return 0;
    }
    /* A symbolic bound is read once by the kernel: a write in the body
     * diverges. (A constant bound cannot be written.) */
    if (bound_sym && ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, bound_sym)) {
      return 0;
    }
  }
  return iv_inc_ok;
}

static int ir_try_vectorize_slp_mac_i32_at(IRFunction *function,
                                           size_t header_index, int *changed) {
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  const char *loop_label = header->text;
  size_t compare_index = 0, branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }
  const char *iv_symbol = compare->lhs.name;

  /* Back-edge jump to the header. */
  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, branch->text)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
  }
  if (!ir_slp_loop_frame_is_replayable(function, header_index, branch_index,
                                       jump_index, compare, iv_symbol)) {
    return 1;
  }

  /* Collect accumulator chains: `S = S + T` where T = av * bload. The shared
   * broadcast `av` is a symbol (a named local); each `bload` is a temp. */
  const char *sum_sym[IR_SLP_MAX_LANES];
  const char *bload_temp[IR_SLP_MAX_LANES];
  const char *av_sym = NULL;
  int K = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_BINARY || in->is_float || !in->text ||
        strcmp(in->text, "+") != 0 || in->dest.kind != IR_OPERAND_SYMBOL ||
        !in->dest.name || !ir_operand_is_symbol_named(&in->lhs, in->dest.name) ||
        in->rhs.kind != IR_OPERAND_TEMP || !in->rhs.name) {
      continue;
    }
    const IRInstruction *mul =
        ir_find_temp_producer_before(function, i, in->rhs.name);
    if (!mul || mul->op != IR_OP_BINARY || !mul->text ||
        strcmp(mul->text, "*") != 0 || mul->is_float) {
      continue;
    }
    /* One operand is the shared symbol (av), the other the per-lane load temp. */
    const char *cand_av = NULL, *cand_ld = NULL;
    if (mul->lhs.kind == IR_OPERAND_SYMBOL && mul->lhs.name &&
        mul->rhs.kind == IR_OPERAND_TEMP && mul->rhs.name) {
      cand_av = mul->lhs.name;
      cand_ld = mul->rhs.name;
    } else if (mul->rhs.kind == IR_OPERAND_SYMBOL && mul->rhs.name &&
               mul->lhs.kind == IR_OPERAND_TEMP && mul->lhs.name) {
      cand_av = mul->rhs.name;
      cand_ld = mul->lhs.name;
    } else {
      continue;
    }
    if (K >= IR_SLP_MAX_LANES) {
      return 1;
    }
    if (av_sym) {
      if (strcmp(cand_av, av_sym) != 0) {
        return 1;
      }
    } else {
      av_sym = cand_av;
    }
    sum_sym[K] = in->dest.name;
    bload_temp[K] = cand_ld;
    K++;
  }
  if ((K != 4 && K != 8) || !av_sym) {
    return 1;
  }

  /* av = a[a_idx]: a load into the symbol av, address a_base + (a_idx << 2). */
  const IRInstruction *avld = NULL;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LOAD && ir_operand_is_symbol_named(&in->dest, av_sym)) {
      avld = in;
      break;
    }
  }
  const char *a_base = NULL, *a_idx_sym = NULL;
  long long a_lane = 0;
  if (!avld || avld->lhs.kind != IR_OPERAND_TEMP || !avld->lhs.name ||
      !ir_slp_load_base_index(function, (size_t)(avld - function->instructions),
                              avld->lhs.name, &a_base, &a_idx_sym, &a_lane) ||
      a_lane != 0) {
    return 1;
  }

  /* Each bload: b_base + ((b_idx_sym + lane) << 2), lanes 0..K-1 (a permutation);
   * build sum_by_lane[lane] = that chain's accumulator. */
  const char *b_base = NULL, *b_idx_sym = NULL;
  const char *sum_by_lane[IR_SLP_MAX_LANES] = {0};
  for (int j = 0; j < K; j++) {
    const IRInstruction *ld = NULL;
    for (size_t i = branch_index + 1; i < jump_index; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (in->op == IR_OP_LOAD &&
          ir_operand_is_temp_named(&in->dest, bload_temp[j])) {
        ld = in;
        break;
      }
    }
    const char *bb = NULL, *bi = NULL;
    long long lane = 0;
    int rok = ld && ld->lhs.kind == IR_OPERAND_TEMP && ld->lhs.name &&
              ir_slp_load_base_index(function,
                                     (size_t)(ld - function->instructions),
                                     ld->lhs.name, &bb, &bi, &lane);
    if (!rok || lane < 0 || lane >= K) {
      return 1;
    }
    if (j == 0) {
      b_base = bb;
      b_idx_sym = bi;
    } else if (strcmp(bb, b_base) != 0 || strcmp(bi, b_idx_sym) != 0) {
      return 1;
    }
    if (sum_by_lane[lane]) {
      return 1; /* duplicate lane */
    }
    sum_by_lane[lane] = sum_sym[j];
  }
  if (!a_base || !b_base || strcmp(a_base, b_base) == 0) {
    return 1;
  }

  /* Loop IV increments: iv (k) += 1, a_idx += 1, b_idx += stride. The stride
   * is the matrix row length: a loop-invariant symbol, OR a compile-time
   * constant -- directly, or as a cast of one -- which is what a folded
   * global bound (`(int64)N` -> `(int64)32`) or a literal row length lowers
   * to. The kernel consumes the stride as an INT or a symbol equally. */
  IROperand stride_op = {0};
  int have_stride = 0;
  int a_inc_ok = 0, b_inc_ok = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_BINARY || !in->text || strcmp(in->text, "+") != 0 ||
        in->dest.kind != IR_OPERAND_SYMBOL || !in->dest.name ||
        !ir_operand_is_symbol_named(&in->lhs, in->dest.name)) {
      continue;
    }
    if (strcmp(in->dest.name, a_idx_sym) == 0 &&
        in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 1) {
      a_inc_ok = 1;
    } else if (strcmp(in->dest.name, b_idx_sym) == 0) {
      if (in->rhs.kind == IR_OPERAND_SYMBOL && in->rhs.name) {
        stride_op = ir_operand_symbol(in->rhs.name);
        b_inc_ok = have_stride = stride_op.name != NULL;
      } else if (in->rhs.kind == IR_OPERAND_INT) {
        stride_op = ir_operand_int(in->rhs.int_value);
        b_inc_ok = have_stride = 1;
      } else if (in->rhs.kind == IR_OPERAND_TEMP && in->rhs.name) {
        const IRInstruction *p =
            ir_find_temp_producer_before(function, i, in->rhs.name);
        if (p && p->op == IR_OP_CAST && p->lhs.kind == IR_OPERAND_SYMBOL &&
            p->lhs.name) {
          stride_op = ir_operand_symbol(p->lhs.name);
          b_inc_ok = have_stride = stride_op.name != NULL;
        } else if (p && p->op == IR_OP_CAST &&
                   p->lhs.kind == IR_OPERAND_INT) {
          stride_op = ir_operand_int(p->lhs.int_value);
          b_inc_ok = have_stride = 1;
        }
      }
    }
  }
  if (!a_inc_ok || !b_inc_ok || !have_stride) {
    ir_operand_destroy(&stride_op);
    return 1;
  }

  /* Initial index values (before the loop). */
  IROperand a_off = {0}, b_off = {0};
  if (!ir_slp_find_init(function, header_index, a_idx_sym, &a_off) ||
      !ir_slp_find_init(function, header_index, b_idx_sym, &b_off)) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    ir_operand_destroy(&stride_op);
    return 1;
  }

  /* Post-loop: K contiguous stores `c[out_idx + lane] = sum_by_lane[lane]` right
   * after the loop-exit label. */
  size_t exit_label_index = (size_t)-1;
  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, branch->text) == 0) {
      exit_label_index = i;
      break;
    }
  }
  if (exit_label_index == (size_t)-1) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    ir_operand_destroy(&stride_op);
    return 1;
  }
  const char *c_base = NULL, *out_idx_sym = NULL;
  size_t store_idx[IR_SLP_MAX_LANES];
  int sfound = ir_slp_find_stores(function, exit_label_index, K, sum_by_lane,
                                  &c_base, &out_idx_sym, store_idx);
  if (!sfound || strcmp(c_base, a_base) == 0) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    ir_operand_destroy(&stride_op);
    return 1;
  }
  if (ir_symbol_live_after_loop(function, exit_label_index, iv_symbol) ||
      ir_symbol_live_after_loop(function, exit_label_index, a_idx_sym) ||
      ir_symbol_live_after_loop(function, exit_label_index, b_idx_sym)) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    ir_operand_destroy(&stride_op);
    return 1;
  }

  /* The trip count: a symbol (loop-invariant bound) or a constant (a folded
   * global / literal bound). The kernel accepts either. */
  IROperand count_op = (compare->rhs.kind == IR_OPERAND_INT)
                           ? ir_operand_int(compare->rhs.int_value)
                           : ir_operand_symbol(compare->rhs.name);

  /* Build the op at the first store; out_idx is live there. */
  IRInstruction fused = {0};
  fused.op = IR_OP_SIMD_SLP_MAC_I32;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(c_base);
  fused.lhs = ir_operand_symbol(a_base);
  fused.rhs = ir_operand_symbol(b_base);
  fused.arguments = calloc(6, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    ir_operand_destroy(&stride_op);
    ir_operand_destroy(&count_op);
    return 0;
  }
  fused.argument_count = 6;
  fused.arguments[0] = ir_operand_int(K);
  fused.arguments[1] = count_op;                       /* count (sym or int) */
  fused.arguments[2] = a_off;                          /* a_off */
  fused.arguments[3] = b_off;                          /* b_off */
  fused.arguments[4] = stride_op;                      /* b stride (sym/int) */
  fused.arguments[5] = ir_operand_symbol(out_idx_sym); /* out_off */

  size_t place = store_idx[0];
  ir_instruction_destroy_storage(&function->instructions[place]);
  function->instructions[place] = fused;
  /* NOP the loop (header..jump) and the other stores. */
  for (size_t i = header_index; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  for (int j = 1; j < K; j++) {
    ir_instruction_make_nop(&function->instructions[store_idx[j]]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_slp_mac_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  {
    static int no_slp = -1;
    if (no_slp < 0) {
      no_slp = getenv("NO_SLP") ? 1 : 0;
    }
    if (no_slp) {
      /* Declining to run is success with no work done. Zero is the failure
       * code, and the driver reports a failed pass as an internal error. */
      return 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_slp_mac_i32_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* int8 variant of the SLP MAC value resolver. The multiply operand `val_name`
 * (the broadcast symbol `av`, or a per-lane bload temp) is the result of a
 * `(int32)` cast of a BYTE load: follow cast -> byte load (width 1) -> address
 * temp, then resolve the address as `base + index` with NO <<2 shift (int8
 * arrays are 1-byte-indexed, unlike the int32 path's base + (index<<2)). The
 * index is a symbol (lane 0) or a temp `sym + C` (lane C, a congruent-IV form).
 * val_is_temp selects whether val_name is a temp (bload) or symbol (av). */
static int ir_slp_i8_resolve(IRFunction *fn, size_t lo, size_t hi,
                             int val_is_temp, const char *val_name,
                             const char **base_out, const char **lane_base_out,
                             long long *lane_out) {
  const IRInstruction *cast = NULL;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op != IR_OP_CAST) {
      continue;
    }
    int m = val_is_temp ? ir_operand_is_temp_named(&in->dest, val_name)
                        : ir_operand_is_symbol_named(&in->dest, val_name);
    if (m) {
      cast = in;
      break;
    }
  }
  if (!cast || cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name) {
    return 0;
  }
  size_t load_idx = (size_t)-1;
  const IRInstruction *ld = NULL;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_LOAD && ir_operand_is_temp_named(&in->dest, cast->lhs.name) &&
        in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 1 &&
        in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name) {
      ld = in;
      load_idx = i;
      break;
    }
  }
  if (!ld) {
    return 0;
  }
  const IRInstruction *addp =
      ir_find_temp_producer_before(fn, load_idx, ld->lhs.name);
  if (!addp || addp->op != IR_OP_BINARY || !addp->text ||
      strcmp(addp->text, "+") != 0 || addp->is_float ||
      addp->lhs.kind != IR_OPERAND_SYMBOL || !addp->lhs.name) {
    return 0;
  }
  *base_out = addp->lhs.name;
  if (addp->rhs.kind == IR_OPERAND_SYMBOL && addp->rhs.name) {
    *lane_base_out = addp->rhs.name;
    *lane_out = 0;
    return 1;
  }
  if (addp->rhs.kind == IR_OPERAND_TEMP && addp->rhs.name) {
    const IRInstruction *off =
        ir_find_temp_producer_before(fn, load_idx, addp->rhs.name);
    if (off && off->op == IR_OP_BINARY && off->text &&
        strcmp(off->text, "+") == 0 && !off->is_float &&
        off->lhs.kind == IR_OPERAND_SYMBOL && off->lhs.name &&
        off->rhs.kind == IR_OPERAND_INT) {
      *lane_base_out = off->lhs.name;
      *lane_out = off->rhs.int_value;
      return 1;
    }
  }
  return 0;
}

/* int8 x int8 -> int32 SLP MAC: the quantized-GEMM tile. Same K-parallel
 * broadcast-MAC shape as ir_try_vectorize_slp_mac_i32_at, but a/b are int8
 * arrays (byte loads + (int32) casts, scale-1 indexing) while c is int32
 * (scale-4 stores, resolved by the shared ir_slp_find_stores). Emits
 * IR_OP_SIMD_SLP_MAC_I8. */
static int ir_try_vectorize_slp_mac_i8_at(IRFunction *function,
                                          size_t header_index, int *changed) {
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  const char *loop_label = header->text;
  size_t compare_index = 0, branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }
  const char *iv_symbol = compare->lhs.name;

  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, branch->text)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
  }
  if (!ir_slp_loop_frame_is_replayable(function, header_index, branch_index,
                                       jump_index, compare, iv_symbol)) {
    return 1;
  }

  /* Collect chains `S = S + (av * bload)`, av a symbol, bload a temp. */
  const char *sum_sym[IR_SLP_MAX_LANES];
  const char *bload_temp[IR_SLP_MAX_LANES];
  const char *av_sym = NULL;
  int K = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_BINARY || in->is_float || !in->text ||
        strcmp(in->text, "+") != 0 || in->dest.kind != IR_OPERAND_SYMBOL ||
        !in->dest.name || !ir_operand_is_symbol_named(&in->lhs, in->dest.name) ||
        in->rhs.kind != IR_OPERAND_TEMP || !in->rhs.name) {
      continue;
    }
    const IRInstruction *mul =
        ir_find_temp_producer_before(function, i, in->rhs.name);
    if (!mul || mul->op != IR_OP_BINARY || !mul->text ||
        strcmp(mul->text, "*") != 0 || mul->is_float) {
      continue;
    }
    const char *cand_av = NULL, *cand_ld = NULL;
    if (mul->lhs.kind == IR_OPERAND_SYMBOL && mul->lhs.name &&
        mul->rhs.kind == IR_OPERAND_TEMP && mul->rhs.name) {
      cand_av = mul->lhs.name;
      cand_ld = mul->rhs.name;
    } else if (mul->rhs.kind == IR_OPERAND_SYMBOL && mul->rhs.name &&
               mul->lhs.kind == IR_OPERAND_TEMP && mul->lhs.name) {
      cand_av = mul->rhs.name;
      cand_ld = mul->lhs.name;
    } else {
      continue;
    }
    if (K >= IR_SLP_MAX_LANES) {
      return 1;
    }
    if (av_sym) {
      if (strcmp(cand_av, av_sym) != 0) {
        return 1;
      }
    } else {
      av_sym = cand_av;
    }
    sum_sym[K] = in->dest.name;
    bload_temp[K] = cand_ld;
    K++;
  }
  if ((K != 4 && K != 8) || !av_sym) {
    return 1;
  }

  /* av = (int32)a[a_idx] (byte load, scale-1 address). */
  const char *a_base = NULL, *a_idx_sym = NULL;
  long long a_lane = 0;
  if (!ir_slp_i8_resolve(function, branch_index + 1, jump_index, 0, av_sym,
                         &a_base, &a_idx_sym, &a_lane) ||
      a_lane != 0) {
    return 1;
  }

  /* Each bload = (int32)b[b_idx + lane] (byte load, scale-1). */
  const char *b_base = NULL, *b_idx_sym = NULL;
  const char *sum_by_lane[IR_SLP_MAX_LANES] = {0};
  for (int j = 0; j < K; j++) {
    const char *bb = NULL, *bi = NULL;
    long long lane = 0;
    if (!ir_slp_i8_resolve(function, branch_index + 1, jump_index, 1,
                           bload_temp[j], &bb, &bi, &lane) ||
        lane < 0 || lane >= K) {
      return 1;
    }
    if (j == 0) {
      b_base = bb;
      b_idx_sym = bi;
    } else if (strcmp(bb, b_base) != 0 || strcmp(bi, b_idx_sym) != 0) {
      return 1;
    }
    if (sum_by_lane[lane]) {
      return 1;
    }
    sum_by_lane[lane] = sum_sym[j];
  }
  if (!a_base || !b_base || strcmp(a_base, b_base) == 0) {
    return 1;
  }

  /* IV increments: a_idx += 1, b_idx += stride. */
  const char *stride_sym = NULL;
  int a_inc_ok = 0, b_inc_ok = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_BINARY || !in->text || strcmp(in->text, "+") != 0 ||
        in->dest.kind != IR_OPERAND_SYMBOL || !in->dest.name ||
        !ir_operand_is_symbol_named(&in->lhs, in->dest.name)) {
      continue;
    }
    if (strcmp(in->dest.name, a_idx_sym) == 0 &&
        in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 1) {
      a_inc_ok = 1;
    } else if (strcmp(in->dest.name, b_idx_sym) == 0) {
      if (in->rhs.kind == IR_OPERAND_SYMBOL && in->rhs.name) {
        stride_sym = in->rhs.name;
        b_inc_ok = 1;
      } else if (in->rhs.kind == IR_OPERAND_TEMP && in->rhs.name) {
        const IRInstruction *p =
            ir_find_temp_producer_before(function, i, in->rhs.name);
        if (p && p->op == IR_OP_CAST && p->lhs.kind == IR_OPERAND_SYMBOL &&
            p->lhs.name) {
          stride_sym = p->lhs.name;
          b_inc_ok = 1;
        }
      }
    }
  }
  if (!a_inc_ok || !b_inc_ok || !stride_sym) {
    return 1;
  }

  IROperand a_off = {0}, b_off = {0};
  if (!ir_slp_find_init(function, header_index, a_idx_sym, &a_off) ||
      !ir_slp_find_init(function, header_index, b_idx_sym, &b_off)) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    return 1;
  }

  size_t exit_label_index = (size_t)-1;
  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, branch->text) == 0) {
      exit_label_index = i;
      break;
    }
  }
  if (exit_label_index == (size_t)-1) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    return 1;
  }
  const char *c_base = NULL, *out_idx_sym = NULL;
  size_t store_idx[IR_SLP_MAX_LANES];
  int sfound = ir_slp_find_stores(function, exit_label_index, K, sum_by_lane,
                                  &c_base, &out_idx_sym, store_idx);
  if (!sfound || strcmp(c_base, a_base) == 0) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    return 1;
  }
  if (ir_symbol_live_after_loop(function, exit_label_index, iv_symbol) ||
      ir_symbol_live_after_loop(function, exit_label_index, a_idx_sym) ||
      ir_symbol_live_after_loop(function, exit_label_index, b_idx_sym)) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    return 1;
  }

  IRInstruction fused = {0};
  fused.op = IR_OP_SIMD_SLP_MAC_I8;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(c_base);
  fused.lhs = ir_operand_symbol(a_base);
  fused.rhs = ir_operand_symbol(b_base);
  fused.arguments = calloc(6, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&a_off);
    ir_operand_destroy(&b_off);
    return 0;
  }
  fused.argument_count = 6;
  fused.arguments[0] = ir_operand_int(K);
  fused.arguments[1] = ir_operand_symbol(compare->rhs.name);
  fused.arguments[2] = a_off;
  fused.arguments[3] = b_off;
  fused.arguments[4] = ir_operand_symbol(stride_sym);
  fused.arguments[5] = ir_operand_symbol(out_idx_sym);

  size_t place = store_idx[0];
  ir_instruction_destroy_storage(&function->instructions[place]);
  function->instructions[place] = fused;
  for (size_t i = header_index; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  for (int j = 1; j < K; j++) {
    ir_instruction_make_nop(&function->instructions[store_idx[j]]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_slp_mac_i8_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  {
    static int no_slp = -1;
    if (no_slp < 0) {
      no_slp = getenv("NO_SLP") ? 1 : 0;
    }
    if (no_slp) {
      /* Declining to run is success with no work done. Zero is the failure
       * code, and the driver reports a failed pass as an internal error. */
      return 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_slp_mac_i8_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* In-place vectorization of `a[i] = expf(a[i])` over a float32 array: a counted
 * unit stride loop whose body loads a float, calls the owned `expf`, and stores
 * the result back to the same element. Like a compiler's libm vectorizer
 * (libmvec/SVML), it replaces the call loop with an AVX2 polynomial exp
 * (IR_OP_SIMD_EXP_F32). Matched by the math-function call + element-wise map
 * shape, not a benchmark. The result tracks the owned expf within tolerance. */
static int ir_try_vectorize_exp_f32_at(IRFunction *function, size_t header_index,
                                       int *changed) {
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  const char *loop_label = header->text;
  size_t compare_index = 0, branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name)) {
    return 1;
  }
  const char *iv_symbol = compare->lhs.name;

  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, branch->text)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
  }

  /* The expf call: r = expf(v), with v and r temps. */
  const char *call_arg = NULL, *call_res = NULL;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_CALL && in->text && strcmp(in->text, "expf") == 0 &&
        in->argument_count == 1 && in->arguments &&
        in->arguments[0].kind == IR_OPERAND_TEMP && in->arguments[0].name &&
        in->dest.kind == IR_OPERAND_TEMP && in->dest.name) {
      call_arg = in->arguments[0].name;
      call_res = in->dest.name;
      break;
    }
  }
  if (!call_arg || !call_res) {
    return 1;
  }

  /* The load that feeds the call: v = a[i] (float32, base+(i<<2), lane 0). */
  const char *a_base = NULL, *a_idx = NULL;
  long long lane = 0;
  int found_load = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LOAD && ir_operand_is_temp_named(&in->dest, call_arg) &&
        in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name &&
        ir_slp_load_base_index(function, i, in->lhs.name, &a_base, &a_idx,
                               &lane) &&
        lane == 0) {
      found_load = 1;
      break;
    }
  }
  if (!found_load || !a_base || !a_idx || strcmp(a_idx, iv_symbol) != 0) {
    return 1;
  }

  /* The store of the result back to a[i] (same base, in-place). */
  int found_store = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    const char *sb = NULL, *si = NULL;
    long long sl = 0;
    if (in->op == IR_OP_STORE &&
        ir_operand_is_temp_named(&in->lhs, call_res) &&
        in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
        ir_slp_load_base_index(function, i, in->dest.name, &sb, &si, &sl) &&
        sl == 0 && sb && si && strcmp(sb, a_base) == 0 &&
        strcmp(si, iv_symbol) == 0) {
      found_store = 1;
      break;
    }
  }
  if (!found_store) {
    return 1;
  }

  /* i increments by 1. */
  int inc_ok = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    if (ir_try_parse_direct_unit_increment(&function->instructions[i],
                                           iv_symbol)) {
      inc_ok = 1;
      break;
    }
  }
  if (!inc_ok) {
    return 1;
  }
  /* The kernel maps a[0..n) in place: the loop must start at iv == 0, and the
   * iv must be dead after the loop (the fused op drops it). */
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    return 1;
  }

  IRInstruction fused = {0};
  fused.op = IR_OP_SIMD_EXP_F32;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(a_base);
  fused.arguments = calloc(1, sizeof(IROperand));
  if (!fused.arguments) {
    return 0;
  }
  fused.argument_count = 1;
  fused.arguments[0] = ir_operand_symbol(compare->rhs.name); /* count n */

  ir_instruction_destroy_storage(header);
  *header = fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_exp_f32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_exp_f32_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* In-place SiLU / SwiGLU gate over a float32 array:
 *   out[i] = silu(g[i])          -> SiLU
 *   out[i] = silu(g[i]) * u[i]   -> SwiGLU gate (the FFN activation)
 * where silu(x) = x / (1 + expf(0 - x)). The body (after inlining `silu`) is the
 * fixed DAG: load g -> `0 - g` -> expf -> `1 + e` -> `g / (1+e)` -> [load u, mul]
 * -> store g (in-place). Lowered to IR_OP_SIMD_SILU_F32, which reuses the AVX2
 * exp polynomial. Matched by shape (not a benchmark); the result tracks the
 * scalar silu within the exp kernel's tolerance. */
static int ir_try_vectorize_silu_f32_at(IRFunction *function,
                                        size_t header_index, int *changed) {
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  const char *loop_label = header->text;
  size_t compare_index = 0, branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name)) {
    return 1;
  }
  const char *iv_symbol = compare->lhs.name;

  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, branch->text)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
  }

  /* expf call: exp_res = expf(exp_arg). */
  const char *exp_arg = NULL, *exp_res = NULL;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_CALL && in->text && strcmp(in->text, "expf") == 0 &&
        in->argument_count == 1 && in->arguments &&
        in->arguments[0].kind == IR_OPERAND_TEMP && in->arguments[0].name &&
        in->dest.kind == IR_OPERAND_TEMP && in->dest.name) {
      exp_arg = in->arguments[0].name;
      exp_res = in->dest.name;
      break;
    }
  }
  if (!exp_arg || !exp_res) {
    return 1;
  }

  /* exp_arg = 0.0 - g_temp  (float negate). */
  const IRInstruction *neg =
      ir_find_temp_producer_before(function, jump_index, exp_arg);
  if (!neg || neg->op != IR_OP_BINARY || !neg->is_float || !neg->text ||
      strcmp(neg->text, "-") != 0 || neg->lhs.kind != IR_OPERAND_FLOAT ||
      neg->lhs.float_value != 0.0 || neg->rhs.kind != IR_OPERAND_TEMP ||
      !neg->rhs.name) {
    return 1;
  }
  const char *g_temp = neg->rhs.name;

  /* g_temp = load base_g[iv] (unit stride, lane 0). */
  const IRInstruction *gload =
      ir_find_temp_producer_before(function, jump_index, g_temp);
  const char *base_g = NULL, *gidx = NULL;
  long long glane = 0;
  if (!gload || gload->op != IR_OP_LOAD || gload->lhs.kind != IR_OPERAND_TEMP ||
      !gload->lhs.name ||
      !ir_slp_load_base_index(function,
                              (size_t)(gload - function->instructions),
                              gload->lhs.name, &base_g, &gidx, &glane) ||
      glane != 0 || !base_g || !gidx || strcmp(gidx, iv_symbol) != 0) {
    return 1;
  }

  /* add_res = 1.0 + exp_res  (commutative). */
  const char *add_res = NULL;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_BINARY && in->is_float && in->text &&
        strcmp(in->text, "+") == 0 && in->dest.kind == IR_OPERAND_TEMP &&
        in->dest.name &&
        ((in->lhs.kind == IR_OPERAND_FLOAT && in->lhs.float_value == 1.0 &&
          ir_operand_is_temp_named(&in->rhs, exp_res)) ||
         (in->rhs.kind == IR_OPERAND_FLOAT && in->rhs.float_value == 1.0 &&
          ir_operand_is_temp_named(&in->lhs, exp_res)))) {
      add_res = in->dest.name;
      break;
    }
  }
  if (!add_res) {
    return 1;
  }

  /* silu_res = g / add_res. The numerator is g: either the exact temp the negate
   * used (inlined `silu(v)` loads g once), or a second load of base_g[iv] (a
   * directly-written `g[i] / (1 + expf(-g[i]))` loads g[i] twice). */
  const char *silu_res = NULL;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_BINARY || !in->is_float || !in->text ||
        strcmp(in->text, "/") != 0 || in->dest.kind != IR_OPERAND_TEMP ||
        !in->dest.name || in->lhs.kind != IR_OPERAND_TEMP || !in->lhs.name ||
        !ir_operand_is_temp_named(&in->rhs, add_res)) {
      continue;
    }
    int num_ok = ir_operand_is_temp_named(&in->lhs, g_temp);
    if (!num_ok) {
      const IRInstruction *nload =
          ir_find_temp_producer_before(function, jump_index, in->lhs.name);
      const char *nb = NULL, *ni = NULL;
      long long nl = 0;
      num_ok = nload && nload->op == IR_OP_LOAD &&
               nload->lhs.kind == IR_OPERAND_TEMP && nload->lhs.name &&
               ir_slp_load_base_index(function,
                                      (size_t)(nload - function->instructions),
                                      nload->lhs.name, &nb, &ni, &nl) &&
               nl == 0 && nb && ni && strcmp(nb, base_g) == 0 &&
               strcmp(ni, iv_symbol) == 0;
    }
    if (num_ok) {
      silu_res = in->dest.name;
      break;
    }
  }
  if (!silu_res) {
    return 1;
  }

  /* Store base_out[iv] = final, base_out == base_g (in-place). final is silu_res
   * (plain SiLU) or silu_res * u[iv] (SwiGLU gate). */
  const char *base_u = NULL;
  int has_mul = 0;
  int found_store = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    const char *base_out = NULL, *sidx = NULL;
    long long slane = 0;
    if (in->op != IR_OP_STORE || in->lhs.kind != IR_OPERAND_TEMP ||
        !in->lhs.name || in->dest.kind != IR_OPERAND_TEMP || !in->dest.name ||
        !ir_slp_load_base_index(function, i, in->dest.name, &base_out, &sidx,
                                &slane) ||
        slane != 0 || !base_out || !sidx || strcmp(sidx, iv_symbol) != 0 ||
        strcmp(base_out, base_g) != 0) {
      continue;
    }
    if (ir_operand_is_temp_named(&in->lhs, silu_res)) {
      has_mul = 0;
      found_store = 1;
      break;
    }
    const IRInstruction *mul =
        ir_find_temp_producer_before(function, jump_index, in->lhs.name);
    if (mul && mul->op == IR_OP_BINARY && mul->is_float && mul->text &&
        strcmp(mul->text, "*") == 0) {
      const IROperand *other = NULL;
      if (ir_operand_is_temp_named(&mul->lhs, silu_res)) {
        other = &mul->rhs;
      } else if (ir_operand_is_temp_named(&mul->rhs, silu_res)) {
        other = &mul->lhs;
      }
      if (other && other->kind == IR_OPERAND_TEMP && other->name) {
        const IRInstruction *uload =
            ir_find_temp_producer_before(function, jump_index, other->name);
        const char *uidx = NULL;
        long long ulane = 0;
        if (uload && uload->op == IR_OP_LOAD &&
            uload->lhs.kind == IR_OPERAND_TEMP && uload->lhs.name &&
            ir_slp_load_base_index(function,
                                   (size_t)(uload - function->instructions),
                                   uload->lhs.name, &base_u, &uidx, &ulane) &&
            ulane == 0 && base_u && uidx && strcmp(uidx, iv_symbol) == 0) {
          has_mul = 1;
          found_store = 1;
          break;
        }
      }
    }
  }
  if (!found_store) {
    return 1;
  }

  /* iv increments by 1, starts at 0, and is dead after the loop. */
  int inc_ok = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    if (ir_try_parse_direct_unit_increment(&function->instructions[i],
                                           iv_symbol)) {
      inc_ok = 1;
      break;
    }
  }
  if (!inc_ok || !ir_iv_zero_at_header(function, header_index, iv_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    return 1;
  }

  IRInstruction fused = {0};
  fused.op = IR_OP_SIMD_SILU_F32;
  fused.location = header->location;
  fused.is_float = 1;
  fused.float_bits = 32;
  fused.dest = ir_operand_symbol(base_g); /* in-place: out == g */
  fused.lhs = ir_operand_symbol(base_g);
  if (has_mul) {
    fused.rhs = ir_operand_symbol(base_u);
  }
  fused.arguments = calloc(1, sizeof(IROperand));
  if (!fused.arguments) {
    return 0;
  }
  fused.argument_count = 1;
  fused.arguments[0] = ir_operand_symbol(compare->rhs.name); /* count n */

  ir_instruction_destroy_storage(header);
  *header = fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_silu_f32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_silu_f32_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

int ir_simd_dot_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_dot_i32_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

int ir_simd_dot_i8_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_dot_i8_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}
