/* An unexported array whose address never escapes and whose stores carry no
 * origin needs no shadow bytes. Solve all arrays together: a numeric map can
 * read another such array, while a pointer cast stored anywhere rejects the
 * destination and then every array which copies from it.
 *
 * This runs after origins have been made explicit. It reasons about identity
 * values, never guesses from an integer type that its bits are not a pointer.
 */
typedef struct {
  const IRModuleSymbol *symbol;
  const IRFunction *scope;
  const IROperand *root;
  int rejected;
} SafetyPlainArray;

typedef struct {
  IRProgram *program;
  SafetyPlainArray *arrays;
  size_t count;
} SafetyPlainStorage;

/* Follow only names with one definition. A second write prevents a join from
 * hiding another array behind the same name. */
static const IRInstruction *safety_plain_definition(const IRFunction *fn,
                                                   const IROperand *value) {
  const IRInstruction *found = NULL;
  if ((value->kind != IR_OPERAND_TEMP && value->kind != IR_OPERAND_SYMBOL) ||
      !value->name) return NULL;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op != IR_OP_DECLARE_LOCAL && ir_instruction_writes_destination(in) &&
        safety_operand_same(&in->dest, value)) {
      if (found) return NULL;
      found = in;
    }
  }
  return found;
}

static SafetyPlainArray *safety_plain_root(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value, int depth) {
  if (depth > 32) return NULL;
  for (size_t a = 0; a < state->count; a++)
    if (state->arrays[a].scope == fn && state->arrays[a].root &&
        safety_operand_same(state->arrays[a].root, value)) return &state->arrays[a];
  if (value->kind == IR_OPERAND_SYMBOL && value->name) {
    for (size_t p = 0; p < fn->parameter_count; p++) {
      if (strcmp(fn->parameter_names[p], value->name)) continue;
      for (size_t i = 0; i < fn->instruction_count; i++)
        if (ir_instruction_writes_destination(&fn->instructions[i]) &&
            safety_operand_same(&fn->instructions[i].dest, value)) return NULL;
      const IRModuleSymbol *decl = ir_program_lookup_symbol(state->program, fn->name);
      if (decl && decl->is_exported) return NULL;
      for (size_t s = 0; s < state->program->module_symbol_count; s++) {
        const IRModuleSymbol *symbol = &state->program->module_symbols[s];
        if (symbol->init_symbol_ref && !strcmp(symbol->init_symbol_ref, fn->name))
          return NULL;
        for (size_t r = 0; r < symbol->init_reloc_count; r++)
          if (symbol->init_relocs[r].symbol &&
              !strcmp(symbol->init_relocs[r].symbol, fn->name)) return NULL;
      }
      SafetyPlainArray *result = NULL;
      for (size_t f = 0; f < state->program->function_count; f++) {
        const IRFunction *caller = state->program->functions[f];
        for (size_t i = 0; i < caller->instruction_count; i++) {
          const IRInstruction *call = &caller->instructions[i];
          if (call->op == IR_OP_ADDRESS_OF && call->lhs.name &&
              !strcmp(call->lhs.name, fn->name) &&
              (!call->dest.name || strncmp(call->dest.name, ".safe", 5))) return NULL;
          if (call->op != IR_OP_CALL || !call->text || strcmp(call->text, fn->name))
            continue;
          if (p >= call->argument_count) return NULL;
          SafetyPlainArray *argument = safety_plain_root(state, caller,
              &call->arguments[p], depth + 1);
          if (!argument || (result && result != argument)) return NULL;
          result = argument;
        }
      }
      return result;
    }
    if (!ir_function_find_declaration(fn, value->name, 1))
      for (size_t a = 0; a < state->count; a++)
        if (state->arrays[a].symbol &&
            !strcmp(value->name, state->arrays[a].symbol->name)) return &state->arrays[a];
  }
  const IRInstruction *in = safety_plain_definition(fn, value);
  if (!in) return NULL;
  if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL)
    return safety_plain_root(state, fn, &in->lhs, depth + 1);
  if (in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST ||
      (in->op == IR_OP_BINARY && in->text && !strcmp(in->text, "+")))
    return safety_plain_root(state, fn, &in->lhs, depth + 1);
  return NULL;
}

static int safety_plain_allowed_use(SafetyPlainStorage *state,
    SafetyPlainArray *array, const IRFunction *fn,
    const IRInstruction *in, int position) {
  if (in->op == IR_OP_ADDRESS_OF) return position == 1 &&
      safety_plain_definition(fn, &in->dest) == in;
  if (in->op == IR_OP_SAFETY_CHECK) return position == 3;
  if (in->op == IR_OP_LOAD) return position == 1;
  if (in->op == IR_OP_STORE) return position == 0 &&
      in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 0 &&
      in->rhs.int_value <= 8;
  if ((in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST ||
       (in->op == IR_OP_BINARY && in->text && !strcmp(in->text, "+"))) &&
      position == 1) return safety_plain_definition(fn, &in->dest) == in;
  if (in->op != IR_OP_CALL || !in->text || position < 3) return 0;
  IRFunction *callee = ir_program_find_function(state->program, in->text);
  if (callee && (size_t)(position - 3) < callee->parameter_count) {
    IROperand parameter = {0};
    parameter.kind = IR_OPERAND_SYMBOL;
    parameter.name = callee->parameter_names[position - 3];
    return safety_plain_root(state, callee, &parameter, 0) == array;
  }
  if (position != 3) return 0;
  return !strcmp(in->text, "mettle_safety_identity") ||
         !strcmp(in->text, "mettle_safety_value_load") ||
         !strcmp(in->text, "mettle_safety_value_store") ||
         !strcmp(in->text, "mettle_safety_register") ||
         !strcmp(in->text, "mettle_safety_register_static") ||
         !strcmp(in->text, "mettle_safety_unregister") ||
         !strcmp(in->text, "mettle_safety_free_identity") ||
         !strcmp(in->text, "free") ||
         !strcmp(in->text, "mettle_heap_free") ||
         !strcmp(in->text, "mettle_safety_check_identity") ||
         !strcmp(in->text, "mettle_safety_span_identity");
}

/* Reachability of a nonzero origin through the shadow copy graph. A cycle
 * adds no origin of its own. Every incoming edge still gets visited, and
 * unknown definitions and depth limits conservatively keep the bookkeeping. */
static int safety_plain_tainted(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value,
    const IROperand **path, int depth) {
  if (value->kind == IR_OPERAND_INT) return value->int_value != 0;
  if (!value->name || depth == 64) return 1;
  for (int i = 0; i < depth; i++)
    if (safety_operand_same(path[i], value)) return 0;
  path[depth] = value;
  int found = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (!ir_instruction_writes_destination(in) ||
        !safety_operand_same(&in->dest, value) || in->op == IR_OP_DECLARE_LOCAL)
      continue;
    found = 1;
    if (in->op == IR_OP_ASSIGN) {
      if (safety_plain_tainted(state, fn, &in->lhs, path, depth + 1)) return 1;
    } else if (in->op == IR_OP_CALL && in->text &&
               !strcmp(in->text, "mettle_safety_value_load") &&
               in->argument_count == 3) {
      SafetyPlainArray *array = safety_plain_root(state, fn, &in->arguments[0], 0);
      if (!array || array->rejected) return 1;
    } else if (in->op == IR_OP_CALL && in->text &&
               (!strcmp(in->text, "mettle_safety_merge_identity") ||
                !strcmp(in->text, "mettle_safety_subtract_identity")) &&
               in->argument_count == 2) {
      if (safety_plain_tainted(state, fn, &in->arguments[0], path, depth + 1) ||
          safety_plain_tainted(state, fn, &in->arguments[1], path, depth + 1))
        return 1;
    } else return 1;
  }
  return !found;
}

static int safety_plain_read_intrinsic(const IRInstruction *in) {
  return in->op == IR_OP_CALL && in->text &&
      (!strcmp(in->text, "mettle_safety_identity") ||
       !strcmp(in->text, "mettle_safety_value_load") ||
       !strcmp(in->text, "mettle_safety_merge_identity") ||
       !strcmp(in->text, "mettle_safety_subtract_identity"));
}

static int safety_plain_value_used(const IRFunction *fn, const IROperand *value) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_NOP) continue;
    if (safety_operand_same(&in->lhs, value) ||
        safety_operand_same(&in->rhs, value) ||
        (in->op == IR_OP_STORE && safety_operand_same(&in->dest, value))) return 1;
    for (size_t a = 0; a < in->argument_count; a++)
      if (safety_operand_same(&in->arguments[a], value)) return 1;
  }
  return 0;
}

/* The runtime defines a merge of an origin with nothing, and with itself, as
 * that origin unchanged, and a subtraction of two of the same origin as an
 * ordinary integer distance. Those three cases are settled here rather than
 * asked at run time, which matters because a pointer walked by arithmetic asks
 * one of them on every turn of the loop that walks it. */
static int safety_fold_identity_arithmetic(IRFunction *fn) {
  int folded = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    IRInstruction *in = &fn->instructions[i];
    if (in->op != IR_OP_CALL || !in->text || in->argument_count != 2 ||
        !in->arguments || in->dest.kind == IR_OPERAND_NONE) {
      continue;
    }
    int merge = strcmp(in->text, "mettle_safety_merge_identity") == 0;
    int subtract = strcmp(in->text, "mettle_safety_subtract_identity") == 0;
    if (!merge && !subtract) {
      continue;
    }
    const IROperand *left = &in->arguments[0];
    const IROperand *right = &in->arguments[1];
    int left_nothing = left->kind == IR_OPERAND_INT && left->int_value == 0;
    int right_nothing = right->kind == IR_OPERAND_INT && right->int_value == 0;
    int same = safety_operand_same(left, right);
    const IROperand *result = NULL;
    IROperand nothing = ir_operand_int(0);
    if (right_nothing || (merge && same)) {
      result = left;
    } else if (left_nothing) {
      result = right;
    } else if (subtract && same) {
      result = &nothing;
    }
    if (!result) {
      continue;
    }
    IROperand dest = ir_operand_none();
    IROperand source = ir_operand_none();
    if (!ir_operand_clone(&in->dest, &dest) ||
        !ir_operand_clone(result, &source)) {
      ir_operand_destroy(&dest);
      ir_operand_destroy(&source);
      return -1;
    }
    ir_instruction_make_nop(in);
    in->op = IR_OP_ASSIGN;
    in->dest = dest;
    in->lhs = source;
    folded++;
  }
  return folded;
}

static int safety_simplify_plain_storage(IRProgram *program) {
  SafetyPlainStorage state = {0};
  state.program = program;
  size_t capacity = program->module_symbol_count;
  for (size_t f = 0; f < program->function_count; f++)
    capacity += program->functions[f]->instruction_count;
  state.arrays = calloc(capacity, sizeof(*state.arrays));
  if (!state.arrays && capacity) return 0;
  for (size_t s = 0; s < program->module_symbol_count; s++) {
    const IRModuleSymbol *symbol = &program->module_symbols[s];
    if (symbol->kind == IR_MODSYM_VARIABLE && symbol->type &&
        symbol->type->kind == MTLC_TYPE_ARRAY && !symbol->is_exported &&
        !symbol->is_extern && !symbol->is_volatile && !symbol->init_reloc_count)
      state.arrays[state.count++].symbol = symbol;
  }
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *fn = program->functions[f];
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      int local = in->op == IR_OP_DECLARE_LOCAL && in->value_type &&
                  in->value_type->kind == MTLC_TYPE_ARRAY;
      int allocation = in->op == IR_OP_CALL && in->text &&
          (!strcmp(in->text, "malloc") || !strcmp(in->text, "calloc") ||
           !strcmp(in->text, "mettle_heap_alloc") ||
           !strcmp(in->text, "mettle_heap_calloc")) &&
          safety_plain_definition(fn, &in->dest) == in;
      if (local || allocation) {
        state.arrays[state.count].scope = fn;
        state.arrays[state.count++].root = &in->dest;
      }
    }
  }
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *fn = program->functions[f];
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      /* Assembly can name any module symbol without an IR operand. */
      if (in->op == IR_OP_INLINE_ASM) {
        safety_trace("plain storage analysis keeps origins around assembly", in->location.line);
        free(state.arrays);
        return 1;
      }
      const IROperand *values[3] = {&in->dest, &in->lhs, &in->rhs};
      for (int v = 0; v < 3; v++) {
        if (v == 0 && (in->op == IR_OP_DECLARE_LOCAL ||
                       ir_instruction_writes_destination(in))) continue;
        SafetyPlainArray *array = safety_plain_root(&state, fn, values[v], 0);
        if (array && !safety_plain_allowed_use(&state, array, fn, in, v)) {
          if (safety_trace_enabled()) fprintf(stderr, "safety: plain storage escape in %s: %s operand %d\n", fn->name, ir_opcode_name(in->op), v);
          array->rejected = 1;
        }
      }
      for (size_t a = 0; a < in->argument_count; a++) {
        SafetyPlainArray *array = safety_plain_root(&state, fn, &in->arguments[a], 0);
        if (array && !safety_plain_allowed_use(&state, array, fn, in, (int)a + 3)) {
          if (safety_trace_enabled()) fprintf(stderr, "safety: plain storage argument escape in %s: %s argument %zu\n", fn->name, in->text ? in->text : "?", a);
          array->rejected = 1;
        }
      }
    }
  }
  /* An initializer can publish an address without an ADDRESS_OF instruction. */
  for (size_t s = 0; s < program->module_symbol_count; s++) {
    const IRModuleSymbol *symbol = &program->module_symbols[s];
    for (size_t a = 0; a < state.count; a++) {
      if (!state.arrays[a].symbol) continue;
      const char *name = state.arrays[a].symbol->name;
      if (symbol->init_symbol_ref && !strcmp(symbol->init_symbol_ref, name))
        state.arrays[a].rejected = 1;
      for (size_t r = 0; r < symbol->init_reloc_count; r++)
        if (symbol->init_relocs[r].symbol && !strcmp(symbol->init_relocs[r].symbol, name))
          state.arrays[a].rejected = 1;
    }
  }
  int changed;
  do {
    changed = 0;
    for (size_t f = 0; f < program->function_count; f++) {
      const IRFunction *fn = program->functions[f];
      for (size_t i = 0; i < fn->instruction_count; i++) {
        const IRInstruction *in = &fn->instructions[i];
        if (in->op != IR_OP_CALL || !in->text ||
            strcmp(in->text, "mettle_safety_value_store") || in->argument_count != 4)
          continue;
        SafetyPlainArray *array = safety_plain_root(&state, fn, &in->arguments[0], 0);
        const IROperand *path[64];
        if (array && !array->rejected &&
            safety_plain_tainted(&state, fn, &in->arguments[2], path, 0)) {
          array->rejected = 1;
          changed = 1;
        }
      }
    }
  } while (changed);
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      if (in->op != IR_OP_CALL || !in->text || !in->argument_count) continue;
      int load = !strcmp(in->text, "mettle_safety_value_load");
      int store = !strcmp(in->text, "mettle_safety_value_store");
      if (!load && !store) continue;
      SafetyPlainArray *array = safety_plain_root(&state, fn, &in->arguments[0], 0);
      if (!array || array->rejected) continue;
      IROperand dest = ir_operand_none();
      if (load && !ir_operand_clone(&in->dest, &dest)) { free(state.arrays); return 0; }
      ir_instruction_make_nop(in);
      if (load) {
        in->op = IR_OP_ASSIGN;
        in->dest = dest;
        in->lhs = ir_operand_int(0);
      }
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      const IROperand *path[64];
      if (!safety_plain_read_intrinsic(in) ||
          safety_plain_tainted(&state, fn, &in->dest, path, 0)) continue;
      IROperand dest = ir_operand_none();
      if (!ir_operand_clone(&in->dest, &dest)) { free(state.arrays); return 0; }
      ir_instruction_make_nop(in);
      in->op = IR_OP_ASSIGN;
      in->dest = dest;
      in->lhs = ir_operand_int(0);
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      for (size_t a = 0; a < in->argument_count + 2; a++) {
        IROperand *value = a == 0 ? &in->lhs : a == 1 ? &in->rhs :
                                                    &in->arguments[a - 2];
        const IROperand *path[64];
        if (value->name && !strncmp(value->name, ".safe_identity_", 15) &&
            !safety_plain_tainted(&state, fn, value, path, 0)) {
          ir_operand_destroy(value);
          *value = ir_operand_int(0);
        }
      }
    }
    if (safety_fold_identity_arithmetic(fn) < 0) {
      free(state.arrays);
      return 0;
    }

    /* Remove now unused shadow copies and reads. These exact runtime reads
     * have no effects; registration and lifetime calls always stay. */
    do {
      changed = 0;
      for (size_t i = 0; i < fn->instruction_count; i++) {
        IRInstruction *in = &fn->instructions[i];
        if (!in->dest.name || strncmp(in->dest.name, ".safe", 5)) continue;
        if (in->op != IR_OP_ASSIGN && in->op != IR_OP_DECLARE_LOCAL &&
            !safety_plain_read_intrinsic(in)) continue;
        if (in->op == IR_OP_DECLARE_LOCAL) {
          int written = 0;
          for (size_t w = 0; w < fn->instruction_count; w++)
            if (w != i && ir_instruction_writes_destination(&fn->instructions[w]) &&
                safety_operand_same(&fn->instructions[w].dest, &in->dest)) {
              written = 1;
              break;
            }
          if (written) continue;
        }
        if (!safety_plain_value_used(fn, &in->dest)) {
          ir_instruction_make_nop(in);
          changed = 1;
        }
      }
    } while (changed);
  }
  free(state.arrays);
  return 1;
}
