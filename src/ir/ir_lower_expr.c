// AST->IR lowering: expression and call lowering.
#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h" // mtlc_type_from_frontend (value_type baking)

int ir_lower_statement_or_expression(IRLoweringContext *context,
                                            IRFunction *function,
                                            ASTNode *node) {
  if (!node) {
    return 1;
  }
  // Treat known statement nodes as statements, otherwise treat as expression.
  switch (node->type) {
  case AST_VAR_DECLARATION:
  case AST_ASSIGNMENT:
  case AST_FUNCTION_CALL:
  case AST_GPU_LAUNCH:
  case AST_RETURN_STATEMENT:
  case AST_IF_STATEMENT:
  case AST_WHILE_STATEMENT:
  case AST_FOR_STATEMENT:
  case AST_SWITCH_STATEMENT:
  case AST_MATCH_STATEMENT:
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT:
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT:
  case AST_INLINE_ASM:
  case AST_PROGRAM:
    return ir_lower_statement_with_defers(context, function, node, NULL);
  default: {
    IROperand ignored = ir_operand_none();
    int ok = ir_lower_expression(context, function, node, &ignored);
    ir_operand_destroy(&ignored);
    return ok;
  }
  }
}


static MtlcType **ir_indirect_slot_types(ASTNode **argument_nodes,
                                        size_t argument_count, size_t lead) {
  MtlcType **slot_types = NULL;

  if (argument_count + lead == 0) {
    return NULL;
  }
  slot_types = calloc(argument_count + lead, sizeof(*slot_types));
  if (!slot_types) {
    return NULL;
  }
  for (size_t i = 0; i < argument_count; i++) {
    Type *argument_type =
        argument_nodes && argument_nodes[i] ? argument_nodes[i]->resolved_type
                                            : NULL;
    if (!argument_type || argument_type->kind == TYPE_STRUCT ||
        argument_type->kind == TYPE_ARRAY ||
        argument_type->kind == TYPE_STRING ||
        argument_type->kind == TYPE_SLICE ||
        argument_type->kind == TYPE_TAGGED_ENUM) {
      continue;
    }
    slot_types[lead + i] = mtlc_type_from_frontend(argument_type);
  }
  return slot_types;
}

static int ir_indirect_arg_passes_by_address(Type *type) {
  return type && type->name &&
         (type->kind == TYPE_STRUCT || type->kind == TYPE_SLICE ||
          type->kind == TYPE_TAGGED_ENUM) &&
         type->size > 8 && type->size <= (size_t)INT_MAX;
}

static int ir_indirect_return_passes_by_pointer(Type *type) {
  return type && type->name &&
         (type->kind == TYPE_STRUCT || type->kind == TYPE_STRING ||
          type->kind == TYPE_SLICE || type->kind == TYPE_TAGGED_ENUM) &&
         type->size > 8 && type->size <= (size_t)INT_MAX;
}

static int ir_make_indirect_return_slot(IRLoweringContext *context,
                                        IRFunction *function, Type *type,
                                        SourceLocation location,
                                        IROperand *out_address) {
  char *slot_name = ir_new_label_name(context, "agg_ret");

  if (!slot_name) {
    ir_set_error(context, "Out of memory while reserving an aggregate return");
    return 0;
  }
  if (!ir_emit_local_declaration(context, function, slot_name, type->name,
                                 location) ||
      !ir_emit_address_of_symbol(context, function, slot_name, location,
                                 out_address)) {
    free(slot_name);
    ir_operand_destroy(out_address);
    return 0;
  }
  free(slot_name);
  return 1;
}

static int ir_pass_aggregate_argument_by_address(IRLoweringContext *context,
                                                 IRFunction *function,
                                                 IROperand *argument,
                                                 Type *type,
                                                 SourceLocation location) {
  char *copy_name = ir_new_label_name(context, "agg_arg");
  IROperand copy_address = ir_operand_none();

  if (!copy_name) {
    ir_set_error(context, "Out of memory while copying aggregate argument");
    return 0;
  }
  if (!ir_emit_local_declaration(context, function, copy_name, type->name,
                                 location) ||
      !ir_emit_address_of_symbol(context, function, copy_name, location,
                                 &copy_address)) {
    free(copy_name);
    ir_operand_destroy(&copy_address);
    return 0;
  }
  free(copy_name);
  if (!ir_try_emit_aggregate_address_memcpy(context, function, &copy_address,
                                            argument, type, location)) {
    ir_operand_destroy(&copy_address);
    ir_set_error(context, "Cannot pass this aggregate through an indirect call");
    return 0;
  }
  ir_operand_destroy(argument);
  *argument = copy_address;
  return 1;
}

static void ir_declare_interpolation_helper(IRLoweringContext *context,
                                            const char *name) {
  IRModuleSymbol entry = {0};
  MtlcType *params[1];
  if (!context || !context->program || !name || !context->type_checker) {
    return;
  }
  if (ir_program_lookup_symbol(context->program, name)) {
    return;
  }
  params[0] = mtlc_type_from_frontend(context->type_checker->builtin_int64);
  entry.name = (char *)name;
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = 1;
  entry.type = mtlc_type_from_frontend(context->type_checker->builtin_string);
  entry.return_type = entry.type;
  entry.param_types = params;
  entry.param_count = 1;
  ir_program_add_symbol(context->program, &entry);
}

static void ir_declare_string_concat_helper(IRLoweringContext *context) {
  IRModuleSymbol entry = {0};
  MtlcType *params[2];
  if (!context || !context->program || !context->type_checker) {
    return;
  }
  if (ir_program_lookup_symbol(context->program, "mettle_string_concat")) {
    return;
  }
  params[0] = mtlc_type_from_frontend(context->type_checker->builtin_string);
  params[1] = params[0];
  entry.name = (char *)"mettle_string_concat";
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = 1;
  entry.type = params[0];
  entry.return_type = entry.type;
  entry.param_types = params;
  entry.param_count = 2;
  ir_program_add_symbol(context->program, &entry);
}

static int ir_lower_interpolation(IRLoweringContext *context,
                                  IRFunction *function,
                                  ASTNode *expression,
                                  IROperand *out_value);

static int ir_task_argument_is_repeatable(ASTNode *node) {
  int guard = 0;
  while (node && guard++ < 16) {
    if (node->type == AST_CAST_EXPRESSION) {
      CastExpression *cast = (CastExpression *)node->data;
      node = cast ? cast->operand : NULL;
      continue;
    }
    if (node->type == AST_IDENTIFIER) {
      return 1;
    }
    if (node->type == AST_MEMBER_ACCESS) {
      MemberAccess *member = (MemberAccess *)node->data;
      node = member ? member->object : NULL;
      continue;
    }
    if (node->type == AST_UNARY_EXPRESSION) {
      UnaryExpression *unary = (UnaryExpression *)node->data;
      if (!unary || !unary->operator ||
          (strcmp(unary->operator, "&") != 0 &&
           strcmp(unary->operator, "*") != 0)) {
        return 0;
      }
      node = unary->operand;
      continue;
    }
    if (node->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
      if (!index || !index->index ||
          (index->index->type != AST_NUMBER_LITERAL &&
           index->index->type != AST_IDENTIFIER)) {
        return 0;
      }
      node = index->array;
      continue;
    }
    return 0;
  }
  return 0;
}

static int ir_emit_task_capture_check(IRLoweringContext *context,
                                      IRFunction *function,
                                      ASTNode *expression,
                                      CallExpression *call) {
  IROperand pointer = ir_operand_none();
  IRInstruction helper = {0};
  int ok = 0;
  ASTNode *argument = NULL;
  if (call->task_capture_argument >= call->argument_count ||
      !call->arguments) {
    return 1;
  }
  argument = call->arguments[call->task_capture_argument];
  if (!ir_task_argument_is_repeatable(argument)) {
    return 1;
  }
  if (!ir_lower_expression(context, function, argument, &pointer)) {
    return 0;
  }
  helper.op = IR_OP_CALL;
  helper.location = expression->location;
  helper.text = "mettle_safety_task_capture_check";
  helper.argument_count = 4;
  helper.arguments = calloc(4, sizeof(IROperand));
  if (!helper.arguments) {
    ir_operand_destroy(&pointer);
    return 0;
  }
  helper.arguments[0] = pointer;
  helper.arguments[1] = ir_operand_string(
      call->task_entry_name ? call->task_entry_name : "?");
  helper.arguments[2] = ir_operand_string(
      context->current_function_name ? context->current_function_name : "?");
  helper.arguments[3] = ir_operand_int((long long)expression->location.line);
  ok = ir_emit(context, function, &helper);
  context->emitted_task_check = 1;
  ir_operand_destroy(&helper.arguments[1]);
  ir_operand_destroy(&helper.arguments[2]);
  free(helper.arguments);
  ir_operand_destroy(&pointer);
  return ok;
}

int ir_lower_call_expression(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *expression,
                                    IROperand *out_value) {
  CallExpression *call = (CallExpression *)expression->data;
  Symbol *callee_symbol = NULL;
  if (!call || !call->function_name) {
    ir_set_error(context, "Malformed call expression");
    return 0;
  }

  if (context->emit_task_checks &&
      call->task_capture_argument != SIZE_MAX &&
      !ir_emit_task_capture_check(context, function, expression, call)) {
    return 0;
  }

  if (strcmp(call->function_name, "typeof") == 0) {
    if (context->type_checker && context->type_checker->builtin_type) {
      type_checker_reject_comptime_escape(context->type_checker,
                                          expression->location,
                                          context->type_checker->builtin_type);
    }
    ir_set_error(context,
                 "value of type 'Type' cannot escape into runtime code");
    return 0;
  }

  if (strcmp(call->function_name, "offsetof") == 0) {
    long long offset = 0;
    if (!context->type_checker ||
        !type_checker_eval_offsetof(context->type_checker, call,
                                    expression->location, &offset)) {
      ir_set_error(context, "Unable to lower offsetof expression");
      return 0;
    }
    *out_value = ir_operand_int(offset);
    return 1;
  }

  if (strcmp(call->function_name, "layoutof") == 0) {
    long long digest = 0;
    if (!context->type_checker ||
        !type_checker_eval_layoutof(context->type_checker, call,
                                    expression->location, &digest)) {
      ir_set_error(context, "Unable to lower layoutof expression");
      return 0;
    }
    *out_value = ir_operand_int(digest);
    return 1;
  }

  if (strcmp(call->function_name, "fieldof") == 0) {
    if (context->type_checker && context->type_checker->builtin_field) {
      type_checker_reject_comptime_escape(
          context->type_checker, expression->location,
          context->type_checker->builtin_field);
    }
    ir_set_error(context,
                 "value of type 'Field' cannot escape into runtime code");
    return 0;
  }

  if (strcmp(call->function_name, "sizeof") == 0) {
    if (call->argument_count != 1 || !call->arguments ||
        !call->arguments[0] || call->arguments[0]->type != AST_IDENTIFIER) {
      ir_set_error(context, "Malformed sizeof expression");
      return 0;
    }

    Identifier *type_id = (Identifier *)call->arguments[0]->data;
    Type *type = (context->type_checker && type_id && type_id->name)
                     ? type_checker_get_type_by_name(context->type_checker,
                                                     type_id->name)
                     : NULL;
    if (!type || type->size > (size_t)LLONG_MAX) {
      ir_set_error(context, "Unable to lower sizeof expression");
      return 0;
    }
    if (type_contains_comptime_only(type)) {
      if (context->type_checker) {
        type_checker_reject_no_runtime_repr(context->type_checker,
                                            expression->location, type);
      }
      ir_set_error(context, "type '%s' has no runtime representation",
                   type->name ? type->name : "Type");
      return 0;
    }

    *out_value = ir_operand_int((long long)type->size);
    return 1;
  }

  if (strcmp(call->function_name, "static_assert") == 0) {
    *out_value = ir_operand_none();
    return 1;
  }

  if (strcmp(call->function_name, "syscall") == 0) {
    if (call->argument_count == 0) {
      ir_set_error(context, "Malformed system call reached IR lowering");
      return 0;
    }
    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      return 0;
    }
    IROperand *operands = calloc(call->argument_count, sizeof(IROperand));
    if (!operands) {
      ir_operand_destroy(&destination);
      ir_set_error(context, "Out of memory while lowering a system call");
      return 0;
    }
    for (size_t i = 0; i < call->argument_count; i++) {
      if (!ir_lower_expression(context, function, call->arguments[i],
                               &operands[i])) {
        for (size_t j = 0; j < i; j++) {
          ir_operand_destroy(&operands[j]);
        }
        free(operands);
        ir_operand_destroy(&destination);
        return 0;
      }
    }
    IRInstruction instruction = {0};
    instruction.op = IR_OP_CALL;
    instruction.text = (char *)IR_SYSCALL_CALL_NAME;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.arguments = operands;
    instruction.argument_count = call->argument_count;
    instruction.value_type =
        expression->resolved_type
            ? mtlc_type_from_frontend(expression->resolved_type)
            : NULL;
    int emitted = ir_emit(context, function, &instruction);
    for (size_t i = 0; i < call->argument_count; i++) {
      ir_operand_destroy(&operands[i]);
    }
    free(operands);
    if (!emitted) {
      ir_operand_destroy(&destination);
      return 0;
    }
    *out_value = destination;
    return 1;
  }

  /* String interpolation conversion. The parser wraps each "{expr}" in
   * __mtl_interp(); rewrite it here to the runtime helper the value's type
   * picks, the same injected-call scheme mettle_string_eq uses. A string value
   * passes through untouched. */
  if (strcmp(call->function_name, "__mtl_interp") == 0) {
    return ir_lower_interpolation(context, function, expression, out_value);
  }

  if (call->is_gpu_async_copy) {
    IRInstruction instruction = {0};
    instruction.location = expression->location;
    if (!strcmp(call->function_name, "async_copy_workgroup")) {
      instruction.async_copy_element_count = call->async_copy_element_count;
      instruction.async_copy_transaction_bytes =
          call->async_copy_transaction_bytes;
      instruction.async_copy_cache = call->async_copy_cache;
      if (call->argument_count < 3 ||
          instruction.async_copy_element_count == 0) {
        ir_set_error(context,
                     "Invalid asynchronous workgroup copy reached IR lowering");
        return 0;
      }
      instruction.op = IR_OP_ASYNC_COPY;
      instruction.argument_count = 2;
      instruction.arguments = calloc(2, sizeof(*instruction.arguments));
      instruction.argument_types =
          calloc(2, sizeof(*instruction.argument_types));
      if (!instruction.arguments || !instruction.argument_types) {
        free(instruction.arguments);
        free(instruction.argument_types);
        ir_set_error(context, "Out of memory lowering asynchronous copy");
        return 0;
      }
      for (size_t i = 0; i < 2; i++) {
        ASTNode *argument = call->arguments[i];
        if (!argument ||
            !ir_lower_expression(context, function, argument,
                                 &instruction.arguments[i])) {
          for (size_t j = 0; j < i; j++)
            ir_operand_destroy(&instruction.arguments[j]);
          free(instruction.arguments);
          free(instruction.argument_types);
          return 0;
        }
        instruction.argument_types[i] =
            argument->resolved_type
                ? mtlc_type_from_frontend(argument->resolved_type)
                : NULL;
      }
    } else if (!strcmp(call->function_name, "async_copy_commit")) {
      instruction.op = IR_OP_ASYNC_COMMIT;
    } else if (!strcmp(call->function_name, "async_copy_wait")) {
      instruction.op = IR_OP_ASYNC_WAIT;
      instruction.async_copy_pending_groups =
          call->async_copy_pending_groups;
    } else {
      ir_set_error(context,
                   "Unknown asynchronous workgroup copy operation reached IR lowering");
      return 0;
    }
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < instruction.argument_count; i++)
        ir_operand_destroy(&instruction.arguments[i]);
      free(instruction.arguments);
      free(instruction.argument_types);
      return 0;
    }
    for (size_t i = 0; i < instruction.argument_count; i++)
      ir_operand_destroy(&instruction.arguments[i]);
    free(instruction.arguments);
    free(instruction.argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_tensor_transfer) {
    int has_view = call->tensor_transfer_view_argument != SIZE_MAX;
    size_t count = ir_tensor_transfer_operand_count(
        &call->tensor_transfer_desc, has_view);
    size_t source_indices[3 + MTLC_TENSOR_MAX_RANK] = {0};
    size_t source_count = 0;
    IROperand *arguments = NULL;
    MtlcType **argument_types = NULL;
    source_indices[source_count++] = 0;
    source_indices[source_count++] = 1;
    if (has_view)
      source_indices[source_count++] = call->tensor_transfer_view_argument;
    for (uint8_t dimension = 0;
         dimension < call->tensor_transfer_desc.rank; dimension++)
      source_indices[source_count++] =
          call->tensor_transfer_coordinate_arguments[dimension];
    if (!count || count != source_count) {
      ir_set_error(context,
                   "Invalid tensor transfer descriptor reached IR lowering");
      return 0;
    }
    arguments = calloc(count, sizeof(*arguments));
    argument_types = calloc(count, sizeof(*argument_types));
    if (!arguments || !argument_types) {
      free(arguments);
      free(argument_types);
      ir_set_error(context, "Out of memory lowering tensor transfer");
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      size_t source_index = source_indices[i];
      ASTNode *source = source_index < call->argument_count
                            ? call->arguments[source_index]
                            : NULL;
      if (!source || !ir_lower_expression(context, function, source,
                                          &arguments[i])) {
        for (size_t j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        free(argument_types);
        return 0;
      }
      argument_types[i] = source->resolved_type
                              ? mtlc_type_from_frontend(source->resolved_type)
                              : NULL;
    }
    IRInstruction instruction = {0};
    /* Stack block: ir_emit deep-copies it, and heap_owned == 0 means no destroy
     * path can try to free stack memory. */
    IRTensorAux tensor;
    ir_instruction_tensor_attach(&instruction, &tensor);
    tensor.transfer = call->tensor_transfer_desc;
    instruction.op = IR_OP_TENSOR_TRANSFER;
    instruction.location = expression->location;
    instruction.arguments = arguments;
    instruction.argument_types = argument_types;
    instruction.argument_count = count;
    instruction.tensor_transfer_has_prepared_view = has_view;
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
      free(arguments);
      free(argument_types);
      return 0;
    }
    for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    free(argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_tensor_epilogue) {
    size_t count =
        ir_tensor_epilogue_operand_count(&call->tensor_epilogue_desc);
    size_t source_indices[8] = {0, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                SIZE_MAX, SIZE_MAX, SIZE_MAX};
    size_t source_count = 1;
    IROperand *arguments = NULL;
    MtlcType **argument_types = NULL;
    if (call->tensor_epilogue_bias_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_bias_argument;
    if (call->tensor_epilogue_alpha_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_alpha_argument;
    if (call->tensor_epilogue_beta_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_beta_argument;
    if (call->tensor_epilogue_clamp_min_argument != SIZE_MAX)
      source_indices[source_count++] =
          call->tensor_epilogue_clamp_min_argument;
    if (call->tensor_epilogue_clamp_max_argument != SIZE_MAX)
      source_indices[source_count++] =
          call->tensor_epilogue_clamp_max_argument;
    if (call->tensor_epilogue_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_stride_argument;
    if (call->tensor_epilogue_bias_stride_argument != SIZE_MAX)
      source_indices[source_count++] =
          call->tensor_epilogue_bias_stride_argument;
    if (!count || count != source_count) {
      ir_set_error(context,
                   "Invalid tensor epilogue descriptor reached IR lowering");
      return 0;
    }
    arguments = calloc(count, sizeof(*arguments));
    argument_types = calloc(count, sizeof(*argument_types));
    if (!arguments || !argument_types) {
      free(arguments);
      free(argument_types);
      ir_set_error(context, "Out of memory lowering tensor epilogue");
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      size_t source_index = source_indices[i];
      ASTNode *source = source_index < call->argument_count
                            ? call->arguments[source_index]
                            : NULL;
      if (!source || !ir_lower_expression(context, function, source,
                                          &arguments[i])) {
        for (size_t j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        free(argument_types);
        return 0;
      }
      argument_types[i] = source->resolved_type
                              ? mtlc_type_from_frontend(source->resolved_type)
                              : NULL;
    }
    IRInstruction instruction = {0};
    IRTensorAux tensor;
    ir_instruction_tensor_attach(&instruction, &tensor);
    tensor.epilogue = call->tensor_epilogue_desc;
    instruction.op = IR_OP_TENSOR_EPILOGUE;
    instruction.location = expression->location;
    instruction.arguments = arguments;
    instruction.argument_types = argument_types;
    instruction.argument_count = count;
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
      free(arguments);
      free(argument_types);
      return 0;
    }
    for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    free(argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_tensor_mma || call->is_tensor_matmul) {
    size_t count = call->is_tensor_matmul
                       ? ir_tensor_matmul_operand_count(&call->tensor_mma_desc)
                       : ir_tensor_mma_operand_count(&call->tensor_mma_desc);
    size_t source_indices[16] = {0, 1, 2, 3, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                 SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                 SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                 SIZE_MAX};
    size_t source_count = 4;
    IROperand *arguments = NULL;
    MtlcType **argument_types = NULL;
    if (call->tensor_metadata_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_metadata_argument;
    if (call->tensor_a_scale_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_a_scale_argument;
    if (call->tensor_b_scale_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_b_scale_argument;
    if (call->tensor_a_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_a_stride_argument;
    if (call->tensor_b_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_b_stride_argument;
    if (call->tensor_c_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_c_stride_argument;
    if (call->tensor_d_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_d_stride_argument;
    if (call->is_tensor_matmul) {
      for (size_t i = 4; i < 9; i++) source_indices[source_count++] = i;
    }
    if (!count || count != source_count) {
      ir_set_error(context,
                   "Invalid tensor matrix descriptor reached IR lowering");
      return 0;
    }
    arguments = calloc(count, sizeof(*arguments));
    argument_types = calloc(count, sizeof(*argument_types));
    if (!arguments || !argument_types) {
      free(arguments);
      free(argument_types);
      ir_set_error(context, "Out of memory lowering tensor matrix operation");
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      size_t source_index = source_indices[i];
      ASTNode *source = source_index < call->argument_count
                            ? call->arguments[source_index]
                            : NULL;
      if (!source || !ir_lower_expression(context, function, source,
                                          &arguments[i])) {
        for (size_t j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        free(argument_types);
        return 0;
      }
      argument_types[i] = source->resolved_type
                              ? mtlc_type_from_frontend(source->resolved_type)
                              : NULL;
    }
    IRInstruction instruction = {0};
    IRTensorAux tensor;
    ir_instruction_tensor_attach(&instruction, &tensor);
    tensor.mma = call->tensor_mma_desc;
    instruction.op = call->is_tensor_matmul ? IR_OP_TENSOR_MATMUL
                                            : IR_OP_TENSOR_MMA;
    instruction.location = expression->location;
    instruction.arguments = arguments;
    instruction.argument_types = argument_types;
    instruction.argument_count = count;
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
      free(arguments);
      free(argument_types);
      return 0;
    }
    for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    free(argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_gpu_atomic) {
    MtlcIntrinsic intrinsic = ir_intrinsic_from_name(call->function_name);
    int arity = ir_intrinsic_arity(intrinsic);
    int returns_void =
        ir_intrinsic_atomic_result_kind(intrinsic) == MTLC_TYPE_VOID;
    IROperand destination = ir_operand_none();
    IROperand *arguments = NULL;
    if (!ir_intrinsic_is_atomic(intrinsic) || arity < 0 ||
        call->argument_count < (size_t)arity ||
        call->atomic_address_space == MTLC_ADDRESS_SPACE_DEFAULT ||
        call->atomic_memory_order == MTLC_MEMORY_ORDER_DEFAULT ||
        call->atomic_memory_scope == MTLC_MEMORY_SCOPE_DEFAULT) {
      ir_set_error(context, "Invalid native atomic reached IR lowering");
      return 0;
    }
    if (!returns_void && !ir_make_temp_operand(context, &destination))
      return 0;
    arguments = calloc((size_t)arity, sizeof(*arguments));
    if (!arguments) {
      ir_operand_destroy(&destination);
      ir_set_error(context, "Out of memory lowering native atomic");
      return 0;
    }
    for (int i = 0; i < arity; i++) {
      if (!ir_lower_expression(context, function, call->arguments[i],
                               &arguments[i])) {
        for (int j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        ir_operand_destroy(&destination);
        return 0;
      }
    }
    IRInstruction instruction = {0};
    instruction.op = IR_OP_CALL;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.arguments = arguments;
    instruction.argument_count = (size_t)arity;
    instruction.text = call->function_name;
    instruction.intrinsic = intrinsic;
    instruction.address_space = call->atomic_address_space;
    instruction.memory_order = call->atomic_memory_order;
    instruction.failure_memory_order = call->atomic_failure_order;
    instruction.memory_scope = call->atomic_memory_scope;
    instruction.value_type = expression->resolved_type
                                 ? mtlc_type_from_frontend(
                                       expression->resolved_type)
                                 : NULL;
    int ok = ir_emit(context, function, &instruction);
    for (int i = 0; i < arity; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    if (!ok) {
      ir_operand_destroy(&destination);
      return 0;
    }
    *out_value = destination;
    return 1;
  }

  callee_symbol = context->symbol_table
                      ? symbol_table_lookup(context->symbol_table,
                                            call->function_name)
                      : NULL;
  if (callee_symbol &&
      callee_symbol->kind == SYMBOL_TAGGED_ENUM_CONSTRUCTOR) {
    return ir_lower_tagged_enum_constructor_call(
        context, function, expression, callee_symbol, out_value);
  }

  int is_func_ptr_var = call->is_indirect_call;

  IROperand destination = ir_operand_none();
  /* A void call has no SSA result. Keeping a synthetic destination used to be
   * mostly harmless for the host backend, but it gives target-neutral device
   * calls a false value and makes a frontend detail leak into both GPU ABIs. */
  int returns_void = expression->resolved_type &&
                     expression->resolved_type->kind == TYPE_VOID;
  if (!returns_void && !ir_make_temp_operand(context, &destination)) {
    return 0;
  }

  IROperand *arguments = NULL;
  if (call->argument_count > 0) {
    arguments = calloc(call->argument_count, sizeof(IROperand));
    if (!arguments) {
      ir_operand_destroy(&destination);
      ir_set_error(context, "Out of memory while lowering call arguments");
      return 0;
    }
  }

  for (size_t i = 0; i < call->argument_count; i++) {
    /* An aggregate literal has no value of its own to lower: it takes the type
       of what it initializes, which here is the parameter. Give it a home and
       pass that, so a struct or an array can be written at the call. */
    if (call->arguments[i] &&
        call->arguments[i]->type == AST_AGGREGATE_LITERAL &&
        call->arguments[i]->resolved_type) {
      Type *literal_type = call->arguments[i]->resolved_type;
      char *home = ir_new_label_name(context, "arg_literal");
      if (!home ||
          !ir_emit_local_declaration(context, function, home,
                                     literal_type->name,
                                     call->arguments[i]->location) ||
          !ir_emit_aggregate_literal_copy_to_symbol(
              context, function, home, call->arguments[i], literal_type,
              call->arguments[i]->location)) {
        free(home);
        for (size_t j = 0; j < i; j++) {
          ir_operand_destroy(&arguments[j]);
        }
        free(arguments);
        ir_operand_destroy(&destination);
        return 0;
      }
      arguments[i] = ir_operand_symbol(home);
      free(home);
      if (!arguments[i].name) {
        for (size_t j = 0; j < i; j++) {
          ir_operand_destroy(&arguments[j]);
        }
        free(arguments);
        ir_operand_destroy(&destination);
        return 0;
      }
      continue;
    }
    if (!ir_lower_expression(context, function, call->arguments[i],
                             &arguments[i])) {
      for (size_t j = 0; j < i; j++) {
        ir_operand_destroy(&arguments[j]);
      }
      free(arguments);
      ir_operand_destroy(&destination);
      return 0;
    }
  }

  /* Give width-less float literal arguments the declared parameter precision
   * so a float32 parameter receives a single-precision value, not a truncated
   * double, and hand an array argument its address rather than its bytes. */
  Type **call_param_types = NULL;
  size_t call_param_count = 0;
  if (callee_symbol && callee_symbol->kind == SYMBOL_FUNCTION) {
    call_param_types = callee_symbol->data.function.parameter_types;
    call_param_count = callee_symbol->data.function.parameter_count;
  } else if (is_func_ptr_var) {
    /* A call through a function-pointer variable names no callee symbol, so
     * the signature comes from the variable's own type. A local's scope is
     * gone by lowering time, so its declared spelling comes from the binding. */
    const IRLocalBinding *fp_binding =
        ir_local_binding_find(context, call->function_name);
    Type *fp_type = ir_lookup_symbol_type(context, call->function_name);
    if (!fp_type && fp_binding) {
      fp_type = ir_resolve_named_type(context, fp_binding->type_text);
    }
    if (fp_type && fp_type->kind == TYPE_FUNCTION_POINTER) {
      call_param_types = fp_type->fn_param_types;
      call_param_count = fp_type->fn_param_count;
    }
  }
  if (call_param_types) {
    size_t typed = call_param_count;
    for (size_t i = 0; i < call->argument_count && i < typed; i++) {
      Type *ptype = call_param_types[i];
      if (ir_should_decay_array_to_address(ptype, call->arguments[i])) {
        if (!ir_decay_array_operand_to_address(
                context, function, &arguments[i],
                call->arguments[i]->location)) {
          for (size_t j = 0; j < call->argument_count; j++) {
            ir_operand_destroy(&arguments[j]);
          }
          free(arguments);
          ir_operand_destroy(&destination);
          return 0;
        }
        continue;
      }
      if (ir_should_build_slice_from_array(ptype, call->arguments[i])) {
        if (!ir_build_slice_operand_from_array(
                context, function, &arguments[i],
                call->arguments[i]->resolved_type, ptype,
                call->arguments[i]->location)) {
          for (size_t j = 0; j < call->argument_count; j++) {
            ir_operand_destroy(&arguments[j]);
          }
          free(arguments);
          ir_operand_destroy(&destination);
          return 0;
        }
        continue;
      }
      if (ir_should_coerce_string_to_cstring(context, ptype,
                                             call->arguments[i])) {
        if (!ir_coerce_string_operand_to_cstring(
                context, function, &arguments[i], call->arguments[i]->location)) {
          for (size_t j = 0; j < call->argument_count; j++) {
            ir_operand_destroy(&arguments[j]);
          }
          free(arguments);
          ir_operand_destroy(&destination);
          return 0;
        }
        continue;
      }
      if (ptype && (ptype->kind == TYPE_FLOAT32 ||
                    ptype->kind == TYPE_FLOAT64 ||
                    ptype->kind == TYPE_FLOAT16 ||
                    ptype->kind == TYPE_BFLOAT16)) {
        ir_operand_apply_float_bits(&arguments[i], ir_type_float_bits(ptype));
      }
    }
  }

  if (call->callee_closure_env || is_func_ptr_var) {
    for (size_t i = 0; i < call->argument_count; i++) {
      Type *argument_type =
          call->arguments[i] ? call->arguments[i]->resolved_type : NULL;
      if (!ir_indirect_arg_passes_by_address(argument_type)) {
        continue;
      }
      if (!ir_pass_aggregate_argument_by_address(context, function,
                                                 &arguments[i], argument_type,
                                                 call->arguments[i]->location)) {
        for (size_t j = 0; j < call->argument_count; j++) {
          ir_operand_destroy(&arguments[j]);
        }
        free(arguments);
        ir_operand_destroy(&destination);
        return 0;
      }
    }
  }

  IROperand indirect_return_address = ir_operand_none();
  if ((call->callee_closure_env || is_func_ptr_var) &&
      ir_indirect_return_passes_by_pointer(expression->resolved_type) &&
      !ir_make_indirect_return_slot(context, function,
                                    expression->resolved_type,
                                    expression->location,
                                    &indirect_return_address)) {
    for (size_t i = 0; i < call->argument_count; i++)
      ir_operand_destroy(&arguments[i]);
    free(arguments);
    ir_operand_destroy(&destination);
    return 0;
  }

  if (call->callee_closure_env) {
    /* Closure call: the variable holds an 8-byte pointer to a heap record whose
     * field 0 is the code pointer. Load the code pointer, then call it passing
     * the record pointer (the environment) as a hidden leading argument that the
     * lifted function receives as its first parameter. */
    size_t lead = 1;
    size_t at = 0;
    IROperand code = ir_operand_none();
    if (indirect_return_address.kind != IR_OPERAND_NONE) {
      lead = 2;
    }
    if (!ir_make_temp_operand(context, &code)) {
      for (size_t i = 0; i < call->argument_count; i++)
        ir_operand_destroy(&arguments[i]);
      free(arguments);
      ir_operand_destroy(&indirect_return_address);
      ir_operand_destroy(&destination);
      return 0;
    }
    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = expression->location;
    load.dest = code;
    load.lhs =
        ir_operand_symbol(ir_local_ir_name(context, call->function_name));
    load.rhs = ir_operand_int(8);
    int load_ok = ir_emit(context, function, &load);
    ir_operand_destroy(&load.lhs);
    if (!load_ok) {
      for (size_t i = 0; i < call->argument_count; i++)
        ir_operand_destroy(&arguments[i]);
      free(arguments);
      ir_operand_destroy(&code);
      ir_operand_destroy(&indirect_return_address);
      ir_operand_destroy(&destination);
      return 0;
    }

    IROperand *cargs = calloc(call->argument_count + lead, sizeof(IROperand));
    if (!cargs) {
      for (size_t i = 0; i < call->argument_count; i++)
        ir_operand_destroy(&arguments[i]);
      free(arguments);
      ir_operand_destroy(&code);
      ir_operand_destroy(&indirect_return_address);
      ir_operand_destroy(&destination);
      return 0;
    }
    if (lead == 2) {
      cargs[at++] = indirect_return_address;
      indirect_return_address = ir_operand_none();
    }
    cargs[at++] =
        ir_operand_symbol(ir_local_ir_name(context, call->function_name));
    for (size_t i = 0; i < call->argument_count; i++)
      cargs[at + i] = arguments[i];
    free(arguments);

    IRInstruction cinstr = {0};
    cinstr.op = IR_OP_CALL_INDIRECT;
    cinstr.location = expression->location;
    cinstr.dest = destination;
    cinstr.lhs = code;
    cinstr.effect_signature = call->effect_signature;
    cinstr.value_type = expression->resolved_type
                            ? mtlc_type_from_frontend(expression->resolved_type)
                            : NULL;
    cinstr.arguments = cargs;
    cinstr.argument_count = call->argument_count + lead;
    cinstr.argument_types =
        ir_indirect_slot_types(call->arguments, call->argument_count, lead);
    int ok = ir_emit(context, function, &cinstr);
    free(cinstr.argument_types);
    for (size_t i = 0; i < call->argument_count + lead; i++)
      ir_operand_destroy(&cargs[i]);
    free(cargs);
    ir_operand_destroy(&code);
    if (!ok) {
      ir_operand_destroy(&destination);
      return 0;
    }
    *out_value = destination;
    return 1;
  }

  IROperand *emitted_arguments = arguments;
  size_t emitted_argument_count = call->argument_count;
  IROperand *prefixed_arguments = NULL;
  if (indirect_return_address.kind != IR_OPERAND_NONE) {
    prefixed_arguments = calloc(call->argument_count + 1, sizeof(IROperand));
    if (!prefixed_arguments) {
      for (size_t i = 0; i < call->argument_count; i++)
        ir_operand_destroy(&arguments[i]);
      free(arguments);
      ir_operand_destroy(&indirect_return_address);
      ir_operand_destroy(&destination);
      ir_set_error(context, "Out of memory while lowering an indirect call");
      return 0;
    }
    prefixed_arguments[0] = indirect_return_address;
    indirect_return_address = ir_operand_none();
    for (size_t i = 0; i < call->argument_count; i++)
      prefixed_arguments[i + 1] = arguments[i];
    emitted_arguments = prefixed_arguments;
    emitted_argument_count = call->argument_count + 1;
  }

  IRInstruction instruction = {0};
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.arguments = emitted_arguments;
  instruction.argument_count = emitted_argument_count;
  instruction.value_type = expression->resolved_type
                               ? mtlc_type_from_frontend(expression->resolved_type)
                               : NULL;

  if (is_func_ptr_var) {
    instruction.op = IR_OP_CALL_INDIRECT;
    instruction.effect_signature = call->effect_signature;
    instruction.argument_types = ir_indirect_slot_types(
        call->arguments, call->argument_count,
        emitted_argument_count - call->argument_count);
    instruction.lhs =
        ir_operand_symbol(ir_local_ir_name(context, call->function_name));
    if (instruction.lhs.kind != IR_OPERAND_SYMBOL || !instruction.lhs.name) {
      free(instruction.argument_types);
      ir_operand_destroy(&instruction.lhs);
      for (size_t i = 0; i < emitted_argument_count; i++) {
        ir_operand_destroy(&emitted_arguments[i]);
      }
      free(prefixed_arguments);
      free(arguments);
      ir_operand_destroy(&destination);
      ir_set_error(context,
                   "Out of memory while lowering function pointer call");
      return 0;
    }
  } else {
    instruction.op = IR_OP_CALL;
    instruction.text = call->function_name;
    instruction.intrinsic = ir_intrinsic_from_name(call->function_name);
    if (ir_intrinsic_is_atomic(instruction.intrinsic)) {
      instruction.address_space = MTLC_ADDRESS_SPACE_GLOBAL;
      instruction.memory_order = MTLC_MEMORY_ORDER_RELAXED;
      instruction.failure_memory_order = MTLC_MEMORY_ORDER_RELAXED;
      instruction.memory_scope = MTLC_MEMORY_SCOPE_DEVICE;
    }
  }

  if (!ir_emit(context, function, &instruction)) {
    free(instruction.argument_types);
    for (size_t i = 0; i < emitted_argument_count; i++) {
      ir_operand_destroy(&emitted_arguments[i]);
    }
    free(prefixed_arguments);
    free(arguments);
    ir_operand_destroy(&destination);
    return 0;
  }
  free(instruction.argument_types);
  instruction.argument_types = NULL;

  for (size_t i = 0; i < emitted_argument_count; i++) {
    ir_operand_destroy(&emitted_arguments[i]);
  }
  free(prefixed_arguments);
  free(arguments);

  *out_value = destination;
  return 1;
}

static int ir_lower_interpolation(IRLoweringContext *context,
                                  IRFunction *function,
                                  ASTNode *expression,
                                  IROperand *out_value) {
  CallExpression *call = (CallExpression *)expression->data;
  if (call->argument_count != 1 || !call->arguments || !call->arguments[0] ||
      !context->type_checker) {
    ir_set_error(context, "Malformed string interpolation");
    return 0;
  }
  ASTNode *value_node = call->arguments[0];
  Type *value_type = type_checker_infer_type(context->type_checker,
                                             value_node);
  if (!value_type) {
    ir_set_error(context, "Cannot determine interpolated value type");
    return 0;
  }

  IROperand operand = ir_operand_none();
  if (!ir_lower_expression(context, function, value_node, &operand)) {
    return 0;
  }
  if (value_type->kind == TYPE_STRING) {
    *out_value = operand;
    return 1;
  }

  const char *helper = NULL;
  const char *widen_to = NULL;
  int source_is_float = 0;
  int source_float_bits = 0;
  switch (value_type->kind) {
  case TYPE_BOOL:
    helper = "mettle_string_from_bool";
    widen_to = "int64";
    break;
  /* The whole reason `char` is its own type: "{c}" writes the character,
   * where the uint8 holding the same byte would write its number. */
  case TYPE_CHAR:
    helper = "mettle_string_from_char";
    widen_to = "int64";
    break;
  case TYPE_INT8:
  case TYPE_INT16:
  case TYPE_INT32:
    helper = "mettle_string_from_int";
    widen_to = "int64";
    break;
  case TYPE_INT64:
    helper = "mettle_string_from_int";
    break;
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
    helper = "mettle_string_from_uint";
    widen_to = "uint64";
    break;
  case TYPE_UINT64:
    helper = "mettle_string_from_uint";
    break;
  case TYPE_FLOAT32:
    helper = "mettle_string_from_f64";
    widen_to = "float64";
    source_is_float = 1;
    source_float_bits = 32;
    break;
  case TYPE_FLOAT64:
    helper = "mettle_string_from_f64";
    break;
  case TYPE_FLOAT16:
  case TYPE_BFLOAT16:
    helper = "mettle_string_from_f64";
    widen_to = "float64";
    source_is_float = 1;
    source_float_bits = 32;
    break;
  default:
    ir_operand_destroy(&operand);
    ir_set_error(context, "Cannot interpolate a value of type '%s'",
                 value_type->name ? value_type->name : "?");
    return 0;
  }

  if (widen_to) {
    IROperand widened = ir_operand_none();
    if (!ir_make_temp_operand(context, &widened)) {
      ir_operand_destroy(&operand);
      return 0;
    }
    IRInstruction cast = {0};
    cast.op = IR_OP_CAST;
    cast.location = expression->location;
    cast.dest = widened;
    cast.lhs = operand;
    cast.text = (char *)widen_to;
    cast.is_float = source_is_float;
    if (source_is_float) {
      cast.float_bits = source_float_bits;
      cast.dest.float_bits = 64;
      widened.float_bits = 64;
    }
    int cast_ok = ir_emit(context, function, &cast);
    ir_operand_destroy(&operand);
    if (!cast_ok) {
      ir_operand_destroy(&widened);
      return 0;
    }
    operand = widened;
  }
  /* mettle_string_from_f64 takes the value's raw bits in a GP register (the
   * symbol-less call has no parameter types for float routing), so a float64
   * operand passes through untagged. */

  ir_declare_interpolation_helper(context, helper);

  IROperand converted = ir_operand_none();
  if (!ir_make_temp_operand(context, &converted)) {
    ir_operand_destroy(&operand);
    return 0;
  }
  IROperand helper_args[1];
  helper_args[0] = operand;
  IRInstruction convert = {0};
  convert.op = IR_OP_CALL;
  convert.location = expression->location;
  convert.dest = converted;
  convert.text = (char *)helper;
  convert.arguments = helper_args;
  convert.argument_count = 1;
  convert.value_type =
      mtlc_type_from_frontend(context->type_checker->builtin_string);
  convert.allocates = 1;
  int convert_ok = ir_emit(context, function, &convert);
  ir_operand_destroy(&operand);
  if (!convert_ok) {
    ir_operand_destroy(&converted);
    return 0;
  }
  *out_value = converted;
  return 1;
}

int ir_emit_condition_false_branch(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *expression,
                                          const char *false_label) {
  if (!context || !function || !expression || !false_label) {
    return 0;
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    if (binary && binary->operator && binary->left && binary->right) {
      if (strcmp(binary->operator, "&&") == 0) {
        return ir_emit_condition_false_branch(context, function, binary->left,
                                              false_label) &&
               ir_emit_condition_false_branch(context, function, binary->right,
                                              false_label);
      }

      if (strcmp(binary->operator, "||") == 0) {
        char *done_label = ir_new_label_name(context, "cond_done");
        if (!done_label) {
          ir_set_error(context, "Out of memory while allocating condition labels");
          return 0;
        }

        if (!ir_emit_condition_true_branch(context, function, binary->left,
                                           done_label) ||
            !ir_emit_condition_false_branch(context, function, binary->right,
                                            false_label) ||
            !ir_emit_label_instruction(context, function, done_label,
                                       expression->location)) {
          free(done_label);
          return 0;
        }

        free(done_label);
        return 1;
      }
    }
  }

  IROperand condition = ir_operand_none();
  if (!ir_lower_expression(context, function, expression, &condition)) {
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = expression->location;
  branch.lhs = condition;
  branch.text = (char *)false_label;
  if (!ir_emit(context, function, &branch)) {
    ir_operand_destroy(&condition);
    return 0;
  }
  ir_operand_destroy(&condition);
  return 1;
}

int ir_emit_condition_true_branch(IRLoweringContext *context,
                                         IRFunction *function,
                                         ASTNode *expression,
                                         const char *true_label) {
  if (!context || !function || !expression || !true_label) {
    return 0;
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    if (binary && binary->operator && binary->left && binary->right) {
      if (strcmp(binary->operator, "||") == 0) {
        return ir_emit_condition_true_branch(context, function, binary->left,
                                             true_label) &&
               ir_emit_condition_true_branch(context, function, binary->right,
                                             true_label);
      }

      if (strcmp(binary->operator, "&&") == 0) {
        char *done_label = ir_new_label_name(context, "cond_done");
        if (!done_label) {
          ir_set_error(context, "Out of memory while allocating condition labels");
          return 0;
        }

        if (!ir_emit_condition_false_branch(context, function, binary->left,
                                            done_label) ||
            !ir_emit_condition_true_branch(context, function, binary->right,
                                           true_label) ||
            !ir_emit_label_instruction(context, function, done_label,
                                       expression->location)) {
          free(done_label);
          return 0;
        }

        free(done_label);
        return 1;
      }
    }
  }

  IROperand condition = ir_operand_none();
  if (!ir_lower_expression(context, function, expression, &condition)) {
    return 0;
  }

  char *skip_label = ir_new_label_name(context, "cond_false");
  if (!skip_label) {
    ir_operand_destroy(&condition);
    ir_set_error(context, "Out of memory while allocating condition labels");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = expression->location;
  branch.lhs = condition;
  branch.text = skip_label;
  if (!ir_emit(context, function, &branch) ||
      !ir_emit_jump_instruction(context, function, true_label,
                                expression->location) ||
      !ir_emit_label_instruction(context, function, skip_label,
                                 expression->location)) {
    ir_operand_destroy(&condition);
    free(skip_label);
    return 0;
  }

  ir_operand_destroy(&condition);
  free(skip_label);
  return 1;
}

static int ir_try_load_aggregate_by_value(IRLoweringContext *context,
                                          IRFunction *function,
                                          IROperand *address, Type *value_type,
                                          SourceLocation location,
                                          IROperand *out_value) {
  char *agg_name = NULL;
  IROperand dest_addr = ir_operand_none();
  IRInstruction store = {0};
  int ok = 0;

  if (!value_type || !value_type->name ||
      (value_type->kind != TYPE_STRUCT && value_type->kind != TYPE_ARRAY &&
       value_type->kind != TYPE_STRING && value_type->kind != TYPE_SLICE &&
       value_type->kind != TYPE_TAGGED_ENUM) ||
      value_type->size <= 8 || value_type->size > (size_t)INT_MAX) {
    return -1;
  }

  agg_name = ir_new_label_name(context, "agg_byval");
  if (!agg_name) {
    ir_operand_destroy(address);
    ir_set_error(context, "Out of memory while copying aggregate value");
    return 0;
  }
  ok = ir_emit_local_declaration(context, function, agg_name, value_type->name,
                                 location) &&
       ir_emit_address_of_symbol(context, function, agg_name, location,
                                 &dest_addr);
  if (!ok) {
    free(agg_name);
    ir_operand_destroy(&dest_addr);
    ir_operand_destroy(address);
    return 0;
  }

  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = dest_addr;
  store.lhs = *address;
  store.rhs = ir_operand_int((long long)value_type->size);
  ok = ir_emit(context, function, &store);
  ir_operand_destroy(&dest_addr);
  ir_operand_destroy(address);
  *address = ir_operand_none();
  if (!ok) {
    free(agg_name);
    return 0;
  }

  *out_value = ir_operand_symbol(agg_name);
  free(agg_name);
  if (!out_value->name) {
    ir_set_error(context, "Out of memory while copying aggregate value");
    return 0;
  }
  return 1;
}

  /* `==` / `!=` on strings compare contents. The generic binary path would
   * compare the 16-byte record as a scalar, which answered no for two views
   * of the same bytes and compiled without a word, so `if (input == "quit")`
   * was a branch that never ran. Contents also match `+`, which already
   * concatenates bytes rather than pointers. */
static int ir_lower_string_compare(IRLoweringContext *context,
                       IRFunction *function, ASTNode *expression,
                       BinaryExpression *binary,
                       IROperand *out_value, int *handled) {
  *handled = 1;
  if (strcmp(binary->operator, "==") == 0 ||
      strcmp(binary->operator, "!=") == 0) {
    Type *left_type =
        type_checker_infer_type(context->type_checker, binary->left);
    Type *right_type =
        type_checker_infer_type(context->type_checker, binary->right);
    if (left_type && right_type && left_type->kind == TYPE_STRING &&
        right_type->kind == TYPE_STRING) {
      IROperand left = ir_operand_none();
      IROperand right = ir_operand_none();
      IROperand equal = ir_operand_none();
      if (!ir_lower_expression(context, function, binary->left, &left)) {
        return 0;
      }
      if (!ir_lower_expression(context, function, binary->right, &right)) {
        ir_operand_destroy(&left);
        return 0;
      }
      if (!ir_make_temp_operand(context, &equal)) {
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        return 0;
      }

      IROperand call_args[2];
      call_args[0] = left;
      call_args[1] = right;
      IRInstruction call = {0};
      call.op = IR_OP_CALL;
      call.location = expression->location;
      call.dest = equal;
      call.text = "mettle_string_eq";
      call.arguments = call_args;
      call.argument_count = 2;
      int call_ok = ir_emit(context, function, &call);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      if (!call_ok) {
        ir_operand_destroy(&equal);
        return 0;
      }

      if (strcmp(binary->operator, "==") == 0) {
        *out_value = equal;
        return 1;
      }

      /* `!=` is the same answer inverted, and inverting it here keeps the
       * runtime surface to one function. */
      IROperand negated = ir_operand_none();
      if (!ir_make_temp_operand(context, &negated)) {
        ir_operand_destroy(&equal);
        return 0;
      }
      IRInstruction invert = {0};
      invert.op = IR_OP_BINARY;
      invert.location = expression->location;
      invert.dest = negated;
      invert.lhs = equal;
      invert.rhs = ir_operand_int(0);
      invert.text = "==";
      int invert_ok = ir_emit(context, function, &invert);
      ir_operand_destroy(&equal);
      if (!invert_ok) {
        ir_operand_destroy(&negated);
        return 0;
      }
      *out_value = negated;
      return 1;
    }
  }
  *handled = 0;
  return 1;
}

  // Keep string concatenation in AST form for codegen. The current IR binary
  // fallback models '+' as integer arithmetic, which is invalid for string
  // records.
static int ir_lower_string_concat(IRLoweringContext *context,
                       IRFunction *function, ASTNode *expression,
                       BinaryExpression *binary,
                       IROperand *out_value, int *handled) {
  *handled = 1;
  if (strcmp(binary->operator, "+") == 0) {
    Type *expr_type = ir_infer_expression_type(context, expression);
    if (expr_type && expr_type->kind == TYPE_STRING) {
      IROperand destination = ir_operand_none();
      IROperand left = ir_operand_none();
      IROperand right = ir_operand_none();
      if (!ir_make_temp_operand(context, &destination)) {
        return 0;
      }
      if (!ir_lower_expression(context, function, binary->left, &left)) {
        ir_operand_destroy(&destination);
        return 0;
      }
      if (!ir_lower_expression(context, function, binary->right, &right)) {
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      IRInstruction instruction = {0};
      instruction.op = IR_OP_BINARY;
      instruction.location = expression->location;
      instruction.dest = destination;
      instruction.lhs = left;
      instruction.rhs = right;
      instruction.text = binary->operator;
      instruction.ast_ref = expression;
      /* Bake the result type onto the IR so codegen reads it instead of
       * re-inferring from the AST (replaces code_generator_infer_expression_type;
       * mirrors its primary path). */
      instruction.value_type = mtlc_type_from_frontend(
          type_checker_infer_type(context->type_checker, expression));
      /* String '+' becomes a heap-allocating concat kernel in codegen; mark
       * it so the `@noalloc` contract checker can see the allocation. */
      instruction.allocates = 1;
      ir_declare_string_concat_helper(context);
      if (!ir_emit(context, function, &instruction)) {
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      *out_value = destination;
      return 1;
    }
  }
  *handled = 0;
  return 1;
}

static int ir_lower_short_circuit(IRLoweringContext *context,
                       IRFunction *function, ASTNode *expression,
                       BinaryExpression *binary,
                       IROperand *out_value, int *handled) {
  *handled = 1;
  if (strcmp(binary->operator, "&&") == 0 ||
      strcmp(binary->operator, "||") == 0) {
    int is_and = strcmp(binary->operator, "&&") == 0;
    IROperand destination = ir_operand_none();
    IROperand left = ir_operand_none();
    IROperand right = ir_operand_none();
    char *rhs_label = NULL;
    char *true_label = NULL;
    char *false_label = NULL;
    char *end_label = NULL;

    if (!ir_make_temp_operand(context, &destination)) {
      return 0;
    }
    if (!ir_lower_expression(context, function, binary->left, &left)) {
      ir_operand_destroy(&destination);
      return 0;
    }

    rhs_label = ir_new_label_name(context, "sc_rhs");
    true_label = ir_new_label_name(context, "sc_true");
    false_label = ir_new_label_name(context, "sc_false");
    end_label = ir_new_label_name(context, "sc_end");
    if (!rhs_label || !true_label || !false_label || !end_label) {
      ir_set_error(context,
                   "Out of memory while creating short-circuit labels");
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    IRInstruction instruction = {0};
    instruction.location = expression->location;

    instruction.op = IR_OP_BRANCH_ZERO;
    instruction.lhs = left;
    instruction.text = is_and ? false_label : rhs_label;
    if (!ir_emit(context, function, &instruction)) {
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    if (is_and) {
      instruction = (IRInstruction){0};
      instruction.op = IR_OP_LABEL;
      instruction.location = expression->location;
      instruction.text = rhs_label;
      if (!ir_emit(context, function, &instruction) ||
          !ir_lower_expression(context, function, binary->right, &right)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_BRANCH_ZERO;
      instruction.location = expression->location;
      instruction.lhs = right;
      instruction.text = false_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }
    } else {
      instruction = (IRInstruction){0};
      instruction.op = IR_OP_JUMP;
      instruction.location = expression->location;
      instruction.text = true_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_LABEL;
      instruction.location = expression->location;
      instruction.text = rhs_label;
      if (!ir_emit(context, function, &instruction) ||
          !ir_lower_expression(context, function, binary->right, &right)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_BRANCH_ZERO;
      instruction.location = expression->location;
      instruction.lhs = right;
      instruction.text = false_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }
    }

    instruction = (IRInstruction){0};
    instruction.op = IR_OP_LABEL;
    instruction.location = expression->location;
    instruction.text = true_label;
    if (!ir_emit(context, function, &instruction)) {
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    instruction = (IRInstruction){0};
    instruction.op = IR_OP_ASSIGN;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.lhs = ir_operand_int(1);
    if (!ir_emit(context, function, &instruction)) {
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    instruction = (IRInstruction){0};
    instruction.op = IR_OP_JUMP;
    instruction.location = expression->location;
    instruction.text = end_label;
    if (!ir_emit(context, function, &instruction)) {
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    instruction = (IRInstruction){0};
    instruction.op = IR_OP_LABEL;
    instruction.location = expression->location;
    instruction.text = false_label;
    if (!ir_emit(context, function, &instruction)) {
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    instruction = (IRInstruction){0};
    instruction.op = IR_OP_ASSIGN;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.lhs = ir_operand_int(0);
    if (!ir_emit(context, function, &instruction)) {
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    instruction = (IRInstruction){0};
    instruction.op = IR_OP_LABEL;
    instruction.location = expression->location;
    instruction.text = end_label;
    if (!ir_emit(context, function, &instruction)) {
      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      ir_operand_destroy(&destination);
      return 0;
    }

    free(rhs_label);
    free(true_label);
    free(false_label);
    free(end_label);
    ir_operand_destroy(&right);
    ir_operand_destroy(&left);
    *out_value = destination;
    return 1;
  }
  *handled = 0;
  return 1;
}

static int ir_lower_binary_expression(IRLoweringContext *context,
                       IRFunction *function, ASTNode *expression,
                       IROperand *out_value) {
  BinaryExpression *binary = (BinaryExpression *)expression->data;
  if (!binary || !binary->left || !binary->right || !binary->operator) {
    ir_set_error(context, "Malformed binary expression");
    return 0;
  }

  {
    int handled = 0;
    int lowered = ir_lower_string_compare(context, function, expression, binary,
                                          out_value, &handled);
    if (handled) {
      return lowered;
    }
    lowered = ir_lower_string_concat(context, function, expression, binary,
                                     out_value, &handled);
    if (handled) {
      return lowered;
    }
    lowered = ir_lower_short_circuit(context, function, expression, binary,
                                     out_value, &handled);
    if (handled) {
      return lowered;
    }
  }

  if (ir_try_lower_pointer_arithmetic(context, function, binary,
                                      expression->location, out_value)) {
    return 1;
  }

  IROperand left = ir_operand_none();
  IROperand right = ir_operand_none();
  if (!ir_lower_expression(context, function, binary->left, &left) ||
      !ir_lower_expression(context, function, binary->right, &right)) {
    ir_operand_destroy(&left);
    ir_operand_destroy(&right);
    return 0;
  }

  /* A shift on a narrow type runs in a 64-bit register, where the hardware
   * masks the count to 6 bits instead of the width the type has. int64
   * shifts read that way already and M0115 says so, so the count is brought
   * down to this type's own width: `x >> 32` on an int32 answered 0 while
   * the same shift on an int64 answered x. A constant count folds here and
   * leaves the shape every address recognizer reads. */
  {
    int shift_bits = ir_narrow_integer_shift_bits(
        ir_infer_expression_type(context, expression));
    if (shift_bits &&
        (strcmp(binary->operator, "<<") == 0 ||
         strcmp(binary->operator, ">>") == 0)) {
      long long mask = shift_bits - 1;
      if (right.kind == IR_OPERAND_INT) {
        right.int_value &= mask;
      } else {
        IROperand masked = ir_operand_none();
        IRInstruction bound = {0};
        if (!ir_make_temp_operand(context, &masked)) {
          ir_operand_destroy(&left);
          ir_operand_destroy(&right);
          return 0;
        }
        bound.op = IR_OP_BINARY;
        bound.location = expression->location;
        bound.dest = masked;
        bound.lhs = right;
        bound.rhs = ir_operand_int(mask);
        bound.text = (char *)"&";
        if (!ir_emit(context, function, &bound)) {
          ir_operand_destroy(&masked);
          ir_operand_destroy(&left);
          ir_operand_destroy(&right);
          return 0;
        }
        ir_operand_destroy(&right);
        right = masked;
      }
    }
  }

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    ir_operand_destroy(&left);
    ir_operand_destroy(&right);
    return 0;
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_BINARY;
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.lhs = left;
  instruction.rhs = right;
  instruction.text = binary->operator;
  /* Record that this operation is unsigned so the optimizer's constant
   * folder, which evaluates in signed long long, declines to fold a divide,
   * remainder, right shift, or ordering that signed arithmetic would get
   * wrong. Nothing else set this for a binary, so `var c: uint64 = 1e19; c /
   * 2` folded with a signed divide under -O and produced a negative result
   * while the unoptimized build divided correctly. Either operand being
   * unsigned makes the operation unsigned, matching the usual arithmetic
   * conversions the type checker already applied. */
  instruction.is_unsigned =
      ir_type_is_unsigned_integer(
          ir_infer_expression_type(context, binary->left)) ||
      ir_type_is_unsigned_integer(
          ir_infer_expression_type(context, binary->right));
  int operation_float_bits = ir_binary_expression_operation_float_bits(
      context, expression, binary);
  instruction.is_float = operation_float_bits != 0;
  if (instruction.is_float) {
    instruction.float_bits = operation_float_bits;
    if (!ir_binary_operator_is_comparison(binary->operator)) {
      instruction.dest.float_bits = instruction.float_bits;
      destination.float_bits = instruction.float_bits;
    }
  }

  if (!ir_emit(context, function, &instruction)) {
    ir_operand_destroy(&destination);
    ir_operand_destroy(&left);
    ir_operand_destroy(&right);
    return 0;
  }

  ir_operand_destroy(&left);
  ir_operand_destroy(&right);

  /* A temp is 64 bits wide whatever the expression's type is, and the
   * arithmetic that produced it ran at that width. `int32 + int32` overflows
   * at 32 bits by the language's own rule, so the value has to come back to
   * its declared width here: storing it into a narrow location truncated it,
   * and nothing else did, so `big + big > 0` answered yes for two values
   * whose int32 sum is negative. */
  {
    const char *narrow = ir_narrow_integer_result_type(
        instruction.is_float ? NULL : ir_infer_expression_type(context,
                                                               expression),
        binary->operator);
    if (narrow) {
      IROperand wrapped = ir_operand_none();
      if (!ir_make_temp_operand(context, &wrapped)) {
        ir_operand_destroy(&destination);
        return 0;
      }
      IRInstruction truncate = {0};
      truncate.op = IR_OP_CAST;
      truncate.location = expression->location;
      truncate.dest = wrapped;
      truncate.lhs = destination;
      truncate.text = (char *)narrow;
      if (!ir_emit(context, function, &truncate)) {
        ir_operand_destroy(&wrapped);
        ir_operand_destroy(&destination);
        return 0;
      }
      ir_operand_destroy(&destination);
      *out_value = wrapped;
      return 1;
    }
  }

  *out_value = destination;
  return 1;
}

static int ir_lower_unary_expression(IRLoweringContext *context,
                       IRFunction *function, ASTNode *expression,
                       IROperand *out_value) {
  UnaryExpression *unary = (UnaryExpression *)expression->data;
  if (!unary || !unary->operator || !unary->operand) {
    ir_set_error(context, "Malformed unary expression");
    return 0;
  }


  if (strcmp(unary->operator, "&") == 0) {
    Type *target_type = NULL;
    if (!ir_lower_lvalue_address(context, function, unary->operand, out_value,
                                 &target_type)) {
      return 0;
    }
    return 1;
  }

  if (strcmp(unary->operator, "*") == 0) {
    IROperand address = ir_operand_none();
    Type *target_type = NULL;
    if (!ir_lower_lvalue_address(context, function, expression, &address,
                                 &target_type)) {
      return 0;
    }
    if (!target_type) {
      ir_operand_destroy(&address);
      ir_set_error(context, "Cannot dereference unknown type");
      return 0;
    }

    {
      int handled = ir_try_load_aggregate_by_value(
          context, function, &address, target_type, expression->location,
          out_value);
      if (handled >= 0) {
        return handled;
      }
    }

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&address);
      return 0;
    }

    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = expression->location;
    load.dest = destination;
    load.lhs = address;
    load.rhs = ir_operand_int(ir_type_storage_size(target_type));
    ir_load_apply_float_type(&load, target_type);
    ir_load_apply_unsigned(&load, target_type);
    ir_access_apply_alias_class(&load, target_type);
    if (!ir_emit(context, function, &load)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&address);
      return 0;
    }
    destination.float_bits = load.dest.float_bits;

    ir_operand_destroy(&address);
    *out_value = destination;
    return 1;
  }

  IROperand operand = ir_operand_none();
  if (!ir_lower_expression(context, function, unary->operand, &operand)) {
    return 0;
  }

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    ir_operand_destroy(&operand);
    return 0;
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_UNARY;
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.lhs = operand;
  instruction.text = unary->operator;
  instruction.is_float = ir_expression_is_floating(context, expression);
  if (instruction.is_float) {
    instruction.float_bits = ir_expression_float_bits(context, expression);
    if (instruction.float_bits == 0) {
      instruction.float_bits = 64;
    }
    instruction.dest.float_bits = instruction.float_bits;
    destination.float_bits = instruction.float_bits;
  }

  if (!ir_emit(context, function, &instruction)) {
    ir_operand_destroy(&destination);
    ir_operand_destroy(&operand);
    return 0;
  }

  ir_operand_destroy(&operand);

  /* Same rule as a binary: the temp is 64 bits and the arithmetic ran at
   * that width, so a narrow result comes back to its declared width here.
   * `~x` on a uint8 set 56 bits the type does not have. A constant operand
   * is folded and range-checked instead, because `-90` is every negative
   * literal in the language and a cast around one hides the constant from
   * every recognizer that reads it. */
  {
    const char *narrow = ir_narrow_integer_result_type(
        instruction.is_float ? NULL
                             : ir_infer_expression_type(context, expression),
        unary->operator);
    if (narrow && instruction.lhs.kind == IR_OPERAND_INT &&
        ir_unary_constant_fits(narrow, unary->operator,
                               instruction.lhs.int_value)) {
      narrow = NULL;
    }
    if (narrow) {
      IROperand wrapped = ir_operand_none();
      if (!ir_make_temp_operand(context, &wrapped)) {
        ir_operand_destroy(&destination);
        return 0;
      }
      IRInstruction truncate = {0};
      truncate.op = IR_OP_CAST;
      truncate.location = expression->location;
      truncate.dest = wrapped;
      truncate.lhs = destination;
      truncate.text = (char *)narrow;
      if (!ir_emit(context, function, &truncate)) {
        ir_operand_destroy(&wrapped);
        ir_operand_destroy(&destination);
        return 0;
      }
      ir_operand_destroy(&destination);
      *out_value = wrapped;
      return 1;
    }
  }

  *out_value = destination;
  return 1;
}

/* Closure value: func_ptr is the environment pointer. Load the code
 * pointer from field 0 and pass the environment as a hidden leading
 * argument. */
static int ir_lower_closure_call(IRLoweringContext *context,
                                 IRFunction *function,
                                 ASTNode *expression,
                                 FuncPtrCall *fp_call,
                                 IROperand *arguments,
                                 IROperand func_ptr,
                                 IROperand destination,
                                 IROperand indirect_return_address,
                                 IROperand *out_value) {
  size_t lead = 1;
  size_t at = 0;
  IROperand code = ir_operand_none();
  if (indirect_return_address.kind != IR_OPERAND_NONE) {
    lead = 2;
  }
  if (!ir_make_temp_operand(context, &code)) {
    for (size_t i = 0; i < fp_call->argument_count; i++)
      ir_operand_destroy(&arguments[i]);
    free(arguments);
    ir_operand_destroy(&func_ptr);
    ir_operand_destroy(&destination);
    return 0;
  }
  IRInstruction load = {0};
  load.op = IR_OP_LOAD;
  load.location = expression->location;
  load.dest = code;
  load.lhs = func_ptr;
  load.rhs = ir_operand_int(8);
  int load_ok = ir_emit(context, function, &load);
  if (!load_ok) {
    for (size_t i = 0; i < fp_call->argument_count; i++)
      ir_operand_destroy(&arguments[i]);
    free(arguments);
    ir_operand_destroy(&func_ptr);
    ir_operand_destroy(&code);
    ir_operand_destroy(&indirect_return_address);
    ir_operand_destroy(&destination);
    return 0;
  }
  IROperand *cargs =
      calloc(fp_call->argument_count + lead, sizeof(IROperand));
  if (!cargs) {
    for (size_t i = 0; i < fp_call->argument_count; i++)
      ir_operand_destroy(&arguments[i]);
    free(arguments);
    ir_operand_destroy(&func_ptr);
    ir_operand_destroy(&code);
    ir_operand_destroy(&indirect_return_address);
    ir_operand_destroy(&destination);
    return 0;
  }
  if (lead == 2) {
    cargs[at++] = indirect_return_address;
    indirect_return_address = ir_operand_none();
  }
  cargs[at++] = func_ptr;
  for (size_t i = 0; i < fp_call->argument_count; i++)
    cargs[at + i] = arguments[i];
  free(arguments);
  IRInstruction cinstr = {0};
  cinstr.op = IR_OP_CALL_INDIRECT;
  cinstr.location = expression->location;
  cinstr.dest = destination;
  cinstr.lhs = code;
  cinstr.effect_signature = fp_call->effect_signature;
  cinstr.value_type =
      expression->resolved_type
          ? mtlc_type_from_frontend(expression->resolved_type)
          : NULL;
  cinstr.arguments = cargs;
  cinstr.argument_count = fp_call->argument_count + lead;
  cinstr.argument_types = ir_indirect_slot_types(
      fp_call->arguments, fp_call->argument_count, lead);
  int ok = ir_emit(context, function, &cinstr);
  free(cinstr.argument_types);
  for (size_t i = 0; i < fp_call->argument_count + lead; i++)
    ir_operand_destroy(&cargs[i]);
  free(cargs);
  ir_operand_destroy(&code);
  if (!ok) {
    ir_operand_destroy(&destination);
    return 0;
  }
  *out_value = destination;
  return 1;
}

static int ir_lower_func_ptr_call(IRLoweringContext *context,
                       IRFunction *function, ASTNode *expression,
                       IROperand *out_value) {
  FuncPtrCall *fp_call = (FuncPtrCall *)expression->data;
  if (!fp_call || !fp_call->function) {
    ir_set_error(context, "Invalid function pointer call");
    return 0;
  }

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    return 0;
  }

  IROperand func_ptr = ir_operand_none();
  if (!ir_lower_expression(context, function, fp_call->function, &func_ptr)) {
    ir_operand_destroy(&destination);
    return 0;
  }

  IROperand *arguments = NULL;
  if (fp_call->argument_count > 0) {
    arguments = calloc(fp_call->argument_count, sizeof(IROperand));
    if (!arguments) {
      ir_operand_destroy(&func_ptr);
      ir_operand_destroy(&destination);
      ir_set_error(
          context,
          "Out of memory while lowering function pointer call arguments");
      return 0;
    }
  }

  for (size_t i = 0; i < fp_call->argument_count; i++) {
    if (!ir_lower_expression(context, function, fp_call->arguments[i],
                             &arguments[i])) {
      for (size_t j = 0; j < i; j++) {
        ir_operand_destroy(&arguments[j]);
      }
      free(arguments);
      ir_operand_destroy(&func_ptr);
      ir_operand_destroy(&destination);
      return 0;
    }
  }

  Type *func_type = ir_infer_expression_type(context, fp_call->function);
  if (func_type && func_type->kind == TYPE_FUNCTION_POINTER &&
      func_type->fn_param_types) {
    for (size_t i = 0; i < fp_call->argument_count &&
                       i < func_type->fn_param_count;
         i++) {
      if (ir_should_decay_array_to_address(func_type->fn_param_types[i],
                                           fp_call->arguments[i]) &&
          !ir_decay_array_operand_to_address(
              context, function, &arguments[i],
              fp_call->arguments[i]->location)) {
        for (size_t j = 0; j < fp_call->argument_count; j++) {
          ir_operand_destroy(&arguments[j]);
        }
        free(arguments);
        ir_operand_destroy(&func_ptr);
        ir_operand_destroy(&destination);
        return 0;
      }
      if (ir_should_coerce_string_to_cstring(
              context, func_type->fn_param_types[i], fp_call->arguments[i]) &&
          !ir_coerce_string_operand_to_cstring(
              context, function, &arguments[i],
              fp_call->arguments[i]->location)) {
        for (size_t j = 0; j < fp_call->argument_count; j++) {
          ir_operand_destroy(&arguments[j]);
        }
        free(arguments);
        ir_operand_destroy(&func_ptr);
        ir_operand_destroy(&destination);
        return 0;
      }
    }
  }

  for (size_t i = 0; i < fp_call->argument_count; i++) {
    Type *argument_type =
        fp_call->arguments[i] ? fp_call->arguments[i]->resolved_type : NULL;
    if (!ir_indirect_arg_passes_by_address(argument_type)) {
      continue;
    }
    if (!ir_pass_aggregate_argument_by_address(
            context, function, &arguments[i], argument_type,
            fp_call->arguments[i]->location)) {
      for (size_t j = 0; j < fp_call->argument_count; j++) {
        ir_operand_destroy(&arguments[j]);
      }
      free(arguments);
      ir_operand_destroy(&func_ptr);
      ir_operand_destroy(&destination);
      return 0;
    }
  }

  IROperand indirect_return_address = ir_operand_none();
  if (ir_indirect_return_passes_by_pointer(expression->resolved_type) &&
      !ir_make_indirect_return_slot(context, function,
                                    expression->resolved_type,
                                    expression->location,
                                    &indirect_return_address)) {
    for (size_t i = 0; i < fp_call->argument_count; i++) {
      ir_operand_destroy(&arguments[i]);
    }
    free(arguments);
    ir_operand_destroy(&func_ptr);
    ir_operand_destroy(&destination);
    return 0;
  }

  if (func_type && func_type->kind == TYPE_FUNCTION_POINTER &&
      func_type->closure_env) {
    return ir_lower_closure_call(context, function, expression, fp_call,
                                 arguments, func_ptr, destination,
                                 indirect_return_address, out_value);
  }

  IROperand *emitted_arguments = arguments;
  size_t emitted_argument_count = fp_call->argument_count;
  IROperand *prefixed_arguments = NULL;
  if (indirect_return_address.kind != IR_OPERAND_NONE) {
    prefixed_arguments =
        calloc(fp_call->argument_count + 1, sizeof(IROperand));
    if (!prefixed_arguments) {
      for (size_t i = 0; i < fp_call->argument_count; i++) {
        ir_operand_destroy(&arguments[i]);
      }
      free(arguments);
      ir_operand_destroy(&func_ptr);
      ir_operand_destroy(&indirect_return_address);
      ir_operand_destroy(&destination);
      ir_set_error(context, "Out of memory while lowering an indirect call");
      return 0;
    }
    prefixed_arguments[0] = indirect_return_address;
    indirect_return_address = ir_operand_none();
    for (size_t i = 0; i < fp_call->argument_count; i++)
      prefixed_arguments[i + 1] = arguments[i];
    emitted_arguments = prefixed_arguments;
    emitted_argument_count = fp_call->argument_count + 1;
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_CALL_INDIRECT;
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.effect_signature = fp_call->effect_signature;
  // For indirect calls, we use lhs to hold the function pointer operand
  instruction.lhs = func_ptr;
  instruction.value_type =
      expression->resolved_type
          ? mtlc_type_from_frontend(expression->resolved_type)
          : NULL;
  instruction.arguments = emitted_arguments;
  instruction.argument_count = emitted_argument_count;
  instruction.argument_types = ir_indirect_slot_types(
      fp_call->arguments, fp_call->argument_count,
      emitted_argument_count - fp_call->argument_count);

  if (!ir_emit(context, function, &instruction)) {
    free(instruction.argument_types);
    for (size_t i = 0; i < emitted_argument_count; i++) {
      ir_operand_destroy(&emitted_arguments[i]);
    }
    free(prefixed_arguments);
    free(arguments);
    ir_operand_destroy(&func_ptr);
    ir_operand_destroy(&destination);
    return 0;
  }
  free(instruction.argument_types);
  instruction.argument_types = NULL;

  for (size_t i = 0; i < emitted_argument_count; i++) {
    ir_operand_destroy(&emitted_arguments[i]);
  }
  free(prefixed_arguments);
  free(arguments);
  ir_operand_destroy(&func_ptr);

  *out_value = destination;
  return 1;
}

static int ir_lower_new_view(IRLoweringContext *context, IRFunction *function,
                             ASTNode *expression, NewExpression *new_expression,
                             Type *view_type, IROperand *out_value) {
  size_t rank = type_view_rank(view_type);
  Type *element = view_type->base_type;
  IROperand extents[16];
  IROperand leads[16];
  IROperand total = ir_operand_none();
  IROperand bytes = ir_operand_none();
  IROperand pointer = ir_operand_none();
  IROperand view_address = ir_operand_none();
  IROperand element_size = ir_operand_none();
  IRInstruction allocate = {0};
  char *view_name = NULL;
  int ok = 0;

  for (size_t k = 0; k < 16; k++) {
    extents[k] = ir_operand_none();
    leads[k] = ir_operand_none();
  }
  if (rank > 16 || new_expression->extent_count + 1 != rank) {
    ir_set_error(context, "'new T[m, n]' extent count does not match its view");
    return 0;
  }
  if (!ir_lower_expression(context, function, new_expression->count,
                           &extents[0])) {
    return 0;
  }
  for (size_t k = 1; k < rank; k++) {
    if (!ir_lower_expression(context, function,
                             new_expression->extents[k - 1], &extents[k])) {
      goto done;
    }
  }
  for (size_t k = rank - 1; k > 0; k--) {
    if (k == rank - 1) {
      leads[k - 1] = ir_clone_operand_local(&extents[k]);
      if (leads[k - 1].kind != extents[k].kind) {
        goto done;
      }
    } else if (!ir_emit_binary_temp(context, function, "*", &leads[k],
                                    &extents[k], expression->location,
                                    &leads[k - 1])) {
      goto done;
    }
  }
  if (!ir_emit_binary_temp(context, function, "*", &extents[0], &leads[0],
                           expression->location, &total)) {
    goto done;
  }
  element_size = ir_operand_int((long long)element->size);
  if (!ir_emit_binary_temp(context, function, "*", &total, &element_size,
                           expression->location, &bytes) ||
      !ir_make_temp_operand(context, &pointer)) {
    goto done;
  }
  allocate.op = IR_OP_NEW;
  allocate.location = expression->location;
  allocate.dest = pointer;
  allocate.rhs = bytes;
  allocate.text = (char *)ir_backend_type_name(new_expression->type_name);
  if (!ir_emit(context, function, &allocate)) {
    goto done;
  }
  view_name = ir_new_label_name(context, "view");
  if (!view_name ||
      !ir_emit_local_declaration(context, function, view_name,
                                 view_type->name, expression->location) ||
      !ir_emit_address_of_symbol(context, function, view_name,
                                 expression->location, &view_address) ||
      !ir_emit_store_word(context, function, &view_address, 0, &pointer,
                          expression->location)) {
    goto done;
  }
  for (size_t k = 0; k < rank; k++) {
    if (!ir_emit_store_word(context, function, &view_address, 8 + 8 * k,
                            &extents[k], expression->location)) {
      goto done;
    }
  }
  for (size_t k = 0; k + 1 < rank; k++) {
    if (!ir_emit_store_word(context, function, &view_address,
                            8 + 8 * rank + 8 * k, &leads[k],
                            expression->location)) {
      goto done;
    }
  }
  *out_value = ir_operand_symbol(view_name);
  ok = out_value->name != NULL;

done:
  free(view_name);
  for (size_t k = 0; k < 16; k++) {
    ir_operand_destroy(&extents[k]);
    ir_operand_destroy(&leads[k]);
  }
  ir_operand_destroy(&total);
  ir_operand_destroy(&bytes);
  ir_operand_destroy(&pointer);
  ir_operand_destroy(&view_address);
  return ok;
}

static int ir_lower_new_expression(IRLoweringContext *context,
                                   IRFunction *function,
                                   ASTNode *expression,
                                   IROperand *out_value);
static int ir_lower_cast_expression(IRLoweringContext *context,
                                    IRFunction *function,
                                    ASTNode *expression,
                                    IROperand *out_value);

static int ir_lower_expression_inner(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     IROperand *out_value);

int ir_lower_expression(IRLoweringContext *context, IRFunction *function,
                        ASTNode *expression, IROperand *out_value) {
  if (!ir_lower_expression_inner(context, function, expression, out_value)) {
    return 0;
  }
  if (context && context->emit_refinement_checks && expression &&
      expression->proven_refinement) {
    if (expression->proven_predicate) {
      if (!ir_emit_refinement_predicate(context, function,
                                        expression->location, out_value,
                                        expression->proven_refinement,
                                        expression->proven_predicate,
                                        expression->proven_binding)) {
        return 0;
      }
    } else if (expression->proven_refinement->refine_has_range &&
               !ir_emit_refinement_check(context, function,
                                         expression->location, out_value,
                                         expression->proven_refinement)) {
      return 0;
    }
  }
  return 1;
}

static int ir_lower_expression_inner(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     IROperand *out_value) {
  if (!context || !function || !expression || !out_value) {
    return 0;
  }

  *out_value = ir_operand_none();

  /* Type and Field are comptime-only. If one reached lowering, the type
   * checker missed an escape; report it as a user diagnostic, never an ICE. */
  if (expression->resolved_type &&
      type_is_comptime_only(expression->resolved_type)) {
    if (context->type_checker) {
      type_checker_reject_comptime_escape(context->type_checker,
                                          expression->location,
                                          expression->resolved_type);
    }
    ir_set_error(context,
                 "value of type '%s' cannot escape into runtime code",
                 expression->resolved_type->name
                     ? expression->resolved_type->name
                     : "Type");
    return 0;
  }

  /* A Field member read (`f.offset`) is folded to a literal by const eval, so
   * one still shaped like a member access here means the fold was skipped and
   * there is no storage to load from. Report it, never load garbage. */
  if (expression->type == AST_MEMBER_ACCESS) {
    MemberAccess *member = (MemberAccess *)expression->data;
    if (member && member->object && member->object->resolved_type &&
        type_is_comptime_only(member->object->resolved_type)) {
      ir_set_error(context,
                   "compile-time field member '%s' was not folded",
                   member->member ? member->member : "<unknown>");
      return 0;
    }
  }

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      ir_set_error(context, "Malformed number literal");
      return 0;
    }
    if (literal->is_float) {
      *out_value = ir_operand_float(literal->float_value);
    } else {
      *out_value = ir_operand_int(literal->int_value);
    }
    return 1;
  }

  case AST_STRING_LITERAL: {
    StringLiteral *literal = (StringLiteral *)expression->data;
    if (!literal || !literal->value) {
      ir_set_error(context, "Malformed string literal");
      return 0;
    }
    *out_value = ir_operand_string_n(literal->value, literal->length);
    if (!out_value->name) {
      ir_set_error(context, "Out of memory while lowering string literal");
      return 0;
    }
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      ir_set_error(context, "Malformed identifier expression");
      return 0;
    }
    if (context->refine_binding_active && context->refine_binding_name &&
        strcmp(identifier->name, context->refine_binding_name) == 0) {
      *out_value = ir_operand_copy(&context->refine_binding_value);
      return 1;
    }
    Symbol *symbol =
        context->symbol_table
            ? symbol_table_lookup(context->symbol_table, identifier->name)
            : NULL;
    if (symbol && symbol->kind == SYMBOL_CONSTANT) {
      *out_value = ir_operand_int(symbol->data.constant.value);
      return 1;
    }

    /* A bare nullary tagged-enum variant (e.g. `var a: Option = None`) names
     * a constructor symbol, not a runtime value. Construct an enum local with
     * just the tag set; payloadful variants must use call syntax `Some(x)`. */
    if (symbol && symbol->kind == SYMBOL_TAGGED_ENUM_CONSTRUCTOR &&
        symbol->data.constructor.payload_type == NULL) {
      return ir_emit_tagged_enum_construct(context, function, symbol,
                                           NULL, expression->location,
                                           out_value);
    }

    /* A function's name in value position is its address, which is what the
     * type checker read it as and what `&name` spells out. Falling through to
     * the symbol read below named a local that was never declared, so
     * `apply(twice, 7)` compiled and then jumped through whatever the slot
     * happened to hold.
     *
     * The type the CHECKER settled on decides this, not the symbol table: by
     * lowering time the local scopes are popped, so a local named after a
     * function finds the function here. `var value: int32 = 3;` beside a
     * `fn value()` would otherwise read the function's address. */
    if (symbol && symbol->kind == SYMBOL_FUNCTION &&
        expression->resolved_type &&
        expression->resolved_type->kind == TYPE_FUNCTION_POINTER) {
      return ir_emit_address_of_symbol(
          context, function, ir_local_ir_name(context, identifier->name),
          expression->location, out_value);
    }

    if (ir_small_float_local(context, function, identifier->name,
                             ir_local_ir_name(context, identifier->name),
                             expression->resolved_type)) {
      return ir_emit_small_float_home_load(
          context, function, ir_local_ir_name(context, identifier->name),
          expression->resolved_type, expression->location, out_value);
    }
    *out_value =
        ir_operand_symbol(ir_local_ir_name(context, identifier->name));
    if (!out_value->name) {
      ir_set_error(context, "Out of memory while lowering identifier");
      return 0;
    }
    return 1;
  }

  case AST_BINARY_EXPRESSION:
    return ir_lower_binary_expression(context, function, expression,
                                      out_value);

  case AST_CLOSURE_ADAPT_EXPRESSION: {
    /* A thin value (`&func` or a non-capturing lambda) wrapped by the
     * closure-adapt pass to satisfy an `Fn(...)` boundary: lower the thin value,
     * then call the generated adapter constructor with it as the sole argument
     * to produce a real closure value. */
    ClosureAdapt *adapt = (ClosureAdapt *)expression->data;
    if (!adapt || !adapt->ctor_name || !adapt->inner) {
      ir_set_error(context, "Internal: closure adapter was not synthesized");
      return 0;
    }
    IROperand thin_val = ir_operand_none();
    if (!ir_lower_expression(context, function, adapt->inner, &thin_val)) {
      return 0;
    }
    IROperand dest = ir_operand_none();
    if (!ir_make_temp_operand(context, &dest)) {
      ir_operand_destroy(&thin_val);
      return 0;
    }
    IROperand args[1];
    args[0] = thin_val;
    IRInstruction call = {0};
    call.op = IR_OP_CALL;
    call.location = expression->location;
    call.dest = dest;
    call.text = adapt->ctor_name;
    call.arguments = args;
    call.argument_count = 1;
    int ok = ir_emit(context, function, &call);
    ir_operand_destroy(&thin_val);
    if (!ok) {
      ir_operand_destroy(&dest);
      return 0;
    }
    *out_value = dest;
    return 1;
  }

  case AST_LAMBDA_EXPRESSION: {
    FunctionDeclaration *lam = (FunctionDeclaration *)expression->data;
    if (!lam || !lam->name) {
      ir_set_error(context, "Internal: lambda was not converted");
      return 0;
    }
    if (lam->captured_count > 0) {
      /* Capturing closure value: call the synthesized constructor with the
       * current value of each captured variable; it allocates and populates the
       * environment record and returns the 8-byte closure pointer. */
      IROperand dest = ir_operand_none();
      if (!ir_make_temp_operand(context, &dest)) {
        return 0;
      }
      IROperand *args = calloc(lam->captured_count, sizeof(IROperand));
      if (!args) {
        ir_operand_destroy(&dest);
        return 0;
      }
      for (size_t i = 0; i < lam->captured_count; i++)
        args[i] = ir_operand_symbol(
            ir_local_ir_name(context, lam->captured_names[i]));
      IRInstruction call = {0};
      call.op = IR_OP_CALL;
      call.location = expression->location;
      call.dest = dest;
      call.text = lam->name;
      call.arguments = args;
      call.argument_count = lam->captured_count;
      int ok = ir_emit(context, function, &call);
      for (size_t i = 0; i < lam->captured_count; i++)
        ir_operand_destroy(&args[i]);
      free(args);
      if (!ok) {
        ir_operand_destroy(&dest);
        return 0;
      }
      *out_value = dest;
      return 1;
    }
    /* A non-capturing lambda is the address of its lifted top-level function. */
    return ir_emit_address_of_symbol(context, function, lam->name,
                                     expression->location, out_value);
  }

  case AST_UNARY_EXPRESSION:
    return ir_lower_unary_expression(context, function, expression,
                                     out_value);

  case AST_MEMBER_ACCESS: {
    MemberAccess *m = (MemberAccess *)expression->data;
    /* Qualified enum variant: `EnumName.Variant` lowers to either an integer
     * constant (plain enum) or a tagged-enum construction (tagged enum). */
    if (m && m->object && m->object->type == AST_IDENTIFIER && m->member) {
      Identifier *obj_id = (Identifier *)m->object->data;
      if (obj_id && obj_id->name && context->symbol_table) {
        Symbol *enum_sym =
            symbol_table_lookup(context->symbol_table, obj_id->name);
        if (enum_sym && enum_sym->kind == SYMBOL_ENUM) {
          Symbol *variant_sym =
              symbol_table_lookup(context->symbol_table, m->member);
          if (variant_sym && variant_sym->kind == SYMBOL_CONSTANT) {
            *out_value = ir_operand_int(variant_sym->data.constant.value);
            return 1;
          }
          if (variant_sym &&
              variant_sym->kind == SYMBOL_TAGGED_ENUM_CONSTRUCTOR &&
              variant_sym->data.constructor.payload_type == NULL) {
            return ir_emit_tagged_enum_construct(context, function, variant_sym,
                                                 NULL, expression->location,
                                                 out_value);
          }
        }
      }
    }
    /* Fall through to the lvalue-load path for struct/array member access. */
  }
  /* fallthrough */
  case AST_INDEX_EXPRESSION: {
    IROperand address = ir_operand_none();
    Type *value_type = NULL;
    if (!ir_lower_lvalue_address(context, function, expression, &address,
                                 &value_type)) {
      return 0;
    }
    if (!value_type) {
      ir_operand_destroy(&address);
      ir_set_error(context, "Cannot determine type for load");
      return 0;
    }

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&address);
      return 0;
    }

    {
      int handled = ir_try_load_aggregate_by_value(
          context, function, &address, value_type, expression->location,
          out_value);
      if (handled >= 0) {
        ir_operand_destroy(&destination);
        return handled;
      }
    }

    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = expression->location;
    load.dest = destination;
    load.lhs = address;
    load.rhs = ir_operand_int(ir_type_storage_size(value_type));
    ir_load_apply_float_type(&load, value_type);
    ir_load_apply_unsigned(&load, value_type);
    ir_access_apply_alias_class(&load, value_type);
    if (!ir_emit(context, function, &load)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&address);
      return 0;
    }
    destination.float_bits = load.dest.float_bits;

    ir_operand_destroy(&address);
    *out_value = destination;
    return 1;
  }

  case AST_NEW_EXPRESSION:
    return ir_lower_new_expression(context, function, expression, out_value);

  case AST_CAST_EXPRESSION:
    return ir_lower_cast_expression(context, function, expression, out_value);

  case AST_FUNCTION_CALL:
    return ir_lower_call_expression(context, function, expression, out_value);

  case AST_FUNC_PTR_CALL:
    return ir_lower_func_ptr_call(context, function, expression,
                                  out_value);

  case AST_MATCH_STATEMENT:
    return ir_lower_match_expression(context, function, expression,
                                     out_value);

  default:
    ir_set_error(context, "Unsupported expression type in pure IR lowering");
    return 0;
  }
}

static int ir_lower_new_expression(IRLoweringContext *context,
                                   IRFunction *function,
                                   ASTNode *expression,
                                   IROperand *out_value) {
  NewExpression *new_expression = (NewExpression *)expression->data;
  if (!new_expression || !new_expression->type_name) {
    ir_set_error(context, "Invalid new expression");
    return 0;
  }

  /* `new T[n]`: n elements' worth of zeroed heap, and the count stored
     beside the pointer, so what comes back is a slice and every read
     through it can be checked against a length that is really there. */
  if (new_expression->count) {
    Type *slice_type = expression->resolved_type;
    Type *element = slice_type ? slice_type->base_type : NULL;
    IROperand count = ir_operand_none();
    IROperand bytes = ir_operand_none();
    IROperand pointer = ir_operand_none();
    IROperand slice_address = ir_operand_none();
    IROperand slot = ir_operand_none();
    char *slice_name = NULL;
    IRInstruction multiply = {0};
    IRInstruction allocate = {0};
    IRInstruction store = {0};

    if (!slice_type || slice_type->kind != TYPE_SLICE || !element ||
        element->size == 0) {
      ir_set_error(context, "'new T[n]' reached lowering without a slice "
                            "type");
      return 0;
    }
    if (type_view_rank(slice_type) > 1) {
      return ir_lower_new_view(context, function, expression, new_expression,
                               slice_type, out_value);
    }
    if (!ir_lower_expression(context, function, new_expression->count,
                             &count) ||
        !ir_make_temp_operand(context, &bytes) ||
        !ir_make_temp_operand(context, &pointer)) {
      ir_operand_destroy(&count);
      ir_operand_destroy(&bytes);
      ir_operand_destroy(&pointer);
      return 0;
    }

    multiply.op = IR_OP_BINARY;
    multiply.location = expression->location;
    multiply.dest = bytes;
    multiply.lhs = count;
    multiply.rhs = ir_operand_int((long long)element->size);
    multiply.text = "*";
    if (!ir_emit(context, function, &multiply)) {
      ir_operand_destroy(&count);
      ir_operand_destroy(&bytes);
      ir_operand_destroy(&pointer);
      return 0;
    }

    allocate.op = IR_OP_NEW;
    allocate.location = expression->location;
    allocate.dest = pointer;
    allocate.rhs = bytes;
    allocate.text = (char *)ir_backend_type_name(new_expression->type_name);
    if (!ir_emit(context, function, &allocate)) {
      ir_operand_destroy(&count);
      ir_operand_destroy(&bytes);
      ir_operand_destroy(&pointer);
      return 0;
    }

    slice_name = ir_new_label_name(context, "slice");
    if (!slice_name ||
        !ir_emit_local_declaration(context, function, slice_name,
                                   slice_type->name, expression->location) ||
        !ir_emit_address_of_symbol(context, function, slice_name,
                                   expression->location, &slice_address)) {
      free(slice_name);
      ir_operand_destroy(&count);
      ir_operand_destroy(&bytes);
      ir_operand_destroy(&pointer);
      ir_operand_destroy(&slice_address);
      return 0;
    }

    store.op = IR_OP_STORE;
    store.location = expression->location;
    store.dest = ir_clone_operand_local(&slice_address);
    store.lhs = pointer;
    store.rhs = ir_operand_int(8);
    if (!ir_emit(context, function, &store)) {
      ir_operand_destroy(&store.dest);
      free(slice_name);
      ir_operand_destroy(&count);
      ir_operand_destroy(&bytes);
      ir_operand_destroy(&pointer);
      ir_operand_destroy(&slice_address);
      return 0;
    }
    ir_operand_destroy(&store.dest);

    if (!ir_emit_address_with_offset(context, function, &slice_address, 8,
                                     expression->location, &slot)) {
      free(slice_name);
      ir_operand_destroy(&count);
      ir_operand_destroy(&bytes);
      ir_operand_destroy(&pointer);
      ir_operand_destroy(&slice_address);
      return 0;
    }
    {
      IRInstruction length = {0};
      length.op = IR_OP_STORE;
      length.location = expression->location;
      length.dest = slot;
      length.lhs = count;
      length.rhs = ir_operand_int(8);
      if (!ir_emit(context, function, &length)) {
        ir_operand_destroy(&slot);
        free(slice_name);
        ir_operand_destroy(&count);
        ir_operand_destroy(&bytes);
        ir_operand_destroy(&pointer);
        ir_operand_destroy(&slice_address);
        return 0;
      }
    }
    ir_operand_destroy(&slot);
    ir_operand_destroy(&slice_address);
    ir_operand_destroy(&count);
    ir_operand_destroy(&bytes);
    ir_operand_destroy(&pointer);

    *out_value = ir_operand_symbol(slice_name);
    free(slice_name);
    return out_value->name != NULL;
  }

  Type *allocated_type = NULL;
  if (context->type_checker) {
    /*
     * Prefer the already-resolved expression type: `new T` infers to `T*`,
     * and using that avoids scope-sensitive type-name lookups here.
     */
    Type *new_expr_type =
        type_checker_infer_type(context->type_checker, expression);
    if (new_expr_type && new_expr_type->kind == TYPE_POINTER) {
      allocated_type = new_expr_type->base_type;
    }
    if (!allocated_type) {
      allocated_type = type_checker_get_type_by_name(context->type_checker,
                                                     new_expression->type_name);
    }
  }
  /*
   * Allocation must use the full concrete type size.
   * ir_type_storage_size() intentionally normalizes many operations to
   * register-width storage, which is incorrect for `new` on structs/arrays.
   */
  int allocation_size =
      (allocated_type && allocated_type->size > 0 &&
       allocated_type->size <= (size_t)INT_MAX)
          ? (int)allocated_type->size
          : 8;

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    return 0;
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_NEW;
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.rhs = ir_operand_int(allocation_size);
  instruction.text = (char *)ir_backend_type_name(new_expression->type_name);
  if (!ir_emit(context, function, &instruction)) {
    ir_operand_destroy(&destination);
    return 0;
  }

  *out_value = destination;
  return 1;
}

static int ir_lower_cast_expression(IRLoweringContext *context,
                                    IRFunction *function,
                                    ASTNode *expression,
                                    IROperand *out_value) {
  CastExpression *cast_expr = (CastExpression *)expression->data;
  if (!cast_expr || !cast_expr->type_name || !cast_expr->operand) {
    ir_set_error(context, "Invalid cast expression");
    return 0;
  }

  Type *cast_target = ir_resolve_named_type(context, cast_expr->type_name);
  int target_is_pointer =
      cast_target && (cast_target->kind == TYPE_POINTER ||
                      cast_target->kind == TYPE_FUNCTION_POINTER);
  ASTNode *cast_operand = cast_expr->operand;

  /* `(T*)((int64)p)` where p is already a pointer: the integer carries the
   * same address with its provenance dropped. Lower the pointer instead, so
   * the alias analysis, the borrow checker and --verify keep following the
   * value the source laundered. The type checker reports M0120 on the same
   * shape, so the spelling gets cleaned up as well. */
  if (target_is_pointer && cast_operand->type == AST_CAST_EXPRESSION &&
      cast_operand->data) {
    CastExpression *inner = (CastExpression *)cast_operand->data;
    Type *mid = inner->type_name
                    ? ir_resolve_named_type(context, inner->type_name)
                    : NULL;
    Type *source = inner->operand ? inner->operand->resolved_type : NULL;
    if (mid && source && inner->operand &&
        type_checker_is_integer_type(mid) &&
        (source->kind == TYPE_POINTER ||
         source->kind == TYPE_FUNCTION_POINTER)) {
      cast_operand = inner->operand;
    }
  }

  IROperand operand = ir_operand_none();
  if (!ir_lower_expression(context, function, cast_operand, &operand)) {
    return 0;
  }

  /* Casting a `string` to a pointer or an integer means its characters, the
   * same conversion a `cstring` binding gets implicitly. The record itself
   * is not the address. */
  if (cast_target && ir_expression_is_string(context, cast_operand) &&
      (target_is_pointer || type_checker_is_integer_type(cast_target)) &&
      !ir_coerce_string_operand_to_cstring(context, function, &operand,
                                           expression->location)) {
    ir_operand_destroy(&operand);
    return 0;
  }

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    ir_operand_destroy(&operand);
    return 0;
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_CAST;
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.lhs = operand;
  instruction.text = (char *)ir_backend_type_name(cast_expr->type_name);
  instruction.is_float = ir_expression_is_floating(context, cast_operand);
  /* is_unsigned on a CAST records that the SOURCE is an unsigned integer,
   * the same way float_bits records the source's float width. x86-64 and
   * AArch64 both convert a 64-bit integer to floating point as SIGNED
   * unless told otherwise, so without this `(float64)(uint64)~0` answered
   * -1.0. Only the backends that convert read it; the narrowing paths take
   * their signedness from the target type in instruction->text. */
  instruction.is_unsigned =
      !instruction.is_float &&
      ir_type_is_unsigned_integer(
          ir_infer_expression_type(context, cast_operand));
  if (instruction.is_float) {
    /* float_bits on a CAST records the SOURCE operand width so the backend
     * can pick cvttss2si/cvtss2sd (f32) vs cvttsd2si (f64). The TARGET
     * width is resolved separately from instruction->text. */
    instruction.float_bits = ir_expression_float_bits(context, cast_operand);
    if (instruction.float_bits == 0) {
      instruction.float_bits = 64;
    }
  }
  {
    /* Tag the destination with the target float width so a value produced
     * by e.g. (float32)x is recognized as float32 by later consumers. */
    int target_bits =
        ir_named_type_float_bits(context, cast_expr->type_name);
    if (target_bits) {
      instruction.dest.float_bits = target_bits;
      destination.float_bits = target_bits;
    }
  }

  if (!ir_emit(context, function, &instruction)) {
    ir_operand_destroy(&destination);
    ir_operand_destroy(&operand);
    return 0;
  }

  ir_operand_destroy(&operand);
  *out_value = destination;
  return 1;
}
