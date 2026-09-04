// AST->IR lowering: runtime checks and control-flow (break/continue) frames.
#include "ir_lowering_internal.h"

/* Local name bindings. See IRLocalBinding for why a redeclaration at a
 * different type needs a name of its own. */

void ir_local_scope_enter(IRLoweringContext *context) {
  if (context) {
    context->local_scope_depth++;
  }
}

void ir_local_scope_leave(IRLoweringContext *context) {
  if (!context) {
    return;
  }
  for (size_t i = context->local_binding_count; i-- > 0;) {
    if (context->local_bindings[i].depth < context->local_scope_depth) {
      break;
    }
    context->local_bindings[i].active = 0;
  }
  if (context->local_scope_depth > 0) {
    context->local_scope_depth--;
  }
}

void ir_local_bindings_reset(IRLoweringContext *context) {
  if (!context) {
    return;
  }
  for (size_t i = 0; i < context->local_binding_count; i++) {
    if (context->local_bindings[i].owns_ir_name) {
      free((char *)context->local_bindings[i].ir_name);
    }
  }
  free(context->local_bindings);
  context->local_bindings = NULL;
  context->local_binding_count = 0;
  context->local_binding_capacity = 0;
  context->local_scope_depth = 0;
  context->local_rename_serial = 0;
}

static int ir_local_type_text_matches(const char *a, const char *b) {
  if (!a || !b) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

static const char *ir_local_bind_impl(IRLoweringContext *context,
                                      const char *name, const char *type_text,
                                      int allow_rename) {
  if (!context || !name) {
    return name;
  }

  const char *ir_name = NULL;
  const char *reusable = NULL;
  int seen = 0;
  int owns = 0;
  int active = 0;
  for (size_t i = 0; i < context->local_binding_count; i++) {
    const IRLocalBinding *b = &context->local_bindings[i];
    if (strcmp(b->name, name) != 0) {
      continue;
    }
    seen = 1;
    /* A binding whose scope has ended can lend its slot: two `var x: int32` in
     * sibling blocks are never live at once, so sharing costs nothing and the
     * emitted IR is unchanged. One that is still active cannot. A `var x`
     * nested inside another `var x`'s scope is a second variable, and sharing
     * the slot made the inner one write through -- the outer read 5 back from
     * an inner block that set 5, and a loop body's `var x` survived the loop. */
    if (b->active) {
      active = 1;
      continue;
    }
    if (!reusable && ir_local_type_text_matches(b->type_text, type_text)) {
      reusable = b->ir_name;
    }
  }
  if (!active) {
    ir_name = reusable;
  }
  /* A local shadowing a module-level global needs a name of its own for the
   * same reason a local shadowing a local does: every backend keys its slot,
   * float, string and declared-type tables on the name, so the local and the
   * global shared one storage symbol. `var s: int64` at module scope with a
   * `var s: P` inside a function read the global's bytes through the local's
   * fields. */
  int shadows_global = 0;
  if (allow_rename && context->symbol_table) {
    const Symbol *global = symbol_table_lookup(context->symbol_table, name);
    shadows_global = global != NULL && global->kind == SYMBOL_VARIABLE;
  }
  if (!allow_rename) {
    ir_name = name;
  }
  if (!ir_name) {
    if (!seen && !shadows_global) {
      ir_name = name;
    } else {
      size_t len = strlen(name) + 24;
      char *renamed = (char *)malloc(len);
      if (!renamed) {
        /* Out of memory: keep the source name. The declaration still lowers;
         * it just shares a slot the way it did before, as no rename happened. */
        return name;
      }
      /* `$$`, not `$`: SROA names a split field `<member>$<offset>`, so a
       * single `$` could collide with the scalars of a same-named struct in
       * the same function. Source identifiers cannot contain either. */
      snprintf(renamed, len, "%s$$%d", name, ++context->local_rename_serial);
      ir_name = renamed;
      owns = 1;
    }
  }

  if (context->local_binding_count == context->local_binding_capacity) {
    size_t grown = context->local_binding_capacity
                       ? context->local_binding_capacity * 2
                       : 8;
    IRLocalBinding *items = (IRLocalBinding *)realloc(
        context->local_bindings, grown * sizeof(IRLocalBinding));
    if (!items) {
      if (owns) {
        free((char *)ir_name);
      }
      return name;
    }
    context->local_bindings = items;
    context->local_binding_capacity = grown;
  }

  IRLocalBinding *slot = &context->local_bindings[context->local_binding_count++];
  slot->name = name;
  slot->ir_name = ir_name;
  slot->type_text = type_text;
  slot->depth = context->local_scope_depth;
  slot->active = 1;
  slot->owns_ir_name = owns;
  return ir_name;
}

const char *ir_local_bind(IRLoweringContext *context, const char *name,
                          const char *type_text) {
  return ir_local_bind_impl(context, name, type_text, 1);
}

/* A parameter is recorded so its declared type can be read back from the
 * binding, and it keeps the source name: the backends home an incoming
 * argument into the slot named by function->parameter_names, so renaming one
 * here would leave every use looking for a slot the prologue never filled.
 * Without the binding, a parameter shadowing a global resolved to the global's
 * type, which is how a global named `s` broke std/io's `cstr(s: string)`. */
const char *ir_local_bind_parameter(IRLoweringContext *context,
                                    const char *name, const char *type_text) {
  return ir_local_bind_impl(context, name, type_text, 0);
}

const IRLocalBinding *ir_local_binding_find(IRLoweringContext *context,
                                            const char *name) {
  if (!context || !name) {
    return NULL;
  }
  for (size_t i = context->local_binding_count; i-- > 0;) {
    const IRLocalBinding *b = &context->local_bindings[i];
    if (b->active && strcmp(b->name, name) == 0) {
      return b;
    }
  }
  return NULL;
}

const char *ir_local_ir_name(IRLoweringContext *context, const char *name) {
  const IRLocalBinding *b = ir_local_binding_find(context, name);
  return b ? b->ir_name : name;
}

int ir_emit_runtime_trap_ex(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location, uint32_t kind,
                                   const char *message, const IROperand *arg0,
                                   const IROperand *arg1) {
  if (!context || !function || !message) {
    return 0;
  }

  IRInstruction trap_call = {0};
  trap_call.op = IR_OP_CALL;
  trap_call.location = location;
  trap_call.text = "mettle_crash_trap_ex";
  trap_call.argument_count = 4;
  trap_call.arguments = calloc(4, sizeof(IROperand));
  if (!trap_call.arguments) {
    ir_set_error(context, "Out of memory while lowering runtime trap");
    return 0;
  }
  trap_call.arguments[0] = ir_operand_int((long long)kind);
  trap_call.arguments[1] = ir_operand_string(message);
  trap_call.arguments[2] = arg0 ? ir_operand_copy(arg0) : ir_operand_int(0);
  trap_call.arguments[3] = arg1 ? ir_operand_copy(arg1) : ir_operand_int(0);
  if (!ir_emit(context, function, &trap_call)) {
    ir_operand_destroy(&trap_call.arguments[0]);
    ir_operand_destroy(&trap_call.arguments[1]);
    ir_operand_destroy(&trap_call.arguments[2]);
    ir_operand_destroy(&trap_call.arguments[3]);
    free(trap_call.arguments);
    return 0;
  }
  ir_operand_destroy(&trap_call.arguments[0]);
  ir_operand_destroy(&trap_call.arguments[1]);
  ir_operand_destroy(&trap_call.arguments[2]);
  ir_operand_destroy(&trap_call.arguments[3]);
  free(trap_call.arguments);
  return 1;
}

int ir_emit_null_check(IRLoweringContext *context, IRFunction *function,
                              SourceLocation location, const IROperand *value) {
  if (!context || !function || !value) {
    return 0;
  }
  if (!context->emit_runtime_checks) {
    return 1;
  }

  char *trap_label = ir_new_label_name(context, "trap_null");
  char *ok_label = ir_new_label_name(context, "nonnull");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_set_error(context, "Out of memory while lowering null check");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = *value;
  branch.text = trap_label;
  if (!ir_emit(context, function, &branch)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  if (!ir_emit(context, function, &jump)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction trap = {0};
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  if (!ir_emit(context, function, &trap) ||
      !ir_emit_runtime_trap_ex(
          context, function, location, 1u,
          "Fatal error: Null pointer dereference", NULL, NULL)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction ok = {0};
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  if (!ir_emit(context, function, &ok)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  free(trap_label);
  free(ok_label);
  return 1;
}

static int ir_emit_refinement_side(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location,
                                   const IROperand *value, const char *op,
                                   long long bound, const char *message) {
  IROperand holds = ir_operand_none();
  IRInstruction compare = {0};
  IRInstruction branch = {0};
  IRInstruction jump = {0};
  IRInstruction trap = {0};
  IRInstruction ok = {0};
  char *trap_label;
  char *ok_label;
  if (!ir_make_temp_operand(context, &holds)) {
    return 0;
  }
  compare.op = IR_OP_BINARY;
  compare.location = location;
  compare.dest = holds;
  compare.lhs = ir_operand_copy(value);
  compare.rhs = ir_operand_int(bound);
  compare.text = (char *)op;
  if (!ir_emit(context, function, &compare)) {
    ir_operand_destroy(&holds);
    return 0;
  }
  trap_label = ir_new_label_name(context, "trap_refine");
  ok_label = ir_new_label_name(context, "refined");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&holds);
    ir_set_error(context, "Out of memory while lowering refinement check");
    return 0;
  }
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = holds;
  branch.text = trap_label;
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  if (!ir_emit(context, function, &branch) ||
      !ir_emit(context, function, &jump) ||
      !ir_emit(context, function, &trap) ||
      !ir_emit_runtime_trap_ex(context, function, location, 2u, message,
                               value, &compare.rhs) ||
      !ir_emit(context, function, &ok)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&holds);
    return 0;
  }
  free(trap_label);
  free(ok_label);
  ir_operand_destroy(&holds);
  return 1;
}

/* The whole predicate, run at the site the proof was made, with the binding
 * standing for the value that was proven. A relational type has no interval to
 * compare against, so this is what "re-checked at run time like every other
 * proof" means for one. Nothing here trusts the prover: it evaluates the same
 * condition the program wrote and traps when it does not hold. */
int ir_emit_refinement_predicate(IRLoweringContext *context,
                                 IRFunction *function, SourceLocation location,
                                 const IROperand *value, const Type *refined,
                                 ASTNode *predicate, const char *binding) {
  IROperand held = ir_operand_none();
  char message[192];
  char *trap_label;
  char *ok_label;
  const char *saved_name;
  IROperand saved_value;
  int saved_active;
  int ok;
  if (!context || !function || !value || !predicate) {
    return 1;
  }
  saved_name = context->refine_binding_name;
  saved_value = context->refine_binding_value;
  saved_active = context->refine_binding_active;
  if (binding) {
    context->refine_binding_name = binding;
    context->refine_binding_value = *value;
    context->refine_binding_active = 1;
  }
  ok = ir_lower_expression(context, function, predicate, &held);
  context->refine_binding_name = saved_name;
  context->refine_binding_value = saved_value;
  context->refine_binding_active = saved_active;
  if (!ok) {
    return 0;
  }
  trap_label = ir_new_label_name(context, "trap_refine");
  ok_label = ir_new_label_name(context, "refine_holds");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&held);
    return 0;
  }
  snprintf(message, sizeof(message),
           "Fatal error: a value the compiler proved to be '%s' is not one",
           refined && refined->name ? refined->name : "?");
  {
    IRInstruction branch = {0};
    branch.op = IR_OP_BRANCH_ZERO;
    branch.location = location;
    branch.lhs = held;
    branch.text = trap_label;
    if (!ir_emit(context, function, &branch)) {
      free(trap_label);
      free(ok_label);
      ir_operand_destroy(&held);
      return 0;
    }
  }
  {
    IRInstruction jump = {0};
    jump.op = IR_OP_JUMP;
    jump.location = location;
    jump.text = ok_label;
    if (!ir_emit(context, function, &jump)) {
      free(trap_label);
      free(ok_label);
      ir_operand_destroy(&held);
      return 0;
    }
  }
  {
    IRInstruction trap = {0};
    trap.op = IR_OP_LABEL;
    trap.location = location;
    trap.text = trap_label;
    if (!ir_emit(context, function, &trap) ||
        !ir_emit_runtime_trap_ex(context, function, location, 5u, message,
                                 value, NULL)) {
      free(trap_label);
      free(ok_label);
      ir_operand_destroy(&held);
      return 0;
    }
  }
  {
    IRInstruction done = {0};
    done.op = IR_OP_LABEL;
    done.location = location;
    done.text = ok_label;
    if (!ir_emit(context, function, &done)) {
      free(trap_label);
      free(ok_label);
      ir_operand_destroy(&held);
      return 0;
    }
  }
  free(trap_label);
  free(ok_label);
  ir_operand_destroy(&held);
  return 1;
}


size_t g_ir_overflow_emitted;
size_t g_ir_overflow_proved;

void ir_lowering_overflow_totals(size_t *emitted, size_t *proved) {
  if (emitted) {
    *emitted = g_ir_overflow_emitted;
  }
  if (proved) {
    *proved = g_ir_overflow_proved;
  }
}

/* Trap when `holds` is zero. The compare that produced it is the caller's,
 * because the three overflow shapes compute it differently. */
static int ir_emit_overflow_trap(IRLoweringContext *context,
                                 IRFunction *function, SourceLocation location,
                                 const IROperand *holds, const char *message) {
  IRInstruction branch = {0};
  IRInstruction jump = {0};
  IRInstruction trap = {0};
  IRInstruction ok = {0};
  char *trap_label = ir_new_label_name(context, "trap_overflow");
  char *ok_label = ir_new_label_name(context, "in_range");
  int emitted;
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    return 0;
  }
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = *holds;
  branch.text = trap_label;
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  emitted = ir_emit(context, function, &branch) &&
            ir_emit(context, function, &jump) &&
            ir_emit(context, function, &trap) &&
            ir_emit_runtime_trap_ex(context, function, location, 6u, message,
                                    NULL, NULL) &&
            ir_emit(context, function, &ok);
  free(trap_label);
  free(ok_label);
  if (emitted) {
    g_ir_overflow_emitted++;
  }
  return emitted;
}

/* A narrow signed result: the truncation the language already applies is the
 * check. `wide` is what the arithmetic produced at register width and
 * `narrow` is that value in its declared type, so the two differing is
 * exactly the definition of the result not fitting. */
int ir_emit_overflow_check_narrow(IRLoweringContext *context,
                                  IRFunction *function, SourceLocation location,
                                  const IROperand *wide,
                                  const IROperand *narrow, const char *op,
                                  const char *type_name) {
  IROperand holds = ir_operand_none();
  IRInstruction compare = {0};
  char message[192];
  if (!ir_make_temp_operand(context, &holds)) {
    return 0;
  }
  compare.op = IR_OP_BINARY;
  compare.location = location;
  compare.dest = holds;
  compare.lhs = ir_operand_copy(narrow);
  compare.rhs = ir_operand_copy(wide);
  compare.text = "==";
  if (!ir_emit(context, function, &compare)) {
    ir_operand_destroy(&holds);
    return 0;
  }
  snprintf(message, sizeof(message),
           "Fatal error: signed '%s' overflowed %s", op,
           type_name ? type_name : "its type");
  {
    int ok = ir_emit_overflow_trap(context, function, location, &holds,
                                   message);
    ir_operand_destroy(&holds);
    return ok;
  }
}

/* A 64-bit signed result has nothing wider to be compared against, so the
 * question is asked of the operands' signs instead: an add overflows when
 * both operands differ in sign from the result, a subtract when the operands
 * differ from each other and the result differs from the left one, and a
 * multiply when dividing the result back does not return what went in. */
int ir_emit_overflow_check_wide(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *result, const IROperand *left,
                                const IROperand *right, const char *op,
                                const char *type_name) {
  IROperand a = ir_operand_none();
  IROperand b = ir_operand_none();
  IROperand c = ir_operand_none();
  IROperand holds = ir_operand_none();
  IRInstruction one = {0};
  IRInstruction two = {0};
  IRInstruction three = {0};
  IRInstruction test = {0};
  char message[192];
  int multiply = strcmp(op, "*") == 0;
  if (!ir_make_temp_operand(context, &a) || !ir_make_temp_operand(context, &b) ||
      !ir_make_temp_operand(context, &c) ||
      !ir_make_temp_operand(context, &holds)) {
    ir_operand_destroy(&a);
    ir_operand_destroy(&b);
    ir_operand_destroy(&c);
    ir_operand_destroy(&holds);
    return 0;
  }
  if (multiply) {
    /* left == 0 || result / left == right */
    one.op = IR_OP_BINARY;
    one.location = location;
    one.dest = a;
    one.lhs = ir_operand_copy(left);
    one.rhs = ir_operand_int(0);
    one.text = "==";
    two.op = IR_OP_BINARY;
    two.location = location;
    two.dest = b;
    two.lhs = ir_operand_copy(result);
    two.rhs = ir_operand_copy(left);
    two.text = "/";
    three.op = IR_OP_BINARY;
    three.location = location;
    three.dest = c;
    three.lhs = b;
    three.rhs = ir_operand_copy(right);
    three.text = "==";
    test.op = IR_OP_BINARY;
    test.location = location;
    test.dest = holds;
    test.lhs = a;
    test.rhs = c;
    test.text = "|";
  } else {
    int adding = strcmp(op, "+") == 0;
    one.op = IR_OP_BINARY;
    one.location = location;
    one.dest = a;
    one.lhs = ir_operand_copy(left);
    one.rhs = adding ? ir_operand_copy(result) : ir_operand_copy(right);
    one.text = "^";
    two.op = IR_OP_BINARY;
    two.location = location;
    two.dest = b;
    two.lhs = adding ? ir_operand_copy(right) : ir_operand_copy(left);
    two.rhs = ir_operand_copy(result);
    two.text = "^";
    three.op = IR_OP_BINARY;
    three.location = location;
    three.dest = c;
    three.lhs = a;
    three.rhs = b;
    three.text = "&";
    test.op = IR_OP_BINARY;
    test.location = location;
    test.dest = holds;
    test.lhs = c;
    test.rhs = ir_operand_int(0);
    test.text = ">=";
  }
  if (!ir_emit(context, function, &one) ||
      (multiply && !ir_emit(context, function, &two)) ||
      (!multiply && !ir_emit(context, function, &two)) ||
      !ir_emit(context, function, &three) ||
      !ir_emit(context, function, &test)) {
    ir_operand_destroy(&holds);
    return 0;
  }
  snprintf(message, sizeof(message),
           "Fatal error: signed '%s' overflowed %s", op,
           type_name ? type_name : "its type");
  {
    int ok = ir_emit_overflow_trap(context, function, location, &holds,
                                   message);
    ir_operand_destroy(&holds);
    return ok;
  }
}

int ir_emit_refinement_check(IRLoweringContext *context, IRFunction *function,
                             SourceLocation location, const IROperand *value,
                             const Type *refined) {
  char message[160];
  if (!context || !function || !value || !refined ||
      !refined->refine_has_range) {
    return 1;
  }
  snprintf(message, sizeof(message),
           "Fatal error: a value the compiler proved to be '%s' is not one",
           refined->name ? refined->name : "?");
  if (!ir_emit_refinement_side(context, function, location, value, ">=",
                               refined->refine_min, message) ||
      !ir_emit_refinement_side(context, function, location, value, "<=",
                               refined->refine_max, message)) {
    return 0;
  }
  return 1;
}

int ir_emit_bounds_check(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *index, size_t array_size) {
  if (!context || !function || !index) {
    return 0;
  }
  if (!context->emit_runtime_checks) {
    return 1;
  }

  IROperand in_bounds = ir_operand_none();
  if (!ir_make_temp_operand(context, &in_bounds)) {
    return 0;
  }

  IRInstruction compare = {0};
  compare.op = IR_OP_BINARY;
  compare.location = location;
  compare.dest = in_bounds;
  compare.lhs = *index;
  compare.rhs = ir_operand_int((long long)array_size);
  compare.text = "<";
  if (!ir_emit(context, function, &compare)) {
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  char *trap_label = ir_new_label_name(context, "trap_bounds");
  char *ok_label = ir_new_label_name(context, "in_bounds");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    ir_set_error(context, "Out of memory while lowering bounds check");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = in_bounds;
  branch.text = trap_label;
  if (!ir_emit(context, function, &branch)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  if (!ir_emit(context, function, &jump)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction trap = {0};
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  if (!ir_emit(context, function, &trap) ||
      !ir_emit_runtime_trap_ex(context, function, location, 2u,
                               "Fatal error: Array index out of bounds", index,
                               &compare.rhs)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction ok = {0};
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  if (!ir_emit(context, function, &ok)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  free(trap_label);
  free(ok_label);
  ir_operand_destroy(&in_bounds);
  return 1;
}

/* The bounds check a pointer could never have. A slice carries its length
 * beside its data, so the extent is loaded from the value itself, and a
 * negative index fails the same check as an oversized one. */
int ir_small_float_local(IRLoweringContext *context, IRFunction *function,
                         const char *source_name, const char *ir_name,
                         Type *type) {
  Symbol *symbol = NULL;
  if (!context || !function || !ir_name || !type ||
      (type->kind != TYPE_FLOAT16 && type->kind != TYPE_BFLOAT16)) {
    return 0;
  }
  if (function->parameter_names) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      if (function->parameter_names[i] &&
          strcmp(function->parameter_names[i], ir_name) == 0) {
        return 0;
      }
    }
  }
  symbol = context->symbol_table && source_name
               ? symbol_table_lookup(context->symbol_table, source_name)
               : NULL;
  if (symbol && symbol->scope && symbol->scope->type == SCOPE_GLOBAL &&
      strcmp(source_name, ir_name) == 0) {
    return 0;
  }
  return 1;
}

int ir_emit_small_float_home_store(IRLoweringContext *context,
                                   IRFunction *function, const char *ir_name,
                                   Type *type, const IROperand *value,
                                   SourceLocation location) {
  IROperand address = ir_operand_none();
  IRInstruction store = {0};
  int ok = 0;
  if (!ir_emit_address_of_symbol(context, function, ir_name, location,
                                 &address)) {
    return 0;
  }
  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = address;
  store.lhs = *value;
  store.rhs = ir_operand_int(2);
  store.is_float = 1;
  store.float_bits = 32;
  ir_access_apply_alias_class(&store, type);
  ok = ir_emit(context, function, &store);
  ir_operand_destroy(&address);
  return ok;
}

int ir_emit_small_float_home_load(IRLoweringContext *context,
                                  IRFunction *function, const char *ir_name,
                                  Type *type, SourceLocation location,
                                  IROperand *out_value) {
  IROperand address = ir_operand_none();
  IRInstruction load = {0};
  int ok = 0;
  *out_value = ir_operand_none();
  if (!ir_emit_address_of_symbol(context, function, ir_name, location,
                                 &address) ||
      !ir_make_temp_operand(context, out_value)) {
    ir_operand_destroy(&address);
    return 0;
  }
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = *out_value;
  load.lhs = address;
  load.rhs = ir_operand_int(2);
  ir_load_apply_float_type(&load, type);
  ir_access_apply_alias_class(&load, type);
  ok = ir_emit(context, function, &load);
  out_value->float_bits = load.dest.float_bits;
  ir_operand_destroy(&address);
  if (!ok) {
    ir_operand_destroy(out_value);
  }
  return ok;
}

int ir_emit_load_word(IRLoweringContext *context, IRFunction *function,
                      const IROperand *base_address, size_t offset,
                      SourceLocation location, IROperand *out_value) {
  IROperand slot = ir_operand_none();
  IRInstruction load = {0};
  int ok = 0;
  if (!context || !function || !base_address || !out_value) {
    return 0;
  }
  *out_value = ir_operand_none();
  if (!ir_emit_address_with_offset(context, function, base_address, offset,
                                   location, &slot) ||
      !ir_make_temp_operand(context, out_value)) {
    ir_operand_destroy(&slot);
    return 0;
  }
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = *out_value;
  load.lhs = slot;
  load.rhs = ir_operand_int(8);
  load.alias_class =
      offset == 0 ? IR_ALIAS_CLASS_POINTER : IR_ALIAS_CLASS_I64;
  ok = ir_emit(context, function, &load);
  ir_operand_destroy(&slot);
  if (!ok) {
    ir_operand_destroy(out_value);
  }
  return ok;
}

int ir_emit_store_word(IRLoweringContext *context, IRFunction *function,
                       const IROperand *base_address, size_t offset,
                       const IROperand *value, SourceLocation location) {
  IROperand slot = ir_operand_none();
  IRInstruction store = {0};
  int ok = 0;
  if (!context || !function || !base_address || !value) {
    return 0;
  }
  if (!ir_emit_address_with_offset(context, function, base_address, offset,
                                   location, &slot)) {
    return 0;
  }
  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = slot;
  store.lhs = *value;
  store.rhs = ir_operand_int(8);
  store.alias_class =
      offset == 0 ? IR_ALIAS_CLASS_POINTER : IR_ALIAS_CLASS_I64;
  ok = ir_emit(context, function, &store);
  ir_operand_destroy(&slot);
  return ok;
}

int ir_emit_binary_temp(IRLoweringContext *context, IRFunction *function,
                        const char *operator_text, const IROperand *lhs,
                        const IROperand *rhs, SourceLocation location,
                        IROperand *out_value) {
  IRInstruction binary = {0};
  if (!context || !function || !operator_text || !lhs || !rhs || !out_value) {
    return 0;
  }
  if (!ir_make_temp_operand(context, out_value)) {
    return 0;
  }
  binary.op = IR_OP_BINARY;
  binary.location = location;
  binary.dest = *out_value;
  binary.lhs = *lhs;
  binary.rhs = *rhs;
  binary.text = (char *)operator_text;
  if (!ir_emit(context, function, &binary)) {
    ir_operand_destroy(out_value);
    return 0;
  }
  return 1;
}

int ir_emit_slice_bounds_check(IRLoweringContext *context, IRFunction *function,
                               SourceLocation location,
                               const IROperand *slice_address,
                               const IROperand *index) {
  IROperand length = ir_operand_none();
  IROperand length_slot = ir_operand_none();
  IROperand in_bounds = ir_operand_none();
  IROperand non_negative = ir_operand_none();
  char *trap_label = NULL;
  char *ok_label = NULL;
  int ok = 0;

  if (!context || !function || !slice_address || !index) {
    return 0;
  }
  if (!context->emit_runtime_checks && !context->emit_safety_checks) {
    return 1;
  }

  if (!ir_emit_address_with_offset(context, function, slice_address, 8,
                                   location, &length_slot) ||
      !ir_make_temp_operand(context, &length)) {
    ir_operand_destroy(&length_slot);
    return 0;
  }
  {
    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = location;
    load.dest = length;
    load.lhs = length_slot;
    load.rhs = ir_operand_int(8);
    load.alias_class = IR_ALIAS_CLASS_I64;
    if (!ir_emit(context, function, &load)) {
      ir_operand_destroy(&length_slot);
      ir_operand_destroy(&length);
      return 0;
    }
  }
  ir_operand_destroy(&length_slot);

  trap_label = ir_new_label_name(context, "trap_slice_bounds");
  ok_label = ir_new_label_name(context, "in_slice_bounds");
  if (!trap_label || !ok_label ||
      !ir_make_temp_operand(context, &in_bounds) ||
      !ir_make_temp_operand(context, &non_negative)) {
    goto done;
  }

  {
    IRInstruction compare = {0};
    IRInstruction branch = {0};
    compare.op = IR_OP_BINARY;
    compare.location = location;
    compare.dest = in_bounds;
    compare.lhs = *index;
    compare.rhs = length;
    compare.text = "<";
    if (!ir_emit(context, function, &compare)) {
      goto done;
    }
    branch.op = IR_OP_BRANCH_ZERO;
    branch.location = location;
    branch.lhs = in_bounds;
    branch.text = trap_label;
    if (!ir_emit(context, function, &branch)) {
      goto done;
    }
  }
  {
    IRInstruction compare = {0};
    IRInstruction branch = {0};
    compare.op = IR_OP_BINARY;
    compare.location = location;
    compare.dest = non_negative;
    compare.lhs = *index;
    compare.rhs = ir_operand_int(0);
    compare.text = ">=";
    if (!ir_emit(context, function, &compare)) {
      goto done;
    }
    branch.op = IR_OP_BRANCH_ZERO;
    branch.location = location;
    branch.lhs = non_negative;
    branch.text = trap_label;
    if (!ir_emit(context, function, &branch)) {
      goto done;
    }
  }
  {
    IRInstruction jump = {0};
    IRInstruction trap = {0};
    IRInstruction after = {0};
    jump.op = IR_OP_JUMP;
    jump.location = location;
    jump.text = ok_label;
    if (!ir_emit(context, function, &jump)) {
      goto done;
    }
    trap.op = IR_OP_LABEL;
    trap.location = location;
    trap.text = trap_label;
    if (!ir_emit(context, function, &trap) ||
        !ir_emit_runtime_trap_ex(context, function, location, 2u,
                                 "Fatal error: Slice index out of bounds",
                                 index, &length)) {
      goto done;
    }
    after.op = IR_OP_LABEL;
    after.location = location;
    after.text = ok_label;
    if (!ir_emit(context, function, &after)) {
      goto done;
    }
  }
  ok = 1;

done:
  free(trap_label);
  free(ok_label);
  ir_operand_destroy(&in_bounds);
  ir_operand_destroy(&non_negative);
  ir_operand_destroy(&length);
  return ok;
}

int ir_emit_safety_check(IRLoweringContext *context, IRFunction *function,
                         SourceLocation location, const IROperand *base,
                         const IROperand *offset, long long access_size,
                         long long extent, int access_kind, const char *what) {
  if (!context || !function || !base || !offset) {
    return 0;
  }
  if (!context->emit_safety_checks) {
    return 1;
  }
  /* A zero-width access reads nothing, and an object of unknown element size
   * gives the check no range to test. Neither can fail, so neither is worth a
   * check. */
  if (access_size <= 0) {
    return 1;
  }

  IRInstruction check = {0};
  check.op = IR_OP_SAFETY_CHECK;
  check.location = location;
  check.text = (char *)what;
  check.arguments = calloc(IR_SAFETY_ARG_COUNT, sizeof(IROperand));
  if (!check.arguments) {
    ir_set_error(context, "Out of memory while lowering safety check");
    return 0;
  }
  check.argument_count = IR_SAFETY_ARG_COUNT;
  check.arguments[IR_SAFETY_ARG_BASE] = ir_operand_copy(base);
  check.arguments[IR_SAFETY_ARG_OFFSET] = ir_operand_copy(offset);
  check.arguments[IR_SAFETY_ARG_SIZE] = ir_operand_int(access_size);
  check.arguments[IR_SAFETY_ARG_EXTENT] = ir_operand_int(extent);
  check.arguments[IR_SAFETY_ARG_ACCESS] = ir_operand_int(access_kind);

  int emitted = ir_emit(context, function, &check);
  for (size_t i = 0; i < IR_SAFETY_ARG_COUNT; i++) {
    ir_operand_destroy(&check.arguments[i]);
  }
  free(check.arguments);
  return emitted;
}

int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label,
                                         IRDeferScope *defers) {
  if (!context) {
    return 0;
  }

  if (context->control_count >= context->control_capacity) {
    size_t new_capacity =
        context->control_capacity == 0 ? 8 : context->control_capacity * 2;
    IRControlFrame *new_stack =
        realloc(context->control_stack, new_capacity * sizeof(IRControlFrame));
    if (!new_stack) {
      ir_set_error(context,
                   "Out of memory while growing IR control-flow stack");
      return 0;
    }
    context->control_stack = new_stack;
    context->control_capacity = new_capacity;
  }

  IRControlFrame *frame = &context->control_stack[context->control_count++];
  frame->break_label = break_label ? mettle_strdup(break_label) : NULL;
  frame->continue_label =
      continue_label ? mettle_strdup(continue_label) : NULL;
  frame->user_label = user_label ? mettle_strdup(user_label) : NULL;
  frame->fallthrough_label = NULL;
  frame->defers = defers;
  if ((break_label && !frame->break_label) ||
      (continue_label && !frame->continue_label) ||
      (user_label && !frame->user_label)) {
    free(frame->break_label);
    free(frame->continue_label);
    free(frame->user_label);
    frame->break_label = NULL;
    frame->continue_label = NULL;
    frame->user_label = NULL;
    context->control_count--;
    ir_set_error(context, "Out of memory while setting up control-flow labels");
    return 0;
  }
  return 1;
}

int ir_push_control_frame(IRLoweringContext *context,
                                 const char *break_label,
                                 const char *continue_label,
                                 IRDeferScope *defers) {
  return ir_push_labeled_control_frame(context, break_label, continue_label,
                                       NULL, defers);
}

void ir_pop_control_frame(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return;
  }

  IRControlFrame *frame = &context->control_stack[context->control_count - 1];
  free(frame->break_label);
  free(frame->continue_label);
  free(frame->user_label);
  frame->break_label = NULL;
  frame->continue_label = NULL;
  frame->user_label = NULL;
  context->control_count--;
}

const char *ir_current_break_label(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return NULL;
  }
  return context->control_stack[context->control_count - 1].break_label;
}

const IRControlFrame *ir_current_fallthrough_frame(IRLoweringContext *context) {
  size_t i;
  if (!context) {
    return NULL;
  }
  for (i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->fallthrough_label) {
      return frame;
    }
  }
  return NULL;
}

void ir_set_fallthrough_label(IRLoweringContext *context, const char *label) {
  if (!context || context->control_count == 0) {
    return;
  }
  context->control_stack[context->control_count - 1].fallthrough_label = label;
}

const char *ir_current_continue_label(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return NULL;
  }

  for (size_t i = context->control_count; i > 0; i--) {
    const char *label = context->control_stack[i - 1].continue_label;
    if (label) {
      return label;
    }
  }
  return NULL;
}

const char *ir_find_labeled_break(IRLoweringContext *context,
                                         const char *user_label) {
  if (!context || !user_label) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->break_label;
    }
  }
  return NULL;
}

const char *ir_find_labeled_continue(IRLoweringContext *context,
                                            const char *user_label) {
  if (!context || !user_label) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->continue_label;
    }
  }
  return NULL;
}

/* The label lookups above answer where the jump goes. These answer which
   frame owns it, which is what the deferred statements between here and there
   are measured against. The search rules match one for one: a bare `break`
   takes the innermost frame, a bare `continue` the innermost frame that has a
   continue label (a switch has none), and a labeled form the frame carrying
   that name. */
const IRControlFrame *ir_break_target_frame(IRLoweringContext *context,
                                            const char *user_label) {
  if (!context || context->control_count == 0) {
    return NULL;
  }
  if (!user_label) {
    return &context->control_stack[context->control_count - 1];
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->break_label ? frame : NULL;
    }
  }
  return NULL;
}

const IRControlFrame *ir_continue_target_frame(IRLoweringContext *context,
                                               const char *user_label) {
  if (!context || context->control_count == 0) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (user_label) {
      if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
        return frame->continue_label ? frame : NULL;
      }
      continue;
    }
    if (frame->continue_label) {
      return frame;
    }
  }
  return NULL;
}
