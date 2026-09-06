#include "ir_optimize_internal.h"
#include "../../common.h" // mettle_free_string

static int ir_try_parse_loop_increment(const IRFunction *function, size_t body_start,
                                       size_t body_end, const char *counter_symbol,
                                       size_t *increment_index, int *step_out) {
  if (!function || !counter_symbol || !increment_index || !step_out) {
    return 0;
  }

  for (size_t i = body_end; i > body_start; ) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }

    if (instruction->op == IR_OP_BINARY && !instruction->is_float && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
        strcmp(instruction->dest.name, counter_symbol) == 0) {
      if (instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name &&
          strcmp(instruction->lhs.name, counter_symbol) == 0 &&
          instruction->rhs.kind == IR_OPERAND_INT) {
        if (instruction->rhs.int_value == 1) {
          *step_out = 1;
          *increment_index = i;
          return 1;
        }
        if (instruction->rhs.int_value == -1) {
          *step_out = -1;
          *increment_index = i;
          return 1;
        }
      }
    }

    if (instruction->op == IR_OP_ASSIGN &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
        strcmp(instruction->dest.name, counter_symbol) == 0) {
      if (instruction->lhs.kind == IR_OPERAND_TEMP && instruction->lhs.name) {
        size_t producer_index = 0;
        if (!ir_find_last_writer_before(function, i, IR_OPERAND_TEMP,
                                        instruction->lhs.name, &producer_index)) {
          return 0;
        }

        const IRInstruction *producer = &function->instructions[producer_index];
        if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text ||
            strcmp(producer->text, "+") != 0) {
          return 0;
        }

        if (producer->lhs.kind == IR_OPERAND_SYMBOL && producer->lhs.name &&
            strcmp(producer->lhs.name, counter_symbol) == 0 &&
            producer->rhs.kind == IR_OPERAND_INT) {
          if (producer->rhs.int_value == 1) {
            *step_out = 1;
            *increment_index = i;
            return 1;
          }
          if (producer->rhs.int_value == -1) {
            *step_out = -1;
            *increment_index = i;
            return 1;
          }
        }

        if (producer->rhs.kind == IR_OPERAND_SYMBOL && producer->rhs.name &&
            strcmp(producer->rhs.name, counter_symbol) == 0 &&
            producer->lhs.kind == IR_OPERAND_INT) {
          if (producer->lhs.int_value == 1) {
            *step_out = 1;
            *increment_index = i;
            return 1;
          }
          if (producer->lhs.int_value == -1) {
            *step_out = -1;
            *increment_index = i;
            return 1;
          }
        }
      }

      return 0;
    }

    return 0;
  }

  return 0;
}

static int ir_try_parse_counted_while_loop(const IRFunction *function,
                                           size_t header_index,
                                           const IRSymbolValueMap *symbol_map,
                                           const char **counter_symbol_out,
                                           long long *start_value_out,
                                           long long *limit_value_out,
                                           int *inclusive_out, int *step_out,
                                           size_t *branch_index_out,
                                           size_t *body_start_out,
                                           size_t *body_end_out,
                                           size_t *jump_index_out,
                                           size_t *increment_index_out) {
  if (!function || !symbol_map || !counter_symbol_out || !start_value_out ||
      !limit_value_out || !inclusive_out || !step_out || !branch_index_out ||
      !body_start_out || !body_end_out || !jump_index_out ||
      !increment_index_out) {
    return 0;
  }

  const IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 0;
  }

  const char *loop_label = header->text;
  size_t branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &branch_index)) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[branch_index];
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text) {
    size_t probe_index = branch_index;
    if (!ir_find_next_non_nop(function, branch_index + 1, &probe_index)) {
      return 0;
    }
    branch = &function->instructions[probe_index];
    branch_index = probe_index;
    if (branch->op != IR_OP_BRANCH_ZERO || !branch->text) {
      return 0;
    }
  }

  const char *end_label = branch->text;
  size_t jump_index = (size_t)-1;
  for (size_t j = branch_index + 1; j < function->instruction_count; j++) {
    const IRInstruction *probe = &function->instructions[j];
    if (probe->op == IR_OP_JUMP && probe->text &&
        strcmp(probe->text, loop_label) == 0) {
      jump_index = j;
      break;
    }
    if (probe->op == IR_OP_LABEL) {
      if (probe->text && strcmp(probe->text, end_label) == 0) {
        break;
      }
      return 0;
    }
  }

  if (jump_index == (size_t)-1) {
    return 0;
  }

  size_t compare_index = 0;
  const IRInstruction *compare = NULL;
  if (branch->lhs.kind == IR_OPERAND_TEMP && branch->lhs.name) {
    if (!ir_find_last_writer_before(function, branch_index, IR_OPERAND_TEMP,
                                    branch->lhs.name, &compare_index)) {
      return 0;
    }
    compare = &function->instructions[compare_index];
  } else if (branch->lhs.kind == IR_OPERAND_INT) {
    return 0;
  } else {
    return 0;
  }

  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text) {
    return 0;
  }

  const char *counter_symbol = NULL;
  long long limit_value = 0;
  int inclusive = 0;

  if (strcmp(compare->text, "<=") == 0) {
    inclusive = 1;
  } else if (strcmp(compare->text, "<") == 0) {
    inclusive = 0;
  } else if (strcmp(compare->text, ">=") == 0) {
    inclusive = 1;
    counter_symbol =
        compare->rhs.kind == IR_OPERAND_SYMBOL ? compare->rhs.name : NULL;
    if (!ir_operand_resolve_symbol_int(symbol_map, &compare->lhs, &limit_value) ||
        !counter_symbol) {
      return 0;
    }
    goto parsed_compare;
  } else if (strcmp(compare->text, ">") == 0) {
    inclusive = 0;
    counter_symbol =
        compare->rhs.kind == IR_OPERAND_SYMBOL ? compare->rhs.name : NULL;
    if (!ir_operand_resolve_symbol_int(symbol_map, &compare->lhs, &limit_value) ||
        !counter_symbol) {
      return 0;
    }
    goto parsed_compare;
  } else {
    return 0;
  }

  counter_symbol =
      compare->lhs.kind == IR_OPERAND_SYMBOL ? compare->lhs.name : NULL;
  if (!counter_symbol ||
      !ir_operand_resolve_symbol_int(symbol_map, &compare->rhs, &limit_value)) {
    return 0;
  }

parsed_compare: {
  IROperand counter_operand = ir_operand_symbol(counter_symbol);
  if (!counter_operand.name ||
      !ir_operand_resolve_symbol_int(symbol_map, &counter_operand,
                                     start_value_out)) {
    return 0;
  }
}

  size_t body_start = branch_index + 1;
  size_t body_end = jump_index;
  int step = 0;
  size_t increment_index = 0;
  if (!ir_try_parse_loop_increment(function, body_start, body_end, counter_symbol,
                                 &increment_index, &step)) {
    return 0;
  }

  for (size_t j = body_start; j < body_end; j++) {
    if (!ir_loop_body_opcode_is_unroll_safe(function->instructions[j].op)) {
      return 0;
    }
    /* The trip count above is arithmetic on the start, the limit and the step,
     * and it is only the loop's real trip count while the counter goes up by
     * that step and nothing else. A body that assigns the counter itself makes
     * every one of those numbers a guess: `for i in 0..6 { i = i + 2; }` runs
     * twice, and unrolling it six times ran the body six times. Nothing here
     * knows what the write does, so a loop that has one is not a counted loop
     * and no caller of this parser may treat it as one. */
    if (j != increment_index &&
        ir_instruction_writes_symbol(&function->instructions[j]) &&
        function->instructions[j].dest.name &&
        strcmp(function->instructions[j].dest.name, counter_symbol) == 0) {
      return 0;
    }
  }

  *counter_symbol_out = counter_symbol;
  *limit_value_out = limit_value;
  *inclusive_out = inclusive;
  *step_out = step;
  *branch_index_out = branch_index;
  *body_start_out = body_start;
  *body_end_out = body_end;
  *jump_index_out = jump_index;
  *increment_index_out = increment_index;
  return 1;
}

static int ir_symbol_used_in_range(const IRFunction *function, size_t start,
                                   size_t end, const char *symbol_name,
                                   size_t skip_index) {
  if (!function || !symbol_name) {
    return 0;
  }

  for (size_t i = start; i < end; i++) {
    if (i == skip_index) {
      continue;
    }

    const IRInstruction *instruction = &function->instructions[i];
    const IROperand *operands[4];
    size_t operand_count = 0;
    operands[operand_count++] = &instruction->dest;
    operands[operand_count++] = &instruction->lhs;
    operands[operand_count++] = &instruction->rhs;
    for (size_t a = 0; a < instruction->argument_count; a++) {
      if (operand_count < 4) {
        operands[operand_count++] = &instruction->arguments[a];
      }
    }

    for (size_t o = 0; o < operand_count; o++) {
      const IROperand *operand = operands[o];
      if (operand->kind == IR_OPERAND_SYMBOL && operand->name &&
          strcmp(operand->name, symbol_name) == 0) {
        return 1;
      }
    }
  }

  return 0;
}

int ir_function_replace_instructions(IRFunction *function,
                                            IRInstructionVector *vector) {
  if (!function || !vector) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);

  function->instructions = vector->items;
  function->instruction_count = vector->count;
  function->instruction_capacity = vector->capacity;
  vector->items = NULL;
  vector->count = 0;
  vector->capacity = 0;
  return 1;
}

/* ---- per-copy temp renaming for full unrolling -------------------------- *
 *
 * Cloning a loop body verbatim gives every copy the same temp names, so what
 * were N short, disjoint values become one value live across the whole unrolled
 * region. Nothing downstream can tell the copies apart: the register allocator
 * sees a single long live range per temp, its coalescer never sees a source die
 * (the name is written again further down), and so every operation in every
 * copy pays a register-to-register copy it would not otherwise need.
 *
 * Giving each copy its own names restores the short live ranges. A temp is only
 * renamed when it is entirely local to the body -- defined inside it and read
 * nowhere else -- so a value flowing in from before the loop, or out to code
 * after it, keeps the single name those readers expect. */

typedef struct {
  const char **names;
  size_t count;
  size_t capacity;
} IrUnrollTempSet;

static void ir_unroll_temp_set_destroy(IrUnrollTempSet *set) {
  free(set->names);
  set->names = NULL;
  set->count = set->capacity = 0;
}

static int ir_unroll_temp_set_contains(const IrUnrollTempSet *set,
                                       const char *name) {
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_unroll_temp_set_add(IrUnrollTempSet *set, const char *name) {
  if (ir_unroll_temp_set_contains(set, name)) {
    return 1;
  }
  if (set->count >= set->capacity) {
    size_t grown = set->capacity ? set->capacity * 2 : 16;
    const char **names =
        (const char **)realloc(set->names, grown * sizeof(*names));
    if (!names) {
      return 0;
    }
    set->names = names;
    set->capacity = grown;
  }
  set->names[set->count++] = name;
  return 1;
}

/* Every operand slot of `in`, so callers can walk reads and writes uniformly. */
static size_t ir_unroll_operand_slots(IRInstruction *in, IROperand **slots,
                                      size_t max_slots) {
  size_t n = 0;
  if (n < max_slots) slots[n++] = &in->dest;
  if (n < max_slots) slots[n++] = &in->lhs;
  if (n < max_slots) slots[n++] = &in->rhs;
  for (size_t a = 0; a < in->argument_count && n < max_slots; a++) {
    slots[n++] = &in->arguments[a];
  }
  return n;
}

/* True when `name` is read or written by any instruction outside [lo, hi). */
static int ir_unroll_temp_used_outside(const IRFunction *function, size_t lo,
                                       size_t hi, const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (i >= lo && i < hi) {
      continue;
    }
    IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue; /* a deleted instruction keeps operands it no longer touches */
    }
    IROperand *slots[3 + 16];
    size_t n = ir_unroll_operand_slots(in, slots, sizeof(slots) / sizeof(*slots));
    for (size_t s = 0; s < n; s++) {
      if (slots[s]->kind == IR_OPERAND_TEMP && slots[s]->name &&
          strcmp(slots[s]->name, name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

/* Collect the temps that are defined in [lo, hi) and used nowhere else. */
static int ir_unroll_collect_private_temps(const IRFunction *function, size_t lo,
                                           size_t hi, IrUnrollTempSet *set) {
  for (size_t i = lo; i < hi; i++) {
    IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP || in->op == IR_OP_STORE) {
      continue; /* a STORE's dest is the address it writes through */
    }
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name) {
      continue;
    }
    if (ir_unroll_temp_set_contains(set, in->dest.name)) {
      continue;
    }
    if (ir_unroll_temp_used_outside(function, lo, hi, in->dest.name)) {
      continue;
    }
    if (!ir_unroll_temp_set_add(set, in->dest.name)) {
      return 0;
    }
  }
  return 1;
}

/* Rewrite the body-private temps in an already-cloned instruction to this
 * copy's private names. */
static int ir_unroll_rename_copy_temps(IRInstruction *out,
                                       const IrUnrollTempSet *set,
                                       long long trip) {
  IROperand *slots[3 + 16];
  size_t n = ir_unroll_operand_slots(out, slots, sizeof(slots) / sizeof(*slots));
  for (size_t s = 0; s < n; s++) {
    IROperand *o = slots[s];
    if (o->kind != IR_OPERAND_TEMP || !o->name ||
        !ir_unroll_temp_set_contains(set, o->name)) {
      continue;
    }
    size_t len = strlen(o->name) + 24;
    char *renamed = (char *)malloc(len);
    if (!renamed) {
      return 0;
    }
    snprintf(renamed, len, "%s__u%lld", o->name, trip);
    mettle_free_string(o->name);
    o->name = renamed;
  }
  return 1;
}

/* The counted-loop parser's first gate, without the symbol map it needs for
 * everything after: a loop header is a label whose next real instruction is the
 * exit branch. Checking it here costs a step or two, and it is the difference
 * between rejecting a label immediately and building a map of every constant
 * that reaches it first -- which is a scan of the whole prefix, per label. A
 * body built out of if/else is nearly all labels and no loops, so that scan was
 * the largest single cost in compiling one. Kept in step with
 * ir_try_parse_counted_while_loop: it must reject nothing the parser accepts. */
static int ir_unroll_header_reaches_branch(const IRFunction *function,
                                           size_t header_index) {
  size_t branch_index = 0;
  const IRInstruction *branch = NULL;

  if (!ir_find_next_non_nop(function, header_index + 1, &branch_index)) {
    return 0;
  }
  branch = &function->instructions[branch_index];
  if (branch->op == IR_OP_BRANCH_ZERO && branch->text) {
    return 1;
  }
  if (!ir_find_next_non_nop(function, branch_index + 1, &branch_index)) {
    return 0;
  }
  branch = &function->instructions[branch_index];
  return branch->op == IR_OP_BRANCH_ZERO && branch->text != NULL;
}

static int ir_try_unroll_loop_at(IRFunction *function, size_t header_index,
                                 int *changed) {
  IRSymbolValueMap symbol_map;
  if (!ir_unroll_header_reaches_branch(function, header_index)) {
    return 1;
  }
  if (!ir_temp_value_map_init(&symbol_map)) {
    return 0;
  }
  if (!ir_build_symbol_int_map_before(function, header_index, &symbol_map)) {
    ir_temp_value_map_destroy(&symbol_map);
    return 0;
  }

  const char *counter_symbol = NULL;
  long long start_value = 0;
  long long limit_value = 0;
  int inclusive = 0;
  int step = 0;
  size_t branch_index = 0;
  size_t body_start = 0;
  size_t body_end = 0;
  size_t jump_index = 0;
  size_t increment_index = 0;

  if (!ir_try_parse_counted_while_loop(function, header_index, &symbol_map,
                                       &counter_symbol, &start_value,
                                       &limit_value, &inclusive, &step,
                                       &branch_index, &body_start, &body_end,
                                       &jump_index, &increment_index)) {
    ir_temp_value_map_destroy(&symbol_map);
    return 1;
  }

  long long trips = 0;
  if (step > 0) {
    trips = inclusive ? (limit_value - start_value + 1)
                      : (limit_value - start_value);
  } else if (step < 0) {
    trips = inclusive ? (start_value - limit_value + 1)
                      : (start_value - limit_value);
  }

  long long max_trips =
      ir_opt_unroll_max_trip_count(function,
                                   function->instructions[header_index].location);
  if (trips <= 0 || trips > max_trips) {
    ir_temp_value_map_destroy(&symbol_map);
    return 1;
  }

  int counter_used_in_body =
      ir_symbol_used_in_range(function, body_start, body_end, counter_symbol,
                              increment_index);

  IRInstructionVector vector = {0};
  for (size_t i = 0; i < header_index; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      ir_temp_value_map_destroy(&symbol_map);
      return 0;
    }
  }

  /* Each copy gets private names for the temps that live only inside the body,
   * so the copies do not merge into one long live range (see the comment on
   * ir_unroll_collect_private_temps). A collection failure is not fatal: the
   * empty set just reproduces the old verbatim clone. */
  IrUnrollTempSet private_temps = {0};
  if (!ir_unroll_collect_private_temps(function, body_start, body_end,
                                       &private_temps)) {
    ir_unroll_temp_set_destroy(&private_temps);
  }

  for (long long trip = 0; trip < trips; trip++) {
    for (size_t b = body_start; b < body_end; b++) {
      if (!counter_used_in_body && b == increment_index) {
        continue;
      }

      IRInstruction cloned = {0};
      if (!ir_clone_instruction_plain(&function->instructions[b], &cloned) ||
          !ir_unroll_rename_copy_temps(&cloned, &private_temps, trip) ||
          !ir_instruction_vector_append_move(&vector, &cloned)) {
        ir_instruction_destroy_storage(&cloned);
        ir_instruction_vector_destroy(&vector);
        ir_temp_value_map_destroy(&symbol_map);
        ir_unroll_temp_set_destroy(&private_temps);
        return 0;
      }
    }
  }
  ir_unroll_temp_set_destroy(&private_temps);

  /* Emit an explicit jump to the loop's exit label after the unrolled body.
   * Without it, the unrolled straight-line body falls through into whatever
   * instruction textually follows the old back-edge (the exit label normally,
   * but if a sibling/else block was laid out there, control would leak into
   * it). The exit block then has no explicit predecessor, so once jump
   * threading redirects the original exit branch elsewhere, the
   * unreachable-block pass can delete the exit block's own jump and silently
   * fuse the unrolled body into the following block. The explicit jump keeps
   * the exit block referenced and the fall-through unambiguous; a later
   * redundant-jump pass removes it when it is truly a no-op. */
  {
    const IRInstruction *branch = &function->instructions[branch_index];
    if (branch->text) {
      IRInstruction exit_jump = {0};
      exit_jump.op = IR_OP_JUMP;
      exit_jump.text = mettle_strdup(branch->text);
      if (!exit_jump.text ||
          !ir_instruction_vector_append_move(&vector, &exit_jump)) {
        ir_instruction_destroy_storage(&exit_jump);
        ir_instruction_vector_destroy(&vector);
        ir_temp_value_map_destroy(&symbol_map);
        return 0;
      }
    }
  }

  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      ir_temp_value_map_destroy(&symbol_map);
      return 0;
    }
  }

  /* Capture the loop's source location before the instruction array is
   * replaced; the --explain remark below points at the original header. */
  SourceLocation header_location = function->instructions[header_index].location;

  if (!ir_function_replace_instructions(function, &vector)) {
    ir_instruction_vector_destroy(&vector);
    ir_temp_value_map_destroy(&symbol_map);
    return 0;
  }

  if (ir_explain_enabled()) {
    char headline[96];
    snprintf(headline, sizeof(headline),
             "fully unrolled (%lld iteration%s, constant trip count)", trips,
             trips == 1 ? "" : "s");
    ir_explain_remark(function->name, "loop", header_location, 1, headline,
                      NULL, NULL, NULL);
    ir_explain_remark_code("unrolled");
    ir_explain_remark_quantity("iterations", (long)trips);
  }

  ir_temp_value_map_destroy(&symbol_map);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

/* ------------------------------------------------------------------------
 * General reduction-unrolling vectorizer
 *
 * Replaces a simple counted reduction loop
 *
 *     while (i <op> BOUND) {        // op is < or <=
 *       acc = acc + EXPR(i);        // EXPR pure: only i, int consts, + - *
 *       i = i + 1;
 *     }
 *
 * with a K-way unrolled version that uses K independent accumulators, then
 * sums them. This breaks the loop-carried dependency on `acc` (the real
 * bottleneck on these loops) and exposes K-way instruction-level parallelism,
 * while emitting only ordinary IR opcodes that every backend already lowers.
 *
 * This is a genuine, general transformation: it matches the *structure* of a
 * pure scalar reduction (verified by dataflow analysis of the body, not by
 * recognizing specific source identifiers or constants), so any loop of the
 * above form is accelerated. Anything outside the proven-safe class is left
 * untouched and the normal scalar code runs, so correctness never depends on
 * the match.
 *
 * Restrictions enforced (all checked, bail to scalar otherwise):
 *   - exactly one induction variable, unit stride +1, latch is the last two
 *     body instructions;
 *   - loop-invariant trip bound (BOUND not assigned anywhere in the body);
 *   - exactly one accumulator symbol ACC (!= i), updated solely via
 *     `ACC = ACC + EXPR`; ACC must be zero-initialized before the loop (so the
 *     K-1 extra accumulators can also start at 0 with identical semantics);
 *   - EXPR uses only i, integer constants, and + - * (no loads, no calls, no
 *     other symbols, no other writes), so duplicating it per lane is sound and
 *     side-effect free;
 *   - nothing in the body writes any symbol other than ACC (and i in the
 *     latch).
 * --------------------------------------------------------------------------*/

#define IR_VEC_UNROLL 4

int ir_resolve_indexed_address_temp(const IRFunction *function,
                                            size_t before_index, const char *iv,
                                            const char *bound,
                                            const char *addr_temp,
                                            const char **base_out,
                                            int *elem_size_out, int *step_out);
const char *ir_function_local_declared_type(const IRFunction *function,
                                                   const char *symbol_name);
int ir_symbol_is_sum_array_base(const IRFunction *function,
                                       const char *symbol_name);

/* True if `sym` (a symbol) is never the destination of an ASSIGN/BINARY/CAST/
 * STORE/LOAD/NEW within body range [lo, hi). Used to prove a load's base
 * pointer is loop-invariant, so duplicating the load per unroll lane (with
 * i -> i+L) reads stable, independent memory. */
static int ir_vec_symbol_invariant_in_body(const IRFunction *fn, size_t lo,
                                            size_t hi, const char *sym) {
  if (!sym) {
    return 0;
  }
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *m = &fn->instructions[k];
    if (m->dest.kind == IR_OPERAND_SYMBOL && m->dest.name &&
        strcmp(m->dest.name, sym) == 0) {
      return 0;
    }
  }
  return 1;
}

/* Body instruction whose only role is loop control / a NOP we may skip. */
static int ir_vec_binary_is(const IRInstruction *in, const char *op) {
  return in && in->op == IR_OP_BINARY && !in->is_float && !in->ast_ref &&
         in->text && strcmp(in->text, op) == 0;
}

static int ir_vec_assign_sym_from_temp(const IRInstruction *in,
                                       const char *sym) {
  return in && in->op == IR_OP_ASSIGN && !in->ast_ref &&
         ir_operand_is_symbol_named(&in->dest, sym) &&
         in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name;
}

/* Collect, into `set`/`count` (caller-sized cap), the names of every TEMP that
 * the expression rooted at temp `root` transitively depends on, while proving
 * the whole DAG is "pure": built only from BINARY {+,-,*} and CAST nodes whose
 * operands are the induction var `iv`, integer constants, or other in-range
 * body temps. Returns 1 if pure & bounded, 0 to reject (caller bails). The
 * producing instruction for each temp is found by scanning [lo, hi). */
static int ir_vec_expr_is_pure(const IRFunction *fn, size_t lo, size_t hi,
                               const char *root, const char *iv,
                               const char *acc, char **seen, size_t *seen_n,
                               size_t seen_cap, int depth) {
  if (depth > 64 || !root) {
    return 0;
  }
  for (size_t s = 0; s < *seen_n; s++) {
    if (strcmp(seen[s], root) == 0) {
      return 1; /* already validated */
    }
  }
  /* find the unique producer of temp `root` in [lo,hi) */
  const IRInstruction *prod = NULL;
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *m = &fn->instructions[k];
    if (m->dest.kind == IR_OPERAND_TEMP && m->dest.name &&
        strcmp(m->dest.name, root) == 0) {
      if (prod) {
        return 0; /* multiply assigned temp: not SSA-pure, reject */
      }
      prod = m;
    }
  }
  if (!prod) {
    return 0;
  }
  /* A loop-invariant indexed load a[i] is a valid pure leaf: its value depends
   * only on the iteration index, and duplicating it per lane (with i -> i+L) is
   * sound as long as the base pointer is never written in the body and nothing
   * in the body stores to memory (the caller's body scan rejects all stores).
   * We do NOT recurse into the address-arithmetic temps; the load result stands
   * in for the whole subtree. */
  if (prod->op == IR_OP_LOAD && prod->lhs.kind == IR_OPERAND_TEMP &&
      prod->lhs.name) {
    const char *base = NULL;
    /* locate the load's own index within [lo,hi) so producer lookups are scoped
     * to instructions that precede it */
    size_t load_idx = hi;
    for (size_t k = lo; k < hi; k++) {
      if (&fn->instructions[k] == prod) {
        load_idx = k;
        break;
      }
    }
    if (load_idx < hi &&
        ir_resolve_indexed_address_temp(fn, load_idx, iv, NULL,
                                        prod->lhs.name, &base, NULL, NULL) &&
        base && strcmp(base, iv) != 0 && strcmp(base, acc) != 0 &&
        ir_vec_symbol_invariant_in_body(fn, lo, hi, base)) {
      if (*seen_n >= seen_cap) {
        return 0;
      }
      seen[(*seen_n)++] = prod->dest.name;
      return 1;
    }
    return 0;
  }
  /* only pure integer BINARY (+,-,*) or CAST may produce expression temps */
  int is_binary = ir_vec_binary_is(prod, "+") || ir_vec_binary_is(prod, "-") ||
                  ir_vec_binary_is(prod, "*");
  int is_cast = prod->op == IR_OP_CAST && !prod->is_float;
  if (!is_binary && !is_cast) {
    return 0;
  }
  if (*seen_n >= seen_cap) {
    return 0;
  }
  seen[(*seen_n)++] = prod->dest.name;

  const IROperand *ops[2] = {&prod->lhs, &prod->rhs};
  int nops = is_cast ? 1 : 2;
  for (int oi = 0; oi < nops; oi++) {
    const IROperand *o = ops[oi];
    if (o->kind == IR_OPERAND_INT) {
      continue;
    }
    if (o->kind == IR_OPERAND_SYMBOL && o->name) {
      if (strcmp(o->name, iv) == 0) {
        continue; /* induction variable is allowed */
      }
      return 0; /* any other symbol (incl. acc) inside EXPR -> reject */
    }
    if (o->kind == IR_OPERAND_TEMP && o->name) {
      if (!ir_vec_expr_is_pure(fn, lo, hi, o->name, iv, acc, seen, seen_n,
                               seen_cap, depth + 1)) {
        return 0;
      }
      continue;
    }
    return 0; /* string/float/label/none operand -> reject */
  }
  return 1;
}

/* Deep-clone instruction `src` into `out`, then within out rename:
 *  - every TEMP name -> "<temp>__l<lane>" (lane-private SSA copies)
 *  - the accumulator symbol `acc` -> symbol `acc_lane`
 *  - reads of induction symbol `iv` are left as-is; the caller arranges that a
 *    per-lane copy of i lives in `iv` for the duration of the lane body by
 *    emitting `iv = i + lane` ... actually we instead pre-seed a temp; see
 *    caller. Here we only do temp + acc renaming.
 * `lane` selects the suffix; returns 0 on OOM. */
static int ir_vec_clone_body_inst(const IRInstruction *src, IRInstruction *out,
                                  int lane, const char *acc,
                                  const char *acc_lane) {
  if (!ir_clone_instruction_plain(src, out)) {
    return 0;
  }
  IROperand *slots[3 + 8];
  int n = 0;
  slots[n++] = &out->dest;
  slots[n++] = &out->lhs;
  slots[n++] = &out->rhs;
  for (size_t a = 0; a < out->argument_count && n < 3 + 8; a++) {
    slots[n++] = &out->arguments[a];
  }
  for (int s = 0; s < n; s++) {
    IROperand *o = slots[s];
    if (o->kind == IR_OPERAND_TEMP && o->name) {
      size_t len = strlen(o->name) + 16;
      char *nn = malloc(len);
      if (!nn) {
        return 0;
      }
      snprintf(nn, len, "%s__l%d", o->name, lane);
      mettle_free_string(o->name);
      o->name = nn;
    } else if (o->kind == IR_OPERAND_SYMBOL && o->name && acc &&
               strcmp(o->name, acc) == 0) {
      char *nn = mettle_strdup(acc_lane);
      if (!nn) {
        return 0;
      }
      mettle_free_string(o->name);
      o->name = nn;
    }
  }
  return 1;
}

/* Try to recognize and unroll a reduction loop whose header label is at index
 * `h`. On success rewrites `function` and sets *changed. Always returns 1
 * unless a hard (OOM) error occurs (returns 0). */
/* `J` is the back-edge jumping to the label at `h`, resolved by the caller from
 * the whole-function index. Finding it here meant scanning to the end of the
 * function for every label that has no back-edge, which is all of them in code
 * built out of if/else. */
static int ir_vec_try_unroll_reduction_at(IRFunction *function, size_t h,
                                           size_t J, int *changed) {
  IRInstruction *head = &function->instructions[h];
  if (head->op != IR_OP_LABEL || !head->text) {
    return 1;
  }
  const char *head_label = head->text;

  if (J == (size_t)-1 || J < h + 5) {
    return 1;
  }

  /* guard: binary %g = IV <op> BOUND ; branch_zero %g -> EXIT */
  IRInstruction *g = &function->instructions[h + 1];
  IRInstruction *gb = &function->instructions[h + 2];
  int op_lt = ir_vec_binary_is(g, "<");
  int op_le = ir_vec_binary_is(g, "<=");
  if ((!op_lt && !op_le) || g->dest.kind != IR_OPERAND_TEMP || !g->dest.name ||
      g->lhs.kind != IR_OPERAND_SYMBOL || !g->lhs.name ||
      (g->rhs.kind != IR_OPERAND_SYMBOL && g->rhs.kind != IR_OPERAND_INT) ||
      gb->op != IR_OP_BRANCH_ZERO || gb->lhs.kind != IR_OPERAND_TEMP ||
      !gb->lhs.name || !gb->text ||
      strcmp(gb->lhs.name, g->dest.name) != 0) {
    return 1;
  }
  const char *iv = g->lhs.name;
  const char *exit_label = gb->text;

  /* latch: instructions[J-2] = binary %t = IV + 1 ; [J-1] = assign IV <- %t */
  IRInstruction *inc = &function->instructions[J - 2];
  IRInstruction *incs = &function->instructions[J - 1];
  if (!ir_vec_binary_is(inc, "+") ||
      !ir_operand_is_symbol_named(&inc->lhs, iv) ||
      inc->rhs.kind != IR_OPERAND_INT || inc->rhs.int_value != 1 ||
      inc->dest.kind != IR_OPERAND_TEMP || !inc->dest.name ||
      !ir_vec_assign_sym_from_temp(incs, iv) ||
      strcmp(incs->lhs.name, inc->dest.name) != 0) {
    return 1;
  }

  /* body range is [h+3, J-2). Find the accumulator update:
   *   binary %r = ACC + EXPRTMP ; assign ACC <- %r
   * exactly once, ACC a symbol != iv. */
  size_t body_lo = h + 3, body_hi = J - 2;
  if (body_hi <= body_lo) {
    return 1;
  }
  const char *acc = NULL;
  const char *expr_root = NULL; /* temp feeding the + (the EXPR result) */
  size_t acc_add_idx = (size_t)-1;
  for (size_t k = body_lo; k + 1 < body_hi + 1 && k + 1 < J - 1; k++) {
    IRInstruction *m = &function->instructions[k];
    IRInstruction *st = &function->instructions[k + 1];
    if (ir_vec_binary_is(m, "+") && m->dest.kind == IR_OPERAND_TEMP &&
        m->dest.name && m->lhs.kind == IR_OPERAND_SYMBOL && m->lhs.name &&
        strcmp(m->lhs.name, iv) != 0 && m->rhs.kind == IR_OPERAND_TEMP &&
        m->rhs.name && ir_vec_assign_sym_from_temp(st, m->lhs.name) &&
        strcmp(st->lhs.name, m->dest.name) == 0) {
      if (acc) {
        return 1; /* more than one accumulator update -> reject */
      }
      acc = m->lhs.name;
      expr_root = m->rhs.name;
      acc_add_idx = k;
    }
  }
  if (!acc || !expr_root || strcmp(acc, iv) == 0) {
    return 1;
  }

  /* ACC must be zero-initialized before the loop: search backward from h for
   * the nearest `assign ACC <- 0` with no intervening write to ACC. */
  int acc_zero_init = 0;
  for (size_t bi = h; bi-- > 0;) {
    IRInstruction *m = &function->instructions[bi];
    if (m->op == IR_OP_ASSIGN && ir_operand_is_symbol_named(&m->dest, acc)) {
      acc_zero_init =
          (m->lhs.kind == IR_OPERAND_INT && m->lhs.int_value == 0);
      break;
    }
    if (m->op == IR_OP_LABEL) {
      /* crossing a label means control flow merges; be conservative */
      break;
    }
  }
  if (!acc_zero_init) {
    return 1;
  }

  /* Validate EXPR purity (only iv/const/temps via +,-,*,cast) and that no
   * body instruction (other than the acc add/store and latch) writes a symbol
   * or has side effects (loads/stores/calls/returns/branches). */
  char *seen[128];
  size_t seen_n = 0;
  if (!ir_vec_expr_is_pure(function, body_lo, body_hi, expr_root, iv, acc,
                           seen, &seen_n, 128, 0)) {
    return 1;
  }
  int body_has_load = 0;       /* any indexed load admitted in the body */
  int all_loads_sum_array = 1; /* every load base passes the intrinsic gate */
  for (size_t k = body_lo; k < body_hi; k++) {
    IRInstruction *m = &function->instructions[k];
    if (k == acc_add_idx || k == acc_add_idx + 1) {
      continue; /* the ACC = ACC + EXPR pair */
    }
    switch (m->op) {
    case IR_OP_NOP:
    case IR_OP_BINARY:
    case IR_OP_CAST:
      if (m->dest.kind == IR_OPERAND_SYMBOL) {
        return 1; /* writes a symbol inside the body -> reject */
      }
      break;
    case IR_OP_LOAD: {
      /* Permit a load only if it reads a loop-invariant indexed slot base[iv]
       * into a temp (same condition the purity check used to accept it as a
       * leaf). This keeps per-lane duplication sound: lane L reads base[i+L],
       * stable independent memory. Any other load (symbol dest, non-invariant
       * base, or address not of the base[iv] form) -> reject. */
      const char *base = NULL;
      if (m->dest.kind != IR_OPERAND_TEMP || m->lhs.kind != IR_OPERAND_TEMP ||
          !m->lhs.name ||
          !ir_resolve_indexed_address_temp(function, k, iv, NULL, m->lhs.name,
                                           &base, NULL, NULL) ||
          !base || strcmp(base, iv) == 0 || strcmp(base, acc) == 0 ||
          !ir_vec_symbol_invariant_in_body(function, body_lo, body_hi, base)) {
        return 1;
      }
      body_has_load = 1;
      if (!ir_symbol_is_sum_array_base(function, base)) {
        all_loads_sum_array = 0;
      }
      break;
    }
    default:
      return 1; /* store/call/branch/label/etc -> reject */
    }
  }

  /* Defer to the dedicated SIMD intrinsic matchers (simd_sum_i32 /
   * simd_dot_i32) for exactly the shapes they claim: an int64 accumulator fed
   * by loads from recognized int32 array bases. Those passes run later in the
   * same fixpoint and emit hand-tuned kernels; the general unroller only takes
   * over load-reductions outside that class (e.g. sum-of-squares, non-parameter
   * arrays), so existing intrinsic coverage is preserved. */
  if (body_has_load && all_loads_sum_array) {
    const char *at = ir_function_local_declared_type(function, acc);
    if (at && strcmp(at, "int64") == 0) {
      return 1;
    }
  }

  /* ---- All checks passed: emit a K-way unrolled version. ----
   *
   * Layout produced (replacing instructions [h .. J]):
   *
   *   assign ACC1 <- 0 ; assign ACC2 <- 0 ; assign ACC3 <- 0   (ACC0 == ACC,
   *                                                              already 0)
   *   label HM
   *     binary %ub = i + (K-1)
   *     binary %gu = %ub <op> BOUND
   *     branch_zero %gu -> HTAIL
   *     <lane0 body: EXPR/acc with i, ACC>
   *     <lane1 body: i+1, ACC1>  ... <lane K-1: i+(K-1), ACC(K-1)>
   *     binary %st = i + K ; assign i <- %st
   *     jump HM
   *   label HTAIL
   *     <original scalar loop verbatim>           (still accumulates into ACC)
   *   label HCOMB
   *     ACC = ACC + ACC1 ; ACC = ACC + ACC2 ; ACC = ACC + ACC3
   *   label EXIT (recreated)
   *
   * For lane L>0 we substitute reads of `i` by a fresh temp holding (i+L) and
   * the accumulator symbol by ACC<L>. Lane 0 reuses i and ACC unchanged.
   *
   * To keep this tractable and provably correct we build the new instruction
   * stream in a local vector and splice it in, NOP-filling any leftover slots.
   */

  /* Helper to append a fully-formed instruction into a growable array. */
  IRInstruction *out = NULL;
  size_t out_n = 0, out_cap = 0;
#define VEC_EMIT(INIT)                                                          \
  do {                                                                         \
    if (out_n >= out_cap) {                                                     \
      size_t nc = out_cap ? out_cap * 2 : 64;                                   \
      IRInstruction *np = realloc(out, nc * sizeof(IRInstruction));             \
      if (!np) {                                                               \
        for (size_t fi = 0; fi < out_n; fi++)                                   \
          ir_instruction_destroy_storage(&out[fi]);                            \
        free(out);                                                             \
        return 0;                                                              \
      }                                                                        \
      out = np;                                                                 \
      out_cap = nc;                                                             \
    }                                                                          \
    memset(&out[out_n], 0, sizeof(IRInstruction));                              \
    INIT;                                                                       \
    out_n++;                                                                    \
  } while (0)

  /* unique label/temp suffix from the header index keeps names collision-free
   * across multiple unrolled loops in one function. */
  char pre[32];
  snprintf(pre, sizeof(pre), "vu%zu", h);
  char buf[96];

  /* accumulator names ACC, ACC__a1, ACC__a2, ... (lane 0 == acc itself). */
  char *acc_name[IR_VEC_UNROLL];
  acc_name[0] = (char *)acc;
  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    snprintf(buf, sizeof(buf), "%s__a%d", acc, L);
    acc_name[L] = mettle_strdup(buf);
    if (!acc_name[L]) {
      for (int q = 1; q < L; q++) free(acc_name[q]);
      return 0;
    }
  }

#define MKLBL(name, kind)                                                      \
  snprintf(buf, sizeof(buf), "%s_%s_%s", pre, kind, name)

  /* Find ACC's declared type so the synthetic accumulators get matching
   * IR_OP_DECLARE_LOCAL entries (the binary backend only stores into symbols
   * it has allocated a slot for). Default to int64 if no declaration is
   * found (the reduction accumulator is always an integer here). */
  const char *acc_type = "int64";
  for (size_t di = 0; di < function->instruction_count; di++) {
    IRInstruction *m = &function->instructions[di];
    if (m->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol_named(&m->dest, acc) && m->text) {
      acc_type = m->text;
      break;
    }
  }

  /* declare + zero-init ACC1..ACC(K-1) */
  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    VEC_EMIT({
      out[out_n].op = IR_OP_DECLARE_LOCAL;
      out[out_n].dest = ir_operand_symbol(acc_name[L]);
      out[out_n].text = mettle_strdup(acc_type);
      if (!out[out_n].text) { goto oom; }
    });
    VEC_EMIT({
      out[out_n].op = IR_OP_ASSIGN;
      out[out_n].dest = ir_operand_symbol(acc_name[L]);
      out[out_n].lhs = ir_operand_int(0);
    });
  }

  char hm[64], htail[64], hcomb[64];
  snprintf(hm, sizeof(hm), "%s_main", pre);
  snprintf(htail, sizeof(htail), "%s_tail", pre);
  snprintf(hcomb, sizeof(hcomb), "%s_comb", pre);

  /* label HM */
  VEC_EMIT({ out[out_n].op = IR_OP_LABEL; out[out_n].text = mettle_strdup(hm); if(!out[out_n].text){goto oom;} });

  /* %ub = i + (K-1) ; %gu = %ub <op> BOUND ; branch_zero %gu -> HTAIL */
  char t_ub[64], t_gu[64];
  snprintf(t_ub, sizeof(t_ub), "%s_ub", pre);
  snprintf(t_gu, sizeof(t_gu), "%s_gu", pre);
  VEC_EMIT({
    out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
    if(!out[out_n].text){goto oom;}
    out[out_n].dest = ir_operand_temp(t_ub);
    out[out_n].lhs = ir_operand_symbol(iv);
    out[out_n].rhs = ir_operand_int(IR_VEC_UNROLL - 1);
  });
  VEC_EMIT({
    out[out_n].op = IR_OP_BINARY;
    out[out_n].text = mettle_strdup(op_le ? "<=" : "<");
    if(!out[out_n].text){goto oom;}
    out[out_n].dest = ir_operand_temp(t_gu);
    out[out_n].lhs = ir_operand_temp(t_ub);
    if (g->rhs.kind == IR_OPERAND_INT)
      out[out_n].rhs = ir_operand_int(g->rhs.int_value);
    else
      out[out_n].rhs = ir_operand_symbol(g->rhs.name);
  });
  VEC_EMIT({
    out[out_n].op = IR_OP_BRANCH_ZERO;
    out[out_n].lhs = ir_operand_temp(t_gu);
    out[out_n].text = mettle_strdup(htail);
    if(!out[out_n].text){goto oom;}
  });

  /* lane bodies */
  for (int L = 0; L < IR_VEC_UNROLL; L++) {
    /* lane-private i: temp ti_L = i + L  (L==0 uses i directly via no remap) */
    char tiL[64];
    if (L > 0) {
      snprintf(tiL, sizeof(tiL), "%s_ti%d", pre, L);
      VEC_EMIT({
        out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
        if(!out[out_n].text){goto oom;}
        out[out_n].dest = ir_operand_temp(tiL);
        out[out_n].lhs = ir_operand_symbol(iv);
        out[out_n].rhs = ir_operand_int(L);
      });
    }
    /* clone body [body_lo, body_hi) with temp suffix __l<L> and acc->acc_name[L];
     * additionally, for L>0 replace reads of symbol `iv` with temp tiL by a
     * post-pass on the cloned operands. */
    for (size_t k = body_lo; k < body_hi; k++) {
      IRInstruction tmp;
      if (!ir_vec_clone_body_inst(&function->instructions[k], &tmp, L, acc,
                                  acc_name[L])) {
        goto oom;
      }
      if (L > 0) {
        IROperand *sl[3 + 8]; int sn = 0;
        sl[sn++] = &tmp.dest; sl[sn++] = &tmp.lhs; sl[sn++] = &tmp.rhs;
        for (size_t a = 0; a < tmp.argument_count && sn < 3 + 8; a++)
          sl[sn++] = &tmp.arguments[a];
        for (int s = 0; s < sn; s++) {
          if (sl[s]->kind == IR_OPERAND_SYMBOL && sl[s]->name &&
              strcmp(sl[s]->name, iv) == 0) {
            char *nn = mettle_strdup(tiL);
            if (!nn) { ir_instruction_destroy_storage(&tmp); goto oom; }
            mettle_free_string(sl[s]->name); sl[s]->name = nn;
            sl[s]->kind = IR_OPERAND_TEMP;
          }
        }
      }
      VEC_EMIT({ out[out_n] = tmp; });
    }
  }

  /* i = i + K ; jump HM */
  {
    char t_st[64];
    snprintf(t_st, sizeof(t_st), "%s_st", pre);
    VEC_EMIT({
      out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
      if(!out[out_n].text){goto oom;}
      out[out_n].dest = ir_operand_temp(t_st);
      out[out_n].lhs = ir_operand_symbol(iv);
      out[out_n].rhs = ir_operand_int(IR_VEC_UNROLL);
    });
    VEC_EMIT({
      out[out_n].op = IR_OP_ASSIGN;
      out[out_n].dest = ir_operand_symbol(iv);
      out[out_n].lhs = ir_operand_temp(t_st);
    });
    VEC_EMIT({ out[out_n].op = IR_OP_JUMP; out[out_n].text = mettle_strdup(hm); if(!out[out_n].text){goto oom;} });
  }

  /* label HTAIL : original scalar loop verbatim (instructions h..J), but with
   * its header label renamed to a fresh one so the two loops don't collide,
   * and its exit kept as exit_label. We simply clone the original range. */
  VEC_EMIT({ out[out_n].op = IR_OP_LABEL; out[out_n].text = mettle_strdup(htail); if(!out[out_n].text){goto oom;} });
  {
    /* fresh header label for the scalar remainder loop */
    char sh[64];
    snprintf(sh, sizeof(sh), "%s_sh", pre);
    for (size_t k = h; k <= J; k++) {
      IRInstruction src = function->instructions[k];
      IRInstruction tmp;
      if (!ir_clone_instruction_plain(&src, &tmp)) {
        goto oom;
      }
      /* rename the loop's own header label + back-jump target h->sh */
      if (tmp.op == IR_OP_LABEL && tmp.text &&
          strcmp(tmp.text, head_label) == 0) {
        mettle_free_string(tmp.text); tmp.text = mettle_strdup(sh);
        if(!tmp.text){ir_instruction_destroy_storage(&tmp);goto oom;}
      }
      if (tmp.op == IR_OP_JUMP && tmp.text &&
          strcmp(tmp.text, head_label) == 0) {
        mettle_free_string(tmp.text); tmp.text = mettle_strdup(sh);
        if(!tmp.text){ir_instruction_destroy_storage(&tmp);goto oom;}
      }
      /* The remainder loop's exit must run the accumulator-combine before
       * reaching the original exit label, so redirect its guard branch
       * (branch_zero %g -> exit_label) to HCOMB. */
      if (tmp.op == IR_OP_BRANCH_ZERO && tmp.text &&
          strcmp(tmp.text, exit_label) == 0) {
        mettle_free_string(tmp.text); tmp.text = mettle_strdup(hcomb);
        if(!tmp.text){ir_instruction_destroy_storage(&tmp);goto oom;}
      }
      VEC_EMIT({ out[out_n] = tmp; });
    }
  }

  /* label HCOMB ; ACC = ACC + ACC1 ; ... ; jump EXIT */
  VEC_EMIT({ out[out_n].op = IR_OP_LABEL; out[out_n].text = mettle_strdup(hcomb); if(!out[out_n].text){goto oom;} });
  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    char t_c[64];
    snprintf(t_c, sizeof(t_c), "%s_c%d", pre, L);
    VEC_EMIT({
      out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
      if(!out[out_n].text){goto oom;}
      out[out_n].dest = ir_operand_temp(t_c);
      out[out_n].lhs = ir_operand_symbol(acc);
      out[out_n].rhs = ir_operand_symbol(acc_name[L]);
    });
    VEC_EMIT({
      out[out_n].op = IR_OP_ASSIGN;
      out[out_n].dest = ir_operand_symbol(acc);
      out[out_n].lhs = ir_operand_temp(t_c);
    });
  }
  /* HCOMB falls straight through into the original exit label, which is
   * preserved untouched in the spliced-in suffix (instructions after J). We
   * deliberately do NOT re-emit exit_label here; doing so would duplicate it. */

  /* ---- splice: replace [h .. J] (inclusive) with `out` ---- */
  {
    size_t old_span = J - h + 1;
    size_t tail_n = function->instruction_count - (J + 1);
    size_t new_count = h + out_n + tail_n;
    IRInstruction *ni = calloc(new_count ? new_count : 1, sizeof(IRInstruction));
    if (!ni) {
      goto oom;
    }
    /* prefix [0,h) moved as-is */
    for (size_t k = 0; k < h; k++) {
      ni[k] = function->instructions[k];
    }
    /* destroy the replaced originals [h..J] */
    for (size_t k = h; k <= J; k++) {
      ir_instruction_destroy_storage(&function->instructions[k]);
    }
    /* new body */
    for (size_t k = 0; k < out_n; k++) {
      ni[h + k] = out[k];
    }
    /* suffix (J+1 .. end) */
    for (size_t k = 0; k < tail_n; k++) {
      ni[h + out_n + k] = function->instructions[J + 1 + k];
    }
    free(out);
    free(function->instructions);
    function->instructions = ni;
    function->instruction_count = new_count;
    function->instruction_capacity = new_count;
    (void)old_span;
  }

  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    free(acc_name[L]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;

oom:
  for (size_t fi = 0; fi < out_n; fi++) {
    ir_instruction_destroy_storage(&out[fi]);
  }
  free(out);
  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    free(acc_name[L]);
  }
  return 0;
#undef VEC_EMIT
#undef MKLBL
}

static int ir_unroll_mark_loop_headers(const IRFunction *function,
                                      IRNameIndex *headers);

int ir_reduction_unroll_pass(IRFunction *function, int *changed) {
  IRNameIndex back_edges;

  if (!function) {
    return 1;
  }
  if (!ir_unroll_mark_loop_headers(function, &back_edges)) {
    return 0;
  }
  for (size_t h = 0; h + 5 < function->instruction_count; h++) {
    size_t back_edge = (size_t)-1;
    size_t before = function->instruction_count;
    if (function->instructions[h].op != IR_OP_LABEL ||
        !function->instructions[h].text ||
        !ir_name_index_find(&back_edges, function->instructions[h].text,
                            &back_edge)) {
      continue;
    }
    if (!ir_vec_try_unroll_reduction_at(function, h, back_edge, changed)) {
      ir_name_index_destroy(&back_edges);
      return 0;
    }
    if (function->instruction_count != before) {
      /* structure changed; the index describes the old shape, so rebuild it
       * and restart the scan to stay safe */
      ir_name_index_destroy(&back_edges);
      if (!ir_unroll_mark_loop_headers(function, &back_edges)) {
        return 0;
      }
      h = (size_t)-1;
    }
  }
  ir_name_index_destroy(&back_edges);
  return 1;
}

/* Which labels a later jump comes back to.
 *
 * A counted loop's header is a label with a back-edge, and the parser demands
 * one before it accepts anything. Testing that per label by scanning forward is
 * quadratic, and testing it at all only after building a map of every constant
 * reaching the label is worse: that map costs a walk of the whole prefix, and
 * an if/else body is nearly all labels and no loops, so every one of them paid
 * it to be rejected. One walk answers it for every label. */
static int ir_unroll_mark_loop_headers(const IRFunction *function,
                                       IRNameIndex *headers) {
  IRNameIndex labels;

  if (!ir_name_index_init(&labels, function->instruction_count)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text) {
      ir_name_index_insert(&labels, instruction->text, i);
    }
  }
  if (!ir_name_index_init(headers, function->instruction_count)) {
    ir_name_index_destroy(&labels);
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t target = 0;
    if (instruction->op != IR_OP_JUMP || !instruction->text) {
      continue;
    }
    if (ir_name_index_find(&labels, instruction->text, &target) &&
        target < i) {
      /* Jumps are visited in order and the index keeps its first insertion, so
       * this is the nearest back-edge, which is the one a scan from the header
       * would have stopped at. */
      ir_name_index_insert(headers, instruction->text, i);
    }
  }
  ir_name_index_destroy(&labels);
  return 1;
}

int ir_unroll_small_const_bound_loops_pass(IRFunction *function,
                                                  int *changed) {
  IRNameIndex loop_headers;

  if (!function) {
    return 0;
  }
  if (!ir_unroll_mark_loop_headers(function, &loop_headers)) {
    return 0;
  }

  int local_changed = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL ||
        !function->instructions[i].text ||
        !ir_name_index_find(&loop_headers, function->instructions[i].text,
                            NULL)) {
      continue;
    }

    int unrolled = 0;
    if (!ir_try_unroll_loop_at(function, i, &unrolled)) {
      ir_name_index_destroy(&loop_headers);
      return 0;
    }
    if (unrolled) {
      local_changed = 1;
      break;
    }
  }

  ir_name_index_destroy(&loop_headers);
  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

/* Reports whether @symbol_name's address is taken anywhere in the function
 * (ADDRESS_OF instruction). A symbol whose address is never taken cannot be
 * written by an arbitrary call or store through some other pointer. */
int ir_symbol_address_taken(const IRFunction *function,
                                   const char *symbol_name) {
  if (!function || !symbol_name) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_ADDRESS_OF &&
        instruction->lhs.kind == IR_OPERAND_SYMBOL &&
        instruction->lhs.name &&
        strcmp(instruction->lhs.name, symbol_name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Matches the five-instruction null-trap diamond emitted by ir_emit_null_check:
 *   branch_zero @SYM -> ir_trap_null_N
 *   jump ir_nonnull_M
 *   label ir_trap_null_N
 *   call _ = mettle_crash_trap(...)
 *   label ir_nonnull_M
 * Allows NOPs interleaved. On match, fills the indices and the operand name.
 * @start_index is the index of the branch_zero; on success @end_index is the
 * index of the trailing 'label ir_nonnull_M' (inclusive). */
int ir_match_null_trap_diamond(const IRFunction *function,
                                      size_t start_index, size_t *end_index_out,
                                      const char **symbol_name_out) {
  if (!function || start_index >= function->instruction_count) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[start_index];
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      branch->lhs.kind != IR_OPERAND_SYMBOL || !branch->lhs.name ||
      strncmp(branch->text, "ir_trap_null_", 13) != 0) {
    return 0;
  }
  const char *trap_label = branch->text;
  const char *symbol_name = branch->lhs.name;

  size_t idx = 0;
  if (!ir_find_next_non_nop(function, start_index + 1, &idx)) {
    return 0;
  }
  const IRInstruction *jmp = &function->instructions[idx];
  if (jmp->op != IR_OP_JUMP || !jmp->text ||
      strncmp(jmp->text, "ir_nonnull_", 11) != 0) {
    return 0;
  }
  const char *ok_label = jmp->text;

  if (!ir_find_next_non_nop(function, idx + 1, &idx)) {
    return 0;
  }
  const IRInstruction *trap_lbl = &function->instructions[idx];
  if (trap_lbl->op != IR_OP_LABEL || !trap_lbl->text ||
      strcmp(trap_lbl->text, trap_label) != 0) {
    return 0;
  }

  if (!ir_find_next_non_nop(function, idx + 1, &idx)) {
    return 0;
  }
  const IRInstruction *call = &function->instructions[idx];
  if (call->op != IR_OP_CALL || !call->text ||
      (strcmp(call->text, "mettle_crash_trap") != 0 &&
       strcmp(call->text, "mettle_crash_trap_ex") != 0)) {
    return 0;
  }

  if (!ir_find_next_non_nop(function, idx + 1, &idx)) {
    return 0;
  }
  const IRInstruction *ok_lbl = &function->instructions[idx];
  if (ok_lbl->op != IR_OP_LABEL || !ok_lbl->text ||
      strcmp(ok_lbl->text, ok_label) != 0) {
    return 0;
  }

  *end_index_out = idx;
  *symbol_name_out = symbol_name;
  return 1;
}

/* Null-check Loop-Invariant Code Motion.
 *
 * For each while-style loop, finds null-trap diamonds whose checked pointer
 * is a symbol that's not written inside the loop body, then hoists exactly
 * one copy of the diamond to immediately before the loop header. Subsequent
 * copies inside the body are replaced with NOPs.
 *
 * Semantics: a hoisted trap moves from "executed on every iteration after the
 * loop-entry check" to "executed once on loop entry". The only observable
 * change is for loops that execute zero iterations: previously a null pointer
 * would not trap; now it traps once before the loop is skipped. This is
 * consistent with Mettle's null-safety contract (every dereference traps), so
 * we accept the change. The lowering already proves the pointer is reachable
 * by emitting the check, so trapping unconditionally on a null param is
 * never a new bug, just earlier detection.
 *
 * Generality: this pass triggers on any pointer-reading loop whose pointer
 * is invariant. Grep, word_count, and any byte-walking loop benefit. */

/* ---- `@unroll(n)`: partial unroll of an annotated counted loop. ----
 *
 * Marker: an IR_OP_NOP carrying "@@unroll:<factor>" immediately before the
 * loop's header label (emitted by the frontend for `@unroll(n)` loops).
 * Recognized shape:
 *
 *   header:  t = counter < limit        ; or <=; integer compare, counter lhs
 *            branch_zero t -> end
 *            ...straight-line body, no labels or branches...
 *            counter = counter + step   ; any positive constant step, last
 *            jump header
 *
 * The counter must be written only by that increment inside the body, and a
 * symbol/temp limit must not be written there. The transform inserts a
 * factor-wide main loop before the original loop and keeps the original as
 * the remainder:
 *
 *   main:    g0 = counter + step*(factor-1)
 *            g1 = g0 < limit            ; same comparison operator
 *            branch_zero g1 -> header
 *            body x factor              ; body-private temps renamed per copy
 *            jump main
 *   header:  ...original loop, unchanged...
 *
 * Iteration order, count, and side effects are preserved exactly; the guard
 * only asks whether the next `factor` iterations all pass the loop test.
 * The marker is consumed when the transform fires and is left in place
 * otherwise, so a later fixpoint iteration can retry once cleanup passes have
 * simplified the loop into shape. */

static int ir_unroll_annotated_body_op_safe(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_NOP:
    /* A nested annotation marker means a nested loop follows: bail. */
    return !(in->text &&
             strncmp(in->text, IR_UNROLL_MARKER_PREFIX,
                     strlen(IR_UNROLL_MARKER_PREFIX)) == 0);
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ASSIGN:
  case IR_OP_CAST:
  case IR_OP_ROTATE_ADD:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_LOAD:
  case IR_OP_STORE:
  case IR_OP_ADDRESS_OF:
  case IR_OP_SELECT:
  case IR_OP_CALL:
    return 1;
  default:
    return 0;
  }
}

static int ir_unroll_annotated_parse_increment(const IRFunction *function,
                                               size_t body_start,
                                               size_t body_end,
                                               const char *counter_symbol,
                                               size_t *increment_index,
                                               size_t *producer_index_out,
                                               long long *step_out) {
  *producer_index_out = (size_t)-1;
  for (size_t i = body_end; i > body_start;) {
    i--;
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP) continue;
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        strcmp(in->text, "+") == 0 && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, counter_symbol) == 0 &&
        in->lhs.kind == IR_OPERAND_SYMBOL && in->lhs.name &&
        strcmp(in->lhs.name, counter_symbol) == 0 &&
        in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 0) {
      *increment_index = i;
      *step_out = in->rhs.int_value;
      return 1;
    }
    if (in->op == IR_OP_ASSIGN && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, counter_symbol) == 0 &&
        in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name) {
      size_t producer_index = 0;
      if (!ir_find_last_writer_before(function, i, IR_OPERAND_TEMP,
                                      in->lhs.name, &producer_index) ||
          producer_index < body_start) {
        return 0;
      }
      const IRInstruction *producer = &function->instructions[producer_index];
      if (producer->op != IR_OP_BINARY || producer->is_float ||
          !producer->text || strcmp(producer->text, "+") != 0) {
        return 0;
      }
      if (producer->lhs.kind == IR_OPERAND_SYMBOL && producer->lhs.name &&
          strcmp(producer->lhs.name, counter_symbol) == 0 &&
          producer->rhs.kind == IR_OPERAND_INT &&
          producer->rhs.int_value > 0) {
        *increment_index = i;
        *producer_index_out = producer_index;
        *step_out = producer->rhs.int_value;
        return 1;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

static int ir_unroll_annotated_append_clone(IRInstructionVector *vector,
                                            const IRInstruction *source) {
  IRInstruction cloned = {0};
  if (!ir_clone_instruction_plain(source, &cloned) ||
      !ir_instruction_vector_append_move(vector, &cloned)) {
    ir_instruction_destroy_storage(&cloned);
    return 0;
  }
  return 1;
}

static int ir_unroll_annotated_try_marker(IRFunction *function,
                                          size_t marker_index, int factor,
                                          int *changed) {
  size_t header_index = 0;
  if (!ir_find_next_non_nop(function, marker_index + 1, &header_index)) {
    return 1;
  }
  const IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 1;
  }
  const char *header_label = header->text;

  size_t compare_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index)) {
    return 1;
  }
  const IRInstruction *compare = &function->instructions[compare_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      (strcmp(compare->text, "<") != 0 && strcmp(compare->text, "<=") != 0) ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name) {
    return 1;
  }
  const char *counter_symbol = compare->lhs.name;
  const IROperand *limit = &compare->rhs;
  if (limit->kind != IR_OPERAND_INT && limit->kind != IR_OPERAND_SYMBOL &&
      limit->kind != IR_OPERAND_TEMP) {
    return 1;
  }

  size_t branch_index = 0;
  if (!ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  const IRInstruction *branch = &function->instructions[branch_index];
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      branch->lhs.kind != IR_OPERAND_TEMP || !branch->lhs.name ||
      strcmp(branch->lhs.name, compare->dest.name) != 0) {
    return 1;
  }

  size_t jump_index = (size_t)-1;
  for (size_t j = branch_index + 1; j < function->instruction_count; j++) {
    const IRInstruction *probe = &function->instructions[j];
    if (probe->op == IR_OP_JUMP && probe->text &&
        strcmp(probe->text, header_label) == 0) {
      jump_index = j;
      break;
    }
    if (!ir_unroll_annotated_body_op_safe(probe)) {
      return 1;
    }
  }
  if (jump_index == (size_t)-1) {
    return 1;
  }

  size_t body_start = branch_index + 1;
  size_t increment_index = 0;
  size_t producer_index = (size_t)-1;
  long long step = 0;
  if (!ir_unroll_annotated_parse_increment(function, body_start, jump_index,
                                           counter_symbol, &increment_index,
                                           &producer_index, &step)) {
    return 1;
  }
  (void)producer_index;

  /* The counter may be written only by the increment; a named limit must be
   * loop-invariant. */
  for (size_t j = body_start; j < jump_index; j++) {
    if (j == increment_index) continue;
    const IRInstruction *in = &function->instructions[j];
    if (in->op == IR_OP_NOP || in->op == IR_OP_STORE) continue;
    if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        strcmp(in->dest.name, counter_symbol) == 0) {
      return 1;
    }
    if ((limit->kind == IR_OPERAND_SYMBOL || limit->kind == IR_OPERAND_TEMP) &&
        in->dest.kind == limit->kind && in->dest.name && limit->name &&
        strcmp(in->dest.name, limit->name) == 0) {
      return 1;
    }
  }

  long long lead = step * (long long)(factor - 1);

  IRInstructionVector vector = {0};
  for (size_t i = 0; i < header_index; i++) {
    if (i == marker_index) continue; /* consume the marker */
    if (!ir_unroll_annotated_append_clone(&vector, &function->instructions[i]))
      goto fail;
  }

  char main_label[64];
  char guard_add[64];
  char guard_cmp[64];
  snprintf(main_label, sizeof(main_label), "ir_unroll_main_%zu", marker_index);
  snprintf(guard_add, sizeof(guard_add), ".unroll%zu_g0", marker_index);
  snprintf(guard_cmp, sizeof(guard_cmp), ".unroll%zu_g1", marker_index);

  {
    IRInstruction label = {0};
    label.op = IR_OP_LABEL;
    label.location = header->location;
    label.text = mettle_strdup(main_label);
    if (!label.text || !ir_instruction_vector_append_move(&vector, &label)) {
      ir_instruction_destroy_storage(&label);
      goto fail;
    }
  }
  {
    IRInstruction add = {0};
    add.op = IR_OP_BINARY;
    add.location = header->location;
    add.text = mettle_strdup("+");
    add.dest = ir_operand_temp(guard_add);
    add.lhs = ir_operand_symbol(counter_symbol);
    add.rhs = ir_operand_int(lead);
    if (!add.text || !add.dest.name || !add.lhs.name ||
        !ir_instruction_vector_append_move(&vector, &add)) {
      ir_instruction_destroy_storage(&add);
      goto fail;
    }
  }
  {
    IRInstruction cmp = {0};
    cmp.op = IR_OP_BINARY;
    cmp.location = header->location;
    cmp.text = mettle_strdup(compare->text);
    cmp.dest = ir_operand_temp(guard_cmp);
    cmp.lhs = ir_operand_temp(guard_add);
    cmp.rhs = limit->kind == IR_OPERAND_INT
                  ? ir_operand_int(limit->int_value)
                  : (limit->kind == IR_OPERAND_SYMBOL
                         ? ir_operand_symbol(limit->name)
                         : ir_operand_temp(limit->name));
    if (!cmp.text || !cmp.dest.name || !cmp.lhs.name ||
        (limit->kind != IR_OPERAND_INT && !cmp.rhs.name) ||
        !ir_instruction_vector_append_move(&vector, &cmp)) {
      ir_instruction_destroy_storage(&cmp);
      goto fail;
    }
  }
  {
    IRInstruction guard = {0};
    guard.op = IR_OP_BRANCH_ZERO;
    guard.location = header->location;
    guard.text = mettle_strdup(header_label);
    guard.lhs = ir_operand_temp(guard_cmp);
    if (!guard.text || !guard.lhs.name ||
        !ir_instruction_vector_append_move(&vector, &guard)) {
      ir_instruction_destroy_storage(&guard);
      goto fail;
    }
  }

  {
    IrUnrollTempSet private_temps = {0};
    if (!ir_unroll_collect_private_temps(function, body_start, jump_index,
                                         &private_temps)) {
      ir_unroll_temp_set_destroy(&private_temps);
      goto fail;
    }
    for (int copy = 0; copy < factor; copy++) {
      for (size_t b = body_start; b < jump_index; b++) {
        const IRInstruction *in = &function->instructions[b];
        if (in->op == IR_OP_NOP) continue;
        IRInstruction cloned = {0};
        if (!ir_clone_instruction_plain(in, &cloned) ||
            !ir_unroll_rename_copy_temps(
                &cloned, &private_temps,
                (long long)marker_index * 16 + copy) ||
            !ir_instruction_vector_append_move(&vector, &cloned)) {
          ir_instruction_destroy_storage(&cloned);
          ir_unroll_temp_set_destroy(&private_temps);
          goto fail;
        }
      }
    }
    ir_unroll_temp_set_destroy(&private_temps);
  }

  {
    IRInstruction back = {0};
    back.op = IR_OP_JUMP;
    back.location = header->location;
    back.text = mettle_strdup(main_label);
    if (!back.text || !ir_instruction_vector_append_move(&vector, &back)) {
      ir_instruction_destroy_storage(&back);
      goto fail;
    }
  }

  for (size_t i = header_index; i < function->instruction_count; i++) {
    if (!ir_unroll_annotated_append_clone(&vector, &function->instructions[i]))
      goto fail;
  }

  if (!ir_function_replace_instructions(function, &vector)) {
    goto fail;
  }
  *changed = 1;
  return 1;

fail:
  ir_instruction_vector_destroy(&vector);
  return 0;
}

int ir_unroll_annotated_loops_pass(IRFunction *function, int *changed) {
  if (!function || !changed) {
    return 0;
  }
  size_t prefix_len = strlen(IR_UNROLL_MARKER_PREFIX);
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_NOP || !in->text ||
        strncmp(in->text, IR_UNROLL_MARKER_PREFIX, prefix_len) != 0) {
      continue;
    }
    int factor = atoi(in->text + prefix_len);
    if (factor < 2 || factor > 16) {
      continue;
    }
    int fired = 0;
    if (!ir_unroll_annotated_try_marker(function, i, factor, &fired)) {
      return 0;
    }
    if (fired) {
      /* Indices shifted; the fixpoint driver reruns this pass for any
       * remaining markers. */
      *changed = 1;
      return 1;
    }
  }
  return 1;
}
