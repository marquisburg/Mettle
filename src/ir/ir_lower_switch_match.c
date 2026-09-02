// AST->IR lowering: switch, match, and tagged-enum construction.
#include "ir_lowering_internal.h"

// Emit the dispatch test for a range case `lo..hi`: if the switch value lies
// in [lo, hi], jump to case_label; otherwise fall through to the next test.
// Lowered with the existing comparison/branch primitives:
//   ge = (sv >= lo); if (!ge) goto skip;
//   le = (sv <= hi); if (!le) goto skip;
//   goto case_label;
//   skip:
int ir_emit_switch_range_dispatch(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *switch_value,
                                         ASTNode *lo_node, ASTNode *hi_node,
                                         const char *case_label,
                                         SourceLocation loc) {
  IROperand lo = ir_operand_none();
  IROperand hi = ir_operand_none();
  IROperand ge = ir_operand_none();
  IROperand le = ir_operand_none();
  char *skip_label = ir_new_label_name(context, "switch_range_skip");
  int ok = 0;
  if (!skip_label) {
    return 0;
  }

  if (!ir_lower_expression(context, function, lo_node, &lo) ||
      !ir_make_temp_operand(context, &ge)) {
    goto done;
  }
  IRInstruction cmp_lo = {0};
  cmp_lo.op = IR_OP_BINARY;
  cmp_lo.location = loc;
  cmp_lo.dest = ge;
  cmp_lo.lhs = *switch_value;
  cmp_lo.rhs = lo;
  cmp_lo.text = ">=";
  IRInstruction br_lo = {0};
  br_lo.op = IR_OP_BRANCH_ZERO;
  br_lo.location = loc;
  br_lo.lhs = ge;
  br_lo.text = skip_label;
  if (!ir_emit(context, function, &cmp_lo) ||
      !ir_emit(context, function, &br_lo)) {
    goto done;
  }

  if (!ir_lower_expression(context, function, hi_node, &hi) ||
      !ir_make_temp_operand(context, &le)) {
    goto done;
  }
  IRInstruction cmp_hi = {0};
  cmp_hi.op = IR_OP_BINARY;
  cmp_hi.location = loc;
  cmp_hi.dest = le;
  cmp_hi.lhs = *switch_value;
  cmp_hi.rhs = hi;
  cmp_hi.text = "<=";
  IRInstruction br_hi = {0};
  br_hi.op = IR_OP_BRANCH_ZERO;
  br_hi.location = loc;
  br_hi.lhs = le;
  br_hi.text = skip_label;
  if (!ir_emit(context, function, &cmp_hi) ||
      !ir_emit(context, function, &br_hi)) {
    goto done;
  }

  if (!ir_emit_jump_instruction(context, function, case_label, loc)) {
    goto done;
  }
  IRInstruction skip = {0};
  skip.op = IR_OP_LABEL;
  skip.location = loc;
  skip.text = skip_label;
  if (!ir_emit(context, function, &skip)) {
    goto done;
  }
  ok = 1;

done:
  ir_operand_destroy(&lo);
  ir_operand_destroy(&hi);
  ir_operand_destroy(&ge);
  ir_operand_destroy(&le);
  free(skip_label);
  return ok;
}

/* The statements of a block, or nothing when the node is not one. A case body
 * is a block; an empty one is how several case values share a body. */
static const ASTNode *ir_block_last_statement(const ASTNode *body,
                                              size_t *out_count) {
  const Program *block = NULL;
  if (out_count) {
    *out_count = 0;
  }
  if (!body || body->type != AST_PROGRAM) {
    return body;
  }
  block = (const Program *)body->data;
  if (!block || block->declaration_count == 0) {
    return NULL;
  }
  if (out_count) {
    *out_count = block->declaration_count;
  }
  return block->declarations[block->declaration_count - 1];
}

/* Does this case body already leave the switch by itself? The jump to the end
 * is emitted after every other one, and emitting it after a `return` would
 * leave a branch nothing can reach. */
static int ir_case_body_transfers_control(const ASTNode *body) {
  const ASTNode *last = body;
  size_t count = 0;
  /* A braced body is one statement holding the real ones, so the last
     statement of the case is the last statement of the innermost block. */
  while (last && last->type == AST_PROGRAM) {
    last = ir_block_last_statement(last, &count);
  }
  if (!last) {
    return 0;
  }
  return last->type == AST_RETURN_STATEMENT ||
         last->type == AST_BREAK_STATEMENT ||
         last->type == AST_CONTINUE_STATEMENT ||
         last->type == AST_FALLTHROUGH_STATEMENT;
}

static char **ir_alloc_switch_case_labels(IRLoweringContext *context,
                                          size_t case_count) {
  char **case_labels = calloc(case_count, sizeof(char *));
  size_t i = 0u;

  if (!case_labels) {
    return NULL;
  }
  for (i = 0u; i < case_count; i++) {
    case_labels[i] = ir_new_label_name(context, "case");
    if (!case_labels[i]) {
      size_t j = 0u;
      for (j = 0u; j < i; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      return NULL;
    }
  }
  return case_labels;
}

static char *ir_find_switch_default_label(SwitchStatement *switch_data,
                                          char **case_labels,
                                          char *end_label) {
  size_t i = 0u;

  for (i = 0u; i < switch_data->case_count; i++) {
    ASTNode *case_node = switch_data->cases[i];
    CaseClause *clause = case_node ? (CaseClause *)case_node->data : NULL;
    if (clause && clause->is_default) {
      return case_labels ? case_labels[i] : end_label;
    }
  }
  return end_label;
}

int ir_lower_switch_statement(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *statement,
                                     IRDeferScope *defers) {
  if (!context || !function || !statement ||
      statement->type != AST_SWITCH_STATEMENT) {
    return 0;
  }

  SwitchStatement *switch_data = (SwitchStatement *)statement->data;
  if (!switch_data || !switch_data->expression) {
    ir_set_error(context, "Malformed switch statement");
    return 0;
  }

  char *end_label = ir_new_label_name(context, "switch_end");
  if (!end_label) {
    ir_set_error(context, "Out of memory while allocating switch labels");
    return 0;
  }

  IROperand switch_value = ir_operand_none();
  if (!ir_lower_expression(context, function, switch_data->expression,
                           &switch_value)) {
    free(end_label);
    return 0;
  }

  char **case_labels = NULL;
  if (switch_data->case_count > 0) {
    case_labels = ir_alloc_switch_case_labels(context,
                                             switch_data->case_count);
    if (!case_labels) {
      ir_operand_destroy(&switch_value);
      free(end_label);
      ir_set_error(context,
                   "Out of memory while allocating switch case labels");
      return 0;
    }
  }

  char *default_label =
      ir_find_switch_default_label(switch_data, case_labels, end_label);

  // Dispatch chain: if (switch_value == case_value) jump case label.
  for (size_t i = 0; i < switch_data->case_count; i++) {
    ASTNode *case_node = switch_data->cases[i];
    CaseClause *clause = case_node ? (CaseClause *)case_node->data : NULL;
    if (!case_node || !clause) {
      continue;
    }
    if (clause->is_default) {
      continue;
    }
    if (!clause->value) {
      continue;
    }

    // Range case `lo..hi`: emit a two-sided bounds test instead of equality.
    if (clause->value_high) {
      if (!ir_emit_switch_range_dispatch(context, function, &switch_value,
                                         clause->value, clause->value_high,
                                         case_labels[i], statement->location)) {
        for (size_t j = 0; j < switch_data->case_count; j++) {
          free(case_labels[j]);
        }
        free(case_labels);
        ir_operand_destroy(&switch_value);
        free(end_label);
        return 0;
      }
      continue;
    }

    IROperand case_value = ir_operand_none();
    if (!ir_lower_expression(context, function, clause->value, &case_value)) {
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }

    IRInstruction cmp = {0};
    cmp.op = IR_OP_BRANCH_EQ;
    cmp.location = statement->location;
    cmp.lhs = switch_value;
    cmp.rhs = case_value;
    cmp.text = case_labels[i];
    if (!ir_emit(context, function, &cmp)) {
      ir_operand_destroy(&case_value);
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }
    ir_operand_destroy(&case_value);
  }

  // No match.
  if (!ir_emit_jump_instruction(context, function, default_label,
                                statement->location)) {
    for (size_t j = 0; j < switch_data->case_count; j++) {
      free(case_labels[j]);
    }
    free(case_labels);
    ir_operand_destroy(&switch_value);
    free(end_label);
    return 0;
  }

  // Emit cases.
  if (!ir_push_control_frame(context, end_label, NULL, defers)) {
    for (size_t j = 0; j < switch_data->case_count; j++) {
      free(case_labels[j]);
    }
    free(case_labels);
    ir_operand_destroy(&switch_value);
    free(end_label);
    return 0;
  }

  for (size_t i = 0; i < switch_data->case_count; i++) {
    ASTNode *case_node = switch_data->cases[i];
    CaseClause *clause = case_node ? (CaseClause *)case_node->data : NULL;
    if (!case_node || !clause) {
      continue;
    }

    if (!ir_emit_label_instruction(context, function, case_labels[i],
                                   case_node->location)) {
      ir_pop_control_frame(context);
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }

    /* Where a `fallthrough` written in this case goes. The last case has
       nowhere to go, which the checker has already reported. */
    ir_set_fallthrough_label(
        context, i + 1 < switch_data->case_count ? case_labels[i + 1] : NULL);

    // The case body is a scope of its own. Passing the live defer chain lets
    // a `defer` written inside a case run when that case ends, whether it
    // falls through to the next label or breaks out.
    if (clause->body && !ir_lower_statement_with_defers(context, function,
                                                        clause->body, defers)) {
      ir_pop_control_frame(context);
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }

    /* A case ends where the next one begins. An empty body is the exception,
       and the only way several case values share one: with no statements of
       its own there is nothing for the case to do but continue into the next.
       `fallthrough;` is what asks for that from a case that does have a body. */
    {
      size_t statement_count = 0;
      ir_block_last_statement(clause->body, &statement_count);
      if (statement_count > 0 && !ir_case_body_transfers_control(clause->body) &&
          !ir_emit_jump_instruction(context, function, end_label,
                                    case_node->location)) {
        ir_pop_control_frame(context);
        for (size_t j = 0; j < switch_data->case_count; j++) {
          free(case_labels[j]);
        }
        free(case_labels);
        ir_operand_destroy(&switch_value);
        free(end_label);
        return 0;
      }
    }
  }
  ir_set_fallthrough_label(context, NULL);

  ir_pop_control_frame(context);

  if (!ir_emit_label_instruction(context, function, end_label,
                                 statement->location)) {
    for (size_t j = 0; j < switch_data->case_count; j++) {
      free(case_labels[j]);
    }
    free(case_labels);
    ir_operand_destroy(&switch_value);
    free(end_label);
    return 0;
  }

  for (size_t j = 0; j < switch_data->case_count; j++) {
    free(case_labels[j]);
  }
  free(case_labels);
  ir_operand_destroy(&switch_value);
  free(end_label);
  return 1;
}

int ir_lower_match_statement(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *statement,
                                    IRDeferScope *defers) {
  MatchStatement *match = NULL;
  Type *subject_type = NULL;
  IROperand subject_value = ir_operand_none();
  IROperand subject_address = ir_operand_none();
  IROperand tag_value = ir_operand_none();
  char *owned_subject_name = NULL;
  const char *subject_name = NULL;
  char *end_label = NULL;
  char **arm_labels = NULL;
  char *default_label = NULL;
  int bound_scope = 0;
  int ok = 0;

  if (!context || !function || !statement ||
      statement->type != AST_MATCH_STATEMENT) {
    return 0;
  }

  match = (MatchStatement *)statement->data;
  if (!match || !match->expression) {
    ir_set_error(context, "Malformed match statement");
    return 0;
  }

  subject_type = ir_infer_expression_type(context, match->expression);
  if (!subject_type || subject_type->kind != TYPE_TAGGED_ENUM ||
      !subject_type->name) {
    ir_set_error(context, "IR match lowering requires a tagged-enum subject");
    return 0;
  }

  if (!ir_lower_expression(context, function, match->expression,
                           &subject_value)) {
    return 0;
  }

  if (subject_value.kind == IR_OPERAND_SYMBOL && subject_value.name) {
    subject_name = subject_value.name;
  } else {
    owned_subject_name = ir_new_label_name(context, "match_subject");
    if (!owned_subject_name) {
      ir_set_error(context,
                   "Out of memory while allocating match subject storage");
      goto cleanup;
    }
    if (!ir_emit_local_declaration(context, function, owned_subject_name,
                                   subject_type->name, statement->location) ||
        !ir_emit_symbol_assignment(context, function, owned_subject_name,
                                   &subject_value, statement->location)) {
      goto cleanup;
    }
    subject_name = owned_subject_name;
  }

  if (!ir_emit_address_of_symbol(context, function, subject_name,
                                 match->expression->location,
                                 &subject_address)) {
    goto cleanup;
  }

  if (!ir_make_temp_operand(context, &tag_value)) {
    goto cleanup;
  }

  {
    IRInstruction load_tag = {0};
    load_tag.op = IR_OP_LOAD;
    load_tag.location = match->expression->location;
    load_tag.dest = tag_value;
    load_tag.lhs = subject_address;
    load_tag.rhs = ir_operand_int(4);
    if (!ir_emit(context, function, &load_tag)) {
      goto cleanup;
    }
  }

  end_label = ir_new_label_name(context, "match_end");
  if (!end_label) {
    ir_set_error(context, "Out of memory while allocating match labels");
    goto cleanup;
  }

  if (match->arm_count > 0) {
    arm_labels = calloc(match->arm_count, sizeof(char *));
    if (!arm_labels) {
      ir_set_error(context, "Out of memory while allocating match labels");
      goto cleanup;
    }
    for (size_t i = 0; i < match->arm_count; i++) {
      arm_labels[i] = ir_new_label_name(context, "match_arm");
      if (!arm_labels[i]) {
        ir_set_error(context, "Out of memory while allocating match labels");
        goto cleanup;
      }
      if (match->arms[i].is_default) {
        default_label = arm_labels[i];
      }
    }
  }
  if (!default_label) {
    default_label = end_label;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;

    if (!arm || arm->is_default) {
      continue;
    }

    for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
      if (subject_type->tagged_variant_names[v] &&
          strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
              0) {
        variant_idx = (int)v;
        break;
      }
    }

    if (variant_idx < 0) {
      ir_set_error(context, "Unknown tagged-enum variant '%s' in match",
                   arm->variant_name ? arm->variant_name : "<unnamed>");
      goto cleanup;
    }

    IRInstruction cmp = {0};
    cmp.op = IR_OP_BRANCH_EQ;
    cmp.location = statement->location;
    cmp.lhs = tag_value;
    cmp.rhs =
        ir_operand_int((long long)subject_type->tagged_variant_tags[variant_idx]);
    cmp.text = arm_labels[i];
    if (!ir_emit(context, function, &cmp)) {
      goto cleanup;
    }
  }

  if (!ir_emit_jump_instruction(context, function, default_label,
                                statement->location)) {
    goto cleanup;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;

    if (!arm) {
      continue;
    }

    if (!ir_emit_label_instruction(context, function, arm_labels[i],
                                   arm->body ? arm->body->location
                                             : statement->location)) {
      goto cleanup;
    }

    if (!arm->is_default) {
      for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
        if (subject_type->tagged_variant_names[v] &&
            strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
                0) {
          variant_idx = (int)v;
          break;
        }
      }
    }

    ir_local_scope_enter(context);
    bound_scope = 1;
    if (arm->binding_name && variant_idx >= 0) {
      Type *payload_type = subject_type->tagged_variant_payloads[variant_idx];
      int payload_size = 0;
      IROperand payload_address = ir_operand_none();
      const char *bound_name = NULL;

      if (!payload_type || !payload_type->name) {
        ir_set_error(context, "Match binding '%s' has no payload to bind",
                     arm->binding_name);
        goto cleanup;
      }

      bound_name =
          ir_local_bind(context, arm->binding_name, payload_type->name);
      payload_size = (payload_type->size > 0) ? (int)payload_type->size
                                              : ir_type_storage_size(payload_type);
      if (!ir_emit_local_declaration(context, function, bound_name,
                                     payload_type->name, statement->location) ||
          !ir_emit_address_with_offset(context, function, &subject_address,
                                       subject_type->tagged_data_offset,
                                       statement->location, &payload_address)) {
        ir_operand_destroy(&payload_address);
        goto cleanup;
      }

      if (payload_size > 8) {
        IROperand binding_address = ir_operand_none();
        IRInstruction copy = {0};

        if (!ir_emit_address_of_symbol(context, function, bound_name,
                                       statement->location,
                                       &binding_address)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        copy.op = IR_OP_STORE;
        copy.location = statement->location;
        copy.dest = binding_address;
        copy.lhs = payload_address;
        copy.rhs = ir_operand_int(payload_size);
        if (!ir_emit(context, function, &copy)) {
          ir_operand_destroy(&binding_address);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&binding_address);
      } else {
        IROperand payload_value = ir_operand_none();
        IRInstruction load = {0};

        if (!ir_make_temp_operand(context, &payload_value)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        load.op = IR_OP_LOAD;
        load.location = statement->location;
        load.dest = payload_value;
        load.lhs = payload_address;
        load.rhs = ir_operand_int(payload_size);
        /* Type the load like every other load: a float payload read without
         * is_float travels the integer paths, and a float64 binding returned
         * from the arm went out in RAX while the caller read XMM0. */
        ir_load_apply_float_type(&load, payload_type);
        ir_load_apply_unsigned(&load, payload_type);
        ir_access_apply_alias_class(&load, payload_type);
        payload_value.float_bits = load.dest.float_bits;
        if (!ir_emit(context, function, &load) ||
            !ir_emit_symbol_assignment(context, function, bound_name,
                                       &payload_value, statement->location)) {
          ir_operand_destroy(&payload_value);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&payload_value);
      }

      ir_operand_destroy(&payload_address);
    }

    if (arm->body &&
        !ir_lower_statement_with_defers(context, function, arm->body, defers)) {
      goto cleanup;
    }

    ir_local_scope_leave(context);
    bound_scope = 0;

    if (!ir_emit_jump_instruction(context, function, end_label,
                                  statement->location)) {
      goto cleanup;
    }
  }

  if (!ir_emit_label_instruction(context, function, end_label,
                                 statement->location)) {
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (bound_scope) {
    ir_local_scope_leave(context);
  }
  if (arm_labels) {
    for (size_t i = 0; i < match->arm_count; i++) {
      free(arm_labels[i]);
    }
    free(arm_labels);
  }
  free(end_label);
  free(owned_subject_name);
  ir_operand_destroy(&tag_value);
  ir_operand_destroy(&subject_address);
  ir_operand_destroy(&subject_value);
  return ok;
}

// Lower a match used in expression position. Mirrors ir_lower_match_statement
// but allocates a result local; each arm lowers its body *expression* and
// stores the value into that local, which becomes the value of the match.
int ir_lower_match_expression(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     IROperand *out_value) {
  MatchStatement *match = NULL;
  Type *subject_type = NULL;
  Type *result_type = NULL;
  IROperand subject_value = ir_operand_none();
  IROperand subject_address = ir_operand_none();
  IROperand tag_value = ir_operand_none();
  char *owned_subject_name = NULL;
  const char *subject_name = NULL;
  char *result_name = NULL;
  char *end_label = NULL;
  char **arm_labels = NULL;
  char *default_label = NULL;
  int bound_scope = 0;
  int ok = 0;

  if (!context || !function || !expression || !out_value ||
      expression->type != AST_MATCH_STATEMENT) {
    return 0;
  }

  match = (MatchStatement *)expression->data;
  if (!match || !match->expression || !match->is_expression) {
    ir_set_error(context, "Malformed match expression");
    return 0;
  }

  subject_type = ir_infer_expression_type(context, match->expression);
  if (!subject_type || subject_type->kind != TYPE_TAGGED_ENUM ||
      !subject_type->name) {
    ir_set_error(context, "IR match lowering requires a tagged-enum subject");
    return 0;
  }

  result_type = ir_infer_expression_type(context, expression);
  if (!result_type || !result_type->name) {
    ir_set_error(context, "Could not determine match expression result type");
    return 0;
  }

  if (!ir_lower_expression(context, function, match->expression,
                           &subject_value)) {
    return 0;
  }

  result_name = ir_new_label_name(context, "match_result");
  if (!result_name ||
      !ir_emit_local_declaration(context, function, result_name,
                                 result_type->name, expression->location)) {
    ir_set_error(context, "Out of memory while allocating match result");
    goto cleanup;
  }

  if (subject_value.kind == IR_OPERAND_SYMBOL && subject_value.name) {
    subject_name = subject_value.name;
  } else {
    owned_subject_name = ir_new_label_name(context, "match_subject");
    if (!owned_subject_name) {
      ir_set_error(context,
                   "Out of memory while allocating match subject storage");
      goto cleanup;
    }
    if (!ir_emit_local_declaration(context, function, owned_subject_name,
                                   subject_type->name,
                                   expression->location) ||
        !ir_emit_symbol_assignment(context, function, owned_subject_name,
                                   &subject_value, expression->location)) {
      goto cleanup;
    }
    subject_name = owned_subject_name;
  }

  if (!ir_emit_address_of_symbol(context, function, subject_name,
                                 match->expression->location,
                                 &subject_address)) {
    goto cleanup;
  }

  if (!ir_make_temp_operand(context, &tag_value)) {
    goto cleanup;
  }

  {
    IRInstruction load_tag = {0};
    load_tag.op = IR_OP_LOAD;
    load_tag.location = match->expression->location;
    load_tag.dest = tag_value;
    load_tag.lhs = subject_address;
    load_tag.rhs = ir_operand_int(4);
    if (!ir_emit(context, function, &load_tag)) {
      goto cleanup;
    }
  }

  end_label = ir_new_label_name(context, "match_end");
  if (!end_label) {
    ir_set_error(context, "Out of memory while allocating match labels");
    goto cleanup;
  }

  if (match->arm_count > 0) {
    arm_labels = calloc(match->arm_count, sizeof(char *));
    if (!arm_labels) {
      ir_set_error(context, "Out of memory while allocating match labels");
      goto cleanup;
    }
    for (size_t i = 0; i < match->arm_count; i++) {
      arm_labels[i] = ir_new_label_name(context, "match_arm");
      if (!arm_labels[i]) {
        ir_set_error(context, "Out of memory while allocating match labels");
        goto cleanup;
      }
      if (match->arms[i].is_default) {
        default_label = arm_labels[i];
      }
    }
  }
  if (!default_label) {
    default_label = end_label;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;

    if (!arm || arm->is_default) {
      continue;
    }

    for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
      if (subject_type->tagged_variant_names[v] &&
          strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
              0) {
        variant_idx = (int)v;
        break;
      }
    }

    if (variant_idx < 0) {
      ir_set_error(context, "Unknown tagged-enum variant '%s' in match",
                   arm->variant_name ? arm->variant_name : "<unnamed>");
      goto cleanup;
    }

    IRInstruction cmp = {0};
    cmp.op = IR_OP_BRANCH_EQ;
    cmp.location = expression->location;
    cmp.lhs = tag_value;
    cmp.rhs = ir_operand_int(
        (long long)subject_type->tagged_variant_tags[variant_idx]);
    cmp.text = arm_labels[i];
    if (!ir_emit(context, function, &cmp)) {
      goto cleanup;
    }
  }

  if (!ir_emit_jump_instruction(context, function, default_label,
                                expression->location)) {
    goto cleanup;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;
    IROperand arm_value = ir_operand_none();

    if (!arm) {
      continue;
    }

    if (!ir_emit_label_instruction(context, function, arm_labels[i],
                                   arm->body ? arm->body->location
                                             : expression->location)) {
      goto cleanup;
    }

    if (!arm->is_default) {
      for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
        if (subject_type->tagged_variant_names[v] &&
            strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
                0) {
          variant_idx = (int)v;
          break;
        }
      }
    }

    ir_local_scope_enter(context);
    bound_scope = 1;
    if (arm->binding_name && variant_idx >= 0) {
      Type *payload_type = subject_type->tagged_variant_payloads[variant_idx];
      int payload_size = 0;
      IROperand payload_address = ir_operand_none();
      const char *bound_name = NULL;

      if (!payload_type || !payload_type->name) {
        ir_set_error(context, "Match binding '%s' has no payload to bind",
                     arm->binding_name);
        goto cleanup;
      }

      bound_name =
          ir_local_bind(context, arm->binding_name, payload_type->name);
      payload_size = (payload_type->size > 0)
                         ? (int)payload_type->size
                         : ir_type_storage_size(payload_type);
      if (!ir_emit_local_declaration(context, function, bound_name,
                                     payload_type->name,
                                     expression->location) ||
          !ir_emit_address_with_offset(context, function, &subject_address,
                                       subject_type->tagged_data_offset,
                                       expression->location,
                                       &payload_address)) {
        ir_operand_destroy(&payload_address);
        goto cleanup;
      }

      if (payload_size > 8) {
        IROperand binding_address = ir_operand_none();
        IRInstruction copy = {0};

        if (!ir_emit_address_of_symbol(context, function, bound_name,
                                       expression->location,
                                       &binding_address)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        copy.op = IR_OP_STORE;
        copy.location = expression->location;
        copy.dest = binding_address;
        copy.lhs = payload_address;
        copy.rhs = ir_operand_int(payload_size);
        if (!ir_emit(context, function, &copy)) {
          ir_operand_destroy(&binding_address);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&binding_address);
      } else {
        IROperand payload_value = ir_operand_none();
        IRInstruction load = {0};

        if (!ir_make_temp_operand(context, &payload_value)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        load.op = IR_OP_LOAD;
        load.location = expression->location;
        load.dest = payload_value;
        load.lhs = payload_address;
        load.rhs = ir_operand_int(payload_size);
        /* Same typing as the statement form above; see that comment. */
        ir_load_apply_float_type(&load, payload_type);
        ir_load_apply_unsigned(&load, payload_type);
        ir_access_apply_alias_class(&load, payload_type);
        payload_value.float_bits = load.dest.float_bits;
        if (!ir_emit(context, function, &load) ||
            !ir_emit_symbol_assignment(context, function, bound_name,
                                       &payload_value,
                                       expression->location)) {
          ir_operand_destroy(&payload_value);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&payload_value);
      }

      ir_operand_destroy(&payload_address);
    }

    if (!arm->body) {
      ir_set_error(context, "match arm has no value expression");
      goto cleanup;
    }

    if (!ir_lower_expression(context, function, arm->body, &arm_value)) {
      goto cleanup;
    }
    if (!ir_emit_symbol_assignment(context, function, result_name, &arm_value,
                                   arm->body->location)) {
      ir_operand_destroy(&arm_value);
      goto cleanup;
    }
    ir_operand_destroy(&arm_value);

    ir_local_scope_leave(context);
    bound_scope = 0;

    if (!ir_emit_jump_instruction(context, function, end_label,
                                  expression->location)) {
      goto cleanup;
    }
  }

  if (!ir_emit_label_instruction(context, function, end_label,
                                 expression->location)) {
    goto cleanup;
  }

  *out_value = ir_operand_symbol(result_name);
  if (!out_value->name) {
    ir_set_error(context, "Out of memory while finalizing match expression");
    goto cleanup;
  }
  ok = 1;

cleanup:
  if (bound_scope) {
    ir_local_scope_leave(context);
  }
  if (arm_labels) {
    for (size_t i = 0; i < match->arm_count; i++) {
      free(arm_labels[i]);
    }
    free(arm_labels);
  }
  free(end_label);
  free(result_name);
  free(owned_subject_name);
  ir_operand_destroy(&tag_value);
  ir_operand_destroy(&subject_address);
  ir_operand_destroy(&subject_value);
  return ok;
}

int ir_emit_tagged_enum_construct(IRLoweringContext *context,
                                         IRFunction *function,
                                         Symbol *constructor_symbol,
                                         ASTNode *payload_arg,
                                         SourceLocation location,
                                         IROperand *out_value) {
  Type *enum_type = NULL;
  Type *payload_type = NULL;
  char *local_name = NULL;
  IROperand enum_address = ir_operand_none();

  if (!context || !function || !constructor_symbol || !out_value) {
    return 0;
  }

  enum_type = constructor_symbol->data.constructor.enum_type;
  payload_type = constructor_symbol->data.constructor.payload_type;
  if (!enum_type || !enum_type->name) {
    ir_set_error(context, "Malformed tagged-enum constructor");
    return 0;
  }

  /* Nullary constructor referenced bare (e.g. `var x: Option = None`) reaches
   * here with payload_arg == NULL and payload_type == NULL; that is valid. A
   * payload_arg without a payload_type, or vice versa, is a type-checker bug. */
  if ((payload_type != NULL) != (payload_arg != NULL)) {
    ir_set_error(context,
                 "Tagged-enum constructor arity mismatch (variant '%s')",
                 constructor_symbol->name ? constructor_symbol->name : "?");
    return 0;
  }

  local_name = ir_new_label_name(context, "tagged_ctor");
  if (!local_name) {
    ir_set_error(context,
                 "Out of memory while allocating tagged-enum constructor");
    return 0;
  }

  if (!ir_emit_local_declaration(context, function, local_name, enum_type->name,
                                 location) ||
      !ir_emit_address_of_symbol(context, function, local_name,
                                 location, &enum_address)) {
    ir_operand_destroy(&enum_address);
    free(local_name);
    return 0;
  }

  {
    IRInstruction store_tag = {0};
    store_tag.op = IR_OP_STORE;
    store_tag.location = location;
    store_tag.dest = enum_address;
    store_tag.lhs =
        ir_operand_int((long long)constructor_symbol->data.constructor.tag_value);
    store_tag.rhs = ir_operand_int(4);
    if (!ir_emit(context, function, &store_tag)) {
      ir_operand_destroy(&enum_address);
      free(local_name);
      return 0;
    }
  }

  if (payload_type) {
    int payload_size =
        (payload_type->size > 0) ? (int)payload_type->size
                                 : ir_type_storage_size(payload_type);
    IROperand payload_value = ir_operand_none();
    IROperand payload_address = ir_operand_none();
    IROperand payload_source = ir_operand_none();
    char *payload_temp_name = NULL;

    if (!ir_lower_expression(context, function, payload_arg,
                             &payload_value) ||
        !ir_emit_address_with_offset(context, function, &enum_address,
                                     enum_type->tagged_data_offset,
                                     location, &payload_address)) {
      ir_operand_destroy(&payload_address);
      ir_operand_destroy(&payload_value);
      ir_operand_destroy(&enum_address);
      free(local_name);
      return 0;
    }

    if (payload_size > 8) {
      payload_temp_name = ir_new_label_name(context, "tagged_payload");
      if (!payload_temp_name ||
          !ir_emit_local_declaration(context, function, payload_temp_name,
                                     payload_type->name, location) ||
          !ir_emit_symbol_assignment(context, function, payload_temp_name,
                                     &payload_value, location) ||
          !ir_emit_address_of_symbol(context, function, payload_temp_name,
                                     location, &payload_source)) {
        free(payload_temp_name);
        ir_operand_destroy(&payload_source);
        ir_operand_destroy(&payload_address);
        ir_operand_destroy(&payload_value);
        ir_operand_destroy(&enum_address);
        free(local_name);
        return 0;
      }
    } else {
      payload_source = payload_value;
    }

    {
      IRInstruction store_payload = {0};
      store_payload.op = IR_OP_STORE;
      store_payload.location = location;
      store_payload.dest = payload_address;
      store_payload.lhs = payload_source;
      store_payload.rhs = ir_operand_int(payload_size);
      ir_access_apply_alias_class(&store_payload, payload_type);
      if (payload_size <= 8 &&
          (payload_type->kind == TYPE_FLOAT32 ||
           payload_type->kind == TYPE_FLOAT64 ||
           payload_type->kind == TYPE_FLOAT16 ||
           payload_type->kind == TYPE_BFLOAT16)) {
        ir_assign_apply_float_bits(&store_payload, &store_payload.lhs,
                                   ir_type_float_bits(payload_type));
      }
      if (!ir_emit(context, function, &store_payload)) {
        free(payload_temp_name);
        ir_operand_destroy(&payload_source);
        ir_operand_destroy(&payload_address);
        ir_operand_destroy(&payload_value);
        ir_operand_destroy(&enum_address);
        free(local_name);
        return 0;
      }
    }

    free(payload_temp_name);
    if (payload_size > 8) {
      ir_operand_destroy(&payload_source);
    }
    ir_operand_destroy(&payload_address);
    ir_operand_destroy(&payload_value);
  }

  ir_operand_destroy(&enum_address);
  *out_value = ir_operand_symbol(local_name);
  free(local_name);
  if (out_value->kind != IR_OPERAND_SYMBOL || !out_value->name) {
    ir_set_error(context,
                 "Out of memory while returning tagged-enum constructor");
    return 0;
  }

  return 1;
}

int ir_lower_tagged_enum_constructor_call(IRLoweringContext *context,
                                                 IRFunction *function,
                                                 ASTNode *expression,
                                                 Symbol *constructor_symbol,
                                                 IROperand *out_value) {
  CallExpression *call = NULL;
  Type *payload_type = NULL;
  ASTNode *payload_arg = NULL;

  if (!context || !function || !expression || !constructor_symbol ||
      !out_value) {
    return 0;
  }

  call = (CallExpression *)expression->data;
  payload_type = constructor_symbol->data.constructor.payload_type;
  if (!call) {
    ir_set_error(context, "Malformed tagged-enum constructor call");
    return 0;
  }

  if (payload_type) {
    if (call->argument_count != 1 || !call->arguments ||
        !call->arguments[0]) {
      ir_set_error(context,
                   "Tagged-enum variant '%s' expects exactly one payload argument",
                   constructor_symbol->name ? constructor_symbol->name : "?");
      return 0;
    }
    payload_arg = call->arguments[0];
  } else if (call->argument_count != 0) {
    ir_set_error(context,
                 "Tagged-enum variant '%s' is nullary; pass no arguments",
                 constructor_symbol->name ? constructor_symbol->name : "?");
    return 0;
  }

  return ir_emit_tagged_enum_construct(context, function, constructor_symbol,
                                       payload_arg, expression->location,
                                       out_value);
}
