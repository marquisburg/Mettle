/* Allocation identities travel beside values as ordinary uint64 locals.
 * This pass runs before optimization, after allocation registration. */
typedef struct {
  IROperand value;
  IROperand identity;
  int memory;
  int active;
  size_t memory_size;
  MtlcType *type;
} SafetyOrigin;
typedef struct {
  SafetyOrigin *items;
  size_t count;
  size_t capacity;
  IRProgram *program;
  IRFunction *function;
  IRInstructionVector out;
  int return_prepared;
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
    origin->type = type;
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
  if (!origin->type) {
    for (size_t i = 0; i < origins->function->instruction_count; i++) {
      const IRInstruction *in = &origins->function->instructions[i];
      if (in->dest.kind == value->kind && in->dest.name &&
          !strcmp(in->dest.name, value->name) && in->op != IR_OP_STORE && in->value_type) {
        origin->type = in->value_type;
        break;
      }
    }
  }
  return 1;
}

static size_t origin_aggregate_size(SafetyOrigins *origins, const IROperand *value) {
  SafetyOrigin *origin = origin_find(origins, value);
  MtlcType *type = origin ? origin->type : NULL;
  return type && (type->kind == MTLC_TYPE_STRUCT || type->kind == MTLC_TYPE_ARRAY ||
                   type->kind == MTLC_TYPE_TAGGED_ENUM ||
                   type->kind == MTLC_TYPE_STRING) ? type->size : 0;
}

static int origin_value_is_string(SafetyOrigins *origins, const IROperand *value) {
  SafetyOrigin *origin = origin_find(origins, value);
  return origin && origin->type && origin->type->kind == MTLC_TYPE_STRING;
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
  if (value->kind == IR_OPERAND_STRING && value->name) {
    *identity = origin_fresh();
    long long length = value->int_value > 0 ? value->int_value : (long long)strlen(value->name);
    IROperand args[2] = {*value, ir_operand_int(length + 1)};
    return identity->name && origin_call(origins, location,
        "mettle_safety_string_identity", args, 2, identity);
  }
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

static int origin_return(SafetyOrigins *origins, SourceLocation location,
                          const IROperand *value, const IROperand *entry) {
  size_t size = origin_aggregate_size(origins, value);
  IROperand metadata = ir_operand_none();
  int ok;
  if (size) {
    if (!origin_address(origins, location, value, &metadata)) return 0;
    IROperand args[3] = {*entry, metadata, ir_operand_int((long long)size)};
    ok = origin_call(origins, location, "mettle_safety_call_return_copy", args, 3, NULL);
  } else {
    if (!origin_get(origins, location, value, &metadata)) return 0;
    IROperand args[2] = {*entry, metadata};
    ok = origin_call(origins, location, "mettle_safety_call_return", args, 2, NULL);
  }
  ir_operand_destroy(&metadata);
  return ok;
}

static int origin_instruction(SafetyOrigins *origins, IRInstruction *in,
                              const IROperand *entry) {
  if (in->op == IR_OP_SAFETY_CHECK) {
    if (in->argument_count != IR_SAFETY_ARG_COUNT) return 0;
    IROperand identity = ir_operand_none();
    if (!origin_get(origins, in->location,
                    &in->arguments[IR_SAFETY_ARG_BASE], &identity)) return 0;
    IROperand *args = realloc(in->arguments,
        IR_SAFETY_TRACKED_ARG_COUNT * sizeof(*args));
    if (!args) { ir_operand_destroy(&identity); return 0; }
    in->arguments = args;
    in->arguments[IR_SAFETY_ARG_IDENTITY] = identity;
    in->argument_count = IR_SAFETY_TRACKED_ARG_COUNT;
    return ir_instruction_vector_append_move(&origins->out, in);
  }
  SourceLocation location = in->location;
  IROperand dest = ir_operand_none(), lhs = ir_operand_none(), rhs = ir_operand_none();
  if (!ir_operand_clone(&in->dest, &dest) || !ir_operand_clone(&in->lhs, &lhs) ||
      !ir_operand_clone(&in->rhs, &rhs)) goto fail;
  SafetyOrigin *output = origin_find(origins, &dest);
  size_t aggregate_size = origin_aggregate_size(origins, &dest);
  if (aggregate_size) output = NULL;
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
                   strcmp(in->text, "mettle_safety_register_static") == 0 ||
                   strcmp(in->text, "mettle_safety_entry_arguments") == 0 ||
                   strcmp(in->text, "mettle_safety_region_begin") == 0) ? 0 :
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
    if (!origins->return_prepared && !origin_return(origins, location, &lhs, entry)) goto done;
    origins->return_prepared = 0;
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
      size_t size = origin_aggregate_size(origins, &in->arguments[a]);
      if (size) {
        IROperand address = ir_operand_none();
        if (!origin_address(origins, location, &in->arguments[a], &address)) goto done;
        IROperand args[4] = {frame, ir_operand_int((long long)a), address, ir_operand_int((long long)size)};
        int passed = origin_call(origins, location, "mettle_safety_call_arg_copy", args, 4, NULL);
        ir_operand_destroy(&address);
        if (!passed) goto done;
        continue;
      }
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
    if (aggregate_size) {
      IROperand address = ir_operand_none();
      if (!origin_address(origins, location, &dest, &address)) goto done;
      IROperand args[3] = {frame, address, ir_operand_int((long long)aggregate_size)};
      int passed = origin_call(origins, location, "mettle_safety_call_result_copy", args, 3, NULL);
      ir_operand_destroy(&address);
      if (!passed) goto done;
    }
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
    if (lhs.kind == IR_OPERAND_STRING && rhs.int_value == 8) {
      /* The frontend obtains a cstring by loading the data pointer from its
       * literal record. Passing the literal itself to a raw pointer runtime
       * parameter would select the characters, not that record's address. */
      IROperand args[2] = {dest, ir_operand_int((long long)ir_operand_string_length(&lhs) + 1)};
      if (!origin_call(origins, location, "mettle_safety_literal_identity", args, 2,
                       &output->identity)) goto done;
    } else {
      IROperand args[3] = {lhs, is_float ? ir_operand_int(0) : dest, rhs};
      if (!origin_call(origins, location, "mettle_safety_value_load", args, 3,
                       &output->identity)) goto done;
    }
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
  /* A string destination is handled by the STRING assignment below, which
     knows a literal source has no tags to copy yet and seeds them instead. */
  if (aggregate_size && op == IR_OP_ASSIGN &&
      !origin_value_is_string(origins, &dest)) {
    IROperand source = ir_operand_none(), destination = ir_operand_none();
    int copied = origin_address(origins, location, &lhs, &source) &&
        origin_address(origins, location, &dest, &destination);
    IROperand args[3] = {destination, source, ir_operand_int((long long)aggregate_size)};
    if (copied) copied = origin_call(origins, location, "mettle_safety_value_copy", args, 3, NULL);
    ir_operand_destroy(&source);
    ir_operand_destroy(&destination);
    if (!copied) goto done;
  }
  SafetyOrigin *stored = origin_find(origins, &dest);
  /* These helpers take pointers, and a string operand is storage, so the
     address is taken rather than left implicit: the two backends do not agree
     about what a bare string operand means where a pointer is declared, and an
     ADDRESS_OF says it once for both. */
  if (op == IR_OP_ASSIGN && stored && stored->type && stored->type->kind == MTLC_TYPE_STRING) {
    IROperand destination = ir_operand_none();
    if (!origin_address(origins, location, &dest, &destination)) goto done;
    if (lhs.kind == IR_OPERAND_STRING) {
      IROperand args[2] = {destination,
                           ir_operand_int((long long)ir_operand_string_length(&lhs) + 1)};
      int seeded = origin_call(origins, location, "mettle_safety_string_identity",
                               args, 2, NULL);
      ir_operand_destroy(&destination);
      if (!seeded) goto done;
    } else {
      IROperand source = ir_operand_none();
      int copied = origin_address(origins, location, &lhs, &source);
      if (copied) {
        IROperand args[3] = {destination, source,
                             ir_operand_int((long long)stored->type->size)};
        copied = origin_call(origins, location, "mettle_safety_value_copy", args, 3, NULL);
      }
      ir_operand_destroy(&source);
      ir_operand_destroy(&destination);
      if (!copied) goto done;
    }
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
  /* Aggregate temporaries need named storage for the byte metadata copy, just
   * as a source level aggregate local does. Keep the value's existing type. */
  for (size_t o = 0; o < origins.count; o++) {
    SafetyOrigin *origin = &origins.items[o];
    if (origin->value.kind != IR_OPERAND_TEMP || !origin_aggregate_size(&origins, &origin->value)) continue;
    const char *name = origin->value.name;
    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *in = &function->instructions[i];
      IROperand *operands[3] = {&in->dest, &in->lhs, &in->rhs};
      for (size_t a = 0; a < 3; a++)
        if (operands[a]->kind == IR_OPERAND_TEMP && operands[a]->name &&
            !strcmp(operands[a]->name, name)) operands[a]->kind = IR_OPERAND_SYMBOL;
      for (size_t a = 0; a < in->argument_count; a++)
        if (in->arguments[a].kind == IR_OPERAND_TEMP && in->arguments[a].name &&
            !strcmp(in->arguments[a].name, name)) in->arguments[a].kind = IR_OPERAND_SYMBOL;
    }
    origin->value.kind = IR_OPERAND_SYMBOL;
    IRInstruction decl = {0};
    decl.op = IR_OP_DECLARE_LOCAL;
    decl.location = function->location;
    decl.value_type = origin->type;
    decl.text = mettle_strdup(origin->type->name);
    if (!decl.text || !ir_operand_clone(&origin->value, &decl.dest) ||
        !ir_instruction_vector_append_move(&origins.out, &decl)) {
      ir_instruction_destroy_storage(&decl);
      goto done;
    }
  }
  /* A string literal handed to a `string` parameter needs storage of its own.
   *
   * The backend does not intern literals: two occurrences of the same text are
   * two records over two copies of the characters. So describing the record
   * written beside the call and passing the record written in the call
   * describes and passes different bytes, and the callee's read lands outside
   * the region the caller described. A literal has no address to hold either.
   * Copy it into a local once, describe that, and pass the local. This is the
   * same reason the aggregate temporaries above are given named storage.
   *
   * The callee's declared parameter type is what says a literal is bound to a
   * `string`; the same literal bound to a `cstring` is a different value and
   * must be left alone. */
  {
    MtlcType *string_type = ir_program_lookup_type(program, "string");
    for (size_t i = 0; string_type && i < function->instruction_count; i++) {
      IRInstruction *in = &function->instructions[i];
      const IRModuleSymbol *callee = NULL;
      if (in->op != IR_OP_CALL || !in->text || !in->argument_count) continue;
      if (strncmp(in->text, "mettle_safety_", 14) == 0) continue;
      callee = ir_program_lookup_symbol(program, in->text);
      if (!callee || callee->is_extern || !callee->param_types) continue;
      for (size_t a = 0; a < in->argument_count && a < callee->param_count; a++) {
        IROperand local, seed_args[2];
        IRInstruction decl = {0}, copy = {0};
        SafetyOrigin *origin = NULL;
        char name[64];
        long long chars;
        if (in->arguments[a].kind != IR_OPERAND_STRING || !in->arguments[a].name)
          continue;
        if (!callee->param_types[a] ||
            callee->param_types[a]->kind != MTLC_TYPE_STRING)
          continue;

        /* Not a `.safe` name: those are the pass's own scaffolding and
           origin_add skips them, and this local is a value to track. */
        snprintf(name, sizeof(name), ".strlit_%u", g_safety_next_id++);
        local = ir_operand_symbol(name);
        if (!local.name) goto done;
        chars = (long long)ir_operand_string_length(&in->arguments[a]) + 1;

        decl.op = IR_OP_DECLARE_LOCAL;
        decl.location = in->location;
        decl.value_type = string_type;
        decl.text = mettle_strdup("string");
        if (!decl.text || !ir_operand_clone(&local, &decl.dest) ||
            !ir_instruction_vector_append_move(&origins.out, &decl)) {
          ir_instruction_destroy_storage(&decl);
          ir_operand_destroy(&local);
          goto done;
        }

        copy.op = IR_OP_ASSIGN;
        copy.location = in->location;
        copy.value_type = string_type;
        if (!ir_operand_clone(&local, &copy.dest) ||
            !ir_operand_clone(&in->arguments[a], &copy.lhs) ||
            !ir_instruction_vector_append_move(&origins.out, &copy)) {
          ir_instruction_destroy_storage(&copy);
          ir_operand_destroy(&local);
          goto done;
        }

        {
          IROperand slot = ir_operand_none();
          int described = origin_address(&origins, in->location, &local, &slot);
          if (described) {
            seed_args[0] = slot;
            seed_args[1] = ir_operand_int(chars);
            described = origin_call(&origins, in->location,
                                    "mettle_safety_string_contents",
                                    seed_args, 2, NULL);
          }
          ir_operand_destroy(&slot);
          if (!described) {
            ir_operand_destroy(&local);
            goto done;
          }
        }

        if (!origin_add(&origins, &local)) {
          ir_operand_destroy(&local);
          goto done;
        }
        origin = origin_find(&origins, &local);
        if (origin) origin->type = string_type;
        ir_operand_destroy(&in->arguments[a]);
        in->arguments[a] = local; /* the call now passes the local */
      }
    }
  }
  for (size_t p = 0; p < function->parameter_count; p++) {
    IROperand parameter = ir_operand_symbol(function->parameter_names[p]);
    SafetyOrigin *origin = origin_find(&origins, &parameter);
    MtlcType *type = origin_symbol_type(&origins, parameter.name);
    if (origin && type && type->size > 0 && type->size <= 8) origin->active = 1;
    ir_operand_destroy(&parameter);
  }
  for (size_t i = 0; i < origins.count; i++)
    if (origins.items[i].memory) origins.items[i].active = 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_CALL && in->text && in->argument_count &&
        (!strcmp(in->text, "mettle_safety_region_begin") ||
         !strcmp(in->text, "mettle_safety_entry_arguments"))) {
      SafetyOrigin *origin = origin_find(&origins, &in->arguments[0]);
      if (origin) origin->active = 1;
    }
  }
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
          (in->op == IR_OP_ASSIGN && in->lhs.kind == IR_OPERAND_STRING) ||
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
    size_t size = origin_aggregate_size(&origins, &parameter);
    if (size) {
      IROperand address = ir_operand_none();
      int passed = origin_address(&origins, function->location, &parameter, &address);
      IROperand copy_args[4] = {entry, args[1], address, ir_operand_int((long long)size)};
      if (passed) passed = origin_call(&origins, function->location,
          "mettle_safety_call_param_copy", copy_args, 4, NULL);
      ir_operand_destroy(&address);
      ir_operand_destroy(&parameter);
      if (!passed) goto done;
      continue;
    }
    int passed = !origin || !origin->active || (origin_call(&origins, function->location,
        "mettle_safety_call_param", args, 2, &origin->identity) &&
        origin_save(&origins, function->location, &parameter));
    ir_operand_destroy(&parameter);
    if (!passed) goto done;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    /* Save return metadata while the local storage is still live. Stack
     * retirement clears its byte tags before the actual machine return. */
    if (!origins.return_prepared) {
      size_t next = i;
      while (next + 1 < function->instruction_count) {
        const IRInstruction *take = &function->instructions[next];
        const IRInstruction *call = &function->instructions[next + 1];
        if (take->op != IR_OP_ADDRESS_OF || !take->dest.name ||
            strncmp(take->dest.name, SAFETY_TEMP_PREFIX, strlen(SAFETY_TEMP_PREFIX)) ||
            call->op != IR_OP_CALL || !call->text || strcmp(call->text, "mettle_safety_unregister")) break;
        next += 2;
      }
      if (next > i && next < function->instruction_count && function->instructions[next].op == IR_OP_RETURN) {
        IRInstruction *ret = &function->instructions[next];
        if (!origin_return(&origins, ret->location, &ret->lhs, &entry)) goto done;
        origins.return_prepared = 1;
      }
    }
    if (!origin_instruction(&origins, &function->instructions[i], &entry)) goto done;
  }
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

/* Resolve aliases of the known runtime APIs before classifying their effects.
 * Otherwise `extern fn release(...) = "free"` bypasses retirement. */
static int safety_normalize_external_calls(IRProgram *program) {
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function || function->is_rule) continue;
    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *in = &function->instructions[i];
      if (in->op != IR_OP_CALL || !in->text) continue;
      const IRModuleSymbol *symbol = ir_program_lookup_symbol(program, in->text);
      if (!symbol || !symbol->is_extern || !symbol->link_name) continue;
      const char *link = symbol->link_name;
      IRInstruction probe = *in;
      probe.text = (char *)link;
      if (safety_classify_call(&probe) == SAFETY_ALLOC_NONE &&
          strncmp(link, "mettle_safety_", 14) && strcmp(link, "memcpy") &&
          strcmp(link, "memmove") && strcmp(link, "memset")) continue;
      char *name = mettle_strdup(link);
      if (!name) return 0;
      if (!ir_program_lookup_symbol(program, name)) {
        IRModuleSymbol declaration = *symbol;
        declaration.name = name;
        declaration.link_name = NULL;
        if (!ir_program_add_symbol(program, &declaration)) { free(name); return 0; }
      }
      free(in->text);
      in->text = name;
    }
  }
  return 1;
}

static int safety_wrap_allocator_address(IRProgram *program, char **reference) {
  if (!reference || !*reference) return 1;
  const IRModuleSymbol *symbol = ir_program_lookup_symbol(program, *reference);
  if (!symbol || symbol->kind != IR_MODSYM_FUNCTION) return 1;
  const char *target = symbol->link_name ? symbol->link_name : symbol->name;
  IRInstruction probe = {0};
  probe.op = IR_OP_CALL;
  probe.text = (char *)target;
  probe.argument_count = symbol->param_count;
  if (safety_classify_call(&probe) == SAFETY_ALLOC_NONE) return 1;
  char name[160];
  snprintf(name, sizeof(name), "__mettle_checked_%s", target);
  char *replacement = mettle_strdup(name);
  if (!replacement) return 0;
  if (!ir_program_lookup_symbol(program, name)) {
    IRFunction *wrapper = ir_function_create(name);
    const char *names[2] = {"pointer_or_size", "size"};
    const char *types[2] = {NULL, NULL};
    size_t count = symbol->param_count;
    for (size_t p = 0; p < count; p++) types[p] = symbol->param_types[p]->name;
    if (!wrapper || !ir_function_set_parameters(wrapper, names, types, count)) {
      ir_function_destroy(wrapper); free(replacement); return 0;
    }
    wrapper->return_type_name = mettle_strdup(symbol->return_type ? symbol->return_type->name : "void");
    IRInstruction call = {0}, ret = {0};
    IROperand arguments[2] = {ir_operand_symbol(names[0]), ir_operand_symbol(names[1])};
    call.op = IR_OP_CALL;
    call.text = symbol->name;
    call.arguments = arguments;
    call.argument_count = count;
    call.value_type = symbol->return_type;
    int returns = symbol->return_type && symbol->return_type->kind != MTLC_TYPE_VOID;
    call.dest = returns ? ir_operand_temp("result") : ir_operand_none();
    ret.op = IR_OP_RETURN;
    ret.lhs = call.dest;
    int ok = wrapper->return_type_name && ir_function_append_instruction(wrapper, &call) &&
        ir_function_append_instruction(wrapper, &ret);
    ir_operand_destroy(&arguments[0]);
    ir_operand_destroy(&arguments[1]);
    ir_operand_destroy(&call.dest);
    if (!ok) { ir_function_destroy(wrapper); free(replacement); return 0; }
    IRModuleSymbol declaration = *symbol;
    declaration.name = name;
    declaration.link_name = NULL;
    declaration.is_extern = 0;
    declaration.has_body = 1;
    if (!ir_program_add_symbol(program, &declaration) || !ir_program_add_function(program, wrapper)) {
      ir_function_destroy(wrapper); free(replacement); return 0;
    }
  }
  free(*reference);
  *reference = replacement;
  return 1;
}

static int safety_wrap_allocator_addresses(IRProgram *program) {
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *in = &function->instructions[i];
      if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
          !safety_wrap_allocator_address(program, &in->lhs.name)) return 0;
    }
  }
  /* Adding a wrapper can grow module_symbols, so keep no element pointer
   * across the call. The initializer's owned string stays valid. */
  size_t count = program->module_symbol_count;
  for (size_t i = 0; i < count; i++) {
    char *reference = program->module_symbols[i].init_symbol_ref;
    if (!safety_wrap_allocator_address(program, &reference)) return 0;
    program->module_symbols[i].init_symbol_ref = reference;
    for (size_t r = 0; r < program->module_symbols[i].init_reloc_count; r++) {
      reference = program->module_symbols[i].init_relocs[r].symbol;
      if (!safety_wrap_allocator_address(program, &reference)) return 0;
      program->module_symbols[i].init_relocs[r].symbol = reference;
    }
  }
  return 1;
}
