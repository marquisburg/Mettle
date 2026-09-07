/* `--safe`: resolving the access marks lowering left behind.
 *
 * Every IR_OP_SAFETY_CHECK is either deleted, because the access provably
 * cannot leave its object, or rewritten into comparisons and safety intrinsics.
 * This follows scalar analysis and precedes vector recognition. */

#include "ir_optimize_internal.h"
#include "../ir_explain_safety.h"
#include "../ir_safety.h"
#include <time.h>

/* Distinct from lowering's ".t%d" temps and from every label prefix the
 * recognizers match on, so a resolved check can never be mistaken for one. */
#define SAFETY_TEMP_PREFIX ".safe"
#define SAFETY_LABEL_PREFIX "ir_safe_ok_"

static unsigned g_safety_next_id;

IRSafetyIntrinsic ir_safety_intrinsic(const IRInstruction *in) {
  if (!in) return IR_SAFETY_INTRINSIC_NONE;
  if (in->op == IR_OP_SAFETY_CHECK) return IR_SAFETY_INTRINSIC_CHECK;
  if (in->op != IR_OP_CALL || !in->text) return IR_SAFETY_INTRINSIC_NONE;
  static const struct {
    const char *name;
    size_t arguments;
    IRSafetyIntrinsic kind;
  } entries[] = {
      {"mettle_safety_check", 5, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_check_identity", 6, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_check_affine", 8, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_buffer_check", 5, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_identity", 1, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_span", 1, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_span_identity", 2, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_loop_length", 5, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_value_load", 3, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_merge_identity", 2, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_subtract_identity", 2, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_value_store", 4, IR_SAFETY_INTRINSIC_WRITE_ORIGIN},
      {"mettle_safety_value_copy", 3, IR_SAFETY_INTRINSIC_WRITE_ORIGIN},
      {"mettle_safety_value_clear", 2, IR_SAFETY_INTRINSIC_WRITE_ORIGIN},
      {"mettle_safety_register", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_register_static", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_unregister", 1, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_reregister", 3, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_free_identity", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_region_begin", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_region_end", 1, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_entry_arguments", 1, IR_SAFETY_INTRINSIC_LIFETIME},
  };
  for (size_t i = 0; i < IR_ARRAY_COUNT(entries); i++)
    if (in->argument_count == entries[i].arguments &&
        !strcmp(in->text, entries[i].name)) return entries[i].kind;
  return IR_SAFETY_INTRINSIC_NONE;
}

static int safety_env_flag(const char *name, int *cache) {
  if (*cache < 0) {
    const char *value = getenv(name);
    *cache = value && value[0] && value[0] != '0';
  }
  return *cache;
}

/* METTLE_SAFETY_TRACE=1 prints why a proof gave up. A check that survives is
 * either a real limit of the analysis or a shape it should have recognized,
 * and from the outside those look identical: both are just a check that is
 * still there. Same purpose as the backend's mir_call_trace. */
static int safety_trace_enabled(void) {
  static int state = -1;
  return safety_env_flag("METTLE_SAFETY_TRACE", &state);
}

/* METTLE_SAFETY_TIME=1 reports how long resolving took. Separate from the
 * trace because that prints a line per unproven access, which on a large input
 * costs far more than the work being measured. */
static int safety_time_enabled(void) {
  static int state = -1;
  return safety_env_flag("METTLE_SAFETY_TIME", &state);
}

static void safety_trace(const char *reason, size_t line) {
  if (safety_trace_enabled()) {
    fprintf(stderr, "safety: line %zu unproven: %s\n", line, reason);
  }
}

typedef struct {
  const IROperand *base;
  const IROperand *offset;
  const IROperand *identity;
  long long diagnostic_size;
  long long size;
  long long extent; /* IR_SAFETY_EXTENT_UNKNOWN when only the runtime knows */
  long long access_kind;
  const char *what;
  SourceLocation location;
} SafetyAccess;

/* Read a check's operands. Returns zero if the instruction is not shaped the
 * way lowering builds one, which leaves it to be copied through untouched
 * rather than silently mishandled. */
static int safety_read(const IRInstruction *instruction, SafetyAccess *access) {
  if ((instruction->argument_count != IR_SAFETY_ARG_COUNT &&
       instruction->argument_count != IR_SAFETY_TRACKED_ARG_COUNT &&
       instruction->argument_count != IR_SAFETY_ANALYZED_ARG_COUNT) ||
      !instruction->arguments) {
    return 0;
  }
  const IROperand *size = &instruction->arguments[IR_SAFETY_ARG_SIZE];
  const IROperand *extent = &instruction->arguments[IR_SAFETY_ARG_EXTENT];
  const IROperand *kind = &instruction->arguments[IR_SAFETY_ARG_ACCESS];
  if (size->kind != IR_OPERAND_INT || extent->kind != IR_OPERAND_INT ||
      kind->kind != IR_OPERAND_INT) {
    return 0;
  }

  access->base = &instruction->arguments[IR_SAFETY_ARG_BASE];
  access->offset = &instruction->arguments[IR_SAFETY_ARG_OFFSET];
  access->identity = instruction->argument_count >= IR_SAFETY_TRACKED_ARG_COUNT
      ? &instruction->arguments[IR_SAFETY_ARG_IDENTITY] : NULL;
  access->diagnostic_size = instruction->argument_count == IR_SAFETY_ANALYZED_ARG_COUNT
      ? instruction->arguments[IR_SAFETY_ARG_DIAGNOSTIC_SIZE].int_value : 0;
  access->size = size->int_value;
  access->extent = extent->int_value;
  access->access_kind = kind->int_value;
  access->what = instruction->text ? instruction->text : "?";
  access->location = instruction->location;
  return 1;
}

/* ---- emission helpers ------------------------------------------------------ */

static int safety_emit_binary(IRInstructionVector *out, SourceLocation location,
                              const char *op_text, const char *dest_temp,
                              const IROperand *lhs, const IROperand *rhs,
                              int is_unsigned) {
  IRInstruction insn = {0};
  insn.op = IR_OP_BINARY;
  insn.location = location;
  insn.text = mettle_strdup(op_text);
  insn.dest = ir_operand_temp(dest_temp);
  insn.is_unsigned = is_unsigned;
  if (!insn.text || !insn.dest.name || !ir_operand_clone(lhs, &insn.lhs) ||
      !ir_operand_clone(rhs, &insn.rhs) ||
      !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_branch_zero(IRInstructionVector *out,
                                   SourceLocation location,
                                   const char *condition_temp,
                                   const char *label) {
  IRInstruction insn = {0};
  insn.op = IR_OP_BRANCH_ZERO;
  insn.location = location;
  insn.text = mettle_strdup(label);
  insn.lhs = ir_operand_temp(condition_temp);
  if (!insn.text || !insn.lhs.name ||
      !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_label(IRInstructionVector *out, SourceLocation location,
                             const char *label) {
  IRInstruction insn = {0};
  insn.op = IR_OP_LABEL;
  insn.location = location;
  insn.text = mettle_strdup(label);
  if (!insn.text || !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

/* A call whose arguments are handed over by value. Each entry of `arguments`
 * is cloned, so the caller keeps ownership of what it passed in. */
static int safety_emit_call(IRInstructionVector *out, SourceLocation location,
                            const char *callee, const IROperand *arguments,
                            size_t count) {
  IRInstruction insn = {0};
  insn.op = IR_OP_CALL;
  insn.location = location;
  insn.text = mettle_strdup(callee);
  if (!insn.text) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  if (count > 0) {
    insn.arguments = calloc(count, sizeof(IROperand));
    if (!insn.arguments) {
      ir_instruction_destroy_storage(&insn);
      return 0;
    }
  }
  insn.argument_count = count;
  for (size_t i = 0; i < count; i++) {
    if (!ir_operand_clone(&arguments[i], &insn.arguments[i])) {
      ir_instruction_destroy_storage(&insn);
      return 0;
    }
  }
  if (!ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

/* A loop, and what it does to its index when that can be read off the header.
 *
 * The two halves are separate because they answer different questions. Where a
 * loop starts and ends is enough to resolve a pointer once and compare against
 * it, and every loop has that. What its index does is needed to argue about
 * which elements it reaches, and plenty of loops do not say: `while (child <=
 * end)` in a sift-down steps nothing this can read. Refusing to record such a
 * loop at all, which is what the first version did, denied the cheap
 * transformation to exactly the code that needed it most. */
typedef struct {
  size_t header_index;
  IRWhileLoopBounds bounds;
  int has_index;        /* the fields below mean anything */
  const char *iv;
  long long step;       /* constant, greater than zero */
  long long adjust;     /* highest index reached is `bound + adjust` */
  const IROperand *bound;
  size_t step_first;
  size_t step_last;
} SafetyLoopForm;

/* Every loop in the function, in source order, so scanning it backwards finds
 * the innermost one containing a given instruction first.
 *
 * Built once per function because it used to be rebuilt per check: each one
 * scanned backwards over every preceding instruction hunting for a header, and
 * parsed each candidate forwards. On one function holding eight thousand
 * accesses that cost several seconds on its own, and grew faster than the
 * input did. */
typedef struct {
  SafetyLoopForm *items;
  size_t count;
  size_t capacity;
} SafetyLoopList;

/* Innermost loop whose body holds `index`, or NULL. */
static const SafetyLoopForm *safety_enclosing_loop(const SafetyLoopList *loops,
                                                  size_t index);

/* ---- proving a check cannot fail ------------------------------------------- */
/*
 * Deleting a check is a claim that the access can never leave its object, and
 * a wrong claim is a miscompile that reads as a safe program. So each proof
 * below establishes the whole range of offsets the access can take and
 * compares it against the extent; anything it cannot pin down exactly returns
 * zero and the check survives. Being wrong in that direction only costs
 * speed.
 */

/* Fold an operand to a constant by walking back through what produced it.
 *
 * Needed because lowering scales every subscript through a multiply into a
 * fresh temp, so even `a[3]` reaches the check as a temp rather than as the
 * twelve it obviously is. The walk is deliberately shallow: this runs before
 * the optimizer's constant folding, and its job is to see through lowering's
 * own scaffolding, not to re-implement that pass. */
/* Globals the program never writes, with the value they were given.
 *
 * A global `var` nothing ever assigns is a constant in all but spelling, and
 * that is how a dimension is usually written: `var N: int32 = 32;`. Read as
 * what it is, a stride of N is a known stride, which is the difference between
 * a loop's checks folding into one and staying where they are. Gathered once
 * per program, since the answer is a property of the whole of it. */
typedef struct {
  char **names;      /* owned, sorted */
  long long *values;
  size_t count;
} SafetyConstGlobals;

static SafetyConstGlobals g_safety_const_globals;

static int safety_const_global_value(const char *name, long long *out) {
  size_t lo = 0;
  size_t hi = g_safety_const_globals.count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int order = strcmp(g_safety_const_globals.names[mid], name);
    if (order == 0) {
      *out = g_safety_const_globals.values[mid];
      return 1;
    }
    if (order < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return 0;
}

static int safety_constant_value(const IRFunction *function, size_t before,
                                 const IROperand *operand, int depth,
                                 long long *out) {
  if (operand->kind == IR_OPERAND_INT) {
    *out = operand->int_value;
    return 1;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    return safety_const_global_value(operand->name, out);
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name || depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, operand->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_ASSIGN) {
    return safety_constant_value(function, before, &producer->lhs, depth + 1,
                                 out);
  }
  /* A cast to a 64-bit integer cannot change what the value is, and the source
   * writes them: this pass runs before the optimizer, so `idx + (int64)N` still
   * carries the cast and a step of N would otherwise read as no step at all.
   * The narrower targets are not read through, since those can change it. */
  if (producer->op == IR_OP_CAST) {
    if (!producer->text) {
      return 0;
    }
    int to_signed = strcmp(producer->text, "int64") == 0;
    if (!to_signed && strcmp(producer->text, "uint64") != 0) {
      return 0;
    }
    long long inner = 0;
    if (!safety_constant_value(function, before, &producer->lhs, depth + 1,
                               &inner) ||
        (!to_signed && inner < 0)) {
      return 0;
    }
    *out = inner;
    return 1;
  }
  if (producer->op != IR_OP_BINARY || !producer->text) {
    return 0;
  }
  long long lhs = 0;
  long long rhs = 0;
  if (!safety_constant_value(function, before, &producer->lhs, depth + 1,
                             &lhs) ||
      !safety_constant_value(function, before, &producer->rhs, depth + 1,
                             &rhs)) {
    return 0;
  }
  /* Only the operators lowering uses to build an offset, and only where the
   * result cannot overflow into a different answer than the machine gives. */
  if (strcmp(producer->text, "*") == 0) {
    if (lhs != 0 && (lhs > INT32_MAX || lhs < INT32_MIN || rhs > INT32_MAX ||
                     rhs < INT32_MIN)) {
      return 0;
    }
    *out = lhs * rhs;
    return 1;
  }
  if (strcmp(producer->text, "+") == 0) {
    *out = lhs + rhs;
    return 1;
  }
  if (strcmp(producer->text, "-") == 0) {
    *out = lhs - rhs;
    return 1;
  }
  return 0;
}

/* The offset is a constant the compiler already holds. */
static int safety_prove_constant(const IRFunction *function, size_t check_index,
                                 const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN) {
    return 0;
  }
  long long offset = 0;
  if (!safety_constant_value(function, check_index, access->offset, 0,
                             &offset)) {
    return 0;
  }
  if (offset < 0 || access->size <= 0 || access->size > access->extent) {
    return 0;
  }
  return offset <= access->extent - access->size;
}

/* Read an index as `variable + constant`, which is how `a[i]` and `a[i + 2]`
 * both arrive. A bare variable is the same thing with a zero constant. The
 * decomposition itself is the shared affine one; the elision proofs here
 * reason about the variable's own bounds, so only the coeff == 1 slice of
 * the general `coeff * name + addend` form is wanted. */
static int safety_index_is_affine(const IRFunction *function, size_t before,
                                  const IROperand *index, const char **name_out,
                                  long long *addend_out) {
  const char *name = NULL;
  long long coeff = 0;
  long long addend = 0;
  if (!ir_affine_index_decompose(function, before, index, &name, &coeff,
                                 &addend) ||
      !name || coeff != 1) {
    return 0;
  }
  *name_out = name;
  *addend_out = addend;
  return 1;
}

/* The largest value an index can take, when that follows from the arithmetic
 * alone rather than from any loop.
 *
 * Masking is the case worth reading: `alpha[(bits >> 2) & 63]` cannot leave
 * [0, 63] whatever `bits` holds, because a non-negative mask clears every
 * higher bit including the sign. That is the shape of every table lookup, and
 * it bounds the access without knowing anything about the surrounding code. */
static int safety_index_upper_bound(const IRFunction *function, size_t before,
                                    const IROperand *index, int depth,
                                    long long *upper_out) {
  if (index->kind == IR_OPERAND_INT) {
    if (index->int_value < 0) {
      return 0;
    }
    *upper_out = index->int_value;
    return 1;
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name || depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, index->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_ASSIGN) {
    return safety_index_upper_bound(function, before, &producer->lhs, depth + 1,
                                    upper_out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text ||
      strcmp(producer->text, "&") != 0) {
    return 0;
  }
  if (producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value >= 0) {
    *upper_out = producer->rhs.int_value;
    return 1;
  }
  if (producer->lhs.kind == IR_OPERAND_INT && producer->lhs.int_value >= 0) {
    *upper_out = producer->lhs.int_value;
    return 1;
  }
  return 0;
}

/* Read the index out of the multiply lowering emits for a subscript, and
 * report the largest value it can take. */
static int safety_offset_scaling(const IRFunction *function, size_t check_index,
                                 const IROperand *offset,
                                 const IROperand **index_out,
                                 long long *stride_out);

static int safety_offset_upper_bound(const IRFunction *function,
                                     size_t check_index,
                                     const IROperand *offset,
                                     long long *stride_out,
                                     long long *upper_out) {
  const IROperand *index = NULL;
  return safety_offset_scaling(function, check_index, offset, &index, stride_out) &&
      safety_index_upper_bound(function, check_index, index, 0, upper_out);
}

/* Read `(iv + addend) * stride` out of the instruction that produced the
 * offset. This is the shape lowering emits for every subscript: the index,
 * then a multiply by the element width. */
/* Split `offset` into the index and the element width it is scaled by. The
 * index itself is left alone: what it is made of depends on which loop is
 * asking, so it is read separately, once per candidate. */
static int safety_offset_scaling(const IRFunction *function, size_t check_index,
                                 const IROperand *offset,
                                 const IROperand **index_out,
                                 long long *stride_out) {
  *index_out = offset;
  *stride_out = 1;
  if (offset->kind == IR_OPERAND_SYMBOL || offset->kind == IR_OPERAND_INT) return 1;
  if (offset->kind != IR_OPERAND_TEMP || !offset->name) return 0;
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, check_index, offset->name);
  if (!producer || producer->op != IR_OP_BINARY || producer->is_float ||
      !producer->text) {
    return 1;
  }
  if (strcmp(producer->text, "*") == 0 &&
      producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value > 0) {
    *index_out = &producer->lhs;
    *stride_out = producer->rhs.int_value;
    return 1;
  }
  /* Lowering scales a power-of-two element width with a shift as often as a
   * multiply, and the two mean the same thing here. */
  if (strcmp(producer->text, "<<") == 0 &&
      producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value >= 0 &&
      producer->rhs.int_value < 32) {
    *index_out = &producer->lhs;
    *stride_out = 1LL << producer->rhs.int_value;
    return 1;
  }
  /* A byte array scales by one, so lowering emits no scaling at all and the
   * offset IS the index. Reading it that way costs nothing on the wider
   * elements either: the coefficient the form comes back with is then in bytes
   * rather than elements, and every use of it below is in bytes anyway. */
  *index_out = offset;
  *stride_out = 1;
  return 1;
}

static int safety_offset_is_scaled_symbol(const IRFunction *function,
                                          size_t check_index,
                                          const IROperand *offset,
                                          const char **iv_out,
                                          long long *stride_out,
                                          long long *addend_out) {
  const IROperand *index = NULL;
  return safety_offset_scaling(function, check_index, offset, &index, stride_out) &&
         safety_index_is_affine(function, check_index, index, iv_out, addend_out);
}

/* Whether `symbol` is written anywhere in [start, end) outside the step's own
 * instructions. Used to confirm the only thing moving an induction variable
 * inside its loop is the step itself. */
static int safety_symbol_written_between(const IRFunction *function,
                                         size_t start, size_t end,
                                         const char *symbol, size_t step_first,
                                         size_t step_last) {
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    if (i >= step_first && i <= step_last) {
      continue;
    }
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        strcmp(instruction->dest.name, symbol) == 0) {
      return 1;
    }
  }
  return 0;
}

/* `<anything> = iv + <positive constant>`, the arithmetic half of a step.
 *
 * The constant is read through any widening the source wrote: this pass runs
 * before the optimizer, so `idx = idx + (int64)N` still has the cast in it and
 * a step of N reads as no step at all without looking past it. */
static int safety_read_step_add(const IRFunction *function, size_t before,
                                const IRInstruction *instruction,
                                const char *iv, long long *step_out) {
  if (!instruction || instruction->op != IR_OP_BINARY ||
      instruction->is_float || !instruction->text ||
      strcmp(instruction->text, "+") != 0 ||
      !ir_operand_is_symbol_named(&instruction->lhs, iv)) {
    return 0;
  }
  long long step = 0;
  if (!safety_constant_value(function, before, &instruction->rhs, 0, &step) ||
      step <= 0) {
    return 0;
  }
  *step_out = step;
  return 1;
}

/* Read how far the loop advances its index each iteration, and report which
 * instructions do it.
 *
 * The body is searched rather than just its last instruction, because a loop
 * often advances more than one counter and only one of them is the index this
 * access uses. Exactly one write to it is required, which is also what proves
 * nothing else in the body moves it.
 *
 * Two shapes, because this pass runs before the optimizer: lowering emits the
 * step as a pair, `t = i + 3` followed by `i = t`, and copy propagation folds
 * that into the single `i = i + 3` every recognizer downstream expects. Both
 * mean the same thing and both have to be read here. */
static int safety_loop_step(const IRFunction *function,
                            const IRWhileLoopBounds *loop, const char *iv,
                            long long *step_out, size_t *step_first,
                            size_t *step_last) {
  size_t write_index = 0;
  size_t write_count = 0;
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name && strcmp(instruction->dest.name, iv) == 0) {
      write_index = i;
      write_count++;
    }
  }
  if (write_count != 1) {
    return 0;
  }

  const IRInstruction *write = &function->instructions[write_index];
  if (safety_read_step_add(function, write_index, write, iv, step_out)) {
    *step_first = write_index;
    *step_last = write_index;
    return 1;
  }

  if (write->op != IR_OP_ASSIGN ||
      write->lhs.kind != IR_OPERAND_TEMP ||
      !write->lhs.name) {
    return 0;
  }
  const IRInstruction *add =
      ir_find_temp_producer_before(function, write_index, write->lhs.name);
  if (!add) {
    return 0;
  }
  size_t add_index = (size_t)(add - function->instructions);
  if (!safety_read_step_add(function, add_index, add, iv, step_out)) {
    return 0;
  }
  if (add_index <= loop->branch_index || add_index >= loop->jump_index) {
    return 0; /* the arithmetic is not in this body, so it is not the step */
  }
  *step_first = add_index;
  *step_last = write_index;
  return 1;
}

/* The check sits in a counted loop whose trip count and the object's extent
 * are both compile time constants, so the largest offset the loop can reach is
 * one too.
 *
 * Every condition here is load bearing. The variable has to start at zero and
 * only ever step by one, or the offsets it visits are not the range this
 * assumes; nothing else in the body may move it, or the increment is not the
 * whole story; and the bound has to be a constant, or there is no largest
 * offset to compare against. */
static int safety_prove_loop_bound(IRFunction *function,
                                   const SafetyLoopList *loops,
                                   size_t check_index,
                                   const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }

  const char *iv = NULL;
  long long stride = 0;
  long long addend = 0;
  if (!safety_offset_is_scaled_symbol(function, check_index, access->offset,
                                      &iv, &stride, &addend)) {
    safety_trace("the offset is not an index scaled by a constant width",
                 access->location.line);
    return 0;
  }
  if (addend < 0) {
    return 0; /* the loop's lower bound says nothing about a negative offset */
  }

  /* Innermost enclosing loop stepping this variable. Working outward matters:
   * a check in a nested loop is indexed by the inner variable, and the outer
   * loop's bound says nothing about it. */
  for (size_t i = loops->count; i-- > 0;) {
    const SafetyLoopForm *loop = &loops->items[i];
    if (check_index <= loop->bounds.branch_index ||
        check_index >= loop->bounds.jump_index) {
      continue; /* the check is not in this loop's body */
    }
    if (!loop->has_index) {
      continue; /* this loop's test says nothing about any index */
    }
    if (strcmp(loop->iv, iv) != 0) {
      continue; /* this loop steps a different variable; keep looking outward */
    }

    long long bound = 0;
    if (!safety_constant_value(function, loop->bounds.compare_index,
                               loop->bound, 0, &bound)) {
      safety_trace("the loop bound is not a constant", access->location.line);
      return 0;
    }
    /* `iv <op> bound` becomes `iv <= bound + adjust`, whichever way the test
     * was spelled. */
    long long highest_index = bound + loop->adjust;
    if (highest_index < 0) {
      return 1; /* the body never runs, so the access never happens */
    }

    if (!ir_iv_zero_at_header(function, loop->header_index, iv)) {
      safety_trace("the loop index does not start at zero",
                   access->location.line);
      return 0;
    }
    size_t step_first = 0;
    size_t step_last = 0;
    long long step = 0;
    if (!safety_loop_step(function, &loop->bounds, iv, &step, &step_first,
                          &step_last)) {
      safety_trace("the loop index does not step by a constant",
                   access->location.line);
      return 0;
    }
    if (safety_symbol_written_between(function, loop->bounds.branch_index + 1,
                                      loop->bounds.jump_index, iv, step_first,
                                      step_last)) {
      safety_trace("the loop index is assigned inside the body",
                   access->location.line);
      return 0;
    }

    if (highest_index > LLONG_MAX - addend || stride <= 0 ||
        highest_index + addend > LLONG_MAX / stride) return 0;
    long long highest = (highest_index + addend) * stride;
    if (highest < 0 || access->size > access->extent) {
      return 0;
    }
    if (highest <= access->extent - access->size) {
      return 1;
    }
    safety_trace("the loop can reach past the end of the object",
                 access->location.line);
    return 0;
  }

  safety_trace("no enclosing loop bounds this index", access->location.line);
  return 0;
}

/* The index is masked into a range the object already covers. */
static int safety_prove_masked_index(const IRFunction *function,
                                     size_t check_index,
                                     const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }
  long long stride = 0;
  long long upper = 0;
  if (!safety_offset_upper_bound(function, check_index, access->offset, &stride,
                                 &upper)) {
    return 0;
  }
  long long highest = upper * stride;
  if (highest < 0 || access->size > access->extent) {
    return 0;
  }
  return highest <= access->extent - access->size;
}

static int safety_prove(IRFunction *function, const SafetyLoopList *loops,
                        size_t check_index, const SafetyAccess *access) {
  return safety_prove_constant(function, check_index, access) ||
         safety_prove_loop_bound(function, loops, check_index, access) ||
         safety_prove_masked_index(function, check_index, access);
}

/* ---- hoisting a loop's checks into one ------------------------------------- */
/*
 * When the object's size is not known, a check per access means a call per
 * access, and in a tight loop that is the whole cost of the mode. But a
 * counted loop walking `base[i]` for i in [0, bound) touches one contiguous
 * range, and one check covers the lot. So the check moves out of the loop and
 * becomes a statement about the range, and the body is left with nothing in
 * it, which is also what lets the vectorizers claim it again.
 *
 * Correctness rests on the range being exactly what the loop touches, no more
 * and no less. More would trap on a correct program; less would miss a real
 * overrun. That is why the body has to be straight line (a conditional access
 * touches a subset, so checking the whole range could accuse a program that
 * never reads the far end) and why it must contain no calls (one of them could
 * free the block partway through, which a check taken beforehand would miss).
 */

typedef struct {
  size_t header_index; /* the loop label the check moves in front of */
  /* When the reach of the access follows from the arithmetic alone, as a
   * masked index does, the range is this many bytes and none of the loop
   * fields below are read. */
  long long constant_length;
  long long diagnostic_size;
  long long stride;        /* bytes per element */
  long long primary_step;  /* how far the tested variable moves each iteration */
  long long index_step;    /* how far the indexing variable moves */
  long long primary_start; /* the tested variable's first value */
  long long index_start;   /* the indexing variable's first value */
  long long adjust;        /* the tested variable tops out at bound + adjust */
  long long coeff;         /* index = coeff * counter + invariant + constant */
  long long constant;
  long long size; /* bytes the access touches */
  long long access_kind;
  IROperand base;  /* owned */
  IROperand bound; /* owned */
  const IROperand *identity; /* borrowed from the retained check */
  /* The runtime term the index is displaced by, as it was written in the loop.
   * Re-read rather than referenced, because the expression that computed it
   * often lives inside the body and has to be worked out again in front of the
   * header; `function` and `bounds` are what that re-reading needs. */
  IROperand invariant; /* owned; kind NONE when absent */
  int has_invariant;
  /* Where the indexing counter began, when that is not a constant: the row
   * offset a blocked matrix multiply starts each inner loop from. Added to
   * index_start rather than replacing it. */
  IROperand index_start_value; /* owned; kind NONE when absent */
  int has_index_start;
  /* Where to start looking for what computed the displacement. It is the
   * access's own position, not the header's: the expression usually lives
   * inside the body, and searching back from the header would find some
   * earlier definition of the same temp, or none. */
  size_t invariant_at;
  const IRFunction *function; /* borrowed */
  IRWhileLoopBounds bounds;
  SourceLocation location;
} SafetyHoist;


/* Read the loop's test and step.
 *
 * The test is `index <op> bound` for `<` or `<=`, where the index may carry a
 * constant of its own: `while (i + 3 <= len)` is how a loop consuming three
 * bytes at a time says where it stops. Each spelling gives a different highest
 * index, and getting that wrong by one is the difference between checking what
 * the loop touches and checking a byte past it. */
static int safety_parse_loop_form(const IRFunction *function,
                                  size_t header_index, SafetyLoopForm *form) {
  if (header_index + 4 >= function->instruction_count) {
    return 0;
  }
  const IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 0;
  }

  /* Find the exit test, then work back to what computed it. A test that needs
   * arithmetic of its own, as `i + 3 <= len` does, puts that arithmetic
   * between the header and the compare, so counting instructions forward from
   * the header finds the wrong one. */
  size_t branch_index = 0;
  int found_branch = 0;
  for (size_t i = header_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_BRANCH_ZERO) {
      branch_index = i;
      found_branch = 1;
      break;
    }
    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_JUMP ||
        instruction->op == IR_OP_BRANCH_EQ) {
      return 0;
    }
  }
  if (!found_branch) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[branch_index];
  if (!branch->text || branch->lhs.kind != IR_OPERAND_TEMP ||
      !branch->lhs.name) {
    return 0;
  }
  const IRInstruction *compare =
      ir_find_temp_producer_before(function, branch_index, branch->lhs.name);
  if (!compare || compare->op != IR_OP_BINARY || compare->is_float ||
      !compare->text) {
    return 0;
  }
  size_t compare_index = (size_t)(compare - function->instructions);
  if (compare_index <= header_index) {
    return 0;
  }

  form->header_index = header_index;
  form->bounds.compare_index = compare_index;
  form->bounds.branch_index = branch_index;
  form->bounds.loop_label = header->text;
  form->bounds.exit_label = branch->text;
  form->bounds.jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_JUMP && instruction->text &&
        strcmp(instruction->text, form->bounds.loop_label) == 0) {
      form->bounds.jump_index = i;
      break;
    }
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, form->bounds.exit_label) == 0) {
      break;
    }
  }
  if (form->bounds.jump_index == (size_t)-1) {
    return 0;
  }

  /* Where the loop runs is settled. Whether its test also says what the index
   * does is a separate question, and a loop that does not say is still a loop
   * worth knowing about. */
  form->has_index = 0;
  long long index_addend = 0;
  if (safety_index_is_affine(function, compare_index, &compare->lhs, &form->iv,
                             &index_addend)) {
    if (strcmp(compare->text, "<") == 0) {
      form->adjust = -index_addend - 1;
      form->has_index = 1;
    } else if (strcmp(compare->text, "<=") == 0) {
      form->adjust = -index_addend;
      form->has_index = 1;
    }
  }
  form->bound = &compare->rhs;
  return 1;
}

/* An operand whose value cannot change across [start, end). */
static int safety_operand_invariant_in(const IRFunction *function, size_t start,
                                       size_t end, const IROperand *operand) {
  if (operand->kind == IR_OPERAND_INT) {
    return 1;
  }
  if ((operand->kind != IR_OPERAND_SYMBOL && operand->kind != IR_OPERAND_TEMP) ||
      !operand->name) {
    return 0;
  }
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.name &&
        strcmp(instruction->dest.name, operand->name) == 0) {
      return 0;
    }
  }
  return 1;
}

/* ---- what a call in the body can do to the memory ------------------------- *
 *
 * A hoisted check speaks for a range of bytes before the loop runs, and a span
 * resolved once speaks for an allocation's extent across the whole loop.
 * Neither survives something freeing the block partway through, and a call is
 * the only thing in a body that can do that.
 *
 * Refusing every body with a call in it was the easy reading of that, and it
 * gives up far too much: a loop around a helper is one of the commonest shapes
 * there is, and a helper that computes cannot take memory away from anyone. So
 * the question asked is whether this callee, or anything it reaches, can
 * release memory -- not whether the body has a call in it.
 *
 * The whole program is here, so this is answered rather than assumed. What is
 * refused is what cannot be answered: a callee with no body in this program, a
 * call through a pointer, a launch, and inline assembly. A write to anything
 * that is not the callee's own local is refused too, since the pointer the
 * loop walks, its bound and its counter may all be reachable that way. */

/* The program being resolved, for reading what a callee does. Set for the
 * duration of one resolve; the pass is not reentrant. */
static const IRProgram *g_safety_program;

static int safety_callee_can_release(const char *name, int depth);

/* C library entry points that cannot release the caller's memory.
 *
 * Reading, writing, comparing and computing are all any of these do. The list
 * is deliberately short and deliberately only standard names: a function this
 * program declares extern for itself stays refused, because its contract is
 * not something the compiler knows. */
static int safety_extern_cannot_release(const char *name) {
  static const char *const known[] = {
      "memcmp",  "memchr", "memcpy", "memmove", "memset",  "strlen",
      "strcmp",  "strncmp", "strchr", "strrchr", "strstr",  "strcpy",
      "strncpy", "strcat",  "abs",    "labs",    "llabs",
      "sin",     "cos",     "tan",    "asin",    "acos",    "atan",
      "atan2",   "sinh",    "cosh",   "tanh",    "exp",     "exp2",
      "log",     "log2",    "log10",  "pow",     "sqrt",    "cbrt",
      "fabs",    "floor",   "ceil",   "round",   "trunc",   "fmod",
      "fmin",    "fmax",    "sinf",   "cosf",    "tanf",    "expf",
      "logf",    "powf",    "sqrtf",  "fabsf",   "floorf",  "ceilf",
      "roundf",  "truncf",  "fmodf"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (strcmp(name, known[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Entry points that take memory away. Both spellings, since --native-heap
 * rewrites one set to the other and this runs on whichever survived. */
static int safety_name_releases_memory(const char *callee) {
  return strcmp(callee, "free") == 0 || strcmp(callee, "realloc") == 0 ||
         strcmp(callee, "mettle_heap_free") == 0 ||
         strcmp(callee, "mettle_heap_realloc") == 0;
}

static const IRFunction *safety_find_function(const char *name) {
  if (!g_safety_program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < g_safety_program->function_count; i++) {
    const IRFunction *function = g_safety_program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }
  return NULL;
}

/* Is this name one of the function's own parameters or locals? A write to
 * anything else outlives the call, and could be the very pointer the check was
 * taken against. */
static int safety_name_is_functions_own(const IRFunction *function,
                                        const char *name) {
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names && function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Verdicts, so a helper called from several loops is read once. Cleared with
 * the rest of the per-program state. */
enum { SAFETY_CALLEE_CACHE_MAX = 512 };
typedef struct {
  const char *name; /* borrowed from the IR */
  int verdict;
  int visiting;
} SafetyCalleeVerdict;
static SafetyCalleeVerdict g_safety_callee_cache[SAFETY_CALLEE_CACHE_MAX];
static int g_safety_callee_cache_count;

static SafetyCalleeVerdict *safety_callee_slot(const char *name) {
  for (int i = 0; i < g_safety_callee_cache_count; i++) {
    if (strcmp(g_safety_callee_cache[i].name, name) == 0) {
      return &g_safety_callee_cache[i];
    }
  }
  if (g_safety_callee_cache_count >= SAFETY_CALLEE_CACHE_MAX) {
    return NULL;
  }
  SafetyCalleeVerdict *slot =
      &g_safety_callee_cache[g_safety_callee_cache_count++];
  slot->name = name;
  slot->verdict = -1;
  slot->visiting = 0;
  return slot;
}

static int safety_function_can_release(const IRFunction *function, int depth) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    switch (in->op) {
    case IR_OP_CALL_INDIRECT:
    case IR_OP_GPU_LAUNCH:
    case IR_OP_INLINE_ASM:
      return 1;
    case IR_OP_CALL:
      if (!in->text || safety_callee_can_release(in->text, depth + 1)) {
        return 1;
      }
      break;
    default:
      break;
    }
    /* Allocation is not release: taking a new block leaves every live one
     * where it was. Reallocation is, and it is a call, so it is caught above. */
    if (ir_instruction_writes_destination(in) &&
        in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        !safety_name_is_functions_own(function, in->dest.name)) {
      return 1;
    }
  }
  return 0;
}

static int safety_callee_can_release(const char *name, int depth) {
  if (!name || depth > 16) {
    return 1;
  }
  /* Only known checking and shadow operations preserve allocation lifetime.
   * Region end and unregister calls must invalidate a lifted check. The table
   * is keyed by arity as well as name, so the probe covers every arity any
   * entry uses; anything else under the prefix is call metadata, which moves
   * no allocation and must not cost a loop its hoisted check. */
  IRInstruction probe = {0};
  probe.op = IR_OP_CALL;
  probe.text = (char *)name;
  for (size_t arity = 1; arity <= 8; arity++) {
    probe.argument_count = arity;
    IRSafetyIntrinsic kind = ir_safety_intrinsic(&probe);
    if (kind != IR_SAFETY_INTRINSIC_NONE)
      return kind == IR_SAFETY_INTRINSIC_LIFETIME;
  }
  if (strncmp(name, "mettle_safety_", 14) == 0) {
    return 0;
  }
  if (safety_name_releases_memory(name)) {
    return 1;
  }
  const IRFunction *callee = safety_find_function(name);
  if (!callee) {
    /* No body here to read. That is the end of it for a function this program
     * declares, but not for the handful the C library defines: their contracts
     * say what they do, and none of them can take memory away from the caller.
     * A loop around memcmp or sqrt is common enough that refusing it was most
     * of what the call restriction still cost. Looked up only after the
     * program's own functions, so a definition here always wins. */
    return !safety_extern_cannot_release(name);
  }
  SafetyCalleeVerdict *slot = safety_callee_slot(name);
  if (!slot) {
    return 1;
  }
  if (slot->verdict >= 0) {
    return slot->verdict;
  }
  if (slot->visiting) {
    /* A cycle. Assuming the back edge releases nothing is safe: releasing is
     * found by reaching a free, and every function in the cycle is still read
     * in full by the walk that is already in progress. */
    return 0;
  }
  slot->visiting = 1;
  int verdict = safety_function_can_release(callee, depth);
  slot->visiting = 0;
  slot->verdict = verdict;
  return verdict;
}

/* Whether the address of this symbol is taken anywhere in the function.
 *
 * A callee cannot reach one of the caller's locals otherwise, which is what
 * lets a call in the body be judged on what it frees alone. If the loop handed
 * out the address of its own bound or counter, a callee could move it, and the
 * range worked out before the loop would stop describing what the loop walks.
 * The callee analysis refuses writes to anything that is not the callee's own,
 * so a global cannot be moved that way either; this covers the rest. */
static int safety_symbol_escapes(const IRFunction *function, const char *name) {
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name && strcmp(in->lhs.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int safety_operand_escapes(const IRFunction *function,
                                  const IROperand *operand) {
  return operand && operand->kind == IR_OPERAND_SYMBOL &&
         safety_symbol_escapes(function, operand->name);
}

/* Does the body call out at all? Asked only to decide whether the escape
 * question above has to be asked; a body with no call in it cannot hand
 * anything to anyone. */
static int safety_body_calls_out(const IRFunction *function,
                                 const IRWhileLoopBounds *loop) {
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if ((in->op == IR_OP_CALL &&
         (!in->text || strncmp(in->text, "mettle_safety_", 14) != 0)) ||
        in->op == IR_OP_CALL_INDIRECT || in->op == IR_OP_NEW) {
      return 1;
    }
  }
  return 0;
}

/* Whether this instruction, sitting in a loop body, can take away the memory
 * the loop walks. */
static int safety_body_instruction_can_release(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_CALL_INDIRECT:
  case IR_OP_GPU_LAUNCH:
  case IR_OP_INLINE_ASM:
    return 1;
  case IR_OP_CALL:
    return !in->text || safety_callee_can_release(in->text, 0);
  default:
    return 0;
  }
}

/* Nothing in the body can release the memory the loop is walking. Weaker than
 * requiring a straight line, deliberately: to reuse one resolved allocation
 * across many accesses it only matters that the allocation outlives them, not
 * that every access happens. Branches are fine, because each access still
 * carries its own comparison. */
static int safety_body_has_no_calls(const IRFunction *function,
                                    const IRWhileLoopBounds *loop) {
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    if (safety_body_instruction_can_release(&function->instructions[i])) {
      return 0;
    }
  }
  return 1;
}

/* Whether `label` is defined inside the loop's body.
 *
 * A branch that lands inside the body is internal shape; one that lands
 * anywhere else leaves the loop, and the trip count then stops being the thing
 * the header test says it is. The header and exit labels are deliberately not
 * body labels: a jump to either is a `continue` or a `break`, and a `break`
 * is exactly what makes a whole-range check claim iterations that never ran. */
static int safety_label_is_in_body(const IRFunction *function,
                                   const IRWhileLoopBounds *loop,
                                   const char *label) {
  if (!label) {
    return 0;
  }
  for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, label) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Every iteration the loop begins runs the access being hoisted, and nothing
 * in the body can release the memory it walks.
 *
 * The body may branch, as long as every branch stays inside it. An `if/else`
 * that rejoins does not change how many times the loop runs, so a check
 * covering the range the header test describes still describes exactly what
 * the loop will touch. What must be refused is anything that cuts the loop
 * short -- a `break`, a `return`, a jump to the exit label -- because then the
 * later elements are never reached and a whole-range check would accuse a
 * program that did nothing wrong.
 *
 * `access_index` must therefore dominate the body: it has to sit ahead of the
 * first branch, so that reaching the top of an iteration means performing it.
 * An access buried inside an `if` runs on some iterations and not others, and
 * one check for the whole range would over-claim in the same way. */
static int safety_body_runs_access_every_iteration(
    const IRFunction *function, const IRWhileLoopBounds *loop,
    size_t access_index) {
  int seen_branch = 0;
  for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    switch (instruction->op) {
    case IR_OP_RETURN:
      return 0;
    case IR_OP_LABEL:
      seen_branch = 1;
      continue;
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!safety_label_is_in_body(function, loop, instruction->text)) {
        return 0;
      }
      seen_branch = 1;
      continue;
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_GPU_LAUNCH:
    case IR_OP_INLINE_ASM:
      /* A call is fine where nothing it reaches can take the memory away. A
       * loop around a helper that computes is one of the commonest shapes
       * there is, and refusing it left the mode's worst cases exactly there. */
      if (safety_body_instruction_can_release(instruction)) {
        return 0;
      }
      continue;
    default:
      continue;
    }
  }
  /* The access has to be reached before the body's control flow can skip it.
   * Scanning for the first branch and comparing indices says the same thing
   * more cheaply than a dominance computation, and errs the safe way. */
  if (seen_branch) {
    for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
      IROpcode op = function->instructions[i].op;
      if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
          op == IR_OP_BRANCH_EQ) {
        return access_index < i;
      }
    }
  }
  return 1;
}

/* ---- reading an index as a line ------------------------------------------- *
 *
 * Most indices that are not a bare counter are still a straight line in one:
 * `mat[base + j]`, `b[j * N + i]`, `src[n - 1 - i]`. Each moves by a fixed
 * amount per iteration, so each touches one contiguous range, and one check
 * covers the range exactly as it does for `a[i]`.
 *
 * The form is `coeff * varying + invariant + constant`. Splitting it needs a
 * loop to be relative to, since the same expression is a counter in one loop
 * and a fixed value in the one outside it, so the reading happens once per
 * candidate loop rather than once per access. */

/* Does the body write this symbol? The induction variable does, by its step,
 * which is what makes it the varying one. */
static int safety_symbol_written_in_body(const IRFunction *function,
                                         const IRWhileLoopBounds *loop,
                                         const char *symbol) {
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        strcmp(instruction->dest.name, symbol) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Whether the value this operand names differs from one iteration to the next.
 * A temp settled before the loop cannot; one computed inside it varies exactly
 * when what it was computed from does. Anything this cannot read is treated as
 * varying, which only costs a hoist. */
static int safety_value_varies(const IRFunction *function, size_t before,
                               const IRWhileLoopBounds *loop,
                               const IROperand *operand, int depth) {
  if (!operand || depth > 8) {
    return 1;
  }
  if (operand->kind == IR_OPERAND_INT) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    return safety_symbol_written_in_body(function, loop, operand->name);
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 1;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, operand->name);
  if (!producer) {
    return 1;
  }
  size_t at = (size_t)(producer - function->instructions);
  if (at <= loop->branch_index) {
    return 0; /* computed before the loop began */
  }
  if (producer->op != IR_OP_BINARY && producer->op != IR_OP_ASSIGN &&
      producer->op != IR_OP_CAST) {
    return 1;
  }
  return safety_value_varies(function, at, loop, &producer->lhs, depth + 1) ||
         (producer->rhs.kind != IR_OPERAND_NONE &&
          safety_value_varies(function, at, loop, &producer->rhs, depth + 1));
}

/* Whether a loop-invariant value can be named in front of the header: either it
 * already is one, or it is a small expression over values that are, which the
 * check can simply compute again there. `mat[i * dim + j]` needs the second
 * form, since `i * dim` is worked out inside the inner loop and the temp
 * holding it does not exist before the header. */
static int safety_invariant_available(const IRFunction *function, size_t before,
                                      size_t header,
                                      const IRWhileLoopBounds *loop,
                                      const IROperand *operand, int depth) {
  if (!operand || depth > 6) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_INT) {
    return 1;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    return safety_operand_invariant_in(function, header, loop->jump_index,
                                       operand);
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, operand->name);
  if (!producer) {
    return 0;
  }
  size_t at = (size_t)(producer - function->instructions);
  if (at < header) {
    return 1; /* already settled where the check will go */
  }
  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
    return safety_invariant_available(function, at, header, loop,
                                      &producer->lhs, depth + 1);
  }
  if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text) {
    return 0;
  }
  if (strcmp(producer->text, "+") != 0 && strcmp(producer->text, "-") != 0 &&
      strcmp(producer->text, "*") != 0 && strcmp(producer->text, "<<") != 0) {
    return 0;
  }
  return safety_invariant_available(function, at, header, loop, &producer->lhs,
                                    depth + 1) &&
         safety_invariant_available(function, at, header, loop, &producer->rhs,
                                    depth + 1);
}

/* `index = coeff * varying + invariant + constant`, read relative to one loop.
 * `varying` is NULL when the whole index holds still, which is worth hoisting
 * too: the same element checked once instead of once per iteration. */
typedef struct {
  long long coeff;
  const char *varying;        /* borrowed from the IR */
  const IROperand *invariant; /* borrowed from the IR; NULL when absent */
  long long constant;
} SafetyIndexForm;

static int safety_index_form_merge(SafetyIndexForm *into,
                                   const SafetyIndexForm *add, long long sign) {
  if (add->varying) {
    if (into->varying) {
      if (strcmp(into->varying, add->varying) != 0) {
        return 0; /* two moving parts is not a line in one of them */
      }
      into->coeff += sign * add->coeff;
    } else {
      into->varying = add->varying;
      into->coeff = sign * add->coeff;
    }
  }
  if (add->invariant) {
    if (into->invariant) {
      return 0; /* only one runtime term is carried into the check */
    }
    if (sign < 0) {
      return 0; /* a subtracted runtime term would need its own negation */
    }
    into->invariant = add->invariant;
  }
  into->constant += sign * add->constant;
  return 1;
}

static int safety_read_index_form(const IRFunction *function, size_t before,
                                  size_t header, const IRWhileLoopBounds *loop,
                                  const IROperand *index, int depth,
                                  SafetyIndexForm *out) {
  if (!index || depth > 6) {
    return 0;
  }
  memset(out, 0, sizeof(*out));

  if (index->kind == IR_OPERAND_INT) {
    out->constant = index->int_value;
    return 1;
  }
  /* A subtree that holds still is one term, whatever its shape. That is what
   * keeps `i * dim` together instead of trying to make a line out of it. */
  if (!safety_value_varies(function, before, loop, index, 0)) {
    if (!safety_invariant_available(function, before, header, loop, index, 0)) {
      return 0;
    }
    out->invariant = index;
    return 1;
  }
  if (index->kind == IR_OPERAND_SYMBOL && index->name) {
    out->varying = index->name;
    out->coeff = 1;
    return 1;
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, index->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  size_t at = (size_t)(producer - function->instructions);
  if (producer->op == IR_OP_ASSIGN) {
    return safety_read_index_form(function, at, header, loop, &producer->lhs,
                                  depth + 1, out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text) {
    return 0;
  }

  SafetyIndexForm lhs;
  SafetyIndexForm rhs;
  if (!safety_read_index_form(function, at, header, loop, &producer->lhs,
                              depth + 1, &lhs) ||
      !safety_read_index_form(function, at, header, loop, &producer->rhs,
                              depth + 1, &rhs)) {
    return 0;
  }

  if (strcmp(producer->text, "+") == 0) {
    *out = lhs;
    return safety_index_form_merge(out, &rhs, 1);
  }
  if (strcmp(producer->text, "-") == 0) {
    *out = lhs;
    return safety_index_form_merge(out, &rhs, -1);
  }
  /* Scaling, but only by something the compiler knows: a runtime factor on the
   * moving part would make the distance between iterations a runtime value the
   * length below cannot be written in terms of. */
  const SafetyIndexForm *scaled = NULL;
  long long factor = 0;
  if (strcmp(producer->text, "*") == 0) {
    if (!lhs.varying && !lhs.invariant) {
      scaled = &rhs;
      factor = lhs.constant;
    } else if (!rhs.varying && !rhs.invariant) {
      scaled = &lhs;
      factor = rhs.constant;
    }
  } else if (strcmp(producer->text, "<<") == 0 && !rhs.varying &&
             !rhs.invariant && rhs.constant >= 0 && rhs.constant < 32) {
    scaled = &lhs;
    factor = 1LL << rhs.constant;
  }
  if (!scaled || (scaled->invariant && factor != 1)) {
    return 0; /* a scaled runtime term has no place to go in the form */
  }
  out->varying = scaled->varying;
  out->coeff = scaled->coeff * factor;
  out->invariant = scaled->invariant;
  out->constant = scaled->constant * factor;
  return 1;
}

/* The value a counter holds when the loop is entered. Like
 * ir_iv_zero_at_header, but reports the constant rather than insisting it is
 * zero: a scan starting at one covers `[1, n)`, which is as describable a range
 * as `[0, n)` and was being turned down only because the code asked the
 * narrower question. */
static int safety_iv_start_at_header(const IRFunction *function,
                                     size_t header_index,
                                     const IRWhileLoopBounds *loop,
                                     const char *iv, long long *start_out,
                                     const IROperand **start_operand_out) {
  *start_out = 0;
  *start_operand_out = NULL;
  for (size_t i = header_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
      return 0; /* another path reaches the loop; the value is not settled */
    }
    if (!ir_instruction_writes_destination(ins) ||
        !ir_operand_is_symbol_named(&ins->dest, iv)) {
      continue;
    }
    /* `idx = (int64)col` widens straight into the counter, so the initializer
     * is as often a cast as an assignment. Only the 64-bit targets are read
     * through: a narrower one can change the value being started from. */
    if (ins->op == IR_OP_CAST) {
      if (ins->is_float || !ins->text ||
          (strcmp(ins->text, "int64") != 0 &&
           strcmp(ins->text, "uint64") != 0)) {
        return 0;
      }
    } else if (ins->op != IR_OP_ASSIGN) {
      return 0;
    }
    if (ins->lhs.kind == IR_OPERAND_INT) {
      *start_out = ins->lhs.int_value;
      return 1;
    }
    /* A counter can start somewhere the compiler does not know, as the row
     * offsets of a blocked matrix multiply do, and the range is still exactly
     * describable: it just begins at a value the check works out rather than
     * one written into it. */
    const IROperand *source = &ins->lhs;
    if (source->kind == IR_OPERAND_TEMP && source->name) {
      const IRInstruction *p =
          ir_find_temp_producer_before(function, i, source->name);
      if (p && (p->op == IR_OP_CAST || p->op == IR_OP_ASSIGN) && !p->is_float) {
        if (p->lhs.kind == IR_OPERAND_INT) {
          *start_out = p->lhs.int_value;
          return 1;
        }
        source = &p->lhs;
      }
    }
    if (safety_invariant_available(function, i, header_index, loop, source, 0)) {
      *start_operand_out = source;
      return 1;
    }
    return 0;
  }
  return 0;
}

static int safety_hoist_masked(IRFunction *function,
                               const SafetyLoopList *loops,
                               size_t check_index,
                               const SafetyAccess *access, long long stride,
                               long long upper, SafetyHoist *out) {
  const SafetyLoopForm *innermost = safety_enclosing_loop(loops, check_index);
  if (!innermost) {
    safety_trace("the index is bounded but there is no loop to lift the "
                 "check out of",
                 access->location.line);
    return 0;
  }
  long long start = 0;
  const IROperand *start_operand = NULL;
  if (!innermost->has_index ||
      !safety_iv_start_at_header(function, innermost->header_index,
          &innermost->bounds, innermost->iv, &start, &start_operand) ||
      start_operand ||
      (innermost->adjust < 0 && start > LLONG_MAX + innermost->adjust) ||
      (innermost->adjust > 0 && start < LLONG_MIN + innermost->adjust) ||
      stride <= 0 || upper < 0 ||
      upper > (LLONG_MAX - access->size) / stride ||
      !safety_operand_invariant_in(function, innermost->header_index,
          innermost->bounds.jump_index, innermost->bound) ||
      !safety_body_runs_access_every_iteration(function, &innermost->bounds,
                                               check_index) ||
      !safety_operand_invariant_in(function, innermost->header_index,
                                   innermost->bounds.jump_index,
                                   access->base)) {
    return 0;
  }
  out->header_index = innermost->header_index;
  out->primary_start = start;
  out->adjust = innermost->adjust;
  if (!ir_operand_clone(innermost->bound, &out->bound)) return 0;
  out->constant_length = upper * stride + access->size;
  out->access_kind = access->access_kind;
  out->location = access->location;
  return ir_operand_clone(access->base, &out->base);
}

static int safety_try_hoist(IRFunction *function, const SafetyLoopList *loops,
                            size_t check_index, const SafetyAccess *access,
                            SafetyHoist *out) {
  if (access->extent != IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }
  const SafetyLoopForm *identity_loop = safety_enclosing_loop(loops, check_index);
  if (access->identity && (!identity_loop ||
      !safety_operand_invariant_in(function, identity_loop->header_index,
          identity_loop->bounds.jump_index, access->identity))) return 0;
  out->identity = access->identity;
  out->diagnostic_size = access->diagnostic_size;

  /* An index the arithmetic already bounds, such as a masked table lookup,
   * reaches the same range on every iteration. One check for that range stands
   * in for all of them, and its length is a constant. */
  long long masked_stride = 0;
  long long masked_upper = 0;
  int masked = safety_offset_upper_bound(function, check_index, access->offset,
                                         &masked_stride, &masked_upper);

  /* The offset is the index scaled by the element width. Only the scaling has
   * to be read here; what the index itself is made of is read once per
   * candidate loop below, since the answer depends on which loop is asking. */
  const IROperand *index = NULL;
  long long stride = 0;
  if (!masked && !safety_offset_scaling(function, check_index, access->offset,
                                        &index, &stride)) {
    safety_trace("the offset is not an index scaled by a constant width",
                 access->location.line);
    return 0;
  }

  if (masked) {
    return safety_hoist_masked(function, loops, check_index, access,
                               masked_stride, masked_upper, out);
  }

  int saw_loop = 0;
  for (size_t loop_index = loops->count; loop_index-- > 0;) {
    SafetyLoopForm form = loops->items[loop_index];
    size_t header = form.header_index;
    if (check_index <= form.bounds.branch_index ||
        check_index >= form.bounds.jump_index || !form.has_index) {
      continue;
    }
    saw_loop = 1;

    /* What the index is made of, as this loop sees it. */
    SafetyIndexForm shape;
    if (!safety_read_index_form(function, check_index, header, &form.bounds,
                                index, 0, &shape)) {
      safety_trace("the index is neither a line in one counter nor bounded by "
                   "its own arithmetic",
                   access->location.line);
      return 0;
    }
    if (!shape.varying || shape.coeff == 0) {
      continue; /* nothing this loop does moves it; ask the loop outside */
    }
    /* Keep the original access when a newly exposed descending range cannot
     * describe its first failing element with the ascending range intrinsic. */
    if (access->diagnostic_size && shape.coeff < 0) return 0;

    /* The variable the loop tests, and the one this access indexes by, need
     * not be the same. A loop reading three bytes and writing four advances
     * two counters; the test bounds one of them, and the other is pinned to it
     * by both starting where it starts and stepping by a constant, so after
     * the same number of iterations each has travelled its own step times the
     * count. */
    size_t primary_first = 0;
    size_t primary_last = 0;
    long long primary_start = 0;
    const IROperand *primary_start_operand = NULL;
    if (!safety_iv_start_at_header(function, header, &form.bounds, form.iv,
                                   &primary_start, &primary_start_operand) ||
        primary_start_operand ||
        !safety_loop_step(function, &form.bounds, form.iv, &form.step,
                          &primary_first, &primary_last)) {
      safety_trace("the tested variable is not a counter that starts at a "
                   "known value and steps by a constant",
                   access->location.line);
      return 0;
    }

    long long index_step = form.step;
    long long index_start = primary_start;
    const IROperand *index_start_operand = NULL;
    if (strcmp(form.iv, shape.varying) != 0) {
      size_t index_first = 0;
      size_t index_last = 0;
      if (!safety_iv_start_at_header(function, header, &form.bounds,
                                     shape.varying, &index_start,
                                     &index_start_operand)) {
        safety_trace("the indexing variable does not start from a value "
                     "settled before the loop",
                     access->location.line);
        return 0;
      }
      if (!safety_loop_step(function, &form.bounds, shape.varying, &index_step,
                            &index_first, &index_last)) {
        safety_trace("the indexing variable does not step by a constant",
                     access->location.line);
        return 0;
      }
    }

    if (!safety_body_runs_access_every_iteration(function, &form.bounds,
                                                 check_index)) {
      safety_trace("the loop body can skip this access or leave early, or "
                   "calls something that could free what it walks",
                   access->location.line);
      return 0;
    }
    /* A body that calls out may have handed the loop's own pointer, bound or
     * counter to the callee, and a check worked out before the loop cannot
     * survive any of the three being moved. */
    if (safety_body_calls_out(function, &form.bounds) &&
        (safety_operand_escapes(function, access->base) ||
         safety_operand_escapes(function, form.bound) ||
         safety_symbol_escapes(function, form.iv) ||
         safety_symbol_escapes(function, shape.varying))) {
      safety_trace("the loop hands out the address of its pointer, bound or "
                   "counter, so a call in the body could move it",
                   access->location.line);
      return 0;
    }
    /* The hoisted check is emitted in front of the header, so both the pointer
     * and the bound have to be settled by then. Scanning from the header
     * rather than from the body is what makes that true of the bound: a test
     * like `while (i < rows * cols)` computes it between the header and the
     * compare, and a check placed in front of the header would name a value
     * that does not exist yet. */
    if (!safety_operand_invariant_in(function, header, form.bounds.jump_index,
                                     access->base) ||
        (access->identity && !safety_operand_invariant_in(function, header,
            form.bounds.jump_index, access->identity)) ||
        !safety_operand_invariant_in(function, header, form.bounds.jump_index,
                                     form.bound)) {
      safety_trace("the pointer or the loop bound is not settled before the "
                   "loop starts",
                   access->location.line);
      return 0;
    }

    out->header_index = header;
    /* The runtime span calculation receives positive, representable steps.
     * Reject an unrepresentable affine coefficient before host arithmetic. */
    if (shape.coeff == LLONG_MIN || stride <= 0 || index_step <= 0 ||
        (form.adjust < 0 && primary_start > LLONG_MAX + form.adjust) ||
        (form.adjust > 0 && primary_start < LLONG_MIN + form.adjust)) return 0;
    unsigned long long reach = (unsigned long long)(shape.coeff < 0 ?
                                                    -shape.coeff : shape.coeff);
    if (reach > (unsigned long long)LLONG_MAX / (unsigned long long)stride ||
        reach * (unsigned long long)stride >
            (unsigned long long)LLONG_MAX / (unsigned long long)index_step) return 0;
    out->stride = stride;
    out->primary_step = form.step;
    out->index_step = index_step;
    out->primary_start = primary_start;
    out->index_start = index_start;
    out->adjust = form.adjust;
    out->coeff = shape.coeff;
    out->constant = shape.constant;
    out->size = access->size;
    out->access_kind = access->access_kind;
    out->location = access->location;
    out->function = function;
    out->bounds = form.bounds;
    out->has_invariant = 0;
    out->has_index_start = 0;
    out->invariant_at = check_index;
    if (!ir_operand_clone(access->base, &out->base)) {
      return 0;
    }
    if (!ir_operand_clone(form.bound, &out->bound)) {
      ir_operand_destroy(&out->base);
      return 0;
    }
    if (shape.invariant) {
      if (!ir_operand_clone(shape.invariant, &out->invariant)) {
        ir_operand_destroy(&out->base);
        ir_operand_destroy(&out->bound);
        return 0;
      }
      out->has_invariant = 1;
    }
    if (index_start_operand) {
      if (!ir_operand_clone(index_start_operand, &out->index_start_value)) {
        ir_operand_destroy(&out->base);
        ir_operand_destroy(&out->bound);
        if (out->has_invariant) {
          ir_operand_destroy(&out->invariant);
        }
        return 0;
      }
      out->has_index_start = 1;
    }
    return 1;
  }
  safety_trace(saw_loop ? "the enclosing loop does not step this index"
                        : "no enclosing loop this pass can read",
               access->location.line);
  return 0;
}

/* ---- building the check's arithmetic -------------------------------------- *
 *
 * A small scratchpad, because the range now takes a dozen instructions in the
 * worst case and threading the failure and the ownership of each temp through
 * by hand was the bulk of the previous version. Every temp handed out is kept
 * and released together; a failure anywhere is remembered and makes every
 * later call a no-op, so the sequence reads as arithmetic rather than as error
 * handling. */
enum { SAFETY_BUILD_MAX = 32 };

typedef struct {
  IRInstructionVector *out;
  SourceLocation location;
  unsigned id;
  int seq;
  int failed;
  IROperand owned[SAFETY_BUILD_MAX];
  int owned_count;
  /* Handed back once something has gone wrong, so the arithmetic below can go
   * on being written as arithmetic without a null check per step. */
  IROperand nowhere;
} SafetyBuild;

static void safety_build_init(SafetyBuild *b, IRInstructionVector *out,
                              SourceLocation location) {
  memset(b, 0, sizeof(*b));
  b->out = out;
  b->location = location;
  b->id = g_safety_next_id++;
}

static void safety_build_release(SafetyBuild *b) {
  for (int i = 0; i < b->owned_count; i++) {
    ir_operand_destroy(&b->owned[i]);
  }
  b->owned_count = 0;
}

/* A named operand needs its name; a literal has none and needs none. Asking
 * for a name unconditionally rejected every constant leaf, which is most of
 * them: the `256` in `i * 256`. */
static int safety_operand_is_usable(const IROperand *operand) {
  if (operand->kind == IR_OPERAND_INT || operand->kind == IR_OPERAND_FLOAT) {
    return 1;
  }
  return (operand->kind == IR_OPERAND_TEMP ||
          operand->kind == IR_OPERAND_SYMBOL) &&
         operand->name != NULL;
}

/* Park an operand this builder owns, and hand back a stable pointer to it. */
static const IROperand *safety_build_keep(SafetyBuild *b, IROperand *value) {
  if (b->failed || b->owned_count >= SAFETY_BUILD_MAX ||
      !safety_operand_is_usable(value)) {
    b->failed = 1;
    ir_operand_destroy(value);
    return &b->nowhere;
  }
  b->owned[b->owned_count] = *value;
  return &b->owned[b->owned_count++];
}

/* `fresh = lhs <op> rhs`. Returns a pointer that stays valid until release. */
static const IROperand *safety_build(SafetyBuild *b, const char *op,
                                     const IROperand *lhs,
                                     const IROperand *rhs) {
  if (b->failed) {
    return lhs;
  }
  char name[64];
  snprintf(name, sizeof(name), SAFETY_TEMP_PREFIX "h%u_%d", b->id, b->seq++);
  if (!safety_emit_binary(b->out, b->location, op, name, lhs, rhs, 0)) {
    b->failed = 1;
    return lhs;
  }
  IROperand fresh = ir_operand_temp(name);
  return safety_build_keep(b, &fresh);
}

/* Name a loop-invariant value in front of the header, working it out again
 * there when the expression that produced it lives inside the body. Mirrors
 * safety_invariant_available, which already decided this would succeed. */
static const IROperand *safety_build_invariant(SafetyBuild *b,
                                               const IRFunction *function,
                                               size_t before, size_t header,
                                               const IRWhileLoopBounds *loop,
                                               const IROperand *operand,
                                               int depth) {
  if (b->failed || depth > 6) {
    b->failed = 1;
    return operand;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    const IRInstruction *producer =
        ir_find_temp_producer_before(function, before, operand->name);
    if (producer) {
      size_t at = (size_t)(producer - function->instructions);
      if (at >= header) {
        if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
          return safety_build_invariant(b, function, at, header, loop,
                                        &producer->lhs, depth + 1);
        }
        if (producer->op == IR_OP_BINARY && producer->text) {
          const IROperand *lhs = safety_build_invariant(
              b, function, at, header, loop, &producer->lhs, depth + 1);
          const IROperand *rhs = safety_build_invariant(
              b, function, at, header, loop, &producer->rhs, depth + 1);
          return safety_build(b, producer->text, lhs, rhs);
        }
        b->failed = 1;
        return operand;
      }
    }
  }
  IROperand copy;
  if (!ir_operand_clone(operand, &copy)) {
    b->failed = 1;
    return operand;
  }
  return safety_build_keep(b, &copy);
}

/* Emit the one check that stands in for all of the loop's:
 *
 *   top     = bound + adjust            highest value the test allows
 *   span    = top - start               how far the counter travels
 *   travel  = (span / step) * index_step   how far the index travels with it
 *   length  = travel * |coeff| * stride + size    the bytes it covers
 *   runs    = span >= 0                 zero when the loop never runs at all
 *   check(base, low * stride, length * runs)
 *
 * Rounding down to a multiple of the step matters once the step is more than
 * one: a loop counting by three stops at the largest multiple of three below
 * its bound, and using the bound itself would check up to two bytes the loop
 * never reads. On an exactly sized buffer those two bytes are the difference
 * between silence and accusing a correct program. The division is skipped
 * where the step is one, which is most loops.
 *
 * Rounding down to a multiple of the step matters once the step is more than
 * one: a loop counting by three stops at the largest multiple of three below
 * its bound, and using the bound itself would check up to two bytes the loop
 * never reads. On an exactly sized buffer those two bytes are the difference
 * between silence and accusing a correct program. The division is skipped
 * where the step is one, which is most loops.
 *
 * Multiplying by `runs` rather than branching around the check is what keeps
 * this free: a label immediately before a loop header stops the recognizers'
 * backward scan for the induction variable's initial value, so a guard branch
 * here would cost the loop its vectorization, which is most of what hoisting
 * was for. */
static int safety_emit_hoisted(IRInstructionVector *out,
                               const SafetyHoist *hoist) {
  /* A range the arithmetic already settled needs no arithmetic of its own. */
  if (hoist->constant_length > 0) {
    SafetyBuild build;
    safety_build_init(&build, out, hoist->location);
    IROperand threshold = ir_operand_int(hoist->primary_start - hoist->adjust);
    IROperand length = ir_operand_int(hoist->constant_length);
    const IROperand *runs = safety_build(&build, ">=", &hoist->bound, &threshold);
    const IROperand *guarded = safety_build(&build, "*", runs, &length);
    IROperand arguments[8];
    if (build.failed || !ir_operand_clone(&hoist->base, &arguments[0])) {
      safety_build_release(&build);
      return 0;
    }
    arguments[1] = ir_operand_int(0);
    arguments[2] = *guarded;
    arguments[3] = ir_operand_int(hoist->access_kind);
    arguments[4] = ir_operand_int((long long)hoist->location.line);
    if (hoist->identity) arguments[5] = *hoist->identity;
    int emitted = safety_emit_call(out, hoist->location,
        hoist->identity ? "mettle_safety_check_identity" : "mettle_safety_check",
        arguments, hoist->identity ? 6 : 5);
    ir_operand_destroy(&arguments[0]);
    safety_build_release(&build);
    return emitted;
  }

  SafetyBuild build;
  safety_build_init(&build, out, hoist->location);

  IROperand adjust = ir_operand_int(hoist->adjust);
  IROperand primary_start = ir_operand_int(hoist->primary_start);
  IROperand primary_step = ir_operand_int(hoist->primary_step);
  IROperand index_step = ir_operand_int(hoist->index_step);
  IROperand stride = ir_operand_int(hoist->stride);
  IROperand size = ir_operand_int(hoist->size);
  /* The highest value the test allows, and how far that is from where the
   * counter began. */
  const IROperand *top =
      safety_build(&build, "+", &hoist->bound, &adjust);
  const IROperand *span = top;
  if (hoist->primary_start != 0) {
    span = safety_build(&build, "-", top, &primary_start);
  }

  /* Iterations less one, and from that how far the indexing counter travels.
   * Dividing is what pins two counters together; where the tested variable
   * steps by one it is already the count. */
  const IROperand *rounds = span;
  if (hoist->primary_step > 1) {
    rounds = safety_build(&build, "/", span, &primary_step);
  }
  const IROperand *travel = rounds;
  if (hoist->index_step != 1) {
    travel = safety_build(&build, "*", rounds, &index_step);
  }

  /* Bytes from the first element the loop touches to one past the last. The
   * displacement drops out of it, since both ends carry the same one, and so
   * does where the counter started. */
  long long reach = hoist->coeff < 0 ? -hoist->coeff : hoist->coeff;
  IROperand per_step = ir_operand_int(reach * hoist->stride);
  IROperand length_args[5] = {
      hoist->bound, ir_operand_int(hoist->primary_start - hoist->adjust),
      primary_step, ir_operand_int(per_step.int_value * hoist->index_step), size};
  if (!safety_emit_call(out, hoist->location, "mettle_safety_loop_length",
                        length_args, 5)) {
    safety_build_release(&build);
    return 0;
  }
  char length_name[64];
  snprintf(length_name, sizeof(length_name), SAFETY_TEMP_PREFIX "length%u", g_safety_next_id++);
  out->items[out->count - 1].dest = ir_operand_temp(length_name);
  if (!out->items[out->count - 1].dest.name) {
    safety_build_release(&build);
    return 0;
  }
  IROperand guarded_length = ir_operand_temp(length_name);
  const IROperand *guarded = safety_build_keep(&build, &guarded_length);

  /* Where the counter stands at the low end of the range. A positive
   * coefficient puts that at the first iteration; a negative one puts it at
   * the last, so the travel has to be walked back to reach it. */
  IROperand index_start = ir_operand_int(hoist->index_start);
  const IROperand *counter = &index_start;
  if (hoist->has_index_start) {
    const IROperand *from = safety_build_invariant(
        &build, hoist->function, hoist->invariant_at, hoist->header_index,
        &hoist->bounds, &hoist->index_start_value, 0);
    counter = hoist->index_start != 0
                  ? safety_build(&build, "+", from, &index_start)
                  : from;
  }
  if (hoist->coeff < 0) {
    counter = safety_build(&build, "+", counter, travel);
  }

  /* And the index there: the counter scaled, displaced, and shifted by
   * whatever constant the expression carried. */
  const IROperand *low = counter;
  if (hoist->coeff != 1) {
    IROperand coeff = ir_operand_int(hoist->coeff);
    low = safety_build(&build, "*", counter, &coeff);
  }
  if (hoist->constant != 0) {
    IROperand constant = ir_operand_int(hoist->constant);
    low = safety_build(&build, "+", low, &constant);
  }
  if (hoist->has_invariant) {
    const IROperand *displacement = safety_build_invariant(
        &build, hoist->function, hoist->invariant_at, hoist->header_index,
        &hoist->bounds, &hoist->invariant, 0);
    low = safety_build(&build, "+", low, displacement);
  }
  const IROperand *offset = low;
  if (hoist->stride != 1) {
    offset = safety_build(&build, "*", low, &stride);
  }

  int ok = 0;
  if (!build.failed) {
    IROperand arguments[8];
    if (ir_operand_clone(&hoist->base, &arguments[0])) {
      arguments[1] = *offset;
      arguments[2] = *guarded;
      arguments[3] = ir_operand_int(hoist->access_kind);
      arguments[4] = ir_operand_int((long long)hoist->location.line);
      if (hoist->identity) arguments[5] = *hoist->identity;
      if (hoist->diagnostic_size && hoist->identity) {
        arguments[6] = ir_operand_int(hoist->diagnostic_size);
        arguments[7] = ir_operand_int(per_step.int_value * hoist->index_step);
        ok = safety_emit_call(out, hoist->location,
            "mettle_safety_check_affine", arguments, 8);
      } else {
        ok = safety_emit_call(out, hoist->location,
            hoist->identity ? "mettle_safety_check_identity" : "mettle_safety_check",
            arguments, hoist->identity ? 6 : 5);
      }
      ir_operand_destroy(&arguments[0]);
    }
  }
  safety_build_release(&build);
  return ok;
}

/* ---- the two survivor shapes ----------------------------------------------- */

/* The object's size is a compile time constant, so the whole check is one
 * unsigned comparison.
 *
 * Comparing without sign is what lets a single test cover both ends: a
 * negative offset reads as an enormous unsigned value and fails the same
 * comparison an oversized one does. The alternative, a signed `offset <
 * extent`, waves every negative index straight through.
 *
 * The trap arm converts the byte offset back into an element index so the
 * message speaks in the units the programmer wrote. It sits after the branch,
 * so that division costs nothing on the path that stays in bounds. */
static int safety_expand_extent(IRInstructionVector *out,
                                const SafetyAccess *access) {
  char ok_label[64];
  char condition[64];
  char index_temp[64];
  unsigned id = g_safety_next_id++;
  snprintf(ok_label, sizeof(ok_label), SAFETY_LABEL_PREFIX "%u", id);
  snprintf(condition, sizeof(condition), SAFETY_TEMP_PREFIX "c%u", id);
  snprintf(index_temp, sizeof(index_temp), SAFETY_TEMP_PREFIX "i%u", id);

  char message[192];
  snprintf(message, sizeof(message), "Fatal error: `%s` is outside its bounds",
           access->what);

  /* An access wider than the whole object can never fit. Emitting the
   * comparison would underflow the limit into a huge unsigned bound and let it
   * pass, so trap outright. */
  if (access->size > access->extent) {
    IROperand arguments[4];
    arguments[0] = ir_operand_int(2);
    arguments[1] = ir_operand_string(message);
    arguments[2] = ir_operand_int(0);
    arguments[3] = ir_operand_int(0);
    int ok = safety_emit_call(out, access->location, "mettle_crash_trap_ex",
                              arguments, 4);
    for (size_t i = 0; i < 4; i++) {
      ir_operand_destroy(&arguments[i]);
    }
    return ok;
  }

  IROperand limit = ir_operand_int(access->extent - access->size);
  int emitted = safety_emit_binary(out, access->location, ">", condition,
                                   access->offset, &limit, 1) &&
                safety_emit_branch_zero(out, access->location, condition,
                                        ok_label);
  ir_operand_destroy(&limit);
  if (!emitted) {
    return 0;
  }

  IROperand element_size = ir_operand_int(access->size);
  int trapped = safety_emit_binary(out, access->location, "/", index_temp,
                                   access->offset, &element_size, 0);
  ir_operand_destroy(&element_size);
  if (!trapped) {
    return 0;
  }

  IROperand arguments[4];
  arguments[0] = ir_operand_int(2); /* METTLE_CRASH_TRAP_ARRAY_BOUNDS */
  arguments[1] = ir_operand_string(message);
  arguments[2] = ir_operand_temp(index_temp);
  arguments[3] = ir_operand_int(access->extent / access->size);
  int ok = arguments[1].kind == IR_OPERAND_STRING && arguments[2].name &&
           safety_emit_call(out, access->location, "mettle_crash_trap_ex",
                            arguments, 4);
  for (size_t i = 0; i < 4; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  if (!ok) {
    return 0;
  }

  return safety_emit_label(out, access->location, ok_label);
}

/* Only the runtime knows how large the allocation behind this pointer is, so
 * hand it the base, the displacement and the width and let the shadow map
 * answer. The base rather than the final address is what carries provenance:
 * it is the allocation the pointer came from that bounds the access, not
 * whichever one the computed address happens to land in. */
static int safety_expand_region(IRInstructionVector *out,
                                const SafetyAccess *access) {
  IROperand arguments[6];
  size_t built = 0;
  int ok = 0;

  if (!ir_operand_clone(access->base, &arguments[0])) {
    return 0;
  }
  built = 1;
  if (!ir_operand_clone(access->offset, &arguments[1])) {
    goto done;
  }
  built = 2;
  arguments[2] = ir_operand_int(access->size);
  arguments[3] = ir_operand_int(access->access_kind);
  arguments[4] = ir_operand_int((long long)access->location.line);
  built = 5;

  if (access->identity) arguments[5] = *access->identity;
  ok = safety_emit_call(out, access->location,
      access->identity ? "mettle_safety_check_identity" : "mettle_safety_check",
      arguments, access->identity ? 6 : 5);

done:
  for (size_t i = 0; i < built; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

/* ---- resolving a pointer once, comparing per access ------------------------- */
/*
 * Where nothing about an index can be settled, the access still has to be
 * checked, and a check that walks the shadow map is a call and four dependent
 * loads. But a loop that indexes one pointer asks about the same allocation
 * every time, and how far that allocation runs is loop-invariant even when the
 * indices are not.
 *
 * So the allocation is resolved once in front of the loop, and each access
 * becomes `(unsigned)offset > span - size`, which is a subtract, a compare and
 * a branch that is never taken. Failing it is not a verdict: it calls the full
 * check, which is what keeps this exact for interior pointers reading
 * backwards, for dead allocations, and for anything else the comparison alone
 * cannot judge.
 *
 * This is what makes a checked heapsort possible. Its indices come out of
 * comparisons so nothing bounds them, and every access was paying for a walk
 * to be told the same thing about the same array.
 */

static int safety_expand_region(IRInstructionVector *out,
                                const SafetyAccess *access);

typedef struct {
  size_t header_index;
  IROperand base; /* owned */
  /* Borrowed from an instruction inside the loop, which the straight-line
   * search guarantees sits after the header: the resolution is written out at
   * the header, so the instruction it points into has not been moved yet. */
  const IROperand *identity;
  char temp[64];  /* the span this loop resolves once */
  SourceLocation location;
} SafetySpan;

/* Follow a base pointer back to the name it was copied from.
 *
 * Lowering reads a pointer into a fresh temporary at each use, so the operand
 * a check carries is defined inside the loop even when the pointer itself
 * never moves. Taken at face value that says the pointer changes every
 * iteration, and it was enough to decline the cheap form for every access in
 * base64_encode. Copies and pointer casts pass the same address along, so the
 * name behind them is what the span should be keyed on. */
static const IROperand *safety_base_root(const IRFunction *function,
                                         size_t before, const IROperand *base,
                                         const IROperand **delta_out,
                                         size_t *delta_from, int depth) {
  if (base->kind != IR_OPERAND_TEMP || !base->name || depth > 4) {
    return base;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, base->name);
  if (!producer || producer->is_float) {
    return base;
  }
  size_t producer_index = (size_t)(producer - function->instructions);

  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
    if (producer->lhs.kind != IR_OPERAND_SYMBOL &&
        producer->lhs.kind != IR_OPERAND_TEMP) {
      return base;
    }
    return safety_base_root(function, producer_index, &producer->lhs,
                            delta_out, delta_from, depth + 1);
  }

  /* `root + something`, where the something moves each iteration. The pointer
   * really does move, so it cannot be resolved once; but what it moves within
   * does not, so the comparison is made against the root's span with the
   * displacement folded into the offset. Only the fast comparison is rebased.
   * A failure still calls the check with the pointer the program actually
   * used, so nothing about what counts as a violation changes. */
  if (producer->op == IR_OP_BINARY && producer->text &&
      strcmp(producer->text, "+") == 0 && !*delta_out &&
      (producer->lhs.kind == IR_OPERAND_SYMBOL ||
       producer->lhs.kind == IR_OPERAND_TEMP)) {
    *delta_out = &producer->rhs;
    *delta_from = producer_index;
    return safety_base_root(function, producer_index, &producer->lhs,
                            delta_out, delta_from, depth + 1);
  }
  return base;
}

static int safety_operand_same(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind) {
    return 0;
  }
  if (a->kind == IR_OPERAND_INT) {
    return a->int_value == b->int_value;
  }
  return a->name && b->name && strcmp(a->name, b->name) == 0;
}

/* `span = mettle_safety_span(base)`, emitted in front of the loop. */
static int safety_emit_span_resolve(IRInstructionVector *out,
                                    const SafetySpan *span) {
  IRInstruction call = {0};
  call.op = IR_OP_CALL;
  call.location = span->location;
  call.text = mettle_strdup(span->identity ? "mettle_safety_span_identity" :
                                           "mettle_safety_span");
  call.dest = ir_operand_temp(span->temp);
  call.arguments = calloc(span->identity ? 2 : 1, sizeof(IROperand));
  if (!call.text || !call.dest.name || !call.arguments) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  call.argument_count = span->identity ? 2 : 1;
  if (span->identity && !ir_operand_clone(span->identity, &call.arguments[1])) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  if (!ir_operand_clone(&span->base, &call.arguments[0]) ||
      !ir_instruction_vector_append_move(out, &call)) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  return 1;
}

/* The access itself: compare against the resolved span, and only ask properly
 * when that comparison says something might be wrong. */
static int safety_emit_span_check(IRInstructionVector *out,
                                  const SafetyAccess *access,
                                  const char *span_temp,
                                  const IROperand *delta) {
  unsigned id = g_safety_next_id++;
  char limit[64];
  char total[64];
  char bad[64];
  char short_span[64];
  char both[64];
  char ok_label[64];
  snprintf(limit, sizeof(limit), SAFETY_TEMP_PREFIX "sl%u", id);
  snprintf(total, sizeof(total), SAFETY_TEMP_PREFIX "st%u", id);
  snprintf(bad, sizeof(bad), SAFETY_TEMP_PREFIX "sb%u", id);
  snprintf(short_span, sizeof(short_span), SAFETY_TEMP_PREFIX "ss%u", id);
  snprintf(both, sizeof(both), SAFETY_TEMP_PREFIX "sx%u", id);
  snprintf(ok_label, sizeof(ok_label), "ir_safe_in_%u", id);

  IROperand span_operand = ir_operand_temp(span_temp);
  IROperand limit_operand = ir_operand_temp(limit);
  IROperand total_operand = ir_operand_temp(total);
  IROperand size_operand = ir_operand_int(access->size);
  int ok = 0;

  if (!span_operand.name || !limit_operand.name || !total_operand.name) {
    goto done;
  }
  if (!safety_emit_binary(out, access->location, "-", limit, &span_operand,
                          &size_operand, 0)) {
    goto done;
  }

  /* An access with no offset operand starts at the span's own base, which is
   * offset zero. Saying so explicitly keeps every instruction below with a
   * real source: an absent operand reached codegen as an add and a compare
   * with nothing on one side, which the register allocator refuses (and
   * `--release` copy-propagates into an assignment from nothing). */
  IROperand zero_offset = ir_operand_int(0);
  const IROperand *offset =
      (access->offset && access->offset->kind != IR_OPERAND_NONE)
          ? access->offset
          : &zero_offset;

  /* Where the pointer was reached through arithmetic, the displacement joins
   * the offset so both are measured from the same root. */
  const IROperand *measured = offset;
  if (delta && delta->kind != IR_OPERAND_NONE) {
    if (!safety_emit_binary(out, access->location, "+", total, offset, delta,
                            0)) {
      goto done;
    }
    measured = &total_operand;
  }

  /* Comparing without sign is what covers both ends at once: a negative offset
   * reads as an enormous unsigned value and fails, which sends it to the full
   * check rather than rejecting it. */
  if (!safety_emit_binary(out, access->location, ">", bad, measured,
                          &limit_operand, 1)) {
    goto done;
  }

  /* A resolution that came back empty means the origin named no live
   * allocation covering the pointer. The subtraction above turns that into a
   * limit larger than any offset, so without this the whole loop would pass
   * unexamined. Only tracked accesses take this branch: an untracked pointer
   * has always been allowed to run, and that is what an absent origin means. */
  const char *condition = bad;
  if (access->identity) {
    IROperand bad_operand = ir_operand_temp(bad);
    IROperand short_operand = ir_operand_temp(short_span);
    IROperand zero = ir_operand_int(0);
    int guarded = bad_operand.name && short_operand.name &&
        safety_emit_binary(out, access->location, "<", short_span,
                           &limit_operand, &zero, 0) &&
        safety_emit_binary(out, access->location, "|", both, &bad_operand,
                           &short_operand, 0);
    ir_operand_destroy(&bad_operand);
    ir_operand_destroy(&short_operand);
    if (!guarded) {
      goto done;
    }
    condition = both;
  }
  if (!safety_emit_branch_zero(out, access->location, condition, ok_label)) {
    goto done;
  }
  ok = safety_expand_region(out, access) &&
       safety_emit_label(out, access->location, ok_label);

done:
  ir_operand_destroy(&span_operand);
  ir_operand_destroy(&limit_operand);
  ir_operand_destroy(&total_operand);
  return ok;
}

/* Where the straight-line run of instructions ending at `index` begins. A
 * write inside that run is the one the instruction at `index` reads, whatever
 * else in the function writes the same name, because control reached it here
 * with no branch in between. */
static size_t safety_block_start(const IRFunction *function, size_t index) {
  for (size_t i = index; i-- > 0;) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
        op == IR_OP_BRANCH_EQ || op == IR_OP_RETURN) {
      return i + 1;
    }
  }
  return 0;
}

/* The origin operand at a check is usually a copy made inside the loop:
 * pointer arithmetic gives its result an origin of its own, merged from the
 * pointer's and the index's, and an ordinary counter carries none. Those links
 * name the allocation the pointer named on entry, so following them back finds
 * the value that settled outside. Without this a loop that could resolve its
 * allocation once falls back to asking the runtime at every access, purely
 * because the origin passed through an instruction in the body.
 *
 * Only a write in the same straight-line run is followed, so the write is the
 * one this operand actually reads, and only a merge whose other side is a
 * literal nothing, which the runtime defines as the first side unchanged. A
 * name with no write in that run is returned as it stands, leaving the caller
 * to decide whether the loop holds it still. */
static const IROperand *safety_identity_root(const IRFunction *function,
                                             const IROperand *identity,
                                             size_t before, int depth) {
  if (!identity || !identity->name || depth > 16 ||
      (identity->kind != IR_OPERAND_TEMP && identity->kind != IR_OPERAND_SYMBOL)) {
    return identity;
  }
  const IRInstruction *definition = NULL;
  size_t at = 0;
  for (size_t i = safety_block_start(function, before); i < before; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL || !ir_instruction_writes_destination(in) ||
        !safety_operand_same(&in->dest, identity)) {
      continue;
    }
    definition = in;
    at = i;
  }
  if (!definition) {
    return identity;
  }
  if (definition->op == IR_OP_ASSIGN) {
    return safety_identity_root(function, &definition->lhs, at, depth + 1);
  }
  if (definition->op == IR_OP_CALL && definition->text &&
      definition->argument_count == 2 && definition->arguments &&
      (strcmp(definition->text, "mettle_safety_merge_identity") == 0 ||
       strcmp(definition->text, "mettle_safety_subtract_identity") == 0)) {
    const IROperand *left = &definition->arguments[0];
    const IROperand *right = &definition->arguments[1];
    if (right->kind == IR_OPERAND_INT && right->int_value == 0) {
      return safety_identity_root(function, left, at, depth + 1);
    }
    if (left->kind == IR_OPERAND_INT && left->int_value == 0) {
      return safety_identity_root(function, right, at, depth + 1);
    }
  }
  return identity;
}

/* ---- the loops, gathered once ----------------------------------------------- */

static const SafetyLoopForm *safety_enclosing_loop(const SafetyLoopList *loops,
                                                   size_t index) {
  for (size_t i = loops->count; i-- > 0;) {
    const SafetyLoopForm *loop = &loops->items[i];
    if (index > loop->bounds.branch_index && index < loop->bounds.jump_index) {
      return loop;
    }
  }
  return NULL;
}

static void safety_loop_list_destroy(SafetyLoopList *loops) {
  free(loops->items);
  loops->items = NULL;
  loops->count = 0;
  loops->capacity = 0;
}

/* Source order, so a backward scan meets the innermost enclosing loop first:
 * an inner loop's header comes after its outer loop's. */
static int safety_loop_list_build(IRFunction *function, SafetyLoopList *loops) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_LABEL ||
        !ir_label_is_while_header(instruction->text)) {
      continue;
    }
    SafetyLoopForm form;
    if (!safety_parse_loop_form(function, i, &form)) {
      continue;
    }
    if (loops->count == loops->capacity) {
      size_t capacity = loops->capacity ? loops->capacity * 2 : 8;
      SafetyLoopForm *grown =
          realloc(loops->items, capacity * sizeof(SafetyLoopForm));
      if (!grown) {
        safety_loop_list_destroy(loops);
        return 0;
      }
      loops->items = grown;
      loops->capacity = capacity;
    }
    loops->items[loops->count++] = form;
  }
  return 1;
}

/* ---- the one exempt module ------------------------------------------------- */

/* An allocator is the one piece of code whose job is to touch memory that is
 * not inside any live allocation. It writes a block header below the pointer
 * it hands out, and it threads its free list through the bodies of blocks the
 * program has already released. Checked against the model those accesses read
 * as a header overrun and a use-after-free, and they are neither: the model is
 * describing the allocator's own bookkeeping as if it were program memory.
 *
 * So the allocator is exempt, identified by role rather than by path: it is
 * whichever source file defines the heap entry points. Nothing else is exempt,
 * and the exemption costs no coverage of the program itself, because the
 * program only reaches this memory through pointers the allocator returned. */
static const char *safety_allocator_source(const IRProgram *program) {
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *function = program->functions[i];
    if (function && function->name &&
        strncmp(function->name, "mettle_heap_", 12) == 0) {
      return function->location.filename;
    }
  }
  return NULL;
}

static int safety_function_is_allocator(const IRFunction *function,
                                        const char *allocator_source) {
  if (!allocator_source || !function) {
    return 0;
  }
  if (function->name && strncmp(function->name, "mettle_heap_", 12) == 0) {
    return 1;
  }
  return function->location.filename &&
         strcmp(function->location.filename, allocator_source) == 0;
}

/* ---- driver ---------------------------------------------------------------- */

/* Drop every check in a function without expanding any of them. */
static int safety_strip_function(IRFunction *function, IRSafetyStats *stats) {
  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_SAFETY_CHECK) {
      if (stats) {
        stats->emitted++;
        stats->exempt++;
      }
      continue;
    }
    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  return 1;
}

static int safety_resolve_function(IRFunction *function, IRSafetyStats *stats) {
  size_t check_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_SAFETY_CHECK) {
      check_count++;
    }
  }
  if (check_count == 0) {
    return 1;
  }

  /* Decide first, rewrite second. The proofs read the instructions that
   * produced a check's operands, and rewriting moves instructions out of the
   * array as it goes, so a proof running mid-rewrite would look back at
   * emptied slots and conclude it knows nothing. */
  enum {
    SAFETY_KEEP = 0,
    SAFETY_PROVED = 1,
    SAFETY_HOISTED = 2,
    SAFETY_SPANNED = 3
  };
  unsigned char *outcome = calloc(function->instruction_count, 1);
  SafetyHoist *hoists = calloc(check_count, sizeof(SafetyHoist));
  SafetySpan *spans = calloc(check_count, sizeof(SafetySpan));
  size_t *span_of = calloc(function->instruction_count, sizeof(size_t));
  const IROperand **span_delta =
      calloc(function->instruction_count, sizeof(const IROperand *));
  size_t hoist_count = 0;
  size_t span_count = 0;
  SafetyLoopList loops = {0};
  if (!outcome || !hoists || !spans || !span_of || !span_delta) {
    free(outcome);
    free(hoists);
    free(spans);
    free(span_of);
    free(span_delta);
    return 0;
  }
  if (!safety_loop_list_build(function, &loops)) {
    free(outcome);
    free(hoists);
    free(spans);
    free(span_of);
    free(span_delta);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_SAFETY_CHECK) {
      continue;
    }
    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      goto fail;
    }
    if (safety_prove(function, &loops, i, &access)) {
      outcome[i] = SAFETY_PROVED;
      continue;
    }
    if (safety_try_hoist(function, &loops, i, &access, &hoists[hoist_count])) {
      hoist_count++;
      outcome[i] = SAFETY_HOISTED;
      continue;
    }

    /* Nothing settles the index, so the access keeps a check. But if it is in
     * a loop that cannot release what it is walking, and the pointer holds
     * still, resolving the allocation once turns the check from a call into a
     * comparison. */
    const SafetyLoopForm *loop = safety_enclosing_loop(&loops, i);
    if (!loop) {
      safety_trace("not in any loop, so there is nothing to resolve against",
                   access.location.line);
      continue;
    }
    const IROperand *identity_root =
        safety_identity_root(function, access.identity, i, 0);
    if (identity_root && !safety_operand_invariant_in(function,
        loop->header_index, loop->bounds.jump_index, identity_root)) {
      safety_trace("the pointer origin is re-read inside the loop, so one "
                   "resolution would not describe every iteration",
                   access.location.line);
      continue;
    }
    if (!safety_body_has_no_calls(function, &loop->bounds)) {
      safety_trace("the loop calls something that could free what it walks",
                   access.location.line);
      continue;
    }
    /* The span is the extent of the allocation the pointer named when the loop
     * began. If the body handed that pointer's address out, a callee could
     * point it somewhere else and the span would describe the wrong block. */
    if (safety_body_calls_out(function, &loop->bounds) &&
        safety_operand_escapes(function, access.base)) {
      safety_trace("the loop hands out the address of the pointer it walks, so "
                   "a call in the body could move it",
                   access.location.line);
      continue;
    }
    const IROperand *delta = NULL;
    size_t delta_from = 0;
    const IROperand *root =
        safety_base_root(function, i, access.base, &delta, &delta_from, 0);
    if (!safety_operand_invariant_in(function, loop->header_index,
                                     loop->bounds.jump_index, root)) {
      safety_trace("the pointer moves inside the loop and is not a fixed one "
                   "displaced, so one resolution would not describe it",
                   access.location.line);
      continue;
    }
    /* The displacement is read again where the check sits, so it has to still
     * hold what it held where the pointer was formed. */
    if (delta && !safety_operand_invariant_in(function, delta_from + 1, i,
                                              delta)) {
      safety_trace("the displacement changes between forming the pointer and "
                   "using it",
                   access.location.line);
      continue;
    }

    size_t found = span_count;
    for (size_t s = 0; s < span_count; s++) {
      if (spans[s].header_index == loop->header_index &&
          safety_operand_same(&spans[s].base, root) &&
          ((!spans[s].identity && !identity_root) ||
           (spans[s].identity && identity_root &&
            safety_operand_same(spans[s].identity, identity_root)))) {
        found = s;
        break;
      }
    }
    if (found == span_count) {
      SafetySpan *fresh = &spans[span_count];
      fresh->header_index = loop->header_index;
      fresh->identity = identity_root;
      fresh->location = access.location;
      snprintf(fresh->temp, sizeof(fresh->temp), SAFETY_TEMP_PREFIX "sp%u",
               g_safety_next_id++);
      if (!ir_operand_clone(root, &fresh->base)) {
        goto fail;
      }
      span_count++;
    }
    span_of[i] = found;
    span_delta[i] = delta;
    outcome[i] = SAFETY_SPANNED;
  }
  safety_loop_list_destroy(&loops);

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    goto fail;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    /* A loop's hoisted checks and resolved pointers go in front of its
     * header. */
    for (size_t h = 0; h < hoist_count; h++) {
      if (hoists[h].header_index == i &&
          !safety_emit_hoisted(&out, &hoists[h])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
    }
    for (size_t s = 0; s < span_count; s++) {
      if (spans[s].header_index == i &&
          !safety_emit_span_resolve(&out, &spans[s])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
    }

    if (instruction->op != IR_OP_SAFETY_CHECK) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
      continue;
    }

    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      ir_instruction_vector_destroy(&out);
      goto fail;
    }
    if (stats) {
      stats->emitted++;
    }

    if (outcome[i] == SAFETY_PROVED) {
      if (stats) {
        stats->proved++;
      }
      continue;
    }
    if (outcome[i] == SAFETY_HOISTED) {
      if (stats) {
        stats->hoisted++;
      }
      continue;
    }
    if (outcome[i] == SAFETY_SPANNED) {
      if (!safety_emit_span_check(&out, &access, spans[span_of[i]].temp,
                                  span_delta[i])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
      if (stats) {
        stats->spanned++;
      }
      ir_explain_safety_note(access.location.filename, access.location.line,
                             function->name, IR_SAFETY_SURVIVOR_SPAN);
      continue;
    }

    int expanded;
    if (access.extent == IR_SAFETY_EXTENT_UNKNOWN) {
      expanded = safety_expand_region(&out, &access);
      if (expanded) {
        if (stats) {
          stats->region_calls++;
        }
        ir_explain_safety_note(access.location.filename, access.location.line,
                               function->name, IR_SAFETY_SURVIVOR_REGION);
      }
    } else {
      expanded = safety_expand_extent(&out, &access);
      if (expanded) {
        if (stats) {
          stats->extent_tests++;
        }
        ir_explain_safety_note(access.location.filename, access.location.line,
                               function->name, IR_SAFETY_SURVIVOR_EXTENT);
      }
    }
    if (!expanded) {
      ir_instruction_vector_destroy(&out);
      goto fail;
    }
    /* The check is not moved into `out`: its operands were cloned into the
     * replacement, and ir_function_replace_instructions frees what is left of
     * the old array below. */
  }

  for (size_t h = 0; h < hoist_count; h++) {
    ir_operand_destroy(&hoists[h].base);
    ir_operand_destroy(&hoists[h].bound);
    if (hoists[h].has_invariant) {
      ir_operand_destroy(&hoists[h].invariant);
    }
    if (hoists[h].has_index_start) {
      ir_operand_destroy(&hoists[h].index_start_value);
    }
  }
  for (size_t s = 0; s < span_count; s++) {
    ir_operand_destroy(&spans[s].base);
  }
  free(spans);
  free(span_of);
  free(span_delta);
  free(hoists);
  free(outcome);
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  return 1;

fail:
  safety_loop_list_destroy(&loops);
  for (size_t h = 0; h < hoist_count; h++) {
    ir_operand_destroy(&hoists[h].base);
    ir_operand_destroy(&hoists[h].bound);
    if (hoists[h].has_invariant) {
      ir_operand_destroy(&hoists[h].invariant);
    }
    if (hoists[h].has_index_start) {
      ir_operand_destroy(&hoists[h].index_start_value);
    }
  }
  for (size_t s = 0; s < span_count; s++) {
    ir_operand_destroy(&spans[s].base);
  }
  free(spans);
  free(span_of);
  free(span_delta);
  free(hoists);
  free(outcome);
  return 0;
}

/* Declare the runtime entry points this pass calls.
 *
 * Without these the calls name functions the program never declared, and the
 * register-allocating backend defers any function containing a call it cannot
 * find a signature for. That turned `--safe` into "compile the whole program
 * with the spill-everything backend": every function holding a single check
 * lost register allocation, which cost far more than the checks did. */
static int safety_declare_runtime(IRProgram *program) {
  /* Metadata APIs consume addresses, including string record addresses. A
   * byte pointer signature would ask codegen to convert records to cstrings. */
  const MtlcType *pointer = mtlc_type_pointer(mtlc_type_scalar(MTLC_TYPE_UINT64));
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const MtlcType *u32 = mtlc_type_scalar(MTLC_TYPE_UINT32);
  const MtlcType *u64 = mtlc_type_scalar(MTLC_TYPE_UINT64);
  const MtlcType *nothing = mtlc_type_scalar(MTLC_TYPE_VOID);
  if (!pointer || !i64 || !u32 || !u64 || !nothing) {
    return 1; /* no signatures available: the calls still work, unallocated */
  }

  const MtlcType *check_params[5] = {pointer, i64, i64, u32, u32};
  const MtlcType *loop_length_params[5] = {i64, i64, i64, i64, i64};
  const MtlcType *span_params[1] = {pointer};
  const MtlcType *register_params[2] = {pointer, u64};
  const MtlcType *unregister_params[1] = {pointer};
  const MtlcType *reregister_params[3] = {pointer, pointer, u64};
  const MtlcType *identity_check_params[6] = {pointer, i64, i64, u32, u32, u64};
  const MtlcType *affine_check_params[8] = {pointer, i64, i64, u32, u32, u64, i64, i64};
  const MtlcType *pointer_u64[2] = {pointer, u64};
  const MtlcType *string_u64[2] = {pointer, u64};
  const MtlcType *two_u64[2] = {u64, u64};
  const MtlcType *value_store_params[4] = {pointer, u64, u64, u64};
  const MtlcType *call_arg_params[3] = {pointer, u64, u64};
  const MtlcType *copy_params[3] = {pointer, pointer, u64};
  const MtlcType *arg_copy_params[4] = {pointer, u64, pointer, u64};
  const MtlcType *buffer_params[5] = {pointer, i64, u32, u32, u64};

  const struct {
    const char *name;
    const MtlcType *return_type;
    const MtlcType **params;
    size_t param_count;
  } entries[] = {
      {"mettle_safety_check", nothing, check_params, 5},
      {"mettle_safety_span", i64, span_params, 1},
      {"mettle_safety_register", nothing, register_params, 2},
      {"mettle_safety_register_static", nothing, register_params, 2},
      {"mettle_safety_unregister", nothing, unregister_params, 1},
      {"mettle_safety_reregister", nothing, reregister_params, 3},
      {"mettle_safety_enter_allocator", nothing, NULL, 0},
      {"mettle_safety_leave_allocator", nothing, NULL, 0},
      {"mettle_safety_identity", u64, span_params, 1},
      {"mettle_safety_loop_length", i64, loop_length_params, 5},
      {"mettle_safety_check_identity", nothing, identity_check_params, 6},
      {"mettle_safety_check_affine", nothing, affine_check_params, 8},
      {"mettle_safety_span_identity", i64, pointer_u64, 2},
      {"mettle_safety_merge_identity", u64, two_u64, 2},
      {"mettle_safety_subtract_identity", u64, two_u64, 2},
      {"mettle_safety_value_load", u64, call_arg_params, 3},
      {"mettle_safety_value_store", nothing, value_store_params, 4},
      {"mettle_safety_call_push", pointer, pointer_u64, 2},
      {"mettle_safety_call_enter", pointer, span_params, 1},
      {"mettle_safety_call_arg", nothing, call_arg_params, 3},
      {"mettle_safety_call_param", u64, pointer_u64, 2},
      {"mettle_safety_call_return", nothing, pointer_u64, 2},
      {"mettle_safety_call_pop", u64, span_params, 1},
      {"mettle_safety_call_arg_copy", nothing, arg_copy_params, 4},
      {"mettle_safety_call_param_copy", nothing, arg_copy_params, 4},
      {"mettle_safety_call_return_copy", nothing, copy_params, 3},
      {"mettle_safety_call_result_copy", nothing, copy_params, 3},
      {"mettle_safety_value_copy", nothing, copy_params, 3},
      {"mettle_safety_value_clear", nothing, pointer_u64, 2},
      {"mettle_safety_free_identity", nothing, pointer_u64, 2},
      {"mettle_safety_buffer_check", nothing, buffer_params, 5},
      {"mettle_safety_region_begin", nothing, pointer_u64, 2},
      {"mettle_safety_region_end", nothing, span_params, 1},
      {"mettle_safety_entry_arguments", nothing, span_params, 1},
      {"mettle_safety_literal_identity", u64, pointer_u64, 2},
      {"mettle_safety_string_identity", u64, string_u64, 2},
      {"mettle_safety_string_contents", nothing, pointer_u64, 2},
      {"mettle_safety_global_pointer", nothing, call_arg_params, 3},
  };

  for (size_t e = 0; e < sizeof(entries) / sizeof(entries[0]); e++) {
    if (ir_program_lookup_symbol(program, entries[e].name)) {
      continue;
    }
    IRModuleSymbol entry = {0};
    entry.name = (char *)entries[e].name;
    entry.kind = IR_MODSYM_FUNCTION;
    entry.is_extern = 1;
    entry.return_type = (MtlcType *)entries[e].return_type;
    entry.type = (MtlcType *)entries[e].return_type;
    entry.param_types = (MtlcType **)entries[e].params;
    entry.param_count = entries[e].param_count;
    if (!ir_program_add_symbol(program, &entry)) {
      return 0;
    }
  }
  return 1;
}

/* Is this name written, or its address taken, anywhere in the program? Either
 * makes it something other than the constant its initializer suggests. */
static int safety_global_is_settled(const IRProgram *program,
                                    const char *name) {
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
          in->lhs.name && strcmp(in->lhs.name, name) == 0) {
        return 0;
      }
      if (ir_instruction_writes_destination(in) &&
          in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
          strcmp(in->dest.name, name) == 0) {
        return 0;
      }
      /* An asm block is opaque: a global it binds may be stored to inside it,
       * and no instruction records that. */
      if (in->op == IR_OP_INLINE_ASM &&
          ir_inline_asm_binds_symbol(in->text, name)) {
        return 0;
      }
    }
  }
  return 1;
}

static int safety_const_globals_order(const void *a, const void *b) {
  const size_t *left = (const size_t *)a;
  const size_t *right = (const size_t *)b;
  return strcmp(g_safety_const_globals.names[*left],
                g_safety_const_globals.names[*right]);
}

static void safety_const_globals_destroy(void) {
  for (size_t i = 0; i < g_safety_const_globals.count; i++) {
    free(g_safety_const_globals.names[i]);
  }
  free(g_safety_const_globals.names);
  free(g_safety_const_globals.values);
  memset(&g_safety_const_globals, 0, sizeof(g_safety_const_globals));
}

static void safety_const_globals_build(const IRProgram *program) {
  memset(&g_safety_const_globals, 0, sizeof(g_safety_const_globals));
  size_t capacity = program->module_symbol_count;
  if (capacity == 0) {
    return;
  }
  g_safety_const_globals.names = (char **)calloc(capacity, sizeof(char *));
  g_safety_const_globals.values =
      (long long *)calloc(capacity, sizeof(long long));
  if (!g_safety_const_globals.names || !g_safety_const_globals.values) {
    safety_const_globals_destroy();
    return;
  }
  for (size_t s = 0; s < program->module_symbol_count; s++) {
    const IRModuleSymbol *symbol = &program->module_symbols[s];
    if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
        !symbol->has_initializer || symbol->init_is_float ||
        symbol->init_string || symbol->init_bytes || !symbol->name) {
      continue;
    }
    /* Exported: something outside this compilation can write it, so scanning
     * this program says nothing about its value. */
    if (symbol->is_exported) {
      continue;
    }
    if (!safety_global_is_settled(program, symbol->name)) {
      continue;
    }
    char *copy = mettle_strdup(symbol->name);
    if (!copy) {
      safety_const_globals_destroy();
      return;
    }
    g_safety_const_globals.names[g_safety_const_globals.count] = copy;
    g_safety_const_globals.values[g_safety_const_globals.count] =
        symbol->init_bits;
    g_safety_const_globals.count++;
  }
  /* Sorted so the lookup, which every constant fold reaches, is a search
   * rather than a scan of every global in the program. */
  size_t count = g_safety_const_globals.count;
  if (count < 2) {
    return;
  }
  size_t *order = (size_t *)malloc(count * sizeof(size_t));
  if (!order) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    order[i] = i;
  }
  qsort(order, count, sizeof(size_t), safety_const_globals_order);
  char **names = (char **)malloc(count * sizeof(char *));
  long long *values = (long long *)malloc(count * sizeof(long long));
  if (names && values) {
    for (size_t i = 0; i < count; i++) {
      names[i] = g_safety_const_globals.names[order[i]];
      values[i] = g_safety_const_globals.values[order[i]];
    }
    free(g_safety_const_globals.names);
    free(g_safety_const_globals.values);
    g_safety_const_globals.names = names;
    g_safety_const_globals.values = values;
  } else {
    free(names);
    free(values);
    g_safety_const_globals.count = 0; /* unsorted is unsearchable */
  }
  free(order);
}

#include "ir_safety_plain_storage.inc"

int ir_safety_analyze_origins(IRProgram *program) {
  if (!program) return 1;
  /* Preserve the diagnostic footprint of the input IR. A range already
   * recognized here keeps its range message. A check widened only after
   * scalar analysis still reports the first failing source access. */
  g_safety_program = program;
  g_safety_callee_cache_count = 0;
  safety_const_globals_build(program);
  int ok = 1;
  for (size_t f = 0; ok && f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    SafetyLoopList loops = {0};
    if (!safety_loop_list_build(fn, &loops)) { ok = 0; break; }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      if (in->op != IR_OP_SAFETY_CHECK ||
          in->argument_count != IR_SAFETY_TRACKED_ARG_COUNT) continue;
      SafetyAccess access = {0};
      SafetyHoist hoist = {0};
      if (!safety_read(in, &access)) { ok = 0; break; }
      int ranged = safety_try_hoist(fn, &loops, i, &access, &hoist);
      ir_operand_destroy(&hoist.base);
      ir_operand_destroy(&hoist.bound);
      ir_operand_destroy(&hoist.invariant);
      ir_operand_destroy(&hoist.index_start_value);
      IROperand *args = realloc(in->arguments,
          IR_SAFETY_ANALYZED_ARG_COUNT * sizeof(*args));
      if (!args) { ok = 0; break; }
      in->arguments = args;
      args[IR_SAFETY_ARG_DIAGNOSTIC_SIZE] = ir_operand_int(ranged ? 0 : access.size);
      in->argument_count = IR_SAFETY_ANALYZED_ARG_COUNT;
    }
    safety_loop_list_destroy(&loops);
  }
  safety_const_globals_destroy();
  g_safety_program = NULL;
  return ok && safety_simplify_plain_storage(program);
}

int ir_safety_resolve_program(IRProgram *program, IRSafetyStats *stats) {
  if (!program) {
    return 1;
  }
  clock_t started = safety_time_enabled() ? clock() : 0;
  if (!safety_declare_runtime(program)) {
    return 0;
  }
  g_safety_program = program;
  g_safety_callee_cache_count = 0;
  safety_const_globals_build(program);
  const char *allocator_source = safety_allocator_source(program);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    /* A rule is not part of the program: it runs in the compile-time
     * interpreter and never reaches a binary, so there is nothing for a
     * checked access to protect. Instrumenting one only hands the interpreter
     * code it cannot run. */
    if (function->is_rule) {
      continue;
    }
    int resolved =
        safety_function_is_allocator(function, allocator_source)
            ? safety_strip_function(function, stats)
            : safety_resolve_function(function, stats);
    if (!resolved) {
      safety_const_globals_destroy();
      g_safety_program = NULL;
      return 0;
    }
  }
  safety_const_globals_destroy();
  g_safety_program = NULL;
  if (!safety_simplify_plain_storage(program)) return 0;
  if (safety_time_enabled()) {
    /* Ticks rather than a converted figure: clock()'s units do not reliably
     * match CLOCKS_PER_SEC across the toolchains this builds with, and a
     * number in the wrong units is worse than none. Runs are comparable, which
     * is what this is for. */
    fprintf(stderr, "safety: resolving took %lld ticks\n",
            (long long)(clock() - started));
  }
  return 1;
}

/* ---- telling the runtime where the heap is --------------------------------- */

typedef enum {
  SAFETY_ALLOC_NONE = 0,
  SAFETY_ALLOC_SIZE,    /* arguments[0] is the byte count */
  SAFETY_ALLOC_PRODUCT, /* arguments[0] * arguments[1] is the byte count */
  SAFETY_ALLOC_REALLOC, /* arguments[0] the old block, arguments[1] the size */
  SAFETY_ALLOC_FREE     /* arguments[0] is the block being retired */
} SafetyAllocKind;

/* Whether the callee is the Mettle-implemented allocator rather than the libc
 * one. Only the former needs bracketing: its body is Mettle code that lowering
 * has checked, and it reaches for the same helpers ordinary code does. The
 * libc allocator is C, never carries a check, and needs no bracket. */
static int safety_callee_is_mettle_allocator(const IRInstruction *instruction) {
  return instruction->op == IR_OP_CALL && instruction->text &&
         strncmp(instruction->text, "mettle_heap_", 12) == 0;
}

/* Both spellings of every entry point: the libc names a program calls by
 * default, and the std/alloc names --native-heap rewrites them to. This runs
 * after that rewrite, so only one set is ever present, but matching both keeps
 * the two flags independent. */
static SafetyAllocKind safety_classify_call(const IRInstruction *instruction) {
  if (instruction->op != IR_OP_CALL || !instruction->text) {
    return SAFETY_ALLOC_NONE;
  }
  const char *callee = instruction->text;
  size_t arguments = instruction->argument_count;

  if (arguments == 1 &&
      (strcmp(callee, "malloc") == 0 ||
       strcmp(callee, "mettle_heap_alloc") == 0 ||
       strcmp(callee, "mettle_heap_zeroed") == 0)) {
    return SAFETY_ALLOC_SIZE;
  }
  if (arguments == 2 && (strcmp(callee, "calloc") == 0 ||
                         strcmp(callee, "mettle_heap_calloc") == 0)) {
    return SAFETY_ALLOC_PRODUCT;
  }
  if (arguments == 2 && (strcmp(callee, "realloc") == 0 ||
                         strcmp(callee, "mettle_heap_realloc") == 0)) {
    return SAFETY_ALLOC_REALLOC;
  }
  if (arguments == 1 && (strcmp(callee, "free") == 0 ||
                         strcmp(callee, "mettle_heap_free") == 0)) {
    return SAFETY_ALLOC_FREE;
  }
  return SAFETY_ALLOC_NONE;
}

static int safety_emit_register(IRInstructionVector *out,
                                SourceLocation location,
                                const IROperand *pointer,
                                const IROperand *size) {
  IROperand arguments[2];
  if (!ir_operand_clone(pointer, &arguments[0])) {
    return 0;
  }
  if (!ir_operand_clone(size, &arguments[1])) {
    ir_operand_destroy(&arguments[0]);
    return 0;
  }
  int ok = safety_emit_call(out, location, "mettle_safety_register", arguments,
                            2);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&arguments[1]);
  return ok;
}

static int safety_emit_one_pointer_call(IRInstructionVector *out,
                                        SourceLocation location,
                                        const char *callee,
                                        const IROperand *pointer) {
  IROperand argument;
  if (!ir_operand_clone(pointer, &argument)) {
    return 0;
  }
  int ok = safety_emit_call(out, location, callee, &argument, 1);
  ir_operand_destroy(&argument);
  return ok;
}

static int safety_emit_reregister(IRInstructionVector *out,
                                  SourceLocation location,
                                  const IROperand *old_pointer,
                                  const IROperand *new_pointer,
                                  const IROperand *size) {
  IROperand arguments[3];
  size_t built = 0;
  int ok = 0;

  if (!ir_operand_clone(old_pointer, &arguments[0])) {
    return 0;
  }
  built = 1;
  if (!ir_operand_clone(new_pointer, &arguments[1])) {
    goto done;
  }
  built = 2;
  if (!ir_operand_clone(size, &arguments[2])) {
    goto done;
  }
  built = 3;
  ok = safety_emit_call(out, location, "mettle_safety_reregister", arguments,
                        3);

done:
  for (size_t i = 0; i < built; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

/* The size `new T` asks for. Mirrors what --native-heap's rewrite does with
 * the same operand, including its eight byte fallback for a missing one. */
static IROperand safety_new_size(const IRInstruction *instruction) {
  if (instruction->rhs.kind == IR_OPERAND_NONE ||
      (instruction->rhs.kind == IR_OPERAND_INT &&
       instruction->rhs.int_value <= 0)) {
    return ir_operand_int(8);
  }
  return instruction->rhs;
}

static int safety_register_function(IRFunction *function) {
  int found = 0;
  for (size_t i = 0; i < function->instruction_count && !found; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    found = instruction->op == IR_OP_NEW ||
            safety_classify_call(instruction) != SAFETY_ALLOC_NONE;
  }
  if (!found) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    SafetyAllocKind kind = safety_classify_call(instruction);
    int is_new = instruction->op == IR_OP_NEW;
    SourceLocation location = instruction->location;

    if (kind == SAFETY_ALLOC_NONE && !is_new) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
      continue;
    }

    /* Retire the block before the call that releases it, not after. Between
     * the two the allocator has not handed the memory out yet, so no other
     * thread can register something else over it. */
    if (kind == SAFETY_ALLOC_FREE) {
      if (!safety_emit_one_pointer_call(&out, location,
                                        "mettle_safety_unregister",
                                        &instruction->arguments[0])) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }

    /* How many bytes the call is about to hand back. calloc states it as a
     * product, which is multiplied out here, before the call, so only the one
     * result has its live range stretched across it instead of both factors. */
    IROperand size = ir_operand_none();
    IROperand product_operand = ir_operand_none();
    if (is_new) {
      size = safety_new_size(instruction);
    } else if (kind == SAFETY_ALLOC_SIZE) {
      size = instruction->arguments[0];
    } else if (kind == SAFETY_ALLOC_REALLOC) {
      size = instruction->arguments[1];
    } else if (kind == SAFETY_ALLOC_PRODUCT) {
      char product[64];
      snprintf(product, sizeof(product), SAFETY_TEMP_PREFIX "n%u",
               g_safety_next_id++);
      product_operand = ir_operand_temp(product);
      if (!product_operand.name ||
          !safety_emit_binary(&out, location, "*", product,
                              &instruction->arguments[0],
                              &instruction->arguments[1], 1)) {
        ir_operand_destroy(&product_operand);
        ir_instruction_vector_destroy(&out);
        return 0;
      }
      size = product_operand;
    }

    /* These alias the instruction's own storage, which the vector takes over
     * below and keeps alive for the rest of this function. */
    IROperand result = instruction->dest;
    IROperand old_pointer = kind == SAFETY_ALLOC_REALLOC
                                ? instruction->arguments[0]
                                : ir_operand_none();
    int bracket = safety_callee_is_mettle_allocator(instruction);

    if (bracket && !safety_emit_call(&out, location,
                                     "mettle_safety_enter_allocator", NULL,
                                     0)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    if (bracket && !safety_emit_call(&out, location,
                                     "mettle_safety_leave_allocator", NULL,
                                     0)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    /* A result nobody keeps cannot be reached through, so there is nothing to
     * describe. Freeing has already been handled above. */
    int ok = 1;
    if (result.kind != IR_OPERAND_NONE && kind != SAFETY_ALLOC_FREE) {
      ok = kind == SAFETY_ALLOC_REALLOC
               ? safety_emit_reregister(&out, location, &old_pointer, &result,
                                        &size)
               : safety_emit_register(&out, location, &result, &size);
    }
    ir_operand_destroy(&product_operand);
    if (!ok) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }

  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(function);
  return 1;
}

/* ---- describing the stack ---------------------------------------------------- */
/*
 * Indexing a local never needs the runtime: its size is in the program, so the
 * check is a comparison against a constant or is proved away. What needs the
 * runtime is a pointer taken into a local and carried somewhere the size no
 * longer travels with it, and until now that pointer resolved to nothing and
 * the access went unexamined.
 *
 * Only locals whose address genuinely leaves are described. Every indexed
 * array has its address taken in the IR, so registering on that alone would
 * charge two calls per invocation to functions that never needed it.
 */

#define SAFETY_MAX_ESCAPE_TEMPS 64

typedef struct {
  const char *names[SAFETY_MAX_ESCAPE_TEMPS];
  size_t count;
  int overflowed;
} SafetyTempSet;

static int safety_temp_set_has(const SafetyTempSet *set, const IROperand *op) {
  if (op->kind != IR_OPERAND_TEMP || !op->name) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], op->name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void safety_temp_set_add(SafetyTempSet *set, const IROperand *op) {
  if (op->kind != IR_OPERAND_TEMP || !op->name || safety_temp_set_has(set, op)) {
    return;
  }
  if (set->count == SAFETY_MAX_ESCAPE_TEMPS) {
    set->overflowed = 1;
    return;
  }
  set->names[set->count++] = op->name;
}

/* Whether a pointer to `local` reaches anywhere its size does not.
 *
 * Reading or writing through the address here is not that: those accesses
 * carry the local's extent already. Handing the address to a call, storing it
 * into memory, returning it, or parking it in a variable all are, because from
 * that point the program can reach the object without anything saying how
 * large it is.
 *
 * Conservative in the direction that costs speed rather than coverage: an
 * address chain too long to follow, or a shape not recognized, counts as
 * escaping. */
static int safety_local_address_escapes(const IRFunction *function,
                                        const char *local) {
  SafetyTempSet addresses = {{0}, 0, 0};

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_ADDRESS_OF &&
        ir_operand_is_symbol_named(&instruction->lhs, local)) {
      safety_temp_set_add(&addresses, &instruction->dest);
      continue;
    }
    if (addresses.count == 0) {
      continue;
    }

    switch (instruction->op) {
    case IR_OP_BINARY:
    case IR_OP_ASSIGN:
    case IR_OP_CAST:
      /* Address arithmetic and copies carry the pointer along. Landing in a
       * variable rather than a temporary is already out of reach. */
      if (safety_temp_set_has(&addresses, &instruction->lhs) ||
          safety_temp_set_has(&addresses, &instruction->rhs)) {
        if (instruction->dest.kind == IR_OPERAND_SYMBOL) {
          return 1;
        }
        safety_temp_set_add(&addresses, &instruction->dest);
      }
      break;
    case IR_OP_LOAD:
      /* lhs is the address being read through, which is not an escape. */
      break;
    case IR_OP_STORE:
      /* dest is the address, lhs the value: storing the pointer is an escape,
       * storing through it is not. */
      if (safety_temp_set_has(&addresses, &instruction->lhs)) {
        return 1;
      }
      break;
    case IR_OP_SAFETY_CHECK:
      break; /* the checks themselves are not a use of the program's */
    case IR_OP_RETURN:
      if (safety_temp_set_has(&addresses, &instruction->lhs)) {
        return 1;
      }
      break;
    default:
      for (size_t a = 0; a < instruction->argument_count; a++) {
        if (safety_temp_set_has(&addresses, &instruction->arguments[a])) {
          return 1;
        }
      }
      if (safety_temp_set_has(&addresses, &instruction->lhs) ||
          safety_temp_set_has(&addresses, &instruction->rhs)) {
        return 1;
      }
      break;
    }
  }

  return addresses.overflowed && addresses.count > 0;
}

/* `t = &local; call mettle_safety_<what>(t, ...)`. */
static int safety_emit_local_note(IRInstructionVector *out, const char *callee,
                                  const char *local, long long size,
                                  SourceLocation location) {
  char address[64];
  snprintf(address, sizeof(address), SAFETY_TEMP_PREFIX "k%u",
           g_safety_next_id++);

  IRInstruction take = {0};
  take.op = IR_OP_ADDRESS_OF;
  take.location = location;
  take.dest = ir_operand_temp(address);
  take.lhs = ir_operand_symbol(local);
  if (!take.dest.name || !take.lhs.name) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }
  if (!ir_instruction_vector_append_move(out, &take)) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }

  IROperand arguments[2];
  arguments[0] = ir_operand_temp(address);
  arguments[1] = ir_operand_int(size);
  int ok = arguments[0].name &&
           safety_emit_call(out, location, callee, arguments,
                            size > 0 ? 2u : 1u);
  ir_operand_destroy(&arguments[0]);
  return ok;
}

typedef struct {
  const char *name;
  long long size;
  SourceLocation location;
} SafetyStackLocal;

/* Register every escaping local at function entry and retire it at every exit.
 *
 * At entry rather than where the declaration appears, because the slot exists
 * for the whole frame either way, and a declaration inside a loop would
 * otherwise re-register once per iteration. */
static int safety_describe_local(IRProgram *program, IRFunction *function,
                                 size_t i, SafetyStackLocal *locals,
                                 size_t *count) {
  const IRInstruction *instruction = &function->instructions[i];
  if (instruction->op != IR_OP_DECLARE_LOCAL ||
      instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
      !instruction->text) {
    return 1;
  }
  MtlcType *type = instruction->value_type
                       ? instruction->value_type
                       : ir_program_lookup_type(program, instruction->text);
  if (!type || type->size == 0) {
    return 1;
  }
  if (!safety_local_address_escapes(function, instruction->dest.name)) {
    return 1;
  }
  locals[*count].name = instruction->dest.name;
  locals[*count].size = (long long)type->size;
  locals[*count].location = instruction->location;
  (*count)++;
  if (safety_trace_enabled()) {
    fprintf(stderr, "safety: describing local %s (%zu bytes) in %s\n",
            instruction->dest.name, type->size,
            function->name ? function->name : "?");
  }
  return 1;
}

static int safety_describe_stack(IRProgram *program, IRFunction *function) {
  size_t declared = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_DECLARE_LOCAL) {
      declared++;
    }
  }
  /* A parameter has a stack slot too, and no DECLARE_LOCAL to find it by.
   * Left undescribed, its slot keeps whatever dead descriptor the previous
   * frame retired over the same bytes, so reading through `&parameter`
   * reported a use-after-free in a program that had freed nothing. */
  declared += function->parameter_count;
  if (declared == 0) {
    return 1;
  }

  /* Sized to what the function actually declares rather than to a fixed cap.
   * A cap would silently stop describing locals past it, and a coverage hole
   * that depends on how many variables a function happens to have is not one
   * anybody would think to look for. */
  SafetyStackLocal *locals = calloc(declared, sizeof(SafetyStackLocal));
  if (!locals) {
    return 0;
  }
  size_t count = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!safety_describe_local(program, function, i, locals, &count)) {
      return 0;
    }
  }

  for (size_t p = 0; p < function->parameter_count; p++) {
    const char *name = function->parameter_names ? function->parameter_names[p]
                                                 : NULL;
    MtlcType *type = name && function->parameter_types
        ? ir_program_lookup_type(program, function->parameter_types[p]) : NULL;
    if (!name || !type || type->size == 0) {
      continue;
    }
    /* A name the body redeclares is a local shadowing the parameter, and the
     * loop above already described it at the right size. */
    if (ir_function_find_declaration(function, name, 1)) {
      continue;
    }
    if (!safety_local_address_escapes(function, name)) {
      continue;
    }
    locals[count].name = name;
    locals[count].size = (long long)type->size;
    locals[count].location = function->location;
    count++;
    if (safety_trace_enabled()) {
      fprintf(stderr, "safety: describing parameter %s (%zu bytes) in %s\n",
              name, type->size, function->name ? function->name : "?");
    }
  }
  if (count == 0) {
    free(locals);
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out,
                                     function->instruction_count + count * 6)) {
    free(locals);
    return 0;
  }

  for (size_t l = 0; l < count; l++) {
    if (!safety_emit_local_note(&out, "mettle_safety_register_static", locals[l].name,
                                locals[l].size, locals[l].location)) {
      ir_instruction_vector_destroy(&out);
      free(locals);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_RETURN) {
      for (size_t l = 0; l < count; l++) {
        if (!safety_emit_local_note(&out, "mettle_safety_unregister",
                                    locals[l].name, 0, locals[l].location)) {
          ir_instruction_vector_destroy(&out);
          free(locals);
          return 0;
        }
      }
    }
    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_instruction_vector_destroy(&out);
      free(locals);
      return 0;
    }
  }

  /* A function that runs off the end has no return to hang the retirement on,
   * so it goes last. Retiring a slot twice is harmless; leaving one live after
   * the frame is gone is not, because the next frame reusing that memory would
   * be described as the old local. */
  const IRInstruction *last =
      out.count > 0 ? &out.items[out.count - 1] : NULL;
  if (!last || last->op != IR_OP_RETURN) {
    for (size_t l = 0; l < count; l++) {
      if (!safety_emit_local_note(&out, "mettle_safety_unregister",
                                  locals[l].name, 0, locals[l].location)) {
        ir_instruction_vector_destroy(&out);
        free(locals);
        return 0;
      }
    }
  }

  free(locals);
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(function);
  return 1;
}

/* ---- describing the globals ------------------------------------------------ */

/* Module variables sit at fixed addresses for the whole run, so one sweep at
 * the top of `main` describes them all and nothing ever retires them.
 *
 * This only matters for a pointer taken into a global and carried somewhere
 * else. Indexing one directly never reaches the map at all: the size is right
 * there in the program, so the check is a comparison against a constant, or is
 * proved away outright. */
static int safety_seed_global_pointer(IRInstructionVector *out, SourceLocation location,
                                       const char *name, size_t offset, int mode, size_t size) {
  char base_name[64], slot_name[64];
  snprintf(base_name, sizeof(base_name), ".safe_global_%u", g_safety_next_id++);
  snprintf(slot_name, sizeof(slot_name), ".safe_global_%u", g_safety_next_id++);
  IRInstruction take = {0};
  take.op = IR_OP_ADDRESS_OF;
  take.location = location;
  take.dest = ir_operand_temp(base_name);
  take.lhs = ir_operand_symbol(name);
  if (!take.dest.name || !take.lhs.name || !ir_instruction_vector_append_move(out, &take)) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }
  IRInstruction add = {0};
  add.op = IR_OP_BINARY;
  add.location = location;
  add.text = mettle_strdup("+");
  add.dest = ir_operand_temp(slot_name);
  add.lhs = ir_operand_temp(base_name);
  add.rhs = ir_operand_int((long long)offset);
  if (!add.text || !add.dest.name || !add.lhs.name || !ir_instruction_vector_append_move(out, &add)) {
    ir_instruction_destroy_storage(&add);
    return 0;
  }
  IROperand args[3] = {ir_operand_temp(slot_name), ir_operand_int(mode), ir_operand_int((long long)size)};
  int ok = args[0].name && safety_emit_call(out, location, "mettle_safety_global_pointer", args, 3);
  ir_operand_destroy(&args[0]);
  return ok;
}

/* main's parameters arrive from the process rather than from a compiled
 * caller, so the frame that normally carries a parameter's origin was never
 * pushed and the argument vector reads as an unknown pointer. Describe it at
 * entry instead, which is the only such parameter a program indexes. */
static int safety_describe_entry_arguments(IRProgram *program, IRFunction *entry) {
  if (entry->parameter_count < 2 || !entry->parameter_names ||
      !entry->parameter_names[1] || !entry->parameter_types) {
    return 1;
  }
  const MtlcType *type = ir_program_lookup_type(program, entry->parameter_types[1]);
  if (!type || type->kind != MTLC_TYPE_POINTER) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, entry->instruction_count + 1)) {
    return 0;
  }
  IROperand vector = ir_operand_symbol(entry->parameter_names[1]);
  int ok = vector.name != NULL &&
           safety_emit_call(&out, entry->location, "mettle_safety_entry_arguments",
                            &vector, 1);
  ir_operand_destroy(&vector);
  for (size_t i = 0; ok && i < entry->instruction_count; i++) {
    ok = ir_instruction_vector_append_move(&out, &entry->instructions[i]);
  }
  if (!ok || !ir_function_replace_instructions(entry, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(entry);
  return 1;
}

static int safety_describe_globals(IRProgram *program, IRFunction *entry) {
  size_t described = 0;
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind == IR_MODSYM_VARIABLE && !symbol->is_extern &&
        symbol->name && symbol->type && symbol->type->size > 0) {
      described++;
    }
  }
  if (described == 0) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out,
                                     entry->instruction_count + described * 2)) {
    return 0;
  }

  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
        !symbol->name || !symbol->type || symbol->type->size == 0) {
      continue;
    }

    char address[96];
    snprintf(address, sizeof(address), SAFETY_TEMP_PREFIX "g%u",
             g_safety_next_id++);
    IROperand size_operand = ir_operand_int((long long)symbol->type->size);

    /* The instruction gets its own copies: appending moves it into the vector,
     * which then owns whatever names it holds. */
    IRInstruction take = {0};
    take.op = IR_OP_ADDRESS_OF;
    take.location = entry->location;
    take.dest = ir_operand_temp(address);
    take.lhs = ir_operand_symbol(symbol->name);
    if (!take.dest.name || !take.lhs.name) {
      ir_instruction_destroy_storage(&take);
      ir_instruction_vector_destroy(&out);
      return 0;
    }
    if (!ir_instruction_vector_append_move(&out, &take)) {
      ir_instruction_destroy_storage(&take);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    IROperand address_operand = ir_operand_temp(address);
    IROperand static_args[2] = {address_operand, size_operand};
    int ok = address_operand.name &&
             safety_emit_call(&out, entry->location, "mettle_safety_register_static",
                              static_args, 2);
    ir_operand_destroy(&address_operand);
    if (!ok) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }

  /* All target regions must exist before seeding relocations, including a
   * pointer whose target appears later in the module's symbol table. */
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern || !symbol->type) continue;
    if (symbol->init_symbol_ref || symbol->init_string) {
      int mode = symbol->init_string ? (symbol->type->kind == MTLC_TYPE_STRING ? 3 : 1) : 0;
      if (!safety_seed_global_pointer(&out, entry->location, symbol->name, 0, mode,
                                      symbol->init_string_length + 1)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }
    for (size_t r = 0; r < symbol->init_reloc_count; r++) {
      const IRInitReloc *reloc = &symbol->init_relocs[r];
      int mode = reloc->string ? (reloc->string_wants_record ? 2 : 1) : 0;
      if (!safety_seed_global_pointer(&out, entry->location, symbol->name, reloc->offset,
                                      mode, reloc->string_length + 1)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }
  }
  for (size_t i = 0; i < entry->instruction_count; i++) {
    if (!ir_instruction_vector_append_move(&out, &entry->instructions[i])) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }
  if (!ir_function_replace_instructions(entry, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(entry);
  return 1;
}

/* A stack local is described at function entry, so the note outlives the block
 * the declaration sits in. When the optimizer folds that block away - a
 * constructor built under `if (1 == 2)`, say - the declaration goes and the
 * note is left taking the address of a local that no longer exists, which the
 * backend refuses. Nothing can reach an undeclared slot, so the note has
 * nothing left to describe: retire it and the address it took.
 *
 * Only stack notes. A global has no declaration in any function by
 * construction, and its note is exactly as good as it ever was. */
static int safety_retire_stack_notes(const IRProgram *program,
                                     IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *call = &function->instructions[i];
    if (call->op != IR_OP_CALL || !call->text || call->argument_count < 1 ||
        !call->arguments) {
      continue;
    }
    if (strcmp(call->text, "mettle_safety_register") != 0 &&
        strcmp(call->text, "mettle_safety_register_static") != 0 &&
        strcmp(call->text, "mettle_safety_unregister") != 0) {
      continue;
    }
    const IROperand *argument = &call->arguments[0];
    if (argument->kind != IR_OPERAND_TEMP || !argument->name) {
      continue;
    }

    /* The address is taken immediately before the call it feeds. */
    IRInstruction *take = NULL;
    for (size_t back = i; back-- > 0;) {
      IRInstruction *candidate = &function->instructions[back];
      if (candidate->op == IR_OP_NOP) {
        continue;
      }
      if (candidate->op == IR_OP_ADDRESS_OF &&
          candidate->dest.kind == IR_OPERAND_TEMP && candidate->dest.name &&
          strcmp(candidate->dest.name, argument->name) == 0) {
        take = candidate;
      }
      break;
    }
    if (!take || take->lhs.kind != IR_OPERAND_SYMBOL || !take->lhs.name) {
      continue;
    }
    if (ir_program_lookup_symbol(program, take->lhs.name)) {
      continue;
    }

    int declared = 0;
    /* A parameter has a slot and no declaration to prove it by. Retiring its
     * note left the slot undescribed, so it kept whatever dead descriptor the
     * previous frame retired over the same bytes and reading through
     * `&parameter` reported a use-after-free in a program that freed nothing. */
    for (size_t d = 0; d < function->parameter_count && !declared; d++) {
      if (function->parameter_names && function->parameter_names[d] &&
          strcmp(function->parameter_names[d], take->lhs.name) == 0) {
        declared = 1;
      }
    }
    for (size_t d = 0; d < function->instruction_count && !declared; d++) {
      const IRInstruction *candidate = &function->instructions[d];
      if (candidate->op == IR_OP_DECLARE_LOCAL &&
          candidate->dest.kind == IR_OPERAND_SYMBOL && candidate->dest.name &&
          strcmp(candidate->dest.name, take->lhs.name) == 0) {
        declared = 1;
      }
    }
    if (declared) {
      continue;
    }
    if (safety_trace_enabled()) {
      fprintf(stderr, "safety: retiring note for %s in %s\n", take->lhs.name,
              function->name ? function->name : "?");
    }
    ir_instruction_make_nop(take);
    ir_instruction_make_nop(call);
  }
  return 1;
}

int ir_safety_retire_dangling_notes(IRProgram *program) {
  if (!program) {
    return 1;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && !safety_retire_stack_notes(program, function)) {
      return 0;
    }
  }
  return 1;
}

#include "ir_safety_provenance.inc"

int ir_safety_register_allocations(IRProgram *program) {
  if (!program) {
    return 1;
  }
  g_safety_next_id = 0;
  if (!safety_declare_runtime(program)) return 0;
  if (!safety_normalize_external_calls(program) || !safety_wrap_allocator_addresses(program) ||
      !safety_normalize_external_calls(program)) return 0;
  const char *allocator_source = safety_allocator_source(program);
  IRFunction *entry = NULL;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    if (function->name && strcmp(function->name, "main") == 0) {
      entry = function;
    }
    /* Exempt for the same reason its accesses are: the calls it makes to
     * itself are the allocator working, not the program allocating, and
     * describing them would register a block once per layer. */
    if (safety_function_is_allocator(function, allocator_source)) {
      continue;
    }
    if (!safety_register_function(function) ||
        !safety_describe_stack(program, function)) {
      return 0;
    }
  }
  if (entry && (!safety_describe_globals(program, entry) ||
                !safety_describe_entry_arguments(program, entry))) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && !function->is_rule &&
        !safety_function_is_allocator(function, allocator_source) &&
        !safety_origins_function(program, function)) return 0;
  }
  return 1;
}
