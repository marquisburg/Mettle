#include "ir_optimize_internal.h"
#include "../../common.h" // mettle_free_string

static int ir_builtin_integer_type_info(const char *name, int *size_out,
                                        int *is_unsigned_out) {
  int size = 0;
  int is_unsigned = 0;

  if (!name) {
    return 0;
  }

  if (strcmp(name, "int8") == 0) {
    size = 1;
  } else if (strcmp(name, "uint8") == 0) {
    size = 1;
    is_unsigned = 1;
  } else if (strcmp(name, "int16") == 0) {
    size = 2;
  } else if (strcmp(name, "uint16") == 0) {
    size = 2;
    is_unsigned = 1;
  } else if (strcmp(name, "int32") == 0) {
    size = 4;
  } else if (strcmp(name, "uint32") == 0) {
    size = 4;
    is_unsigned = 1;
  } else if (strcmp(name, "int64") == 0) {
    size = 8;
  } else if (strcmp(name, "uint64") == 0) {
    size = 8;
    is_unsigned = 1;
  } else {
    return 0;
  }

  if (size_out) {
    *size_out = size;
  }
  if (is_unsigned_out) {
    *is_unsigned_out = is_unsigned;
  }
  return 1;
}

static int ir_find_temp_producer_index_in_current_block(
    const IRFunction *function, size_t before_index, const char *temp_name,
    size_t *producer_index_out) {
  if (!function || !temp_name || !producer_index_out ||
      before_index > function->instruction_count) {
    return 0;
  }

  for (size_t i = before_index; i > 0;) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->op == IR_OP_LABEL) {
      return 0;
    }
    if (ir_instruction_writes_temp(instruction) && instruction->dest.name &&
        strcmp(instruction->dest.name, temp_name) == 0) {
      *producer_index_out = i;
      return 1;
    }
  }

  return 0;
}

static int ir_try_compose_single_use_cast(IRFunction *function,
                                          const IRTempUseMap *uses,
                                          size_t cast_index, int *changed) {
  IRInstruction *cast = NULL;
  IRInstruction *producer = NULL;
  size_t producer_index = 0;
  int producer_size = 0;
  int producer_unsigned = 0;
  int cast_size = 0;
  const char *composed_type = NULL;
  char *type_copy = NULL;
  IROperand source = ir_operand_none();

  if (!function || !uses || cast_index >= function->instruction_count) {
    return 0;
  }

  cast = &function->instructions[cast_index];
  if (cast->op != IR_OP_CAST || cast->is_float || !cast->text ||
      cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name ||
      ir_temp_use_map_get(uses, cast->lhs.name) != 1 ||
      !ir_builtin_integer_type_info(cast->text, &cast_size, NULL) ||
      !ir_find_temp_producer_index_in_current_block(function, cast_index,
                                                    cast->lhs.name,
                                                    &producer_index)) {
    return 1;
  }

  producer = &function->instructions[producer_index];
  if (producer->op != IR_OP_CAST || producer->is_float || !producer->text ||
      !ir_builtin_integer_type_info(producer->text, &producer_size,
                                    &producer_unsigned)) {
    return 1;
  }

  /* Composing keeps the narrower conversion and trusts the backend to
   * materialize it at the destination's width, extending by the narrow type's
   * sign. That holds everywhere but one place: a 32-bit operation on x86-64
   * zero-extends into the 64-bit register, so a signed 32-bit narrowing left
   * the high half clear instead of sign-extended. `(int64)(int32)u` on a
   * uint32 then read -4 back as 4294967292, and Mettle's own linker rejected
   * every REL32 relocation as out of range. Leave that one pair uncomposed. */
  if (cast_size > producer_size && producer_size == 4 && !producer_unsigned) {
    return 1;
  }

  /* Compose integer cast chains by keeping only the narrower conversion.
   * Equal-width chains keep the later cast so signed/unsigned reinterpretation
   * at that width remains visible. A later dead-temp pass removes the first
   * cast, but nopping it here lets this same pass continue coalescing through
   * the new source. */
  composed_type = (cast_size <= producer_size) ? cast->text : producer->text;
  type_copy = mettle_strdup(composed_type);
  if (!type_copy || !ir_operand_clone(&producer->lhs, &source)) {
    free(type_copy);
    ir_operand_destroy(&source);
    return 0;
  }

  ir_operand_destroy(&cast->lhs);
  cast->lhs = source;
  mettle_free_string(cast->text);
  cast->text = type_copy;
  ir_instruction_make_nop(producer);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_try_coalesce_unsigned_load_cast(IRFunction *function,
                                              const IRTempUseMap *uses,
                                              size_t cast_index,
                                              int *changed) {
  IRInstruction *cast = NULL;
  IRInstruction *producer = NULL;
  size_t producer_index = 0;
  int cast_size = 0;
  int is_unsigned = 0;
  IROperand rewritten_dest = ir_operand_none();

  if (!function || !uses || cast_index >= function->instruction_count) {
    return 0;
  }

  cast = &function->instructions[cast_index];
  if (cast->op != IR_OP_CAST || cast->is_float || !cast->text ||
      cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name ||
      ir_temp_use_map_get(uses, cast->lhs.name) != 1 ||
      !ir_builtin_integer_type_info(cast->text, &cast_size, &is_unsigned) ||
      !is_unsigned || cast_size > 2 ||
      !ir_find_temp_producer_index_in_current_block(function, cast_index,
                                                    cast->lhs.name,
                                                    &producer_index)) {
    return 1;
  }

  producer = &function->instructions[producer_index];
  if (producer->op != IR_OP_LOAD || producer->is_float ||
      producer->rhs.kind != IR_OPERAND_INT ||
      producer->rhs.int_value != cast_size) {
    return 1;
  }

  if (!ir_operand_clone(&cast->dest, &rewritten_dest)) {
    return 0;
  }

  ir_operand_destroy(&producer->dest);
  producer->dest = rewritten_dest;
  ir_instruction_make_nop(cast);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_coalesce_single_use_temp_assign_pass(IRFunction *function,
                                                   int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 1; i < function->instruction_count; i++) {
    IRInstruction *assign_instruction = &function->instructions[i];
    if (assign_instruction->op != IR_OP_ASSIGN ||
        assign_instruction->lhs.kind != IR_OPERAND_TEMP ||
        !assign_instruction->lhs.name ||
        assign_instruction->dest.kind == IR_OPERAND_NONE) {
      continue;
    }

    if (ir_temp_use_map_get(&uses, assign_instruction->lhs.name) != 1) {
      continue;
    }

    size_t producer_index = i;
    IRInstruction *producer = NULL;
    while (producer_index > 0) {
      producer_index--;
      if (function->instructions[producer_index].op == IR_OP_NOP) {
        continue;
      }
      producer = &function->instructions[producer_index];
      break;
    }
    if (!producer || !ir_instruction_writes_destination(producer) ||
        producer->dest.kind != IR_OPERAND_TEMP || !producer->dest.name ||
        strcmp(producer->dest.name, assign_instruction->lhs.name) != 0) {
      continue;
    }

    /* A float ASSIGN may encode an IEEE-754 width conversion (e.g. a float64
     * expression narrowed into a float32 destination on return/assignment).
     * Folding the producer's dest forward would drop that cvtsd2ss/cvtss2sd and
     * store the wrong half of the value. Only coalesce when no width change is
     * implied: the producer must itself be float at the same width as the
     * assign's target. */
    if (assign_instruction->is_float) {
      int assign_bits = (assign_instruction->float_bits == 32) ? 32 : 64;
      int producer_bits =
          producer->is_float ? ((producer->float_bits == 32) ? 32 : 64) : 0;
      if (!producer->is_float || producer_bits != assign_bits) {
        continue;
      }
    }

    IROperand rewritten_dest = ir_operand_none();
    if (!ir_operand_clone(&assign_instruction->dest, &rewritten_dest)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }

    ir_operand_destroy(&producer->dest);
    producer->dest = rewritten_dest;
    ir_instruction_make_nop(assign_instruction);
    if (changed) {
      *changed = 1;
    }
  }

  for (size_t i = 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_CAST || instruction->is_float ||
        instruction->lhs.kind != IR_OPERAND_TEMP ||
        !instruction->lhs.name) {
      continue;
    }

    if (!ir_try_compose_single_use_cast(function, &uses, i, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_CAST || instruction->is_float ||
        instruction->lhs.kind != IR_OPERAND_TEMP ||
        !instruction->lhs.name) {
      continue;
    }

    if (!ir_try_coalesce_unsigned_load_cast(function, &uses, i, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

static void ir_symbol_value_map_invalidate_name(IRSymbolValueMap *map,
                                              const char *symbol_name) {
  if (!map || !symbol_name) {
    return;
  }

  ir_temp_value_map_remove(map, symbol_name);

  /* O(1) fast path: no surviving entry values this symbol, so the compaction
   * scan below would remove nothing. */
  if (!ir_temp_value_map_any_value_symbol(map, symbol_name)) {
    return;
  }

  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRTempValueEntry *entry = &map->items[read];
    int remove = 0;
    if (entry->value.kind == IR_OPERAND_SYMBOL && entry->value.name &&
        strcmp(entry->value.name, symbol_name) == 0) {
      remove = 1;
    }
    if (entry->value.kind == IR_OPERAND_TEMP && entry->value.name) {
      /* Temp values may embed propagated symbols; conservatively keep. */
    }

    if (remove) {
      ir_temp_value_map_note_value_removed(map, &entry->value);
      mettle_free_string(entry->name);
      ir_operand_destroy(&entry->value);
      continue;
    }

    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }
  if (map->count != write) {
    map->count = write;
    ir_temp_value_map_reindex(map);
  }
}

static int ir_resolve_propagated_value(const IRTempValueMap *temp_map,
                                       const IRSymbolValueMap *symbol_map,
                                       const IROperand *operand, IROperand *out,
                                       int depth) {
  if (!operand || !out) {
    return 0;
  }

  if (depth > 64) {
    return ir_operand_clone(operand, out);
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name && temp_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(temp_map, operand->name);
    if (mapped) {
      return ir_resolve_propagated_value(temp_map, symbol_map, mapped, out,
                                         depth + 1);
    }
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && symbol_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(symbol_map, operand->name);
    if (mapped) {
      return ir_resolve_propagated_value(temp_map, symbol_map, mapped, out,
                                         depth + 1);
    }
  }

  return ir_operand_clone(operand, out);
}

static int ir_try_propagate_operand(IRTempValueMap *temp_map,
                                    IRSymbolValueMap *symbol_map,
                                    IROperand *operand, int *changed) {
  if (!operand) {
    return 1;
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name && temp_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(temp_map, operand->name);
    if (!mapped) {
      return 1;
    }

    IROperand resolved = ir_operand_none();
    if (!ir_resolve_propagated_value(temp_map, symbol_map, mapped, &resolved,
                                     0)) {
      return 0;
    }

    ir_operand_destroy(operand);
    *operand = resolved;
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && symbol_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(symbol_map, operand->name);
    if (!mapped) {
      return 1;
    }

    IROperand resolved = ir_operand_none();
    if (!ir_resolve_propagated_value(temp_map, symbol_map, mapped, &resolved,
                                     0)) {
      return 0;
    }

    ir_operand_destroy(operand);
    *operand = resolved;
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  return 1;
}

static int ir_propagate_instruction_operands(IRTempValueMap *temp_map,
                                             IRSymbolValueMap *symbol_map,
                                             IRInstruction *instruction,
                                             int *changed) {
  if (!instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASYNC_COPY:
  case IR_OP_TENSOR_EPILOGUE:
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_try_propagate_operand(temp_map, symbol_map,
                                    &instruction->arguments[i], changed)) {
        return 0;
      }
    }
    break;

  case IR_OP_TENSOR_MMA:
  case IR_OP_TENSOR_MATMUL:
  case IR_OP_TENSOR_COMMIT:
  case IR_OP_STORE:
    if (!ir_try_propagate_operand(temp_map, symbol_map, &instruction->dest,
                                  changed) ||
        !ir_try_propagate_operand(temp_map, symbol_map, &instruction->lhs,
                                  changed) ||
        !ir_try_propagate_operand(temp_map, symbol_map, &instruction->rhs,
                                  changed)) {
      return 0;
    }
    break;

  case IR_OP_ROTATE_ADD:
    break;

  case IR_OP_BRANCH_ZERO:
    if (!ir_try_propagate_operand(temp_map, NULL, &instruction->lhs, changed)) {
      return 0;
    }
    break;

  case IR_OP_BRANCH_EQ:
    if (!ir_try_propagate_operand(temp_map, NULL, &instruction->lhs, changed) ||
        !ir_try_propagate_operand(temp_map, NULL, &instruction->rhs, changed)) {
      return 0;
    }
    break;

  case IR_OP_ASSIGN:
    if (!ir_try_propagate_operand(temp_map, symbol_map, &instruction->lhs,
                                  changed)) {
      return 0;
    }
    break;

  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_NEW:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_RETURN:
    if (!ir_try_propagate_operand(temp_map, NULL, &instruction->lhs, changed) ||
        !ir_try_propagate_operand(temp_map, NULL, &instruction->rhs, changed)) {
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_try_propagate_operand(temp_map, symbol_map,
                                    &instruction->arguments[i], changed)) {
        return 0;
      }
    }
    break;

  default:
    break;
  }

  return 1;
}

/* Record `i` as the last position reading each temp/symbol operand of the
 * instruction. dest is included: for stores it is an address read, and for
 * writes the entry gets invalidated anyway, so overcounting is harmless. */
static int ir_cp_note_operand_uses(const IRInstruction *ins, size_t i,
                                   IRTempValueMap *temp_last,
                                   IRTempValueMap *sym_last) {
  const IROperand *ops[3] = {&ins->lhs, &ins->rhs, &ins->dest};
  IROperand pos = {.kind = IR_OPERAND_INT, .int_value = (long long)i};
  for (int k = 0; k < 3; k++) {
    if (!ops[k]->name) {
      continue;
    }
    if (ops[k]->kind == IR_OPERAND_TEMP) {
      if (!ir_temp_value_map_set(temp_last, ops[k]->name, &pos)) {
        return 0;
      }
    } else if (ops[k]->kind == IR_OPERAND_SYMBOL) {
      if (!ir_temp_value_map_set(sym_last, ops[k]->name, &pos)) {
        return 0;
      }
    }
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    const IROperand *op = &ins->arguments[a];
    if (!op->name) {
      continue;
    }
    if (op->kind == IR_OPERAND_TEMP) {
      if (!ir_temp_value_map_set(temp_last, op->name, &pos)) {
        return 0;
      }
    } else if (op->kind == IR_OPERAND_SYMBOL) {
      if (!ir_temp_value_map_set(sym_last, op->name, &pos)) {
        return 0;
      }
    }
  }
  return 1;
}

/* Drop entries whose key is not read anywhere past `i`: they can never be
 * looked up again, but without pruning every label SNAPSHOT copies them --
 * which made this pass O(labels x live-entries), the dominant compile cost
 * on big inlined functions (4M cloned entries per iteration on a 4000-call
 * main). Pruning loses no soundness: it only removes facts. */
static void ir_cp_prune_dead_entries(IRTempValueMap *map,
                                     const IRTempValueMap *last_use,
                                     size_t i) {
  if (!map || map->count == 0) {
    return;
  }
  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRTempValueEntry *entry = &map->items[read];
    const IROperand *lu =
        entry->name ? ir_temp_value_map_lookup(last_use, entry->name) : NULL;
    if (!lu || lu->int_value <= (long long)i) {
      ir_temp_value_map_note_value_removed(map, &entry->value);
      mettle_free_string(entry->name);
      ir_operand_destroy(&entry->value);
      continue;
    }
    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }
  if (map->count != write) {
    map->count = write;
    ir_temp_value_map_reindex(map);
  }
}

static int ir_try_evaluate_integer_binary(const char *op, long long lhs,
                                          long long rhs, long long *result,
                                          int *folded);

/* Constant value of an operand under the current propagation maps, WITHOUT
 * rewriting the operand (a persistent rewrite would perturb the shape the
 * SIMD recognizers pattern-match downstream). */
static int ir_cp_operand_constant(const IRTempValueMap *temp_map,
                                  const IRTempValueMap *symbol_map,
                                  const IROperand *operand, long long *out) {
  if (operand->kind == IR_OPERAND_INT) {
    *out = operand->int_value;
    return 1;
  }
  const IROperand *known = NULL;
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    known = ir_temp_value_map_lookup(temp_map, operand->name);
  } else if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    known = ir_temp_value_map_lookup(symbol_map, operand->name);
  }
  if (known && known->kind == IR_OPERAND_INT) {
    *out = known->int_value;
    return 1;
  }
  return 0;
}

/* A fold runs in 64 bits, but the instruction's result type may be narrower,
 * and the value has to come back in that width or the next instruction reads a
 * number C never produced. `(int)15 << 28` is 0xF0000000, which is negative as
 * an int: folding it as 64-bit left it positive, so the `>> 28` that read it
 * back shifted in zeros and `(x << 28) >> 28` gave 15 instead of -1. */
static int ir_mtlc_type_is_unsigned_integer(const MtlcType *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
    return 1;
  default:
    return 0;
  }
}

static long long ir_narrow_folded(const MtlcType *type, long long value) {
  if (!type) {
    return value;
  }
  switch (type->kind) {
  case MTLC_TYPE_INT8: return (long long)(signed char)value;
  case MTLC_TYPE_INT16: return (long long)(short)value;
  case MTLC_TYPE_INT32: return (long long)(int)value;
  case MTLC_TYPE_UINT8: return (long long)(unsigned char)value;
  case MTLC_TYPE_UINT16: return (long long)(unsigned short)value;
  case MTLC_TYPE_UINT32: return (long long)(unsigned int)value;
  case MTLC_TYPE_BOOL: return value ? 1 : 0;
  default: return value;
  }
}

/* Fold `dest = a op b` to `dest <- C` the moment the propagation maps prove
 * both operands constant, INSIDE the propagation walk. Recording the result
 * immediately lets an entire arithmetic chain over one variable
 * (`m = m ^ K; m = m * P; ...` - the shape inlining a keyed helper with
 * constant arguments produces) collapse in a single pass instead of one
 * step per fixpoint round. Operands are consulted, never rewritten, so
 * partially-constant expressions keep their recognizable shape.
 *
 * Width discipline (the canonical-homes contract): a temp computes at the
 * full 64-bit width; a typed local's result wraps to its declared width on
 * the write. So a TEMP dest folds as-is, a SYMBOL dest folds only when its
 * declared type is known, with the result narrowed accordingly, and
 * comparisons fold at operand width regardless of dest. Sign-sensitive ops
 * are skipped when the instruction is unsigned: the evaluator computes
 * signed semantics. */
/* Declared integer width of a type name, as bits (0 = a full 64) plus
 * signedness. Returns 0 for NULL, or for anything that is not an integer. */
static int ir_declared_integer_width(const char *type, int *bits_out,
                                     int *unsigned_out) {
  static const struct {
    const char *name;
    int bits;
    int is_unsigned;
  } WIDTHS[] = {
      {"int64", 0, 0},  {"uint64", 0, 1},  {"int32", 32, 0}, {"uint32", 32, 1},
      {"int16", 16, 0}, {"uint16", 16, 1}, {"int8", 8, 0},   {"uint8", 8, 1},
      {"bool", 8, 1},
  };
  if (!type) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(WIDTHS) / sizeof(WIDTHS[0]); i++) {
    if (strcmp(type, WIDTHS[i].name) == 0) {
      *bits_out = WIDTHS[i].bits;
      *unsigned_out = WIDTHS[i].is_unsigned;
      return 1;
    }
  }
  return 0;
}

/* Wrap a constant assigned into a narrow integer local to that local's width.
 * The store truncates, so the wrapped value is what the next reader loads --
 * but propagation recorded (and forwarded) the unwrapped one. The fold below
 * narrows a BINARY that writes the home directly; `var b: int8 = a + 1` is
 * lowered as a BINARY into a temp followed by this ASSIGN, so it kept 128 and
 * a later `b == -128` folded false. */
static void ir_cp_wrap_assign_to_home(const IRFunction *function,
                                      IRInstruction *instruction,
                                      int *any_changed) {
  if (!instruction || instruction->op != IR_OP_ASSIGN ||
      instruction->is_float ||
      instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
      instruction->lhs.kind != IR_OPERAND_INT) {
    return;
  }
  int bits = 0;
  int is_unsigned = 0;
  const char *type =
      ir_function_local_declared_type((IRFunction *)function,
                                      instruction->dest.name);
  if (!ir_declared_integer_width(type, &bits, &is_unsigned) || bits == 0) {
    return;
  }
  long long value = instruction->lhs.int_value;
  int shift = 64 - bits;
  long long wrapped =
      is_unsigned ? (long long)(((unsigned long long)value << shift) >> shift)
                  : (value << shift) >> shift;
  if (wrapped != value) {
    instruction->lhs.int_value = wrapped;
    if (any_changed) {
      *any_changed = 1;
    }
  }
}

static void ir_cp_fold_constant_binary(const IRFunction *function,
                                       const IRTempValueMap *temp_map,
                                       const IRTempValueMap *symbol_map,
                                       IRInstruction *instruction,
                                       int *any_changed) {
  if (!instruction || instruction->op != IR_OP_BINARY ||
      instruction->is_float || !instruction->text) {
    return;
  }
  long long lhs = 0, rhs = 0;
  if (!ir_cp_operand_constant(temp_map, symbol_map, &instruction->lhs, &lhs) ||
      !ir_cp_operand_constant(temp_map, symbol_map, &instruction->rhs, &rhs)) {
    return;
  }
  const char *op = instruction->text;
  if (instruction->is_unsigned) {
    if (strcmp(op, ">>") == 0 || strcmp(op, "/") == 0 ||
        strcmp(op, "%") == 0 || strcmp(op, "<") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, ">=") == 0) {
      return;
    }
  }

  /* Result-width narrowing for typed symbol destinations. */
  int narrow_bits = 0; /* 0 = keep 64-bit */
  int narrow_unsigned = 0;
  int is_compare = strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                   strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0;
  if (instruction->dest.kind == IR_OPERAND_SYMBOL && !is_compare) {
    const char *type = instruction->dest.name
                           ? ir_function_local_declared_type(
                                 (IRFunction *)function,
                                 instruction->dest.name)
                           : NULL;
    /* No type is a global or untracked local, and anything that is not an
     * integer is a pointer/float/struct home: width unknown, don't fold. */
    if (!ir_declared_integer_width(type, &narrow_bits, &narrow_unsigned)) {
      return;
    }
  }
  /* The evaluator computes signed semantics, so a sign-sensitive op on an
   * unsigned destination must not fold here. instruction->is_unsigned only ever
   * gets set on loads, so this declared-type answer is the one that catches a
   * `uint64` local: `var c: uint64 = 10000000000000000000; c / 2` folded with a
   * signed divide and silently produced a negative result under -O while the
   * unoptimized build divided correctly. */
  if (narrow_unsigned &&
      (strcmp(op, ">>") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0 ||
       strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
       strcmp(op, ">=") == 0)) {
    return;
  }

  long long result = 0;
  int folded = 0;
  if (!ir_try_evaluate_integer_binary(op, lhs, rhs, &result, &folded) ||
      !folded) {
    return;
  }
  if (narrow_bits > 0) {
    int shift = 64 - narrow_bits;
    if (narrow_unsigned) {
      result = (long long)(((unsigned long long)result << shift) >> shift);
    } else {
      result = (result << shift) >> shift;
    }
  }
  if (narrow_bits == 0 && instruction->dest.kind != IR_OPERAND_SYMBOL) {
    /* A narrow temp is canonicalized at its definition (issue #13), and this
     * fold replaces that definition, so it has to hand back the same canonical
     * value. Without this `(x << 28) >> 28` folded to 0xF0000000 as a 64-bit
     * temp, and the arithmetic shift that read it back saw a positive number:
     * the textbook hand-rolled sign extension returned 15 instead of -1. */
    result = ir_narrow_folded(instruction->value_type, result);
  }
  ir_rewrite_to_assign_int(instruction, result, any_changed);
}

int ir_copy_and_constant_propagation_pass(IRFunction *function,
                                                 int *changed) {
  if (!function) {
    return 0;
  }

  IRTempValueMap map;
  IRSymbolValueMap symbol_map;
  if (!ir_temp_value_map_init(&map) || !ir_temp_value_map_init(&symbol_map)) {
    ir_temp_value_map_destroy(&map);
    ir_temp_value_map_destroy(&symbol_map);
    return 0;
  }

  IRLabelValueMap label_in;
  if (!ir_label_value_map_init(&label_in)) {
    ir_temp_value_map_destroy(&map);
    ir_temp_value_map_destroy(&symbol_map);
    return 0;
  }

  /* Address-taken symbol set for store invalidation, built once: this pass
   * never introduces ADDRESS_OF instructions, so it stays valid across every
   * iteration. (Per-store function rescans were a cubic term here.) Last-use
   * indexes (name -> last instruction position reading it) power the dead-
   * entry pruning at labels; rebuilt per iteration because propagation
   * rewrites operands. */
  IRTempValueMap addr_taken, temp_last_use, sym_last_use, storage_syms;
  if (!ir_temp_value_map_init(&addr_taken) ||
      !ir_temp_value_map_init(&temp_last_use) ||
      !ir_temp_value_map_init(&sym_last_use) ||
      !ir_temp_value_map_init(&storage_syms) ||
      !ir_addr_taken_set_build(function, &addr_taken) ||
      !ir_storage_symbol_set_build(function, &storage_syms)) {
    ir_label_value_map_destroy(&label_in);
    ir_temp_value_map_destroy(&map);
    ir_temp_value_map_destroy(&symbol_map);
    ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    ir_temp_value_map_destroy(&storage_syms);
    return 0;
  }

  int any_changed = 0;
  for (int iteration = 0; iteration < 8; iteration++) {
    int flow_changed = 0;
    ir_temp_value_map_clear(&map);
    ir_temp_value_map_clear(&symbol_map);
    ir_temp_value_map_clear(&temp_last_use);
    ir_temp_value_map_clear(&sym_last_use);
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (!ir_cp_note_operand_uses(&function->instructions[i], i,
                                   &temp_last_use, &sym_last_use)) {
        ir_label_value_map_destroy(&label_in);
        ir_temp_value_map_destroy(&map);
        ir_temp_value_map_destroy(&symbol_map);
        ir_temp_value_map_destroy(&addr_taken);
        ir_temp_value_map_destroy(&temp_last_use);
        ir_temp_value_map_destroy(&sym_last_use);
        return 0;
      }
    }

    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *instruction = &function->instructions[i];

      if (instruction->op == IR_OP_LABEL && instruction->text) {
        /* Entries nobody reads past this point would only bloat the label
         * snapshots below (every label clones the live map; unpruned, that
         * was O(labels x entries) -- the dominant compile cost on big
         * inlined functions). */
        ir_cp_prune_dead_entries(&map, &temp_last_use, i);
        ir_cp_prune_dead_entries(&symbol_map, &sym_last_use, i);

        /* The label is reachable from explicit jumps/branches *and* from
         * fall-through if the previous non-nop instruction is not a JUMP or
         * RETURN. Merge the fall-through map into label_in[L] first so the
         * load below is the intersection of every incoming flow. Without
         * this, a label after two writes "x <- 1 / jump L / x <- 0 / L:"
         * would inherit only the jump's map and wrongly conclude x == 1. */
        int fall_through = 1;
        for (size_t pi = i; pi > 0;) {
          pi--;
          IROpcode prev_op = function->instructions[pi].op;
          if (prev_op == IR_OP_NOP) {
            continue;
          }
          if (prev_op == IR_OP_JUMP || prev_op == IR_OP_RETURN) {
            fall_through = 0;
          }
          break;
        }
        if (i == 0) {
          fall_through = 0; /* first instruction has no predecessor */
        }

        if (fall_through) {
          if (!ir_label_value_map_merge_incoming(&label_in, instruction->text,
                                                 &map, &flow_changed)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    ir_temp_value_map_destroy(&storage_syms);
            return 0;
          }
        }

        const IRLabelValueEntry *entry =
            ir_label_value_map_lookup(&label_in, instruction->text);
        if (entry && entry->initialized) {
          if (!ir_temp_value_map_clone(&map, &entry->in_map)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    ir_temp_value_map_destroy(&storage_syms);
            return 0;
          }
        } else {
          ir_temp_value_map_clear(&map);
        }
        ir_temp_value_map_clear(&symbol_map);
      }

      if (!ir_propagate_instruction_operands(&map, &symbol_map, instruction,
                                             &any_changed)) {
        ir_label_value_map_destroy(&label_in);
        ir_temp_value_map_destroy(&map);
        ir_temp_value_map_destroy(&symbol_map);
        ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    ir_temp_value_map_destroy(&storage_syms);
        return 0;
      }

      ir_cp_fold_constant_binary(function, &map, &symbol_map, instruction,
                                 &any_changed);
      ir_cp_wrap_assign_to_home(function, instruction, &any_changed);

      if (ir_instruction_writes_temp(instruction) && instruction->dest.name) {
        ir_temp_value_map_remove(&map, instruction->dest.name);

        if (instruction->op == IR_OP_ASSIGN &&
            ir_operand_is_propagatable_value(&instruction->lhs)) {
          if (instruction->lhs.kind == IR_OPERAND_TEMP &&
              instruction->lhs.name &&
              strcmp(instruction->lhs.name, instruction->dest.name) == 0) {
            ir_temp_value_map_remove(&map, instruction->dest.name);
          } else if (!ir_temp_value_map_set(&map, instruction->dest.name,
                                            &instruction->lhs)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    ir_temp_value_map_destroy(&storage_syms);
            return 0;
          }
        }
      }

      if (ir_instruction_writes_symbol(instruction) && instruction->dest.name) {
        ir_temp_value_map_remove_symbol_values(&map, instruction->dest.name);
        ir_symbol_value_map_invalidate_name(&symbol_map,
                                            instruction->dest.name);

        if (instruction->op == IR_OP_ASSIGN &&
            ir_operand_is_propagatable_value(&instruction->lhs) &&
            instruction->dest.kind == IR_OPERAND_SYMBOL &&
            !ir_temp_value_map_lookup(&storage_syms, instruction->dest.name)) {
          if (instruction->lhs.kind == IR_OPERAND_SYMBOL &&
              instruction->lhs.name &&
              strcmp(instruction->lhs.name, instruction->dest.name) == 0) {
            ir_symbol_value_map_invalidate_name(&symbol_map,
                                                instruction->dest.name);
          } else if (!ir_temp_value_map_set(&symbol_map, instruction->dest.name,
                                            &instruction->lhs)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    ir_temp_value_map_destroy(&storage_syms);
            return 0;
          }
        }
      }

      if (instruction->op == IR_OP_ROTATE_ADD && instruction->dest.name) {
        ir_symbol_value_map_invalidate_name(&symbol_map, instruction->dest.name);
        if (instruction->lhs.name) {
          ir_symbol_value_map_invalidate_name(&symbol_map, instruction->lhs.name);
        }
        if (instruction->rhs.name) {
          ir_symbol_value_map_invalidate_name(&symbol_map, instruction->rhs.name);
        }
      }

      if (instruction->op == IR_OP_STORE) {
        ir_temp_value_map_invalidate_after_store(&map, &addr_taken);
        ir_temp_value_map_invalidate_after_store(&symbol_map, &addr_taken);
      } else if (instruction->op == IR_OP_CALL ||
                 instruction->op == IR_OP_CALL_INDIRECT ||
                 instruction->op == IR_OP_INLINE_ASM) {
        ir_temp_value_map_remove_symbol_values(&map, NULL);
        ir_temp_value_map_clear(&symbol_map);
      }


      if ((instruction->op == IR_OP_JUMP || instruction->op == IR_OP_BRANCH_ZERO ||
           instruction->op == IR_OP_BRANCH_EQ) &&
          instruction->text) {
        /* Same reasoning as the label prune: don't snapshot dead entries. */
        ir_cp_prune_dead_entries(&map, &temp_last_use, i);
        if (!ir_label_value_map_merge_incoming(&label_in, instruction->text,
                                               &map, &flow_changed)) {
          ir_label_value_map_destroy(&label_in);
          ir_temp_value_map_destroy(&map);
          ir_temp_value_map_destroy(&symbol_map);
          ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    ir_temp_value_map_destroy(&storage_syms);
          return 0;
        }
      }

      if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN) {
        ir_temp_value_map_clear(&map);
        ir_temp_value_map_clear(&symbol_map);
      }
    }

    if (!flow_changed) {
      break;
    }
  }

  if (changed && any_changed) {
    *changed = 1;
  }

  ir_label_value_map_destroy(&label_in);
  ir_temp_value_map_destroy(&map);
  ir_temp_value_map_destroy(&symbol_map);
  ir_temp_value_map_destroy(&storage_syms);
  ir_temp_value_map_destroy(&addr_taken);
  ir_temp_value_map_destroy(&temp_last_use);
  ir_temp_value_map_destroy(&sym_last_use);
  return 1;
}

static int ir_try_evaluate_integer_binary(const char *op, long long lhs,
                                          long long rhs, long long *result,
                                          int *folded) {
  if (!op || !result || !folded) {
    return 0;
  }

  *folded = 1;

  if (strcmp(op, "+") == 0) {
    *result = (long long)((unsigned long long)lhs + (unsigned long long)rhs);
  } else if (strcmp(op, "-") == 0) {
    *result = (long long)((unsigned long long)lhs - (unsigned long long)rhs);
  } else if (strcmp(op, "*") == 0) {
    *result = (long long)((unsigned long long)lhs * (unsigned long long)rhs);
  } else if (strcmp(op, "/") == 0) {
    if (rhs == 0) {
      *folded = 0;
    } else if (rhs == -1) {
      *result = (long long)(0ULL - (unsigned long long)lhs);
    } else {
      *result = lhs / rhs;
    }
  } else if (strcmp(op, "%") == 0) {
    if (rhs == 0) {
      *folded = 0;
    } else if (rhs == -1) {
      *result = 0;
    } else {
      *result = lhs % rhs;
    }
  } else if (strcmp(op, "==") == 0) {
    *result = lhs == rhs;
  } else if (strcmp(op, "!=") == 0) {
    *result = lhs != rhs;
  } else if (strcmp(op, "<") == 0) {
    *result = lhs < rhs;
  } else if (strcmp(op, "<=") == 0) {
    *result = lhs <= rhs;
  } else if (strcmp(op, ">") == 0) {
    *result = lhs > rhs;
  } else if (strcmp(op, ">=") == 0) {
    *result = lhs >= rhs;
  } else if (strcmp(op, "&&") == 0) {
    *result = lhs && rhs;
  } else if (strcmp(op, "||") == 0) {
    *result = lhs || rhs;
  } else if (strcmp(op, "&") == 0) {
    *result = lhs & rhs;
  } else if (strcmp(op, "|") == 0) {
    *result = lhs | rhs;
  } else if (strcmp(op, "^") == 0) {
    *result = lhs ^ rhs;
  } else if (strcmp(op, "<<") == 0) {
    if (rhs < 0 || rhs >= 64) {
      *folded = 0;
    } else {
      *result = (long long)((unsigned long long)lhs << (unsigned long long)rhs);
    }
  } else if (strcmp(op, ">>") == 0) {
    if (rhs < 0 || rhs >= 64) {
      *folded = 0;
    } else {
      *result = lhs >> rhs;
    }
  } else {
    *folded = 0;
  }

  return 1;
}

/* `x / 2^k` -> `x * 2^-k` for a float divisor that is an exact power of two.
 *
 * This is the one float identity that needs no reassociation licence. Both
 * forms scale by the same exact real value and round once, so they agree on
 * every input including NaN, both infinities, both zeros, and the subnormals
 * that round on the way out. Requiring the reciprocal to be normal is what
 * buys that: if 2^-k were subnormal the reciprocal would itself be rounded,
 * and the two forms would part company.
 *
 * A divide is 14 cycles and holds the divider for four; the multiply is four
 * cycles and fully pipelined. physics_grid pays two of these per particle per
 * step for `x / CELL_SIZE`. */
static int ir_pow2_reciprocal(double c, int float_bits, double *out) {
  uint64_t bits = 0;
  uint64_t sign = 0;
  int biased = 0;
  int limit = float_bits == 32 ? 126 : 1022;
  memcpy(&bits, &c, sizeof(bits));
  sign = bits & 0x8000000000000000ull;
  if ((bits & 0x000FFFFFFFFFFFFFull) != 0) {
    return 0; /* mantissa bits set: not a power of two */
  }
  biased = (int)((bits >> 52) & 0x7FFull);
  if (biased == 0 || biased == 0x7FF) {
    return 0; /* zero or subnormal, or an infinity/NaN */
  }
  {
    int k = biased - 1023;
    if (k > limit || k < -limit) {
      return 0;
    }
    bits = sign | ((uint64_t)(1023 - k) << 52);
  }
  memcpy(out, &bits, sizeof(bits));
  return 1;
}
/* The divisor's value, when it is one the compiler already knows: a float
 * literal, or a name bound to an immutable float global. Reading the global
 * here rather than folding every float const at its use site is deliberate.
 * Folding them all replaces one hoisted register with a materialization per
 * use, which cost physics_grid 3.65% and cancelled the entire divide win.
 * Only the divisor is worth substituting, because the rewrite consumes the
 * constant rather than adding one: the divide's constant becomes the
 * multiply's. */
static int ir_float_divisor_value(const IROperand *rhs, double *out) {
  const IRModuleSymbol *symbol;
  if (rhs->kind == IR_OPERAND_FLOAT) {
    *out = rhs->float_value;
    return 1;
  }
  if (rhs->kind != IR_OPERAND_SYMBOL || !rhs->name) {
    return 0;
  }
  symbol = ir_optimize_module_symbol(rhs->name);
  if (!symbol || symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
      !symbol->is_immutable || !symbol->has_initializer ||
      !symbol->init_is_float || symbol->init_symbol_ref) {
    return 0;
  }
  memcpy(out, &symbol->init_bits, sizeof(*out));
  return 1;
}

static int ir_try_fold_float_reciprocal(IRInstruction *instruction,
                                        int *changed) {
  double divisor = 0.0;
  double reciprocal = 0.0;
  if (!instruction || instruction->op != IR_OP_BINARY ||
      !instruction->is_float || !instruction->text ||
      strcmp(instruction->text, "/") != 0) {
    return 1;
  }
  if (!ir_float_divisor_value(&instruction->rhs, &divisor)) {
    return 1;
  }
  if (!ir_pow2_reciprocal(divisor, instruction->float_bits, &reciprocal)) {
    return 1;
  }
  mettle_free_string(instruction->text);
  instruction->text = mettle_strdup("*");
  if (!instruction->text) {
    return 0;
  }
  ir_operand_destroy(&instruction->rhs);
  instruction->rhs = ir_operand_float_sized(reciprocal,
                                            instruction->float_bits);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_try_fold_integer_binary(IRInstruction *instruction,
                                      IRValueRangeCtx *ranges, size_t at,
                                      int *changed) {
  if (!instruction || instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text) {
    return 1;
  }

  /* Same rule as the propagation-time fold above: signed evaluation cannot
   * stand in for an unsigned divide, remainder, shift, or ordering. */
  if ((instruction->is_unsigned ||
       ir_mtlc_type_is_unsigned_integer(instruction->value_type)) &&
      (strcmp(instruction->text, ">>") == 0 ||
       strcmp(instruction->text, "/") == 0 ||
       strcmp(instruction->text, "%") == 0 ||
       strcmp(instruction->text, "<") == 0 ||
       strcmp(instruction->text, "<=") == 0 ||
       strcmp(instruction->text, ">") == 0 ||
       strcmp(instruction->text, ">=") == 0)) {
    return ir_rewrite_apply_binary_identities(instruction, ranges, at, changed);
  }

  if (instruction->lhs.kind == IR_OPERAND_INT &&
      instruction->rhs.kind == IR_OPERAND_INT) {
    long long result = 0;
    int folded = 0;
    if (!ir_try_evaluate_integer_binary(instruction->text,
                                        instruction->lhs.int_value,
                                        instruction->rhs.int_value, &result,
                                        &folded)) {
      return 0;
    }
    if (folded) {
      return ir_rewrite_to_assign_int(
          instruction, ir_narrow_folded(instruction->value_type, result),
          changed);
    }
  }

  /* Every algebraic identity now lives in the declarative table in
   * ir_optimize_rewrite.c; add a rule there to teach a new one. Facts that
   * depend on WHERE the instruction sits (a dividend that cannot be negative,
   * a mask that covers every reachable bit) belong to the range analysis in
   * ir_optimize_value_range.c instead. */
  return ir_rewrite_apply_binary_identities(instruction, ranges, at, changed);
}

int ir_find_next_non_nop_in_block(const IRFunction *function,
                                         size_t start_index, size_t *out_index) {
  if (!function || !out_index || start_index >= function->instruction_count) {
    return 0;
  }

  size_t i = start_index;
  while (i < function->instruction_count) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      return 0;
    }
    if (instruction->op != IR_OP_NOP) {
      *out_index = i;
      return 1;
    }
    i++;
  }
  return 0;
}

int ir_find_next_non_nop(const IRFunction *function, size_t start_index,
                                size_t *out_index) {
  if (!function || !out_index || start_index >= function->instruction_count) {
    return 0;
  }

  size_t i = start_index;
  while (i < function->instruction_count) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_NOP) {
      *out_index = i;
      return 1;
    }
    i++;
  }
  return 0;
}

int ir_find_label_index(const IRFunction *function, const char *label,
                               size_t *out_index) {
  if (!function || !label || !out_index) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, label) == 0) {
      *out_index = i;
      return 1;
    }
  }

  return 0;
}

static int ir_branch_zero_not_equal_zero_forwarding_pass(IRFunction *function,
                                                         int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *cmp = &function->instructions[i];
    if (cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
        strcmp(cmp->text, "!=") != 0 || cmp->dest.kind != IR_OPERAND_TEMP ||
        !cmp->dest.name) {
      continue;
    }

    if (ir_temp_use_map_get(&uses, cmp->dest.name) != 1) {
      continue;
    }

    const IROperand *forward = NULL;
    if (cmp->rhs.kind == IR_OPERAND_INT && cmp->rhs.int_value == 0) {
      forward = &cmp->lhs;
    } else if (cmp->lhs.kind == IR_OPERAND_INT && cmp->lhs.int_value == 0) {
      forward = &cmp->rhs;
    } else {
      continue;
    }

    size_t branch_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &branch_index)) {
      continue;
    }

    IRInstruction *branch = &function->instructions[branch_index];
    if (branch->op != IR_OP_BRANCH_ZERO || branch->lhs.kind != IR_OPERAND_TEMP ||
        !branch->lhs.name || strcmp(branch->lhs.name, cmp->dest.name) != 0) {
      continue;
    }

    IROperand cloned = ir_operand_none();
    if (!ir_operand_clone(forward, &cloned)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
    ir_operand_destroy(&branch->lhs);
    branch->lhs = cloned;

    ir_instruction_make_nop(cmp);
    if (changed) {
      *changed = 1;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

static int ir_rewrite_branch_eq_shortcut(IRInstruction *producer,
                                         IRInstruction *branch_zero,
                                         IRInstruction *jump_true,
                                         int *changed) {
  if (!producer || !branch_zero || !jump_true || !jump_true->text) {
    return 0;
  }

  IROperand lhs = ir_operand_none();
  IROperand rhs = ir_operand_none();
  if (!ir_operand_clone(&producer->lhs, &lhs) ||
      !ir_operand_clone(&producer->rhs, &rhs)) {
    ir_operand_destroy(&lhs);
    ir_operand_destroy(&rhs);
    return 0;
  }

  char *target = mettle_strdup(jump_true->text);
  if (!target) {
    ir_operand_destroy(&lhs);
    ir_operand_destroy(&rhs);
    return 0;
  }

  ir_operand_destroy(&branch_zero->lhs);
  ir_operand_destroy(&branch_zero->rhs);
  mettle_free_string(branch_zero->text);
  ir_instruction_clear_arguments(branch_zero);
  branch_zero->op = IR_OP_BRANCH_EQ;
  branch_zero->lhs = lhs;
  branch_zero->rhs = rhs;
  branch_zero->text = target;
  branch_zero->is_float = producer->is_float;
  branch_zero->ast_ref = NULL;

  ir_instruction_make_nop(producer);
  ir_instruction_make_nop(jump_true);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_branch_eq_chain_shortcut_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *producer = &function->instructions[i];
    if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text ||
        strcmp(producer->text, "==") != 0 ||
        producer->dest.kind != IR_OPERAND_TEMP || !producer->dest.name) {
      continue;
    }

    if (ir_temp_use_map_get(&uses, producer->dest.name) != 1) {
      continue;
    }

    size_t branch_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &branch_index)) {
      continue;
    }
    IRInstruction *branch = &function->instructions[branch_index];
    if (branch->op != IR_OP_BRANCH_ZERO || branch->lhs.kind != IR_OPERAND_TEMP ||
        !branch->lhs.name ||
        strcmp(branch->lhs.name, producer->dest.name) != 0 || !branch->text) {
      continue;
    }

    size_t jump_index = 0;
    if (!ir_find_next_non_nop_in_block(function, branch_index + 1, &jump_index)) {
      continue;
    }
    IRInstruction *jump_true = &function->instructions[jump_index];
    if (jump_true->op != IR_OP_JUMP || !jump_true->text) {
      continue;
    }

    size_t false_label_index = 0;
    if (!ir_find_next_non_nop(function, jump_index + 1, &false_label_index)) {
      continue;
    }
    IRInstruction *false_label = &function->instructions[false_label_index];
    if (false_label->op != IR_OP_LABEL || !false_label->text ||
        strcmp(false_label->text, branch->text) != 0) {
      continue;
    }

    if (!ir_rewrite_branch_eq_shortcut(producer, branch, jump_true, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

#define IR_BITSET_MIN_KEYS 4
#define IR_BITSET_MAX_KEY 62
#define IR_BITSET_SLOTS 7

static int ir_bitset_same_operand(const IROperand *a, const IROperand *b) {
  return a->kind == b->kind && a->name && b->name &&
         strcmp(a->name, b->name) == 0 &&
         (a->kind == IR_OPERAND_TEMP || a->kind == IR_OPERAND_SYMBOL);
}

static int ir_bitset_key_ok(long long *keys, size_t count, long long key) {
  if (key < 0 || key > IR_BITSET_MAX_KEY) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    if (keys[i] == key) {
      return 0;
    }
  }
  return 1;
}

static int ir_bitset_emit(IRFunction *function, size_t at, size_t last,
                          const IROperand *value, long long mask,
                          long long max_key, const char *miss, int *changed) {
  static int counter;
  char guard_name[48];
  char one_name[48];
  char shift_name[48];
  char mask_name[48];
  char and_name[48];
  int id = counter++;
  IRInstruction *slot;
  snprintf(guard_name, sizeof(guard_name), "__bset_g%d", id);
  snprintf(one_name, sizeof(one_name), "__bset_o%d", id);
  snprintf(shift_name, sizeof(shift_name), "__bset_s%d", id);
  snprintf(mask_name, sizeof(mask_name), "__bset_m%d", id);
  snprintf(and_name, sizeof(and_name), "__bset_a%d", id);

  for (size_t k = at; k <= last; k++) {
    ir_instruction_make_nop(&function->instructions[k]);
  }

  slot = &function->instructions[at];
  slot->op = IR_OP_BINARY;
  slot->text = mettle_strdup("<=");
  slot->dest = ir_operand_temp(guard_name);
  slot->lhs = ir_operand_copy(value);
  slot->rhs = ir_operand_int(max_key);
  slot->is_unsigned = 1;

  slot = &function->instructions[at + 1];
  slot->op = IR_OP_BRANCH_ZERO;
  slot->lhs = ir_operand_temp(guard_name);
  slot->text = mettle_strdup(miss);

  slot = &function->instructions[at + 2];
  slot->op = IR_OP_ASSIGN;
  slot->dest = ir_operand_temp(one_name);
  slot->lhs = ir_operand_int(1);

  slot = &function->instructions[at + 3];
  slot->op = IR_OP_BINARY;
  slot->text = mettle_strdup("<<");
  slot->dest = ir_operand_temp(shift_name);
  slot->lhs = ir_operand_temp(one_name);
  slot->rhs = ir_operand_copy(value);

  slot = &function->instructions[at + 4];
  slot->op = IR_OP_ASSIGN;
  slot->dest = ir_operand_temp(mask_name);
  slot->lhs = ir_operand_int(mask);

  slot = &function->instructions[at + 5];
  slot->op = IR_OP_BINARY;
  slot->text = mettle_strdup("&");
  slot->dest = ir_operand_temp(and_name);
  slot->lhs = ir_operand_temp(shift_name);
  slot->rhs = ir_operand_temp(mask_name);

  slot = &function->instructions[at + 6];
  slot->op = IR_OP_BRANCH_ZERO;
  slot->lhs = ir_operand_temp(and_name);
  slot->text = mettle_strdup(miss);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

/* Folds ONE chain and reports whether it did. The emit rewrites the
 * instruction stream and invalidates the use map built above it, so a
 * function holding several chains -- which is what inlining a predicate
 * like `skip_ws` into every caller produces -- needs one call per chain. */
static int ir_bitset_match_chain(const IRFunction *function,
                                 const IRTempUseMap *uses, size_t index,
                                 long long *keys, size_t *key_count,
                                 size_t *branch_out) {
  const IRInstruction *first = &function->instructions[index];
  const IRInstruction *cmp;
  const IRInstruction *br;
  const IRInstruction *lab;
  size_t scan;
  size_t tail;
  size_t branch;
  size_t label;

  *key_count = 0;
  if (first->op != IR_OP_BRANCH_EQ || first->is_float || !first->text ||
      first->rhs.kind != IR_OPERAND_INT ||
      (first->lhs.kind != IR_OPERAND_TEMP &&
       first->lhs.kind != IR_OPERAND_SYMBOL) ||
      !first->lhs.name) {
    return 0;
  }

  scan = index;
  while (scan < function->instruction_count) {
    const IRInstruction *ins = &function->instructions[scan];
    if (ins->op == IR_OP_NOP) {
      scan++;
      continue;
    }
    if (ins->op != IR_OP_BRANCH_EQ || ins->is_float || !ins->text ||
        strcmp(ins->text, first->text) != 0 ||
        ins->rhs.kind != IR_OPERAND_INT ||
        !ir_bitset_same_operand(&ins->lhs, &first->lhs) ||
        !ir_bitset_key_ok(keys, *key_count, ins->rhs.int_value)) {
      break;
    }
    keys[(*key_count)++] = ins->rhs.int_value;
    scan++;
  }
  if (*key_count == 0 || !ir_find_next_non_nop(function, scan, &tail)) {
    return 0;
  }

  cmp = &function->instructions[tail];
  if (cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
      strcmp(cmp->text, "==") != 0 || cmp->rhs.kind != IR_OPERAND_INT ||
      cmp->dest.kind != IR_OPERAND_TEMP || !cmp->dest.name ||
      !ir_bitset_same_operand(&cmp->lhs, &first->lhs) ||
      !ir_bitset_key_ok(keys, *key_count, cmp->rhs.int_value) ||
      ir_temp_use_map_get(uses, cmp->dest.name) != 1) {
    return 0;
  }
  keys[(*key_count)++] = cmp->rhs.int_value;

  if (!ir_find_next_non_nop(function, tail + 1, &branch)) {
    return 0;
  }
  br = &function->instructions[branch];
  if (br->op != IR_OP_BRANCH_ZERO || br->lhs.kind != IR_OPERAND_TEMP ||
      !br->lhs.name || !br->text ||
      strcmp(br->lhs.name, cmp->dest.name) != 0) {
    return 0;
  }

  if (!ir_find_next_non_nop(function, branch + 1, &label)) {
    return 0;
  }
  lab = &function->instructions[label];
  if (lab->op != IR_OP_LABEL || !lab->text ||
      strcmp(lab->text, first->text) != 0) {
    return 0;
  }
  if (*key_count < IR_BITSET_MIN_KEYS) {
    return 0;
  }
  *branch_out = branch;
  return 1;
}

static int ir_bitset_rewrite_chain(IRFunction *function, size_t index,
                                   size_t branch, const long long *keys,
                                   size_t key_count, int *changed) {
  /* Both are read out of the array before anything is inserted into it: the
   * chain is rewritten in place, and when it is shorter than the replacement
   * the room has to be opened first. The old alternative was to decline, which
   * made the fold depend on how many neighbouring instructions some earlier
   * pass happened to retire. */
  IROperand value = ir_operand_copy(&function->instructions[index].lhs);
  char *miss = mettle_strdup(function->instructions[branch].text);
  size_t have = branch - index + 1;
  long long mask = 0;
  long long max_key = 0;
  int ok = value.name && miss != NULL;

  for (size_t k = 0; k < key_count; k++) {
    mask |= 1LL << keys[k];
    if (keys[k] > max_key) {
      max_key = keys[k];
    }
  }

  if (ok && have < IR_BITSET_SLOTS) {
    size_t extra = IR_BITSET_SLOTS - have;
    IRInstruction nop = {0};

    nop.op = IR_OP_NOP;
    nop.location = function->instructions[index].location;
    for (size_t k = 0; k < extra && ok; k++) {
      ok = ir_function_insert_instruction(function, index, &nop);
    }
    if (ok) {
      branch += extra;
    }
  }
  ok = ok && ir_bitset_emit(function, index, branch, &value, mask, max_key,
                            miss, changed);
  ir_operand_destroy(&value);
  mettle_free_string(miss);
  return ok;
}

static int ir_or_chain_to_bitset_once(IRFunction *function, int *changed,
                                      int *folded) {
  IRTempUseMap uses;

  *folded = 0;
  if (!function) {
    return 1;
  }
  if (!ir_temp_use_map_init(&uses)) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 1;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    long long keys[IR_BITSET_MAX_KEY + 2];
    size_t key_count = 0;
    size_t branch = 0;
    if (!ir_bitset_match_chain(function, &uses, i, keys, &key_count,
                               &branch)) {
      continue;
    }
    *folded =
        ir_bitset_rewrite_chain(function, i, branch, keys, key_count, changed);
    ir_temp_use_map_destroy(&uses);
    return 1;
  }
  ir_temp_use_map_destroy(&uses);
  return 1;
}

int ir_or_chain_to_bitset_pass(IRFunction *function, int *changed) {
  for (int round = 0; round < 64; round++) {
    int folded = 0;
    if (!ir_or_chain_to_bitset_once(function, changed, &folded)) {
      return 0;
    }
    if (!folded) {
      break;
    }
  }
  return 1;
}

static int ir_simplify_redundant_assign(IRInstruction *instruction,
                                        int *changed) {
  if (!instruction || instruction->op != IR_OP_ASSIGN ||
      instruction->dest.kind == IR_OPERAND_NONE ||
      instruction->lhs.kind == IR_OPERAND_NONE) {
    return 1;
  }

  if (ir_operand_equals(&instruction->dest, &instruction->lhs)) {
    ir_instruction_make_nop(instruction);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}

static int ir_try_fold_integer_unary(IRInstruction *instruction, int *changed) {
  if (!instruction || instruction->op != IR_OP_UNARY || instruction->is_float ||
      !instruction->text || instruction->lhs.kind != IR_OPERAND_INT) {
    return 1;
  }

  long long value = instruction->lhs.int_value;
  long long result = 0;
  int fold = 1;

  if (strcmp(instruction->text, "+") == 0) {
    result = value;
  } else if (strcmp(instruction->text, "-") == 0) {
    result = (long long)(-(unsigned long long)value);
  } else if (strcmp(instruction->text, "!") == 0) {
    result = !value;
  } else if (strcmp(instruction->text, "~") == 0) {
    result = ~value;
  } else {
    fold = 0;
  }

  if (fold) {
    return ir_rewrite_to_assign_int(instruction, result, changed);
  }

  return 1;
}

static int ir_simplify_branch(IRInstruction *instruction, int *changed) {
  if (!instruction) {
    return 0;
  }

  if (instruction->op == IR_OP_BRANCH_ZERO &&
      instruction->lhs.kind == IR_OPERAND_INT) {
    if (instruction->lhs.int_value == 0) {
      ir_instruction_make_jump(instruction);
    } else {
      ir_instruction_make_nop(instruction);
    }
    if (changed) {
      *changed = 1;
    }
  } else if (instruction->op == IR_OP_BRANCH_EQ &&
             instruction->lhs.kind == IR_OPERAND_INT &&
             instruction->rhs.kind == IR_OPERAND_INT) {
    if (instruction->lhs.int_value == instruction->rhs.int_value) {
      ir_instruction_make_jump(instruction);
    } else {
      ir_instruction_make_nop(instruction);
    }
    if (changed) {
      *changed = 1;
    }
  } else if (instruction->op == IR_OP_BRANCH_EQ &&
             ir_operand_equals(&instruction->lhs, &instruction->rhs)) {
    ir_instruction_make_jump(instruction);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}

static int ir_float_pow2_reciprocal(const IROperand *operand, double *out) {
  unsigned long long bits = 0;
  double value = operand->float_value;
  int exponent;
  memcpy(&bits, &value, sizeof(bits));
  if ((bits & 0x000FFFFFFFFFFFFFULL) != 0ULL) {
    return 0;
  }
  exponent = (int)((bits >> 52) & 0x7FFULL);
  if (operand->float_bits == 32) {
    if (exponent < 923 || exponent > 1123) {
      return 0;
    }
  } else if (exponent < 1 || exponent > 2045) {
    return 0;
  }
  *out = 1.0 / value;
  return 1;
}

int ir_float_divide_by_power_of_two_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    double reciprocal = 0.0;
    char *product;
    if (instruction->op != IR_OP_BINARY || !instruction->is_float ||
        !instruction->text || strcmp(instruction->text, "/") != 0 ||
        instruction->rhs.kind != IR_OPERAND_FLOAT ||
        !ir_float_pow2_reciprocal(&instruction->rhs, &reciprocal)) {
      continue;
    }
    product = mettle_strdup("*");
    if (!product) {
      return 0;
    }
    mettle_free_string(instruction->text);
    instruction->text = product;
    instruction->rhs.float_value = reciprocal;
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}

int ir_constant_and_branch_simplify_pass(IRFunction *function,
                                                int *changed) {
  if (!function) {
    return 0;
  }

  int has_mod = 0;
  int has_eq = 0;
  int has_ne = 0;
  int has_branch_zero = 0;
  int has_jump = 0;
  int has_label = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    switch (instruction->op) {
    case IR_OP_BINARY:
      if (instruction->text) {
        if (strcmp(instruction->text, "%") == 0) {
          has_mod = 1;
        } else if (strcmp(instruction->text, "==") == 0) {
          has_eq = 1;
        } else if (strcmp(instruction->text, "!=") == 0) {
          has_ne = 1;
        }
      }
      break;
    case IR_OP_BRANCH_ZERO:
      has_branch_zero = 1;
      break;
    case IR_OP_JUMP:
      has_jump = 1;
      break;
    case IR_OP_LABEL:
      has_label = 1;
      break;
    default:
      break;
    }
  }

  if (has_mod && (has_eq || has_ne || has_branch_zero)) {
    if (!ir_remainder_zero_test_to_mask_pass(function, changed)) {
      return 0;
    }
  }
  if (has_eq && has_branch_zero && has_jump && has_label) {
    if (!ir_branch_eq_chain_shortcut_pass(function, changed)) {
      return 0;
    }
  }
  if (has_ne && has_branch_zero) {
    if (!ir_branch_zero_not_equal_zero_forwarding_pass(function, changed)) {
      return 0;
    }
  }

  /* The range analysis is what proves the facts the table cannot express as a
   * pattern on operand slots -- "this dividend is never negative", "this mask
   * covers every bit the value can carry", "this comparison is already
   * decided". It builds its tables on the first question, so a function with
   * no divide, mask, or bounded comparison never pays for it. */
  IRValueRangeCtx ranges;
  ir_value_range_ctx_init(&ranges, function);

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (!ir_try_fold_integer_binary(instruction, &ranges, i, changed) ||
        !ir_try_fold_float_reciprocal(instruction, changed) ||
        !ir_try_fold_integer_unary(instruction, changed) ||
        !ir_value_range_simplify(&ranges, i, instruction, changed) ||
        !ir_simplify_redundant_assign(instruction, changed) ||
        !ir_simplify_branch(instruction, changed)) {
      ir_value_range_ctx_destroy(&ranges);
      return 0;
    }
  }

  ir_value_range_ctx_destroy(&ranges);
  return 1;
}

int ir_remove_redundant_jumps_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_JUMP || !instruction->text) {
      continue;
    }

    size_t next = i + 1;
    while (next < function->instruction_count &&
           function->instructions[next].op == IR_OP_NOP) {
      next++;
    }

    if (next < function->instruction_count &&
        function->instructions[next].op == IR_OP_LABEL &&
        function->instructions[next].text &&
        strcmp(function->instructions[next].text, instruction->text) == 0) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

/* Label name -> its instruction index, collected in one walk.
 *
 * Jump threading resolves a branch's target label, then the target of the jump
 * it lands on, up to a bounded depth. Resolving each by scanning the function
 * is quadratic in a function that is mostly branches. Threading rewrites branch
 * targets only -- it never moves or retires a label -- so one map serves the
 * whole pass. */
typedef struct {
  const char **names;
  size_t *indices;
  size_t capacity;
} IRLabelPosIndex;

static void ir_label_pos_index_destroy(IRLabelPosIndex *index) {
  if (!index) {
    return;
  }
  free(index->names);
  free(index->indices);
  index->names = NULL;
  index->indices = NULL;
  index->capacity = 0;
}

static int ir_label_pos_index_build(const IRFunction *function,
                                    IRLabelPosIndex *index) {
  size_t capacity = 16;
  size_t mask;

  index->names = NULL;
  index->indices = NULL;
  index->capacity = 0;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  while (capacity < function->instruction_count * 2) {
    capacity *= 2;
  }
  index->names = (const char **)calloc(capacity, sizeof(*index->names));
  index->indices = (size_t *)calloc(capacity, sizeof(*index->indices));
  if (!index->names || !index->indices) {
    ir_label_pos_index_destroy(index);
    return 0;
  }
  index->capacity = capacity;
  mask = capacity - 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t slot;
    if (instruction->op != IR_OP_LABEL || !instruction->text) {
      continue;
    }
    slot = (size_t)mettle_fnv1a_hash(instruction->text) & mask;
    while (index->names[slot]) {
      if (strcmp(index->names[slot], instruction->text) == 0) {
        break; /* the first definition wins, as the scan did */
      }
      slot = (slot + 1) & mask;
    }
    if (!index->names[slot]) {
      index->names[slot] = instruction->text;
      index->indices[slot] = i;
    }
  }
  return 1;
}

static int ir_label_pos_index_find(const IRLabelPosIndex *index,
                                   const char *name, size_t *out_index) {
  size_t mask;
  size_t slot;

  if (!index->capacity || !name) {
    return 0;
  }
  mask = index->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (index->names[slot]) {
    if (strcmp(index->names[slot], name) == 0) {
      *out_index = index->indices[slot];
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

int ir_thread_jump_targets_pass(IRFunction *function, int *changed) {
  IRLabelPosIndex labels;

  if (!function) {
    return 0;
  }
  if (!ir_label_pos_index_build(function, &labels)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if ((instruction->op != IR_OP_JUMP &&
         instruction->op != IR_OP_BRANCH_ZERO &&
         instruction->op != IR_OP_BRANCH_EQ) ||
        !instruction->text) {
      continue;
    }

    const char *current_target = instruction->text;
    for (int depth = 0; depth < 32; depth++) {
      size_t label_index = 0;
      if (!ir_label_pos_index_find(&labels, current_target, &label_index)) {
        break;
      }

      size_t next_index = 0;
      if (!ir_find_next_non_nop(function, label_index + 1, &next_index)) {
        break;
      }

      IRInstruction *next = &function->instructions[next_index];
      if (next->op != IR_OP_JUMP || !next->text ||
          strcmp(next->text, current_target) == 0) {
        break;
      }

      current_target = next->text;
    }

    if (strcmp(current_target, instruction->text) != 0) {
      char *target_copy = mettle_strdup(current_target);
      if (!target_copy) {
        ir_label_pos_index_destroy(&labels);
        return 0;
      }
      mettle_free_string(instruction->text);
      instruction->text = target_copy;
      if (changed) {
        *changed = 1;
      }
    }
  }

  ir_label_pos_index_destroy(&labels);
  return 1;
}

int ir_remove_redundant_fallthrough_branches_pass(IRFunction *function,
                                                         int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if ((instruction->op != IR_OP_BRANCH_ZERO &&
         instruction->op != IR_OP_BRANCH_EQ) ||
        !instruction->text) {
      continue;
    }

    size_t next = i + 1;
    while (next < function->instruction_count &&
           function->instructions[next].op == IR_OP_NOP) {
      next++;
    }

    if (next < function->instruction_count &&
        function->instructions[next].op == IR_OP_LABEL &&
        function->instructions[next].text &&
        strcmp(function->instructions[next].text, instruction->text) == 0) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

int ir_remove_empty_conditional_diamonds_pass(IRFunction *function,
                                                     int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *branch = &function->instructions[i];
    if ((branch->op != IR_OP_BRANCH_ZERO && branch->op != IR_OP_BRANCH_EQ) ||
        !branch->text) {
      continue;
    }

    size_t jump_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &jump_index)) {
      continue;
    }

    IRInstruction *jump = &function->instructions[jump_index];
    if (jump->op != IR_OP_JUMP || !jump->text) {
      continue;
    }

    if (strcmp(branch->text, jump->text) == 0) {
      /* `branch -> L; jump L` looks redundant (both paths reach L), but only if
       * the `jump` is genuinely this branch's diamond-closer -- i.e. the very
       * next non-nop instruction is `label L` itself. If some OTHER label
       * intervenes (e.g. this is an empty nested then-arm immediately followed
       * by an enclosing if's else-entry label), the `jump` is the then-arm's
       * skip-over-the-else and removing it makes the else run unconditionally
       * (silent miscompile). Guard against that. */
      size_t after_jump = jump_index + 1;
      while (after_jump < function->instruction_count &&
             function->instructions[after_jump].op == IR_OP_NOP) {
        after_jump++;
      }
      if (after_jump < function->instruction_count &&
          function->instructions[after_jump].op == IR_OP_LABEL &&
          function->instructions[after_jump].text &&
          strcmp(function->instructions[after_jump].text, jump->text) == 0) {
        ir_instruction_make_nop(branch);
        ir_instruction_make_nop(jump);
        if (changed) {
          *changed = 1;
        }
      }
      continue;
    }

    size_t cursor = jump_index + 1;
    while (cursor < function->instruction_count &&
           function->instructions[cursor].op == IR_OP_NOP) {
      cursor++;
    }

    if (cursor >= function->instruction_count ||
        function->instructions[cursor].op != IR_OP_LABEL ||
        !function->instructions[cursor].text ||
        strcmp(function->instructions[cursor].text, branch->text) != 0) {
      continue;
    }

    cursor++;
    int reaches_jump_target_with_no_body = 0;
    while (cursor < function->instruction_count) {
      IRInstruction *probe = &function->instructions[cursor];
      if (probe->op == IR_OP_NOP) {
        cursor++;
        continue;
      }
      if (probe->op != IR_OP_LABEL || !probe->text) {
        break;
      }
      if (strcmp(probe->text, jump->text) == 0) {
        reaches_jump_target_with_no_body = 1;
        break;
      }
      cursor++;
    }

    if (!reaches_jump_target_with_no_body) {
      continue;
    }

    ir_instruction_make_nop(branch);
    ir_instruction_make_nop(jump);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}

int ir_eliminate_unreachable_blocks_pass(IRFunction *function,
                                                int *changed) {
  if (!function) {
    return 0;
  }
  if (function->instruction_count == 0) {
    return 1;
  }

  if (!ir_function_rebuild_cfg(function)) {
    return 0;
  }

  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0) {
    return block_count == 0;
  }

  unsigned char *reachable_blocks = calloc(block_count, 1);
  unsigned char *reachable_instructions = calloc(function->instruction_count, 1);
  if (!reachable_blocks || !reachable_instructions) {
    free(reachable_blocks);
    free(reachable_instructions);
    return 0;
  }

  IRIndexVector worklist = {0};
  if (!ir_index_vector_append(&worklist, function->entry_block)) {
    ir_index_vector_destroy(&worklist);
    free(reachable_blocks);
    free(reachable_instructions);
    return 0;
  }

  for (size_t work_index = 0; work_index < worklist.count; work_index++) {
    size_t block_index = worklist.items[work_index];
    if (block_index >= block_count || reachable_blocks[block_index]) {
      continue;
    }

    const IRBasicBlock *block = &blocks[block_index];
    reachable_blocks[block_index] = 1;
    for (size_t i = 0; i < block->instruction_count; i++) {
      size_t instruction_index = block->first_instruction + i;
      if (instruction_index < function->instruction_count) {
        reachable_instructions[instruction_index] = 1;
      }
    }

    for (size_t i = 0; i < block->successor_count; i++) {
      if (!ir_index_vector_append(&worklist, block->successors[i])) {
        ir_index_vector_destroy(&worklist);
        free(reachable_blocks);
        free(reachable_instructions);
        return 0;
      }
    }
  }

  int local_changed = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!reachable_instructions[i] &&
        function->instructions[i].op != IR_OP_NOP) {
      ir_instruction_make_nop(&function->instructions[i]);
      local_changed = 1;
      if (changed) {
        *changed = 1;
      }
    }
  }

  if (local_changed) {
    ir_function_clear_cfg(function);
  }

  ir_index_vector_destroy(&worklist);
  free(reachable_blocks);
  free(reachable_instructions);
  return 1;
}

static int ir_instruction_references_label(const IRInstruction *instruction,
                                           const char *label) {
  if (!instruction || !label || !instruction->text) {
    return 0;
  }

  if (instruction->op != IR_OP_JUMP && instruction->op != IR_OP_BRANCH_ZERO &&
      instruction->op != IR_OP_BRANCH_EQ) {
    return 0;
  }

  return strcmp(instruction->text, label) == 0;
}

static int ir_label_only_referenced_by(const IRFunction *function,
                                       const char *label,
                                       size_t expected_index) {
  int found = 0;
  if (!function || !label || expected_index >= function->instruction_count) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_instruction_references_label(&function->instructions[i], label)) {
      continue;
    }
    if (i != expected_index) {
      return 0;
    }
    found = 1;
  }
  return found;
}

static int ir_ascii_range_compare(const IRInstruction *instruction,
                                  const char *op, long long bound,
                                  const IROperand *value) {
  return instruction && instruction->op == IR_OP_BINARY &&
         !instruction->is_float && !instruction->is_unsigned &&
         instruction->text && strcmp(instruction->text, op) == 0 &&
         instruction->dest.kind == IR_OPERAND_TEMP &&
         instruction->dest.name && instruction->rhs.kind == IR_OPERAND_INT &&
         instruction->rhs.int_value == bound &&
         ir_bitset_same_operand(&instruction->lhs, value);
}

static int ir_ascii_range_branch(const IRInstruction *branch,
                                 const IRInstruction *compare) {
  return branch && compare && branch->op == IR_OP_BRANCH_ZERO &&
         branch->text && branch->lhs.kind == IR_OPERAND_TEMP &&
         branch->lhs.name && compare->dest.kind == IR_OPERAND_TEMP &&
         compare->dest.name &&
         strcmp(branch->lhs.name, compare->dest.name) == 0;
}

static int ir_ascii_next(const IRFunction *function, size_t after,
                         size_t *index) {
  return after < function->instruction_count &&
         ir_find_next_non_nop(function, after, index);
}

static int ir_ascii_casefold_rewrite(IRFunction *function,
                                     const size_t index[14], int *changed) {
  IRInstruction *lo1 = &function->instructions[index[0]];
  IRInstruction *lo1_branch = &function->instructions[index[1]];
  IRInstruction *hi1 = &function->instructions[index[2]];
  IRInstruction *hi1_branch = &function->instructions[index[3]];
  IRInstruction *lo2 = &function->instructions[index[7]];
  IRInstruction *miss_label = &function->instructions[index[13]];
  IROperand value = ir_operand_copy(&lo1->lhs);
  IROperand fold = ir_operand_copy(&lo1->dest);
  IROperand offset = ir_operand_copy(&lo2->dest);
  IROperand guard = ir_operand_copy(&hi1->dest);
  char *or_text = mettle_strdup("|");
  char *sub_text = mettle_strdup("-");
  char *cmp_text = mettle_strdup("<=");
  char *miss = mettle_strdup(miss_label->text);

  if (!value.name || !fold.name || !offset.name || !guard.name || !or_text ||
      !sub_text || !cmp_text || !miss) {
    ir_operand_destroy(&value);
    ir_operand_destroy(&fold);
    ir_operand_destroy(&offset);
    ir_operand_destroy(&guard);
    mettle_free_string(or_text);
    mettle_free_string(sub_text);
    mettle_free_string(cmp_text);
    mettle_free_string(miss);
    return 0;
  }

  ir_instruction_make_nop(lo1);
  lo1->op = IR_OP_BINARY;
  lo1->text = or_text;
  lo1->dest = fold;
  lo1->lhs = value;
  lo1->rhs = ir_operand_int(32);

  ir_instruction_make_nop(lo1_branch);
  lo1_branch->op = IR_OP_BINARY;
  lo1_branch->text = sub_text;
  lo1_branch->dest = offset;
  lo1_branch->lhs = ir_operand_copy(&lo1->dest);
  lo1_branch->rhs = ir_operand_int(97);

  ir_instruction_make_nop(hi1);
  hi1->op = IR_OP_BINARY;
  hi1->text = cmp_text;
  hi1->dest = guard;
  hi1->lhs = ir_operand_copy(&lo1_branch->dest);
  hi1->rhs = ir_operand_int(25);
  hi1->is_unsigned = 1;

  ir_instruction_make_nop(hi1_branch);
  hi1_branch->op = IR_OP_BRANCH_ZERO;
  hi1_branch->lhs = ir_operand_copy(&hi1->dest);
  hi1_branch->text = miss;

  for (size_t i = index[5]; i <= index[12]; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

/* Fold the canonical short circuit form of
 *
 *   (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
 *
 * into `(unsigned)((c | 32) - 'a') <= 25`. The internal labels must have no
 * other incoming edge and every compare result must feed only its branch.
 * These checks make deleting the second range safe even after inlining and
 * earlier control flow rewrites. */
/* `v < LO || v > HI` answered as one unsigned compare.
 *
 * Subtracting the low bound folds the two bounds into one: the values in
 * range land in 0..HI-LO, and everything else -- including everything below
 * LO, which wraps to a huge unsigned value -- lands above it. So a pair of
 * signed compares with a branch each becomes a subtract, one unsigned
 * compare, and one branch.
 *
 * word_freq tests `c < 97 || c > 122` once per byte of its document, in two
 * places, and json_parse tests `c < 48 || c > 57` three times in
 * scan_number. This runs AFTER ascii_casefold_range, which claims the
 * two-range `A-Z or a-z` shape and produces something better than this
 * would.
 *
 * The shape, as the short-circuit `||` lowers it:
 *
 *     %t   = v < LO
 *     branch_zero %t -> ELSE
 *     jump BODY
 *   ELSE:
 *     %t2  = v > HI
 *     branch_zero %t2 -> SKIP
 *   BODY:
 *
 * becomes
 *
 *     %s   = v - LO
 *     %t   = %s >u (HI - LO)
 *     branch_zero %t -> SKIP
 *   BODY:
 */
/* How many jumps and branches name this label. */
static size_t ir_label_reference_count(const IRFunction *function,
                                       const char *name) {
  size_t n = 0;
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if ((in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO ||
         in->op == IR_OP_BRANCH_EQ) &&
        in->text && strcmp(in->text, name) == 0) {
      n++;
    }
  }
  return n;
}

static int ir_range_test_is_foldable(const IRInstruction *lo,
                                     const IRInstruction *hi) {
  if (lo->op != IR_OP_BINARY || hi->op != IR_OP_BINARY || lo->is_float ||
      hi->is_float || !lo->text || !hi->text) {
    return 0;
  }
  if (strcmp(lo->text, "<") != 0 || strcmp(hi->text, ">") != 0) {
    return 0;
  }
  if (lo->rhs.kind != IR_OPERAND_INT || hi->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  /* An unsigned dividend would already compare unsigned, and the wrap the
   * fold relies on is only equivalent for a signed one. */
  if (lo->is_unsigned || hi->is_unsigned) {
    return 0;
  }
  if (lo->rhs.int_value > hi->rhs.int_value) {
    return 0; /* empty range: the fold would invert it */
  }
  /* Taken unsigned: the bounds are ordered, so this is their exact distance,
   * where the signed subtraction would overflow on a pair of extreme literals
   * and read as a small span. */
  if ((unsigned long long)hi->rhs.int_value -
          (unsigned long long)lo->rhs.int_value >
      2147483646ULL) {
    return 0; /* the span has to stay inside the compare's width */
  }
  if (lo->dest.kind != IR_OPERAND_TEMP || hi->dest.kind != IR_OPERAND_TEMP) {
    return 0;
  }
  if (lo->lhs.kind != IR_OPERAND_TEMP && lo->lhs.kind != IR_OPERAND_SYMBOL) {
    return 0;
  }
  return ir_operand_equals(&lo->lhs, &hi->lhs);
}

int ir_fold_range_test_pass(IRFunction *function, int *changed) {
  IRTempUseMap uses;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  /* The low compare's temp stops holding a truth value and starts holding
   * `v - LO`, and the high compare's stops meaning `v > HI` and starts
   * meaning out-of-range. Either one read anywhere but by its own branch
   * would read the new meaning. */
  if (!ir_temp_use_map_init(&uses)) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 1;
    }
  }
  for (size_t i = 0; i + 6 < function->instruction_count; i++) {
    size_t at[7];
    at[0] = i;
    if (function->instructions[at[0]].op != IR_OP_BINARY) {
      continue;
    }
    {
      int ok = 1;
      for (size_t k = 1; k < 7 && ok; k++) {
        ok = ir_ascii_next(function, at[k - 1] + 1, &at[k]);
      }
      if (!ok) {
        continue;
      }
    }
    {
      IRInstruction *lo = &function->instructions[at[0]];
      IRInstruction *lo_branch = &function->instructions[at[1]];
      IRInstruction *shortcut = &function->instructions[at[2]];
      IRInstruction *else_label = &function->instructions[at[3]];
      IRInstruction *hi = &function->instructions[at[4]];
      IRInstruction *hi_branch = &function->instructions[at[5]];
      IRInstruction *body_label = &function->instructions[at[6]];
      long long span;

      if (lo_branch->op != IR_OP_BRANCH_ZERO ||
          !ir_operand_is_temp_named(&lo_branch->lhs, lo->dest.name) ||
          !lo_branch->text) {
        continue;
      }
      if (shortcut->op != IR_OP_JUMP || !shortcut->text) {
        continue;
      }
      if (else_label->op != IR_OP_LABEL || !else_label->text ||
          strcmp(else_label->text, lo_branch->text) != 0) {
        continue;
      }
      if (hi_branch->op != IR_OP_BRANCH_ZERO ||
          !ir_operand_is_temp_named(&hi_branch->lhs, hi->dest.name) ||
          !hi_branch->text) {
        continue;
      }
      /* Whatever control reaches when the high test passes has to be the
       * same place the short-circuit jumped to. It is spelled two ways: the
       * body's label sits right there (`if (...) { ... }`), or the body is
       * itself a jump to the same target, which is how a `break` lowers. */
      if (!body_label->text ||
          (body_label->op != IR_OP_LABEL && body_label->op != IR_OP_JUMP) ||
          strcmp(body_label->text, shortcut->text) != 0) {
        continue;
      }
      /* The short-circuit's own label is about to go. Anything else that
       * branches to it would land on the compare and read a subtraction that
       * never happened on its path. */
      if (ir_label_reference_count(function, else_label->text) != 1) {
        continue;
      }
      if (!ir_range_test_is_foldable(lo, hi)) {
        continue;
      }
      if (ir_temp_use_map_get(&uses, lo->dest.name) != 1 ||
          ir_temp_use_map_get(&uses, hi->dest.name) != 1) {
        continue;
      }
      /* The short-circuit block must hold nothing but its jump, or the
       * fold would drop whatever else it does. ir_ascii_next already
       * skipped the nops, so adjacency in the index array says so. */
      span = hi->rhs.int_value - lo->rhs.int_value;

      {
        long long low = lo->rhs.int_value;
        IROperand value = ir_operand_copy(&lo->lhs);
        char *sub_text = mettle_strdup("-");
        char *cmp_text = mettle_strdup(">");
        if (!sub_text || !cmp_text) {
          ir_operand_destroy(&value);
          mettle_free_string(sub_text);
          mettle_free_string(cmp_text);
          return 0;
        }
        /* lo becomes the subtract, hi becomes the unsigned compare. */
        mettle_free_string(lo->text);
        lo->text = sub_text;
        ir_operand_destroy(&lo->rhs);
        lo->rhs = ir_operand_int(low);

        mettle_free_string(hi->text);
        hi->text = cmp_text;
        ir_operand_destroy(&hi->lhs);
        hi->lhs = ir_operand_copy(&lo->dest);
        ir_operand_destroy(&hi->rhs);
        hi->rhs = ir_operand_int(span);
        hi->is_unsigned = 1;
        ir_operand_destroy(&value);
      }

      /* The low branch, its short-circuit jump and the else label all go:
       * the single compare that replaced them falls straight into the
       * branch that used to test the high bound. */
      ir_instruction_make_nop(&function->instructions[at[1]]);
      ir_instruction_make_nop(&function->instructions[at[2]]);
      ir_instruction_make_nop(&function->instructions[at[3]]);
      if (changed) {
        *changed = 1;
      }
    }
  }
  ir_temp_use_map_destroy(&uses);
  return 1;
}
int ir_ascii_casefold_range_pass(IRFunction *function, int *changed) {
  IRTempUseMap uses;
  int local_changed = 0;
  if (!function || !ir_temp_use_map_init(&uses)) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 1;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    size_t at[14];
    IRInstruction *ins;
    int first_lowercase;
    at[0] = i;
    ins = &function->instructions[at[0]];
    if (ins->op != IR_OP_BINARY || !ins->text ||
        strcmp(ins->text, ">=") != 0 || ins->rhs.kind != IR_OPERAND_INT ||
        (ins->rhs.int_value != 65 && ins->rhs.int_value != 97) ||
        (ins->lhs.kind != IR_OPERAND_TEMP &&
         ins->lhs.kind != IR_OPERAND_SYMBOL) ||
        !ins->lhs.name) {
      continue;
    }
    first_lowercase = ins->rhs.int_value == 97;
    for (size_t k = 1; k < 14; k++) {
      if (!ir_ascii_next(function, at[k - 1] + 1, &at[k])) {
        at[0] = function->instruction_count;
        break;
      }
    }
    if (at[0] >= function->instruction_count) {
      continue;
    }

    IRInstruction *lo1 = &function->instructions[at[0]];
    IRInstruction *lo1_branch = &function->instructions[at[1]];
    IRInstruction *hi1 = &function->instructions[at[2]];
    IRInstruction *hi1_branch = &function->instructions[at[3]];
    IRInstruction *success1 = &function->instructions[at[4]];
    IRInstruction *hi1_fail = &function->instructions[at[5]];
    IRInstruction *lo1_fail = &function->instructions[at[6]];
    IRInstruction *lo2 = &function->instructions[at[7]];
    IRInstruction *lo2_branch = &function->instructions[at[8]];
    IRInstruction *hi2 = &function->instructions[at[9]];
    IRInstruction *hi2_branch = &function->instructions[at[10]];
    IRInstruction *success2 = &function->instructions[at[11]];
    IRInstruction *hi2_fail = &function->instructions[at[12]];
    IRInstruction *lo2_fail = &function->instructions[at[13]];
    long long lo1_bound = first_lowercase ? 97 : 65;
    long long hi1_bound = first_lowercase ? 122 : 90;
    long long lo2_bound = first_lowercase ? 65 : 97;
    long long hi2_bound = first_lowercase ? 90 : 122;

    if (!ir_ascii_range_compare(lo1, ">=", lo1_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(lo1_branch, lo1) ||
        !ir_ascii_range_compare(hi1, "<=", hi1_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(hi1_branch, hi1) ||
        success1->op != IR_OP_JUMP || !success1->text ||
        hi1_fail->op != IR_OP_LABEL || !hi1_fail->text ||
        strcmp(hi1_fail->text, hi1_branch->text) != 0 ||
        lo1_fail->op != IR_OP_LABEL || !lo1_fail->text ||
        strcmp(lo1_fail->text, lo1_branch->text) != 0 ||
        !ir_ascii_range_compare(lo2, ">=", lo2_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(lo2_branch, lo2) ||
        !ir_ascii_range_compare(hi2, "<=", hi2_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(hi2_branch, hi2) ||
        success2->op != IR_OP_JUMP || !success2->text ||
        strcmp(success1->text, success2->text) != 0 ||
        hi2_fail->op != IR_OP_LABEL || !hi2_fail->text ||
        strcmp(hi2_fail->text, hi2_branch->text) != 0 ||
        lo2_fail->op != IR_OP_LABEL || !lo2_fail->text ||
        strcmp(lo2_fail->text, lo2_branch->text) != 0 ||
        ir_temp_use_map_get(&uses, lo1->dest.name) != 1 ||
        ir_temp_use_map_get(&uses, hi1->dest.name) != 1 ||
        ir_temp_use_map_get(&uses, lo2->dest.name) != 1 ||
        ir_temp_use_map_get(&uses, hi2->dest.name) != 1 ||
        !ir_label_only_referenced_by(function, hi1_fail->text, at[3]) ||
        !ir_label_only_referenced_by(function, lo1_fail->text, at[1]) ||
        !ir_label_only_referenced_by(function, hi2_fail->text, at[10])) {
      continue;
    }

    if (!ir_ascii_casefold_rewrite(function, at, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 1;
    }
    local_changed = 1;
  }

  ir_temp_use_map_destroy(&uses);
  if (local_changed) {
    ir_function_clear_cfg(function);
  }
  return 1;
}

/* The set of label names some branch targets, collected in one walk.
 *
 * Asking ir_label_is_referenced per label scans the function per label, which
 * is quadratic in a function that is mostly labels -- anything built out of
 * if/else. Retiring a label never removes a branch, so one set answers every
 * question this pass asks. */
typedef struct {
  const char **names;
  size_t capacity;
} IRLabelRefSet;

static void ir_label_ref_set_destroy(IRLabelRefSet *set) {
  if (!set) {
    return;
  }
  free(set->names);
  set->names = NULL;
  set->capacity = 0;
}

static int ir_label_ref_set_build(const IRFunction *function,
                                  IRLabelRefSet *set) {
  size_t capacity = 16;
  size_t mask;

  set->names = NULL;
  set->capacity = 0;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  while (capacity < function->instruction_count * 2) {
    capacity *= 2;
  }
  set->names = (const char **)calloc(capacity, sizeof(*set->names));
  if (!set->names) {
    return 0;
  }
  set->capacity = capacity;
  mask = capacity - 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t slot;
    if (!instruction->text ||
        (instruction->op != IR_OP_JUMP &&
         instruction->op != IR_OP_BRANCH_ZERO &&
         instruction->op != IR_OP_BRANCH_EQ)) {
      continue;
    }
    slot = (size_t)mettle_fnv1a_hash(instruction->text) & mask;
    while (set->names[slot]) {
      if (strcmp(set->names[slot], instruction->text) == 0) {
        break;
      }
      slot = (slot + 1) & mask;
    }
    set->names[slot] = instruction->text;
  }
  return 1;
}

static int ir_label_ref_set_contains(const IRLabelRefSet *set,
                                     const char *name) {
  size_t mask;
  size_t slot;

  if (!set->capacity || !name) {
    return 0;
  }
  mask = set->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (set->names[slot]) {
    if (strcmp(set->names[slot], name) == 0) {
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

int ir_remove_unused_labels_pass(IRFunction *function, int *changed) {
  IRLabelRefSet referenced;

  if (!function) {
    return 0;
  }
  if (!ir_label_ref_set_build(function, &referenced)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        !ir_label_ref_set_contains(&referenced, instruction->text)) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  ir_label_ref_set_destroy(&referenced);
  return 1;
}

int ir_eliminate_unreachable_straightline_pass(IRFunction *function,
                                                       int *changed) {
  if (!function) {
    return 0;
  }

  int reachable = 1;

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_LABEL) {
      reachable = 1;
      continue;
    }

    if (!reachable) {
      if (instruction->op != IR_OP_NOP) {
        ir_instruction_make_nop(instruction);
        if (changed) {
          *changed = 1;
        }
      }
      continue;
    }

    if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN) {
      reachable = 0;
    }
  }

  return 1;
}

int ir_operand_is_symbol_named(const IROperand *operand,
                                      const char *name) {
  return operand && operand->kind == IR_OPERAND_SYMBOL && operand->name &&
         name && operand->name[0] == name[0] &&
         strcmp(operand->name, name) == 0;
}

static int ir_try_fuse_rotate_add_at(IRFunction *function, size_t index,
                                     int *changed) {
  if (!function || index + 3 >= function->instruction_count) {
    return 1;
  }

  IRInstruction *add_inst = &function->instructions[index];
  IRInstruction *assign_next = &function->instructions[index + 1];
  IRInstruction *assign_a = &function->instructions[index + 2];
  IRInstruction *assign_b = &function->instructions[index + 3];

  if (add_inst->op != IR_OP_BINARY || add_inst->is_float || add_inst->ast_ref ||
      !add_inst->text || strcmp(add_inst->text, "+") != 0 ||
      add_inst->dest.kind != IR_OPERAND_TEMP || !add_inst->dest.name ||
      add_inst->lhs.kind != IR_OPERAND_SYMBOL || !add_inst->lhs.name ||
      add_inst->rhs.kind != IR_OPERAND_SYMBOL || !add_inst->rhs.name) {
    return 1;
  }

  const char *sym_a = add_inst->lhs.name;
  const char *sym_b = add_inst->rhs.name;
  const char *temp_sum = add_inst->dest.name;

  if (assign_next->op != IR_OP_ASSIGN ||
      assign_next->dest.kind != IR_OPERAND_SYMBOL || !assign_next->dest.name ||
      assign_a->op != IR_OP_ASSIGN ||
      assign_a->dest.kind != IR_OPERAND_SYMBOL || !assign_a->dest.name ||
      assign_b->op != IR_OP_ASSIGN ||
      assign_b->dest.kind != IR_OPERAND_SYMBOL || !assign_b->dest.name) {
    return 1;
  }

  const char *sym_next = assign_next->dest.name;
  int next_from_temp =
      assign_next->lhs.kind == IR_OPERAND_TEMP && assign_next->lhs.name &&
      strcmp(assign_next->lhs.name, temp_sum) == 0;
  if (!next_from_temp) {
    return 1;
  }

  if (!ir_operand_is_symbol_named(&assign_a->lhs, sym_b) ||
      !ir_operand_is_symbol_named(&assign_a->dest, sym_a)) {
    return 1;
  }

  int b_from_next =
      ir_operand_is_symbol_named(&assign_b->dest, sym_b) &&
      ir_operand_is_symbol_named(&assign_b->lhs, sym_next);
  int b_from_temp =
      ir_operand_is_symbol_named(&assign_b->dest, sym_b) &&
      assign_b->lhs.kind == IR_OPERAND_TEMP && assign_b->lhs.name &&
      strcmp(assign_b->lhs.name, temp_sum) == 0;
  if (!b_from_next && !b_from_temp) {
    return 1;
  }

  IRInstruction fused = {0};
  fused.op = IR_OP_ROTATE_ADD;
  fused.location = add_inst->location;
  fused.dest = ir_operand_symbol(sym_next);
  fused.lhs = ir_operand_symbol(sym_a);
  fused.rhs = ir_operand_symbol(sym_b);
  fused.text = mettle_strdup("+");
  if (!fused.dest.name || !fused.lhs.name || !fused.rhs.name || !fused.text) {
    ir_operand_destroy(&fused.dest);
    ir_operand_destroy(&fused.lhs);
    ir_operand_destroy(&fused.rhs);
    mettle_free_string(fused.text);
    return 0;
  }

  ir_instruction_destroy_storage(add_inst);
  *add_inst = fused;
  ir_instruction_make_nop(assign_next);
  ir_instruction_make_nop(assign_a);
  ir_instruction_make_nop(assign_b);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_fuse_rotate_add_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i + 3 < function->instruction_count; i++) {
    if (!ir_try_fuse_rotate_add_at(function, i, changed)) {
      return 0;
    }
    if (function->instructions[i].op == IR_OP_ROTATE_ADD) {
      i += 3;
    }
  }

  return 1;
}

int ir_strength_reduce_rotate_loops_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *inst = &function->instructions[i];
    if (inst->op != IR_OP_LABEL || !inst->text) {
      continue;
    }

    const char *loop_label = inst->text;
    size_t body_start = i + 1;
    size_t body_end = function->instruction_count;
    size_t jump_index = (size_t)-1;

    for (size_t j = body_start; j < function->instruction_count; j++) {
      IRInstruction *probe = &function->instructions[j];
      if (probe->op == IR_OP_JUMP && probe->text &&
          strcmp(probe->text, loop_label) == 0) {
        jump_index = j;
        body_end = j;
        break;
      }
      if (probe->op == IR_OP_LABEL) {
        break;
      }
    }

    if (jump_index == (size_t)-1) {
      continue;
    }

    int only_rotate_or_nop = 1;
    for (size_t j = body_start; j < body_end; j++) {
      IROpcode op = function->instructions[j].op;
      if (op == IR_OP_NOP || op == IR_OP_ROTATE_ADD) {
        continue;
      }
      if (op == IR_OP_BINARY || op == IR_OP_ASSIGN) {
        only_rotate_or_nop = 0;
        break;
      }
      if (op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ) {
        continue;
      }
      only_rotate_or_nop = 0;
      break;
    }

    if (!only_rotate_or_nop) {
      continue;
    }

    for (size_t j = body_start; j < body_end; j++) {
      if (!ir_try_fuse_rotate_add_at(function, j, changed)) {
        return 0;
      }
      if (function->instructions[j].op == IR_OP_ROTATE_ADD) {
        j += 3;
      }
    }
  }

  return 1;
}

int ir_instruction_has_side_effect(const IRInstruction *instruction) {
  if (!instruction) {
    return 0;
  }

  /* A volatile access is observable in itself, so it counts as an effect even
   * when it is a load whose value nothing reads. */
  if (instruction->is_volatile) {
    return 1;
  }

  switch (instruction->op) {
  case IR_OP_BARRIER:
  case IR_OP_ASYNC_COPY:
  case IR_OP_ASYNC_COMMIT:
  case IR_OP_ASYNC_WAIT:
  case IR_OP_TENSOR_TRANSFER:
  case IR_OP_GPU_LAUNCH:
  case IR_OP_TENSOR_MMA:
  case IR_OP_TENSOR_MATMUL:
  case IR_OP_TENSOR_EPILOGUE:
  case IR_OP_TENSOR_COMMIT:
  case IR_OP_STORE:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_BYTE_MAP:
  case IR_OP_SIMD_FILL:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
  case IR_OP_LOWER_BOUND_I32:
  case IR_OP_PREFIX_SUM_I32:
  case IR_OP_SIMD_MINMAX_I32:
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32:
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_FIND:
  case IR_OP_SIMD_OUTER_LANE_F64:
  case IR_OP_RETURN:
  case IR_OP_INLINE_ASM:
    return 1;
  default:
    return 0;
  }
}

int ir_operand_is_temp_named(const IROperand *operand,
                                    const char *name) {
  return operand && operand->kind == IR_OPERAND_TEMP && operand->name &&
         name && operand->name[0] == name[0] &&
         strcmp(operand->name, name) == 0;
}

int ir_operand_is_int_value(const IROperand *operand,
                                   long long value) {
  return operand && operand->kind == IR_OPERAND_INT &&
         operand->int_value == value;
}

static int ir_instruction_reads_symbol_operand(const IRInstruction *instruction,
                                               const char *symbol_name) {
  if (!instruction || !symbol_name) {
    return 0;
  }

  if (ir_operand_is_symbol_named(&instruction->lhs, symbol_name) ||
      ir_operand_is_symbol_named(&instruction->rhs, symbol_name)) {
    return 1;
  }

  for (size_t i = 0; i < instruction->argument_count; i++) {
    if (ir_operand_is_symbol_named(&instruction->arguments[i], symbol_name)) {
      return 1;
    }
  }

  return 0;
}

int ir_symbol_read_after(const IRFunction *function, size_t start_index,
                                const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }

  for (size_t i = start_index; i < function->instruction_count; i++) {
    if (ir_instruction_reads_symbol_operand(&function->instructions[i],
                                            symbol_name)) {
      return 1;
    }
  }
  return 0;
}

/* Liveness of a loop's induction variable PAST the loop, for the SIMD
 * recognizers (the fused kernels drop the iv). `exit_index` is the first
 * instruction after the back jump -- usually the loop's own exit label.
 * Unlike ir_symbol_read_after, a full redefinition (`i <- 0` starting the
 * NEXT loop -- iv reuse is everywhere in real code) kills the value: later
 * reads see the new definition, not the loop's final value. The scan is
 * conservative: it trusts a redefinition only while control flow is still
 * straight-line from the exit (one leading label allowed -- the exit label
 * itself); any further label/branch/jump means other paths could observe
 * the old value, and the answer falls back to "live". */
int ir_symbol_live_after_loop(const IRFunction *function, size_t exit_index,
                              const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }
  int labels_seen = 0;
  for (size_t i = exit_index; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ir_instruction_reads_symbol_operand(ins, symbol_name)) {
      return 1;
    }
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, symbol_name)) {
      return 0; /* fully redefined before any read: the old value is dead */
    }
    if (ins->op == IR_OP_RETURN) {
      return 0;
    }
    if (ins->op == IR_OP_LABEL) {
      if (labels_seen++ > 0) {
        /* A join: another path may enter here and read the old value via
         * code we will not scan in order. Fall back to the whole-function
         * read scan. */
        return ir_symbol_read_after(function, i, symbol_name);
      }
      continue;
    }
    if (ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ) {
      return ir_symbol_read_after(function, i, symbol_name);
    }
  }
  return 0;
}

static int ir_symbol_read_count_after_until_write_or_control(
    const IRFunction *function, size_t start_index, const char *symbol_name,
    size_t *only_read_index_out) {
  int count = 0;

  if (only_read_index_out) {
    *only_read_index_out = (size_t)-1;
  }
  if (!function || !symbol_name) {
    return 0;
  }

  for (size_t i = start_index; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    /* A jump may be a loop backedge. Converting the symbol write before it to
     * a temp lets later loop unrolling clone several writes to one temp name,
     * which violates the backend's single-producer rule. */
    if (instruction->op == IR_OP_JUMP) {
      return -1;
    }
    if (instruction->op == IR_OP_RETURN) {
      return count;
    }
    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_BRANCH_ZERO ||
        instruction->op == IR_OP_BRANCH_EQ ||
        instruction->op == IR_OP_CALL || instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_STORE || instruction->op == IR_OP_INLINE_ASM) {
      return -1;
    }
    /* Count the read BEFORE the write ends the scan, because one instruction
     * does both: `s0 = 10.0 - s0` reads the old value and then names it. Ended
     * at the write, that read went uncounted, so a copy with two readers
     * looked like a copy with one and its symbol became a temp. The second
     * reader was still spelled `s0`, which nothing wrote any more, so it read
     * whatever the symbol held on entry to the loop for every iteration. */
    if (ir_instruction_reads_symbol_operand(instruction, symbol_name)) {
      count++;
      if (only_read_index_out) {
        *only_read_index_out = i;
      }
      if (count > 1) {
        return count;
      }
    }
    if (ir_instruction_writes_symbol(instruction) &&
        ir_operand_is_symbol_named(&instruction->dest, symbol_name)) {
      break;
    }
  }

  return count;
}

static int ir_instruction_replace_symbol_reads(IRInstruction *instruction,
                                               const char *symbol_name,
                                               const IROperand *replacement,
                                               int *changed) {
  IROperand *operands[2];
  if (!instruction || !symbol_name || !replacement) {
    return 0;
  }

  operands[0] = &instruction->lhs;
  operands[1] = &instruction->rhs;
  for (size_t i = 0; i < sizeof(operands) / sizeof(operands[0]); i++) {
    if (ir_operand_is_symbol_named(operands[i], symbol_name)) {
      IROperand cloned = ir_operand_none();
      if (!ir_operand_clone(replacement, &cloned)) {
        return 0;
      }
      ir_operand_destroy(operands[i]);
      *operands[i] = cloned;
      if (changed) {
        *changed = 1;
      }
    }
  }

  for (size_t i = 0; i < instruction->argument_count; i++) {
    if (ir_operand_is_symbol_named(&instruction->arguments[i], symbol_name)) {
      IROperand cloned = ir_operand_none();
      if (!ir_operand_clone(replacement, &cloned)) {
        return 0;
      }
      ir_operand_destroy(&instruction->arguments[i]);
      instruction->arguments[i] = cloned;
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

static char *ir_make_float_symbol_copy_temp_name(size_t instruction_index) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "__float_local_%zu", instruction_index);
  return mettle_strdup(buffer);
}

int ir_eliminate_single_use_float_symbol_copies_pass(IRFunction *function,
                                                            int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (!instruction->is_float ||
        instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
        !ir_instruction_is_trivially_dead_if_dest_unused(instruction)) {
      continue;
    }

    if (instruction->float_bits == 32) {
      continue;
    }

    if (ir_instruction_reads_symbol_operand(instruction, instruction->dest.name) ||
        ir_symbol_address_taken(function, instruction->dest.name)) {
      continue;
    }

    size_t only_read_index = (size_t)-1;
    if (ir_symbol_read_count_after_until_write_or_control(
            function, i + 1, instruction->dest.name, &only_read_index) != 1 ||
        only_read_index == (size_t)-1) {
      continue;
    }

    char *temp_name = ir_make_float_symbol_copy_temp_name(i);
    if (!temp_name) {
      return 0;
    }
    IROperand replacement = ir_operand_temp(temp_name);
    if (!ir_instruction_replace_symbol_reads(
            &function->instructions[only_read_index], instruction->dest.name,
            &replacement, changed)) {
      ir_operand_destroy(&replacement);
      free(temp_name);
      return 0;
    }
    ir_operand_destroy(&instruction->dest);
    instruction->dest = ir_operand_temp(temp_name);
    ir_operand_destroy(&replacement);
    free(temp_name);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}
