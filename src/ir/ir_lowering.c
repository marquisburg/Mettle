// AST->IR lowering: driver, context lifecycle, emit primitives.
#include "ir_lowering_internal.h"
#include "frontend/mtlc_lower_module.h" // backend module table population
#include "string_intern.h"

static void ir_lowering_free_control_stack(IRLoweringContext *context) {
  for (size_t j = 0; j < context->control_count; j++) {
    free(context->control_stack[j].break_label);
    free(context->control_stack[j].continue_label);
    free(context->control_stack[j].user_label);
  }
  free(context->control_stack);
  context->control_stack = NULL;
  context->control_count = 0;
}

// Tear down a partially-built program on any lowering failure and hand the
// pending error to the caller (or free it if the caller does not want one).
// Always returns NULL so callers can `return ir_lowering_fail(...)`.
static IRProgram *ir_lowering_fail(IRProgram *ir_program,
                                   IRLoweringContext *context,
                                   char **error_message) {
  ir_program_destroy(ir_program);
  ir_lowering_free_control_stack(context);
  ir_local_bindings_reset(context);
  if (error_message) {
    *error_message = context->error_message
                         ? context->error_message
                         : mettle_strdup("Unknown IR lowering error");
  } else {
    free(context->error_message);
  }
  return NULL;
}

static int ir_symbol_is_volatile_global(const IRProgram *program,
                                        const IROperand *operand) {
  const IRModuleSymbol *symbol;
  if (!operand || operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return 0;
  }
  symbol = ir_program_lookup_symbol(program, operand->name);
  return symbol && symbol->kind == IR_MODSYM_VARIABLE && symbol->is_volatile;
}

static void ir_mark_volatile_global_accesses(IRProgram *program) {
  size_t f;
  if (!program) {
    return;
  }
  for (f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    size_t i;
    if (!function) {
      continue;
    }
    for (i = 0; i < function->instruction_count; i++) {
      IRInstruction *instruction = &function->instructions[i];
      size_t a;
      int touches = ir_symbol_is_volatile_global(program, &instruction->dest) ||
                    ir_symbol_is_volatile_global(program, &instruction->lhs) ||
                    ir_symbol_is_volatile_global(program, &instruction->rhs);
      for (a = 0; !touches && a < instruction->argument_count; a++) {
        touches = ir_symbol_is_volatile_global(program,
                                               &instruction->arguments[a]);
      }
      if (!touches) {
        continue;
      }
      instruction->is_volatile = 1;
      function->has_volatile_access = 1;
    }
  }
}

IRProgram *ir_lower_program(ASTNode *program, TypeChecker *type_checker,
                            SymbolTable *symbol_table, char **error_message,
                            int emit_runtime_checks, int emit_safety_checks) {
  if (error_message) {
    *error_message = NULL;
  }

  if (!program || program->type != AST_PROGRAM) {
    if (error_message) {
      *error_message =
          mettle_strdup("Expected AST_PROGRAM root for IR lowering");
    }
    return NULL;
  }

  IRProgram *ir_program = ir_program_create();
  if (!ir_program) {
    if (error_message) {
      *error_message = mettle_strdup("Failed to allocate IR program");
    }
    return NULL;
  }

  IRLoweringContext context = {0};
  context.type_checker = type_checker;
  context.symbol_table = symbol_table;
  context.emit_runtime_checks = emit_runtime_checks ? 1 : 0;
  context.emit_safety_checks = emit_safety_checks ? 1 : 0;
  context.emit_refinement_checks = g_ir_lowering_refinement_checks;
  context.program = ir_program;

  Program *program_data = (Program *)program->data;
  if (!program_data) {
    return ir_program;
  }

  for (size_t i = 0; i < program_data->declaration_count; i++) {
    ASTNode *declaration = program_data->declarations[i];
    if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    FunctionDeclaration *function_data =
        (FunctionDeclaration *)declaration->data;
    if (!function_data) {
      ir_set_error(&context, "Malformed function declaration");
      return ir_lowering_fail(ir_program, &context, error_message);
    }
    if (!function_data->body) {
      continue;
    }

    IRFunction *function = ir_lower_function(&context, declaration);
    if (!function) {
      if (!context.error_message) {
        ir_set_error(&context, "Failed to lower function declaration to IR");
      }
      return ir_lowering_fail(ir_program, &context, error_message);
    }

    if (!ir_program_add_function(ir_program, function)) {
      ir_function_destroy(function);
      ir_set_error(&context, "Out of memory while appending IR function");
      return ir_lowering_fail(ir_program, &context, error_message);
    }
  }

  ir_lowering_free_control_stack(&context);
  ir_local_bindings_reset(&context);

  if (context.error_message) {
    if (error_message) {
      *error_message = context.error_message;
    } else {
      free(context.error_message);
    }
    return ir_program;
  }

  /* Bake the backend-owned type registry + module symbol table so the code
   * generators no longer consult the frontend TypeChecker/SymbolTable/AST. */
  mtlc_lower_populate_module(ir_program, program, type_checker, symbol_table);
  ir_mark_volatile_global_accesses(ir_program);

  return ir_program;
}

IRFunction *ir_lower_function(IRLoweringContext *context,
                                     ASTNode *declaration) {
  if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
    return NULL;
  }

  FunctionDeclaration *function_data = (FunctionDeclaration *)declaration->data;
  if (!function_data || !function_data->name) {
    ir_set_error(context, "Malformed function declaration");
    return NULL;
  }

  context->current_return_type_name = function_data->return_type;
  context->current_function_name = function_data->name;
  mettle_compiler_ctx_set_function_name(function_data->name);

  IRFunction *function = ir_function_create(function_data->name);
  if (!function) {
    ir_set_error(context, "Out of memory while creating IR function");
    return NULL;
  }
  function->location = declaration->location;
  if (function_data->return_type) {
    function->return_type_name =
        mettle_strdup(ir_backend_type_name(function_data->return_type));
  }
  function->is_inline = function_data->is_inline;
  function->is_inline_contract = function_data->is_inline_contract;
  function->is_swappable = function_data->is_swappable;
  function->is_naked = function_data->is_naked;
  function->is_interrupt = function_data->is_interrupt;
  function->is_exported = function_data->is_exported;
  /* The swap redirects a call, so the call must still exist at run time. This
   * is the cost the decorator buys, and it is why swappability is opt-in. */
  function->is_noinline = function_data->is_noinline ||
                          function_data->is_swappable ||
                          function_data->is_naked ||
                          function_data->is_interrupt;
  function->is_pure = function_data->is_pure;
  function->reference_twin =
      function_data->reference_twin
          ? string_intern(function_data->reference_twin)
          : NULL;
  function->explain_code = function_data->explain_code
                               ? string_intern(function_data->explain_code)
                               : NULL;
  function->explain_text = function_data->explain_text
                               ? string_intern(function_data->explain_text)
                               : NULL;
  function->is_noalloc = function_data->is_noalloc;
  ir_function_set_effects(function, IR_EFFECT_CLAUSE_WITH,
                          (const char *const *)function_data->effects_with,
                          function_data->effects_with_count);
  ir_function_set_effects(function, IR_EFFECT_CLAUSE_FORBIDS,
                          (const char *const *)function_data->effects_forbids,
                          function_data->effects_forbids_count);
  ir_function_set_effects(function, IR_EFFECT_CLAUSE_REQUIRES,
                          (const char *const *)function_data->effects_requires,
                          function_data->effects_requires_count);
  ir_function_set_effects(function, IR_EFFECT_CLAUSE_PROVIDES,
                          (const char *const *)function_data->effects_provides,
                          function_data->effects_provides_count);
  function->is_test = function_data->is_test;
  function->is_rule = function_data->is_rule;
  function->rewrite_role = function_data->rewrite_role;
  function->is_kernel = function_data->is_kernel;
  function->kernel_block[0] = function_data->kernel_block[0];
  function->kernel_block[1] = function_data->kernel_block[1];
  function->kernel_block[2] = function_data->kernel_block[2];
  function->kernel_threads_per_item = function_data->kernel_threads_per_item;
  /* A function-level `@simd` decorator becomes the default mode for every
   * counted loop in the body that has no `@simd` of its own. */
  context->current_function_simd_default = function_data->simd_mode;
  ir_local_bindings_reset(context);
  if (!ir_function_set_parameters(function,
                                  (const char **)function_data->parameter_names,
                                  (const char **)function_data->parameter_types,
                                  function_data->parameter_count)) {
    ir_set_error(context,
                 "Out of memory while recording IR function parameters");
    ir_function_destroy(function);
    return NULL;
  }
  for (size_t i = 0; i < function_data->parameter_count; i++) {
    if (!function_data->parameter_names[i]) {
      continue;
    }
    ir_local_bind_parameter(context, function_data->parameter_names[i],
                            function_data->parameter_types
                                ? ir_backend_type_name(
                                      function_data->parameter_types[i])
                                : NULL);
  }

  char *entry_label = ir_new_label_name(context, "entry");
  if (!entry_label) {
    ir_set_error(context,
                 "Out of memory while allocating function entry label");
    ir_function_destroy(function);
    return NULL;
  }
  if (!ir_emit_label_instruction(context, function, entry_label,
                                 declaration->location)) {
    free(entry_label);
    ir_function_destroy(function);
    return NULL;
  }
  free(entry_label);

  IRDeferScope defers = {0};
  if (function_data->body &&
      !ir_lower_statement_with_defers(context, function, function_data->body,
                                      &defers)) {
    ir_defer_stack_free(&defers.stack);
    ir_function_destroy(function);
    return NULL;
  }

  /* A `@naked` body ends where its asm block ends: there is no frame, so
   * there is nothing for a fall-off return or a defer to unwind, and the
   * scaffolding would emit compiled code into a function that has none. */
  if (!function_data->is_naked &&
      (function->instruction_count == 0 ||
       function->instructions[function->instruction_count - 1].op !=
           IR_OP_RETURN)) {
    IROperand implicit_value = ir_operand_none();
    if (!ir_emit_return_with_defers(context, function, &defers, &implicit_value,
                                    declaration->location)) {
      ir_operand_destroy(&implicit_value);
      ir_defer_stack_free(&defers.stack);
      ir_function_destroy(function);
      return NULL;
    }
    ir_operand_destroy(&implicit_value);
  }

  ir_defer_stack_free(&defers.stack);
  if (!ir_function_rebuild_cfg(function)) {
    ir_set_error(context, "Out of memory while building IR control-flow graph");
    ir_function_destroy(function);
    return NULL;
  }
  return function;
}

int g_ir_lowering_explain = 0;
int g_ir_lowering_refinement_checks = 0;

void ir_lowering_set_explain(int enabled) { g_ir_lowering_explain = enabled; }

void ir_lowering_set_refinement_checks(int enabled) {
  g_ir_lowering_refinement_checks = enabled;
}

void ir_set_error(IRLoweringContext *context, const char *format, ...) {
  if (!context || context->error_message || !format) {
    return;
  }

  if (context->current_function_name) {
    mettle_compiler_ctx_set_function_name(context->current_function_name);
  }
  mettle_compiler_ctx_set_phase(METTLE_COMPILER_PHASE_IR_LOWERING);

  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);

  if (needed > 0) {
    context->error_message = malloc((size_t)needed + 1);
    if (context->error_message) {
      vsnprintf(context->error_message, (size_t)needed + 1, format, args);
    }
  }
  va_end(args);
}

/* Lowering names a temp for most expressions it walks, and snprintf's format
 * interpreter is a poor way to spell one integer. Writes the digits backwards
 * into the tail of `buffer` and returns where the text starts. */
static char *ir_write_id(char *buffer, size_t size, int id) {
  char *at = buffer + size - 1;

  *at = '\0';
  if (id == 0) {
    *--at = '0';
    return at;
  }
  while (id > 0) {
    *--at = (char)('0' + id % 10);
    id /= 10;
  }
  return at;
}

char *ir_new_temp_name(IRLoweringContext *context) {
  char buffer[64];
  // The '.' prefix keeps temp names out of the user-identifier namespace.
  // Several backend tables (the MIR name->vreg map, float-bits marking) key on
  // the bare name, so a temp named "t2" would alias a user local named "t2" and
  // share its storage - a silent miscompile.
  char *digits = ir_write_id(buffer, sizeof(buffer), context->next_temp_id++);

  *--digits = 't';
  *--digits = '.';
  return mettle_strdup(digits);
}

char *ir_new_label_name(IRLoweringContext *context, const char *prefix) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "ir_%s_%d", prefix ? prefix : "label",
           context->next_label_id++);
  return mettle_strdup(buffer);
}

int ir_emit(IRLoweringContext *context, IRFunction *function,
                   const IRInstruction *instruction) {
  /* Single funnel for every instruction lowering produces, so the expansion
   * stamp cannot be forgotten at one of the call sites. */
  IRInstruction stamped;
  if (context && context->current_expansion_note && instruction &&
      !instruction->expansion_note) {
    stamped = *instruction;
    stamped.expansion_note = context->current_expansion_note;
    instruction = &stamped;
  }
  if (!ir_function_append_instruction(function, instruction)) {
    ir_set_error(context, "Out of memory while appending IR instruction");
    return 0;
  }
  return 1;
}

int ir_emit_jump_instruction(IRLoweringContext *context,
                                    IRFunction *function, const char *label,
                                    SourceLocation location) {
  if (!context || !function || !label) {
    return 0;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_JUMP;
  instruction.location = location;
  instruction.text = (char *)label;
  return ir_emit(context, function, &instruction);
}

int ir_emit_label_instruction(IRLoweringContext *context,
                                     IRFunction *function, const char *label,
                                     SourceLocation location) {
  if (!context || !function || !label) {
    return 0;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_LABEL;
  instruction.location = location;
  instruction.text = (char *)label;
  return ir_emit(context, function, &instruction);
}

// `@simd` loop markers. A marker is an IR_OP_NOP carrying a sentinel string in
// `text`: "@@simd:B:<id>:<mode>" before a vectorization-requested loop and
// "@@simd:E:<id>:0" after it. NOP is transparent to every recognizer (they skip
// NOPs) and a no-op in every backend, so the marker never disturbs codegen; the
// release-stage contract verifier (see ir_optimize) pairs B/E by id, checks
// whether a SIMD intrinsic landed between them, then clears the markers.
// `which` is 'B' or 'E'; `mode` is a SimdAttr.
int ir_emit_simd_marker(IRLoweringContext *context, IRFunction *function,
                               char which, int id, int mode,
                               SourceLocation location) {
  if (!context || !function) {
    return 0;
  }
  char buffer[48];
  snprintf(buffer, sizeof(buffer), IR_SIMD_MARKER_PREFIX "%c:%d:%d", which, id,
           mode);
  IRInstruction instruction = {0};
  instruction.op = IR_OP_NOP;
  instruction.location = location;
  instruction.text = buffer; // ir_emit deep-copies text
  return ir_emit(context, function, &instruction);
}

// `@unroll(n)` loop marker: one IR_OP_NOP carrying "@@unroll:<factor>" placed
// immediately before the loop's header label. The annotated-unroll pass finds
// the next label, unrolls that loop when its shape allows, and clears the
// marker either way.
int ir_emit_unroll_marker(IRLoweringContext *context, IRFunction *function,
                          int factor, SourceLocation location) {
  if (!context || !function) {
    return 0;
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), IR_UNROLL_MARKER_PREFIX "%d", factor);
  IRInstruction instruction = {0};
  instruction.op = IR_OP_NOP;
  instruction.location = location;
  instruction.text = buffer; // ir_emit deep-copies text
  return ir_emit(context, function, &instruction);
}

int ir_make_temp_operand(IRLoweringContext *context,
                                IROperand *out_temp) {
  char buffer[32];
  char *temp_name;

  if (!context || !out_temp) {
    return 0;
  }

  temp_name = ir_write_id(buffer, sizeof(buffer), context->next_temp_id++);
  *--temp_name = 't';
  *--temp_name = '.';
  *out_temp = ir_operand_temp(temp_name);
  if (out_temp->kind != IR_OPERAND_TEMP || !out_temp->name) {
    ir_set_error(context, "Failed to create IR temp operand");
    return 0;
  }

  return 1;
}
