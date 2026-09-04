#include "ir_purity.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

IRFunction *ir_program_find_function(IRProgram *program, const char *name);
int ir_function_symbol_is_parameter(const IRFunction *function,
                                    const char *symbol_name);
int ir_instruction_writes_symbol(const IRInstruction *instruction);
const char *ir_function_local_declared_type(const IRFunction *function,
                                            const char *name);

static int purity_is_trap_call(const IRInstruction *inst) {
  return inst && inst->op == IR_OP_CALL && inst->text &&
         (strcmp(inst->text, "mettle_crash_trap_ex") == 0 ||
          strcmp(inst->text, "meth_runtime_debug_trap") == 0);
}

static int purity_writes_global(const IRFunction *function,
                                const IRInstruction *inst) {
  if (!ir_instruction_writes_symbol(inst) || !inst->dest.name) {
    return 0;
  }
  if (ir_function_symbol_is_parameter(function, inst->dest.name)) {
    return 0;
  }
  return ir_function_local_declared_type(function, inst->dest.name) == NULL;
}

static int purity_instruction_is_readonly(IRProgram *program,
                                          const IRFunction *function,
                                          const IRInstruction *inst) {
  if (purity_writes_global(function, inst)) {
    return 0;
  }
  switch (inst->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CAST:
  case IR_OP_RETURN:
    return 1;
  case IR_OP_CALL: {
    IRFunction *callee;
    if (purity_is_trap_call(inst)) {
      return 1;
    }
    callee = inst->text ? ir_program_find_function(program, inst->text) : NULL;
    return callee && callee->is_readonly_inferred;
  }
  default:
    return 0;
  }
}

static int purity_body_is_readonly(IRProgram *program,
                                   const IRFunction *function) {
  for (size_t k = 0; k < function->instruction_count; k++) {
    if (!purity_instruction_is_readonly(program, function,
                                        &function->instructions[k])) {
      return 0;
    }
  }
  return 1;
}

static int purity_divisor_is_nonzero_constant(const IRInstruction *inst) {
  if (!inst->text) {
    return 1;
  }
  if (strcmp(inst->text, "/") != 0 && strcmp(inst->text, "%") != 0) {
    return 1;
  }
  return inst->rhs.kind == IR_OPERAND_INT && inst->rhs.int_value != 0;
}

static int purity_label_defined_before(const IRFunction *function, size_t upto,
                                       const char *name) {
  if (!name) {
    return 1;
  }
  for (size_t k = 0; k < upto && k < function->instruction_count; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (inst->op == IR_OP_LABEL && inst->text &&
        strcmp(inst->text, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int purity_body_is_speculatable(IRProgram *program,
                                       const IRFunction *function) {
  for (size_t k = 0; k < function->instruction_count; k++) {
    const IRInstruction *inst = &function->instructions[k];
    switch (inst->op) {
    case IR_OP_LOAD:
      return 0;
    case IR_OP_BINARY:
      if (!purity_divisor_is_nonzero_constant(inst)) {
        return 0;
      }
      break;
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (purity_label_defined_before(function, k, inst->text)) {
        return 0;
      }
      break;
    case IR_OP_CALL: {
      IRFunction *callee =
          inst->text ? ir_program_find_function(program, inst->text) : NULL;
      if (!callee || !callee->is_speculatable_inferred) {
        return 0;
      }
      break;
    }
    default:
      break;
    }
  }
  return 1;
}

void ir_purity_infer(IRProgram *program) {
  int changed = 1;
  if (!program) {
    return;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn) {
      fn->is_readonly_inferred = 1;
      fn->is_speculatable_inferred = 1;
    }
  }
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < program->function_count; i++) {
      IRFunction *fn = program->functions[i];
      if (!fn || !fn->is_readonly_inferred) {
        continue;
      }
      if (!purity_body_is_readonly(program, fn)) {
        fn->is_readonly_inferred = 0;
        fn->is_speculatable_inferred = 0;
        changed = 1;
      }
    }
  }
  changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < program->function_count; i++) {
      IRFunction *fn = program->functions[i];
      if (!fn || !fn->is_speculatable_inferred) {
        continue;
      }
      if (!fn->is_readonly_inferred ||
          !purity_body_is_speculatable(program, fn)) {
        fn->is_speculatable_inferred = 0;
        changed = 1;
      }
    }
  }
}

const char *ir_purity_proof_name(const IRFunction *function) {
  if (!function) {
    return "no proof";
  }
  if (function->is_speculatable_inferred) {
    return "inferred speculatable";
  }
  if (function->is_readonly_inferred) {
    return "inferred read-only";
  }
  return "no proof";
}

static const IRInstruction *purity_first_impurity(IRProgram *program,
                                                  const IRFunction *function,
                                                  const char **what) {
  for (size_t k = 0; k < function->instruction_count; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (purity_instruction_is_readonly(program, function, inst)) {
      continue;
    }
    if (purity_writes_global(function, inst)) {
      *what = "writes a global";
      return inst;
    }
    switch (inst->op) {
    case IR_OP_STORE:
      *what = "writes through a pointer";
      return inst;
    case IR_OP_NEW:
      *what = "allocates";
      return inst;
    case IR_OP_INLINE_ASM:
      *what = "runs inline assembly";
      return inst;
    case IR_OP_CALL_INDIRECT:
      *what = "calls through a pointer";
      return inst;
    case IR_OP_CALL:
      *what = "calls a function that is not read-only";
      return inst;
    default:
      *what = "writes observable state";
      return inst;
    }
  }
  *what = "performs something the purity proof could not name";
  return NULL;
}

int ir_purity_check_contracts(IRProgram *program, ErrorReporter *reporter,
                              IRPurityStats *stats) {
  int ok = 1;
  IRPurityStats local;
  memset(&local, 0, sizeof(local));
  if (!program) {
    if (stats) {
      *stats = local;
    }
    return 1;
  }
  ir_purity_infer(program);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (!fn) {
      continue;
    }
    local.functions++;
    if (fn->is_readonly_inferred) {
      local.readonly++;
    }
    if (fn->is_speculatable_inferred) {
      local.speculatable++;
    }
    if (!fn->is_pure) {
      continue;
    }
    local.declared_pure++;
    if (fn->is_readonly_inferred) {
      local.declared_pure_proven++;
      continue;
    }
    ok = 0;
    if (reporter) {
      const char *what = NULL;
      const IRInstruction *site = purity_first_impurity(program, fn, &what);
      SourceSpan span = source_span_from_location(
          site ? site->location : fn->location, 1);
      SourceSpan decl = source_span_from_location(fn->location, 2);
      char message[256];
      char note[256];
      snprintf(message, sizeof(message), "`%s` is declared @pure but %s",
               fn->name ? fn->name : "?", what ? what : "is not pure");
      error_reporter_add_error_with_span(reporter, ERROR_SEMANTIC, span,
                                         message);
      error_reporter_set_last_label(reporter, what ? what : "not pure");
      error_reporter_set_last_code(reporter, "F0004");
      snprintf(note, sizeof(note), "`%s` carries @pure here",
               fn->name ? fn->name : "?");
      error_reporter_add_note_of_span(reporter, decl, note);
      error_reporter_add_note_of_span(
          reporter, span,
          "@pure is a contract the compiler checks and never believes; a "
          "call is hoisted only where the compiler proved purity on its own");
    }
  }
  if (stats) {
    *stats = local;
  }
  return ok;
}
