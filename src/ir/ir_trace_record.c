#include "ir_trace_record.h"
#include "ir_effects.h"
#include "../common.h"
#include <stdlib.h>
#include <string.h>

static const char *record_display_name(const char *name) {
  const char *p = name ? name : "?";
  while (strncmp(p, "__import_", 9) == 0) {
    const char *rest = p + 9;
    while (*rest && *rest != '_') {
      rest++;
    }
    if (*rest != '_') {
      break;
    }
    p = rest + 1;
  }
  return p;
}

static int emit_call(IRFunction *fn, size_t at, const char *helper,
                     const IROperand *arguments, size_t count,
                     SourceLocation location) {
  IRInstruction call = {0};
  int ok = 0;
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = (char *)helper;
  call.argument_count = count;
  call.arguments = calloc(count ? count : 1, sizeof(IROperand));
  if (!call.arguments) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    call.arguments[i] = arguments[i];
  }
  ok = ir_function_insert_instruction(fn, at, &call);
  free(call.arguments);
  return ok;
}

static int emit_event(IRFunction *fn, size_t at, const char *kind,
                      const char *name, const IROperand *value,
                      SourceLocation location) {
  IROperand arguments[6];
  int ok;
  arguments[0] = ir_operand_string(kind);
  arguments[1] = ir_operand_string(name);
  arguments[2] = ir_operand_string(location.filename ? location.filename : "");
  arguments[3] = ir_operand_int((long long)location.line);
  arguments[4] = ir_operand_int((long long)location.column);
  if (value) {
    arguments[5] = ir_operand_copy(value);
  } else {
    arguments[5] = ir_operand_int(0);
  }
  ok = emit_call(fn, at, "mettle_trace_event", arguments, 6, location);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&arguments[1]);
  ir_operand_destroy(&arguments[2]);
  ir_operand_destroy(&arguments[5]);
  return ok;
}

static const char *record_lock_kind(const char *called) {
  const char *name = record_display_name(called);
  /* Only an unconditional acquire counts. A `try` that fails took nothing,
   * so recording one as a lock would make every balanced run look unbalanced. */
  if (strcmp(name, "spin_lock") == 0 || strcmp(name, "mutex_lock") == 0 ||
      strcmp(name, "mutex_lock_infinite") == 0 ||
      strcmp(name, "pthread_mutex_lock") == 0 ||
      strcmp(name, "mettle_mutex_wait") == 0) {
    return "lock";
  }
  if (strcmp(name, "spin_unlock") == 0 || strcmp(name, "mutex_unlock") == 0 ||
      strcmp(name, "pthread_mutex_unlock") == 0 ||
      strcmp(name, "ReleaseMutex") == 0 ||
      strcmp(name, "mettle_mutex_release") == 0) {
    return "unlock";
  }
  return NULL;
}

static int record_is_helper(const char *name) {
  return name && strncmp(name, "mettle_trace_", 13) == 0;
}

static size_t record_entry_index(const IRFunction *fn) {
  size_t i = 0;
  while (i < fn->instruction_count &&
         (fn->instructions[i].op == IR_OP_LABEL ||
          fn->instructions[i].op == IR_OP_DECLARE_LOCAL)) {
    i++;
  }
  return i;
}

static int instrument_one(IRFunction *fn, size_t *sites) {
  const char *name = record_display_name(fn->name);
  int is_main = fn->name && strcmp(fn->name, "main") == 0;
  if (fn->instruction_count == 0 || fn->is_rule || fn->rewrite_role ||
      record_is_helper(fn->name)) {
    return 1;
  }
  for (size_t i = fn->instruction_count; i > 0; i--) {
    const IRInstruction *insn = &fn->instructions[i - 1];
    const char *lock = NULL;
    if (insn->op == IR_OP_NEW || insn->allocates ||
        (insn->op == IR_OP_CALL && insn->text &&
         ir_effects_name_is_allocator(insn->text))) {
      if (!emit_event(fn, i - 1, "alloc", name, NULL, insn->location)) {
        return 0;
      }
      (*sites)++;
      continue;
    }
    if (insn->op != IR_OP_CALL || !insn->text) {
      continue;
    }
    if (strcmp(record_display_name(insn->text), "free") == 0) {
      const IROperand *pointer =
          insn->argument_count > 0 ? &insn->arguments[0] : NULL;
      if (!emit_event(fn, i - 1, "free", "free", pointer, insn->location)) {
        return 0;
      }
      (*sites)++;
      continue;
    }
    lock = record_lock_kind(insn->text);
    if (lock) {
      const IROperand *held =
          insn->argument_count > 0 ? &insn->arguments[0] : NULL;
      if (!emit_event(fn, i - 1, lock, record_display_name(insn->text), held,
                      insn->location)) {
        return 0;
      }
      (*sites)++;
    }
  }
  for (size_t i = fn->instruction_count; i > 0; i--) {
    if (fn->instructions[i - 1].op != IR_OP_RETURN) {
      continue;
    }
    /* The flush goes after the leave in source order, which means inserting
     * it first: both land at the same index and the later insert pushes the
     * earlier one down. A program that leaves through `exit` rather than
     * returning from main writes whatever had already filled a buffer, which
     * docs/known-limitations.md says. */
    if (is_main &&
        !emit_call(fn, i - 1, "mettle_trace_flush", NULL, 0,
                   fn->instructions[i - 1].location)) {
      return 0;
    }
    if (!emit_call(fn, i - 1, "mettle_trace_leave", NULL, 0,
                   fn->instructions[i - 1].location)) {
      return 0;
    }
  }
  {
    IROperand arguments[4];
    size_t at = record_entry_index(fn);
    int ok;
    arguments[0] = ir_operand_string(name);
    arguments[1] =
        ir_operand_string(fn->location.filename ? fn->location.filename : "");
    arguments[2] = ir_operand_int((long long)fn->location.line);
    arguments[3] = ir_operand_int((long long)fn->location.column);
    ok = emit_call(fn, at, "mettle_trace_enter", arguments, 4, fn->location);
    ir_operand_destroy(&arguments[0]);
    ir_operand_destroy(&arguments[1]);
    if (!ok) {
      return 0;
    }
    (*sites)++;
  }
  return 1;
}

static void register_helper(IRProgram *program, const char *name,
                            size_t string_params, size_t int_params) {
  IRModuleSymbol entry;
  MtlcType *params[8];
  MtlcType *cstring = ir_program_lookup_type(program, "cstring");
  MtlcType *word = ir_program_lookup_type(program, "int64");
  size_t count = string_params + int_params;
  if (ir_program_lookup_symbol(program, name)) {
    return;
  }
  memset(&entry, 0, sizeof(entry));
  entry.name = (char *)name;
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = 1;
  entry.has_body = 0;
  entry.return_type = ir_program_lookup_type(program, "void");
  if (cstring && word && count > 0 && count <= 8) {
    for (size_t i = 0; i < count; i++) {
      params[i] = i < string_params ? cstring : word;
    }
    entry.param_types = params;
    entry.param_count = count;
  }
  ir_program_add_symbol(program, &entry);
}

int ir_trace_record_instrument(IRProgram *program, size_t *out_sites) {
  size_t sites = 0;
  if (!program) {
    return 1;
  }
  register_helper(program, "mettle_trace_enter", 2, 2);
  register_helper(program, "mettle_trace_leave", 0, 0);
  register_helper(program, "mettle_trace_event", 3, 3);
  register_helper(program, "mettle_trace_flush", 0, 0);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (!fn) {
      continue;
    }
    if (!instrument_one(fn, &sites)) {
      return 0;
    }
  }
  if (out_sites) {
    *out_sites = sites;
  }
  return 1;
}
