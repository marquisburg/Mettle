/* Allocation identities travel beside values as ordinary uint64 locals.
 * This pass runs before optimization, after allocation registration. */
typedef struct {
  IROperand value;
  IROperand identity;
  int memory;
  int active;
  size_t memory_size;
} SafetyOrigin;
typedef struct {
  SafetyOrigin *items;
  size_t count;
  size_t capacity;
  IRProgram *program;
  IRFunction *function;
  IRInstructionVector out;
} SafetyOrigins;

static IROperand origin_fresh(void) {
  char name[64];
  snprintf(name, sizeof(name), ".safe_origin_%u", g_safety_next_id++);
  return ir_operand_temp(name);
}

static int origin_call(SafetyOrigins *origins, SourceLocation location,
                       const char *name, const IROperand *args, size_t count,
                       const IROperand *result) {
  if (!safety_emit_call(&origins->out, location, name, args, count)) return 0;
  if (result && !ir_operand_clone(result,
        &origins->out.items[origins->out.count - 1].dest)) return 0;
  return 1;
}

static SafetyOrigin *origin_find(SafetyOrigins *origins, const IROperand *value) {
  if (!value || !value->name ||
      (value->kind != IR_OPERAND_TEMP && value->kind != IR_OPERAND_SYMBOL)) return NULL;
  for (size_t i = 0; i < origins->count; i++) {
    if (origins->items[i].value.kind == value->kind &&
        strcmp(origins->items[i].value.name, value->name) == 0) return &origins->items[i];
  }
  return NULL;
}

static MtlcType *origin_symbol_type(SafetyOrigins *origins, const char *name) {
  const IRInstruction *decl = ir_function_find_declaration(origins->function, name, 1);
  if (decl) return decl->value_type ? decl->value_type :
      ir_program_lookup_type(origins->program, decl->text);
  for (size_t i = 0; i < origins->function->parameter_count; i++) {
    if (strcmp(name, origins->function->parameter_names[i]) == 0)
      return ir_program_lookup_type(origins->program, origins->function->parameter_types[i]);
  }
  const IRModuleSymbol *symbol = ir_program_lookup_symbol(origins->program, name);
  return symbol && symbol->kind == IR_MODSYM_VARIABLE ? symbol->type : NULL;
}

static int origin_add(SafetyOrigins *origins, const IROperand *value) {
  if (!value->name || (value->kind != IR_OPERAND_TEMP && value->kind != IR_OPERAND_SYMBOL) ||
      strncmp(value->name, ".safe", 5) == 0 || origin_find(origins, value)) return 1;
  if (origins->count == origins->capacity) {
    size_t capacity = origins->capacity ? origins->capacity * 2 : 64;
    SafetyOrigin *items = realloc(origins->items, capacity * sizeof(*items));
    if (!items) return 0;
    origins->items = items;
    origins->capacity = capacity;
  }
  SafetyOrigin *origin = &origins->items[origins->count++];
  memset(origin, 0, sizeof(*origin));
  if (!ir_operand_clone(value, &origin->value)) return 0;
  char name[64];
  snprintf(name, sizeof(name), ".safe_identity_%u", g_safety_next_id++);
  origin->identity = ir_operand_symbol(name);
  if (!origin->identity.name) return 0;
  if (value->kind == IR_OPERAND_SYMBOL) {
    MtlcType *type = origin_symbol_type(origins, value->name);
    if (type && type->size > 0 && type->size <= 8 &&
        type->kind != MTLC_TYPE_ARRAY && type->kind != MTLC_TYPE_STRUCT &&
        type->kind != MTLC_TYPE_STRING && type->kind != MTLC_TYPE_TAGGED_ENUM) {
      origin->memory_size = type->size;
      const IRModuleSymbol *symbol = ir_program_lookup_symbol(origins->program, value->name);
      origin->memory = symbol && symbol->kind == IR_MODSYM_VARIABLE;
      for (size_t i = 0; i < origins->function->instruction_count; i++) {
        const IRInstruction *in = &origins->function->instructions[i];
        if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
            in->lhs.name && strcmp(in->lhs.name, value->name) == 0) origin->memory = 1;
      }
    }
  }
  return 1;
}

static int origin_address(SafetyOrigins *origins, SourceLocation location,
                          const IROperand *value, IROperand *address) {
  *address = origin_fresh();
  IRInstruction in = {0};
  in.op = IR_OP_ADDRESS_OF;
  in.location = location;
  if (!address->name || !ir_operand_clone(address, &in.dest) ||
      !ir_operand_clone(value, &in.lhs) ||
      !ir_instruction_vector_append_move(&origins->out, &in)) {
    ir_instruction_destroy_storage(&in);
    return 0;
  }
  return 1;
}

static int origin_get(SafetyOrigins *origins, SourceLocation location,
                      const IROperand *value, IROperand *identity) {
  SafetyOrigin *origin = origin_find(origins, value);
  if (!origin || !origin->active) { *identity = ir_operand_int(0); return 1; }
  if (!ir_operand_clone(&origin->identity, identity)) return 0;
  if (!origin->memory) return 1;
  IROperand address = ir_operand_none();
  if (!origin_address(origins, location, value, &address)) return 0;
  IROperand args[3] = {address, ir_operand_int(0), ir_operand_int((long long)origin->memory_size)};
  int ok = origin_call(origins, location, "mettle_safety_value_load", args, 3, identity);
  ir_operand_destroy(&address);
  return ok;
}

static int origin_assign(SafetyOrigins *origins, SourceLocation location,
                         const IROperand *dest, const IROperand *value) {
  IRInstruction in = {0};
  in.op = IR_OP_ASSIGN;
  in.location = location;
  in.value_type = (MtlcType *)mtlc_type_scalar(MTLC_TYPE_UINT64);
  if (!ir_operand_clone(dest, &in.dest) || !ir_operand_clone(value, &in.lhs) ||
      !ir_instruction_vector_append_move(&origins->out, &in)) {
    ir_instruction_destroy_storage(&in);
    return 0;
  }
  return 1;
}

static int origin_save(SafetyOrigins *origins, SourceLocation location,
                       const IROperand *value) {
  SafetyOrigin *origin = origin_find(origins, value);
  if (!origin || !origin->active || !origin->memory) return 1;
  IROperand address = ir_operand_none();
  if (!origin_address(origins, location, value, &address)) return 0;
  IROperand args[4] = {address, ir_operand_int(0), origin->identity,
                      ir_operand_int((long long)origin->memory_size)};
  int ok = origin_call(origins, location, "mettle_safety_value_store", args, 4, NULL);
  ir_operand_destroy(&address);
  return ok;
}

static int origin_capture(SafetyOrigins *origins, SourceLocation location,
                          const IROperand *value) {
  SafetyOrigin *origin = origin_find(origins, value);
  return !origin || !origin->active || (origin_call(origins, location, "mettle_safety_identity", value, 1,
                                &origin->identity) && origin_save(origins, location, value));
}

static int origin_runtime_check(SafetyOrigins *origins, IRInstruction *in) {
  IROperand identity = ir_operand_none();
  if (!origin_get(origins, in->location, &in->arguments[0], &identity)) return 0;
  size_t count = in->argument_count;
  IROperand *args = realloc(in->arguments, (count + 1) * sizeof(*args));
  if (!args) { ir_operand_destroy(&identity); return 0; }
  in->arguments = args;
  in->arguments[count] = identity;
  in->argument_count++;
  free(in->argument_types);
  in->argument_types = NULL;
  const char *name = strcmp(in->text, "mettle_safety_span") == 0
      ? "mettle_safety_span_identity" : "mettle_safety_check_identity";
  free(in->text);
  in->text = mettle_strdup(name);
  return in->text && ir_instruction_vector_append_move(&origins->out, in);
}

static int origin_instruction(SafetyOrigins *origins, IRInstruction *in,
                              const IROperand *entry) {
  SourceLocation location = in->location;
  IROperand dest = ir_operand_none(), lhs = ir_operand_none(), rhs = ir_operand_none();
  if (!ir_operand_clone(&in->dest, &dest) || !ir_operand_clone(&in->lhs, &lhs) ||
      !ir_operand_clone(&in->rhs, &rhs)) goto fail;
  SafetyOrigin *output = origin_find(origins, &dest);
  if (output && !output->active) output = NULL;
  IROperand left = ir_operand_none(), right = ir_operand_none();
  IROperand frame = ir_operand_none();
  IROperand memory_result = ir_operand_none();
  int ok = 0;
  int call = in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT;
  if (call && in->text && (strcmp(in->text, "mettle_safety_check") == 0 ||
                          strcmp(in->text, "mettle_safety_span") == 0)) {
    ok = origin_runtime_check(origins, in);
    goto done;
  }
  if (call && in->text && strncmp(in->text, "mettle_safety_", 14) == 0) {
    if (strcmp(in->text, "mettle_safety_unregister") == 0 &&
        origin_find(origins, &in->arguments[0])) {
      IROperand identity = ir_operand_none();
      if (!origin_get(origins, location, &in->arguments[0], &identity)) goto done;
      IROperand args[2] = {in->arguments[0], identity};
      int checked = origin_call(origins, location, "mettle_safety_free_identity", args, 2, NULL);
      ir_operand_destroy(&identity);
      if (!checked) goto done;
    }
    int capture = (strcmp(in->text, "mettle_safety_register") == 0 ||
                   strcmp(in->text, "mettle_safety_register_static") == 0) ? 0 :
                  strcmp(in->text, "mettle_safety_reregister") == 0 ? 1 : -1;
    IROperand result = ir_operand_none();
    if (capture >= 0 && !ir_operand_clone(&in->arguments[capture], &result)) goto done;
    ok = ir_instruction_vector_append_move(&origins->out, in);
    if (ok && capture >= 0) ok = origin_capture(origins, location, &result);
    ir_operand_destroy(&result);
    goto done;
  }

  if (!origin_get(origins, location, &lhs, &left) ||
      !origin_get(origins, location, &rhs, &right)) goto done;
  if (call && safety_classify_call(in) == SAFETY_ALLOC_REALLOC) {
    IROperand identity = ir_operand_none();
    if (!origin_get(origins, location, &in->arguments[0], &identity)) goto done;
    IROperand args[2] = {in->arguments[0], identity};
    int checked = origin_call(origins, location, "mettle_safety_free_identity", args, 2, NULL);
    ir_operand_destroy(&identity);
    if (!checked) goto done;
  }
  if (in->op == IR_OP_RETURN) {
    IROperand args[2] = {*entry, left};
    if (!origin_call(origins, location, "mettle_safety_call_return", args, 2, NULL)) goto done;
  }

  /* Call frames only surround compiled functions. Foreign functions keep their
   * ABI and unknown results carry identity zero until an allocator registers
   * them. Indirect calls also use a frame, claimed only by a matching callee. */
  int instrument_call = call && safety_classify_call(in) == SAFETY_ALLOC_NONE;
  if (instrument_call && in->op == IR_OP_CALL) {
    const IRModuleSymbol *symbol = ir_program_lookup_symbol(origins->program, in->text);
    instrument_call = symbol && !symbol->is_extern;
  }
  if (instrument_call) {
    IROperand callee = ir_operand_none();
    if (in->op == IR_OP_CALL) {
      IROperand name = ir_operand_symbol(in->text);
      int addressed = origin_address(origins, location, &name, &callee);
      ir_operand_destroy(&name);
      if (!addressed) goto done;
    } else if (!ir_operand_clone(&lhs, &callee)) goto done;
    frame = origin_fresh();
    IROperand args[2] = {callee, ir_operand_int((long long)in->argument_count)};
    int pushed = origin_call(origins, location, "mettle_safety_call_push", args, 2, &frame);
    ir_operand_destroy(&callee);
    if (!pushed) goto done;
    for (size_t a = 0; a < in->argument_count; a++) {
      IROperand token = ir_operand_none();
      if (!origin_get(origins, location, &in->arguments[a], &token)) goto done;
      IROperand arg[3] = {frame, ir_operand_int((long long)a), token};
      int passed = origin_call(origins, location, "mettle_safety_call_arg", arg, 3, NULL);
      ir_operand_destroy(&token);
      if (!passed) goto done;
    }
  }

  int memory_copy = call && in->text && in->argument_count == 3 &&
      (!strcmp(in->text, "memcpy") || !strcmp(in->text, "memmove"));
  int memory_fill = call && in->text && in->argument_count == 3 && !strcmp(in->text, "memset");
  int aggregate_copy = in->op == IR_OP_STORE && rhs.kind == IR_OPERAND_INT && rhs.int_value > 8;
  if (memory_copy || memory_fill) {
    if (!origin_get(origins, location, &in->arguments[0], &memory_result)) goto done;
    IROperand args[5] = {in->arguments[0], in->arguments[2], ir_operand_int(1),
                        ir_operand_int((long long)location.line), memory_result};
    if (!origin_call(origins, location, "mettle_safety_buffer_check", args, 5, NULL)) {
      ir_operand_destroy(&memory_result);
      goto done;
    }
    if (memory_copy) {
      IROperand source = ir_operand_none();
      if (!origin_get(origins, location, &in->arguments[1], &source)) {
        ir_operand_destroy(&memory_result);
        goto done;
      }
      args[0] = in->arguments[1]; args[2] = ir_operand_int(0); args[4] = source;
      int checked = origin_call(origins, location, "mettle_safety_buffer_check", args, 5, NULL);
      ir_operand_destroy(&source);
      if (!checked) { ir_operand_destroy(&memory_result); goto done; }
    }
  }
  if (memory_copy || aggregate_copy) {
    IROperand args[3] = {aggregate_copy ? dest : in->arguments[0],
                        aggregate_copy ? lhs : in->arguments[1],
                        aggregate_copy ? rhs : in->arguments[2]};
    if (!origin_call(origins, location, "mettle_safety_value_copy", args, 3, NULL)) {
      ir_operand_destroy(&memory_result);
      goto done;
    }
  } else if (memory_fill) {
    IROperand args[2] = {in->arguments[0], in->arguments[2]};
    if (!origin_call(origins, location, "mettle_safety_value_clear", args, 2, NULL)) {
      ir_operand_destroy(&memory_result);
      goto done;
    }
  }

  IROpcode op = in->op;
  int is_float = in->is_float;
  int subtract = op == IR_OP_BINARY && in->text && !strcmp(in->text, "-");
  int arithmetic = op == IR_OP_BINARY && in->text &&
      strcmp(in->text, "==") && strcmp(in->text, "!=") && strcmp(in->text, "<") &&
      strcmp(in->text, ">") && strcmp(in->text, "<=") && strcmp(in->text, ">=") &&
      strcmp(in->text, "&&") && strcmp(in->text, "||");
  if (!ir_instruction_vector_append_move(&origins->out, in)) {
    ir_operand_destroy(&memory_result);
    goto done;
  }
  if (output && (memory_copy || memory_fill)) {
    int assigned = origin_assign(origins, location, &output->identity, &memory_result);
    ir_operand_destroy(&memory_result);
    if (!assigned) goto done;
  } else if (instrument_call) {
    IROperand ignored = origin_fresh();
    ok = origin_call(origins, location, "mettle_safety_call_pop", &frame, 1,
                     output ? &output->identity : &ignored);
    ir_operand_destroy(&ignored);
    if (!ok) goto done;
  } else if (output && op == IR_OP_ADDRESS_OF) {
    if (!origin_capture(origins, location, &dest)) goto done;
  } else if (output && (op == IR_OP_ASSIGN || op == IR_OP_CAST || op == IR_OP_UNARY)) {
    if (!origin_assign(origins, location, &output->identity, &left)) goto done;
  } else if (output && arithmetic && !is_float) {
    IROperand args[2] = {left, right};
    if (left.kind == IR_OPERAND_INT && left.int_value == 0) {
      if (!origin_assign(origins, location, &output->identity, &right)) goto done;
    } else if (right.kind == IR_OPERAND_INT && right.int_value == 0) {
      if (!origin_assign(origins, location, &output->identity, &left)) goto done;
    } else if (!origin_call(origins, location, subtract ? "mettle_safety_subtract_identity" :
                           "mettle_safety_merge_identity", args, 2, &output->identity)) goto done;
  } else if (output && op == IR_OP_LOAD && rhs.kind == IR_OPERAND_INT &&
             rhs.int_value > 0 && rhs.int_value <= 8) {
    IROperand args[3] = {lhs, is_float ? ir_operand_int(0) : dest, rhs};
    if (!origin_call(origins, location, "mettle_safety_value_load", args, 3,
                     &output->identity)) goto done;
  } else if (output && ir_instruction_writes_destination(&origins->out.items[origins->out.count - 1]) &&
             op != IR_OP_DECLARE_LOCAL) {
    IROperand zero = ir_operand_int(0);
    if (!origin_assign(origins, location, &output->identity, &zero)) goto done;
  }
  if (op == IR_OP_STORE && rhs.kind == IR_OPERAND_INT && rhs.int_value <= 8) {
    IROperand zero = ir_operand_int(0);
    IROperand args[4] = {dest, is_float ? zero : lhs, left, rhs};
    if (!origin_call(origins, location, "mettle_safety_value_store", args, 4, NULL)) goto done;
  }
  if (output && op != IR_OP_STORE && op != IR_OP_DECLARE_LOCAL &&
      !origin_save(origins, location, &dest)) goto done;
  ok = 1;
done:
  ir_operand_destroy(&memory_result);
  ir_operand_destroy(&left);
  ir_operand_destroy(&right);
  ir_operand_destroy(&frame);
  ir_operand_destroy(&dest);
  ir_operand_destroy(&lhs);
  ir_operand_destroy(&rhs);
  return ok;
fail:
  ir_operand_destroy(&dest);
  ir_operand_destroy(&lhs);
  ir_operand_destroy(&rhs);
  return 0;
}

static int safety_origins_function(IRProgram *program, IRFunction *function) {
  SafetyOrigins origins = {0};
  origins.program = program;
  origins.function = function;
  IROperand entry = origin_fresh();
  IROperand address = ir_operand_none();
  int ok = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (!origin_add(&origins, &in->dest) || !origin_add(&origins, &in->lhs) ||
        !origin_add(&origins, &in->rhs)) goto done;
    for (size_t a = 0; a < in->argument_count; a++)
      if (!origin_add(&origins, &in->arguments[a])) goto done;
  }
  /* Only values which can receive an origin need a shadow local. Solve the
   * copy graph to a fixed point so loop carried aliases and branch joins keep
   * their identity without adding work to ordinary integer counters. */
  for (size_t p = 0; p < function->parameter_count; p++) {
    IROperand parameter = ir_operand_symbol(function->parameter_names[p]);
    SafetyOrigin *origin = origin_find(&origins, &parameter);
    MtlcType *type = origin_symbol_type(&origins, parameter.name);
    if (origin && type && type->size > 0 && type->size <= 8) origin->active = 1;
    ir_operand_destroy(&parameter);
  }
  for (size_t i = 0; i < origins.count; i++)
    if (origins.items[i].memory) origins.items[i].active = 1;
  int changed;
  do {
    changed = 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      SafetyOrigin *dest = origin_find(&origins, &in->dest);
      if (!dest || dest->active || in->op == IR_OP_STORE) continue;
      SafetyOrigin *left = origin_find(&origins, &in->lhs);
      SafetyOrigin *right = origin_find(&origins, &in->rhs);
      int root = in->op == IR_OP_ADDRESS_OF || in->op == IR_OP_NEW ||
          (in->op == IR_OP_LOAD && in->rhs.kind == IR_OPERAND_INT &&
           in->rhs.int_value > 0 && in->rhs.int_value <= 8);
      if (in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT) {
        const IRModuleSymbol *callee = in->text ? ir_program_lookup_symbol(program, in->text) : NULL;
        const MtlcType *type = callee ? callee->return_type : in->value_type;
        root = !type || (type->size > 0 && type->size <= 8);
      }
      int copy = in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST || in->op == IR_OP_UNARY ||
          (in->op == IR_OP_BINARY && in->text && strcmp(in->text, "==") &&
           strcmp(in->text, "!=") && strcmp(in->text, "<") && strcmp(in->text, ">") &&
           strcmp(in->text, "<=") && strcmp(in->text, ">=") && strcmp(in->text, "&&") && strcmp(in->text, "||"));
      if (root || (copy && ((left && left->active) || (right && right->active)))) {
        dest->active = 1;
        changed = 1;
      }
    }
  } while (changed);
  for (size_t i = 0; i < origins.count; i++) {
    if (!origins.items[i].active) continue;
    IRInstruction decl = {0};
    decl.op = IR_OP_DECLARE_LOCAL;
    decl.location = function->location;
    decl.text = mettle_strdup("uint64");
    decl.value_type = (MtlcType *)mtlc_type_scalar(MTLC_TYPE_UINT64);
    if (!decl.text || !ir_operand_clone(&origins.items[i].identity, &decl.dest) ||
        !ir_instruction_vector_append_move(&origins.out, &decl)) {
      ir_instruction_destroy_storage(&decl);
      goto done;
    }
    IROperand zero = ir_operand_int(0);
    if (!origin_assign(&origins, function->location, &origins.items[i].identity, &zero)) goto done;
  }
  IROperand name = ir_operand_symbol(function->name);
  int addressed = origin_address(&origins, function->location, &name, &address);
  ir_operand_destroy(&name);
  if (!addressed || !origin_call(&origins, function->location,
       "mettle_safety_call_enter", &address, 1, &entry)) goto done;
  for (size_t p = 0; p < function->parameter_count; p++) {
    IROperand parameter = ir_operand_symbol(function->parameter_names[p]);
    SafetyOrigin *origin = origin_find(&origins, &parameter);
    IROperand args[2] = {entry, ir_operand_int((long long)p)};
    int passed = !origin || !origin->active || (origin_call(&origins, function->location,
        "mettle_safety_call_param", args, 2, &origin->identity) &&
        origin_save(&origins, function->location, &parameter));
    ir_operand_destroy(&parameter);
    if (!passed) goto done;
  }
  for (size_t i = 0; i < function->instruction_count; i++)
    if (!origin_instruction(&origins, &function->instructions[i], &entry)) goto done;
  ok = ir_function_replace_instructions(function, &origins.out);
  if (ok) ir_function_clear_cfg(function);
done:
  ir_operand_destroy(&entry);
  ir_operand_destroy(&address);
  for (size_t i = 0; i < origins.count; i++) {
    ir_operand_destroy(&origins.items[i].value);
    ir_operand_destroy(&origins.items[i].identity);
  }
  free(origins.items);
  ir_instruction_vector_destroy(&origins.out);
  return ok;
}
