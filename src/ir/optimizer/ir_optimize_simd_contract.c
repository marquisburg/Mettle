#include "ir_optimize_internal.h"
#include "common.h"

#include <stdio.h>
#include <string.h>

/* Enforcement of the `@simd` / `@simd!` loop attributes.
 *
 * ir_lowering.c brackets each attributed loop with IR_OP_NOP markers whose
 * `text` is "@@simd:B:<id>:<mode>" / "@@simd:E:<id>:0" (see IR_SIMD_MARKER_PREFIX
 * in ir.h). By the time ir_verify_simd_contracts runs -- last in
 * ir_optimize_function_pipeline, after every vectorizer -- a loop a recognizer
 * accepted has been rewritten into a SIMD intrinsic op sitting between its
 * markers. So the contract test is simply: is there a vectorized op between B
 * and E?
 *
 *   @simd  (hint)     -> warn when not vectorized, keep the scalar loop
 *   @simd! (contract) -> hard compile error when not vectorized
 *
 * Enforcement only happens when optimization runs (-O / --release); plain debug
 * builds leave the markers as inert NOPs (ir_note_simd_contracts_unverified
 * prints one note explaining this and strips them). With --simd-report, every
 * `@simd` loop additionally reports what it became. */

/* Set when a `@simd!` contract is violated, so the driver can tell a user error
 * apart from an internal compiler error. */
static int g_simd_contract_user_error = 0;
/* --simd-report: emit a note for every `@simd` loop (vectorized or not). */
static int g_simd_report = 0;

void ir_optimize_reset_user_error(void) { g_simd_contract_user_error = 0; }

int ir_optimize_had_user_error(void) { return g_simd_contract_user_error; }

/* Other contract checkers (`@inline!`, `@noalloc`) report through the same
 * "user error, not ICE" channel `@simd!` uses. */
void ir_optimize_note_user_error(void) { g_simd_contract_user_error = 1; }

void ir_optimize_set_simd_report(int enabled) { g_simd_report = enabled; }

/* Program access for program-level fix simulations (call-in-body re-runs the
 * inliner, which needs callee lookup). NULL outside the per-function stage. */
static MTLC_THREAD_LOCAL IRProgram *g_explain_program = NULL;

void ir_explain_set_program(IRProgram *program) { g_explain_program = program; }

/* The module symbol table of the program currently being optimized, for a
 * per-function pass that needs to resolve a global by name. NULL outside a
 * program stage, which simply means the pass declines. */
const IRModuleSymbol *ir_optimize_module_symbol(const char *name) {
  if (!g_explain_program || !name) {
    return NULL;
  }
  return ir_program_lookup_symbol(g_explain_program, name);
}

static int ir_instruction_is_simd_marker(const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_NOP && instruction->text &&
         strncmp(instruction->text, IR_SIMD_MARKER_PREFIX,
                 strlen(IR_SIMD_MARKER_PREFIX)) == 0;
}

/* Any op in [IR_OP_COUNT_WORD_STARTS, IR_OP_SIMD_OUTER_LANE_F64] is one of the
 * accelerated idiom / SIMD intrinsics the recognizers emit; its presence means
 * the loop was claimed by a vectorizer. */
static int ir_op_is_vectorized(IROpcode op) {
  return op >= IR_OP_COUNT_WORD_STARTS && op <= IR_OP_SIMD_OUTER_LANE_F64;
}

/* First vectorized instruction in (begin, end). With any_depth == 0 only ops
 * at this loop's own nesting level count (ops inside a nested marked loop
 * belong to that loop); with any_depth == 1 the whole region counts. Returns
 * NULL when nothing vectorized. */
static const IRInstruction *ir_region_vectorized_ins(const IRFunction *function,
                                                     size_t begin, size_t end,
                                                     int any_depth) {
  int depth = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (ir_instruction_is_simd_marker(instruction)) {
      depth += (instruction->text[strlen(IR_SIMD_MARKER_PREFIX)] == 'B') ? 1
                                                                         : -1;
      continue;
    }
    if ((any_depth || depth == 0) && ir_op_is_vectorized(instruction->op)) {
      return instruction;
    }
  }
  return NULL;
}

static int ir_region_vectorized_op(const IRFunction *function, size_t begin,
                                   size_t end) {
  const IRInstruction *ins = ir_region_vectorized_ins(function, begin, end, 0);
  return ins ? (int)ins->op : -1;
}

/* The find skip-ahead vectorizes a search loop WITHOUT removing it: the
 * counter's init (which sits BEFORE a while-loop's marker region) becomes an
 * IR_OP_SIMD_FIND and the surviving scalar loop replays only the hit
 * iteration. Detect it so @simd contracts and the --explain report credit
 * the loop as vectorized: find the region's loop counter (the header
 * compare's lhs) and walk the straight-line code above the region for the
 * SIMD_FIND that initializes it. */
static const IRInstruction *ir_region_skipahead_ins(const IRFunction *function,
                                                    size_t begin, size_t end) {
  const char *iv = NULL;
  for (size_t i = begin + 1; i < end && !iv; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL || !ir_label_is_while_header(ins->text)) {
      continue;
    }
    for (size_t k = i + 1; k < end; k++) {
      const IRInstruction *c = &function->instructions[k];
      if (c->op == IR_OP_NOP) {
        continue;
      }
      if (c->op == IR_OP_BINARY && c->lhs.kind == IR_OPERAND_SYMBOL &&
          c->lhs.name) {
        iv = c->lhs.name;
      }
      break;
    }
  }
  if (!iv) {
    return NULL;
  }
  for (size_t i = begin + 1; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_SIMD_FIND && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && strcmp(ins->dest.name, iv) == 0) {
      return ins;
    }
    if (ins->op == IR_OP_LABEL ||
        (ir_instruction_writes_destination(ins) &&
         ir_operand_is_symbol_named(&ins->dest, iv))) {
      return NULL;
    }
  }
  return NULL;
}

/* True when (begin, end) still contains a loop header label -- i.e. an actual
 * loop survived optimization. A region with markers but no loop label was
 * fully unrolled (constant trip count) or removed outright. */
static int ir_region_has_loop_label(const IRFunction *function, size_t begin,
                                    size_t end) {
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text &&
        (strstr(ins->text, "ir_while_") != NULL ||
         strstr(ins->text, "ir_for_cond_") != NULL)) {
      return 1;
    }
  }
  return 0;
}

static int ir_label_is_loop_header(const char *label);

/* Loop headers in a region. Distinguishes "the body gained a loop" from "this
   loop is still a loop", which a plain label test cannot. */
static size_t ir_region_loop_header_count(const IRFunction *function,
                                          size_t begin, size_t end) {
  size_t count = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_label_is_loop_header(ins->text)) {
      count++;
    }
  }
  return count;
}

/* A loop's ENTRY label, as opposed to any of the other labels a loop emits.
 * `while` lowers to `ir_while_N` (entry) plus `ir_while_end_N` (exit), and the
 * exit contains the entry's name as a prefix -- so a plain substring test
 * counts one loop twice. Anything that walks headers to find nesting has to
 * tell them apart. */
static int ir_label_is_loop_header(const char *label) {
  if (!label) {
    return 0;
  }
  if (strstr(label, "ir_for_cond_") != NULL) {
    return 1;
  }
  return strstr(label, "ir_while_") != NULL &&
         strstr(label, "ir_while_end_") == NULL;
}

/* The source line of a loop header nested strictly inside (begin, end), or 0
 * when the region holds a single loop.
 *
 * The `@simd` marker records cannot answer this: the inliner deliberately drops
 * markers from an inlined copy, so a loop that arrived with an inlined call
 * leaves no record behind. Without a structural check the outer loop reads as a
 * leaf, and the leaf classifier then blames the INNER loop's exit test on a
 * data-dependent `if` and prescribes a branchless rewrite -- for a body whose
 * source contains no branch at all. Past the region's own header, a second
 * header can only belong to a nested loop. */
static size_t ir_region_inner_loop_line(const IRFunction *function,
                                        size_t begin, size_t end) {
  int seen_header = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL || !ir_label_is_loop_header(ins->text)) {
      continue;
    }
    if (!seen_header) {
      seen_header = 1;
      continue;
    }
    /* Labels carry no location of their own; take the first instruction after
     * the header that does. */
    for (size_t j = i + 1; j < end; j++) {
      if (function->instructions[j].location.line) {
        return function->instructions[j].location.line;
      }
    }
    return 0;
  }
  return 0;
}

/* A branch/jump whose target is one of the runtime-check labels the lowerer
 * injects (null-check, bounds-check). These appear per pointer/array access at
 * -O (they're absent at --release), so they must NOT count as user control flow
 * -- otherwise every loop that touches a pointer is misreported as having "its
 * own control flow". */
static int ir_label_is_runtime_check(const char *label) {
  if (!label) {
    return 0;
  }
  /* `nullhoist` is the skip label null_check_licm leaves when it lifts a
   * check out of the loop. Leaving it off this list made the diagnosis count
   * the compiler's own branch as the programmer's, so a straight-line
   * `@simd! for i in 0..n { a[i] = a[i] * 3 + 1; }` was told its body
   * "branches on data (an `if` or `&&`/`||` per iteration)". */
  return strstr(label, "trap_null") != NULL || strstr(label, "nonnull") != NULL ||
         strstr(label, "nullhoist") != NULL ||
         strstr(label, "trap_bounds") != NULL || strstr(label, "in_bounds") != NULL;
}

/* What a data-dependent `if` in a loop body is actually doing.
 *
 * "Compute both arms and select arithmetically" is sound counsel for a
 * predicated store and no help at all for a running maximum, which has no
 * arithmetic form to rewrite into -- and which now vectorizes on its own when
 * the rest of the loop allows it, so a reader who followed that advice would
 * be rewriting a loop the compiler had already declined for some other reason.
 * Naming the shape is what makes the sentence after it worth acting on. */
typedef enum {
  IR_BRANCH_SHAPE_OTHER = 0,
  IR_BRANCH_SHAPE_EXTREMUM,    /* if (v > m) { m = v; } */
  IR_BRANCH_SHAPE_COUNT,       /* if (cond) { c = c + 1; } */
  IR_BRANCH_SHAPE_CLAMP_STORE  /* if (v > hi) { v = hi; } ... a[i] = v; */
} IRBranchShape;

/* The symbol a diamond's arm writes, plus what kind of write it is. `at` is
 * the BRANCH_ZERO; the arm runs to its jump-to-end. */
static IRBranchShape ir_classify_one_diamond(const IRFunction *function,
                                             size_t at, size_t end,
                                             const IRInstruction *compare,
                                             const char **written_out) {
  size_t jump = at + 1;
  size_t write = 0;
  int found = 0;

  for (; jump < end; jump++) {
    IROpcode op = function->instructions[jump].op;
    if (op == IR_OP_JUMP) {
      break;
    }
    if (op == IR_OP_LABEL || op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ) {
      return IR_BRANCH_SHAPE_OTHER;
    }
  }
  if (jump >= end) {
    return IR_BRANCH_SHAPE_OTHER;
  }
  /* An arm may do more than one thing (`v = lim; clipped = clipped + 1;`).
   * The write that names a tested operand is the one the shape is about; the
   * others are bookkeeping alongside it. */
  for (size_t i = at + 1; i < jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    found++;
    if (!write) {
      write = i;
    }
    if (compare && (ir_operand_is_symbol_named(&compare->lhs, ins->dest.name) ||
                    ir_operand_is_symbol_named(&compare->rhs, ins->dest.name))) {
      write = i;
      break;
    }
  }
  if (!found) {
    return IR_BRANCH_SHAPE_OTHER;
  }
  {
    const IRInstruction *w = &function->instructions[write];
    int writes_tested_operand =
        compare && (ir_operand_is_symbol_named(&compare->lhs, w->dest.name) ||
                    ir_operand_is_symbol_named(&compare->rhs, w->dest.name));
    *written_out = w->dest.name;
    /* `c = c + <x>`: a predicated count, or a conditional running sum. */
    if (!writes_tested_operand && w->op == IR_OP_BINARY && w->text &&
        w->text[0] == '+' && !w->text[1] &&
        (ir_operand_is_symbol_named(&w->lhs, w->dest.name) ||
         ir_operand_is_symbol_named(&w->rhs, w->dest.name))) {
      return IR_BRANCH_SHAPE_COUNT;
    }
    /* A tested operand copied back UNCHANGED is an extremum. Recomputing it
     * (`v = 0.0 - v`, an absolute value) is a different shape entirely, and
     * calling that a running maximum would name the wrong variable. The copy
     * is either the other tested operand by name, or the load temp behind it:
     * an arm that re-reads `a[i]` writes a fresh temp for the same element. */
    if (writes_tested_operand && w->op == IR_OP_LOAD) {
      return IR_BRANCH_SHAPE_EXTREMUM;
    }
    if (writes_tested_operand && w->op == IR_OP_ASSIGN && w->lhs.name) {
      const IROperand *other =
          ir_operand_is_symbol_named(&compare->lhs, w->dest.name)
              ? &compare->rhs
              : &compare->lhs;
      if (other->name && strcmp(other->name, w->lhs.name) == 0) {
        return IR_BRANCH_SHAPE_EXTREMUM;
      }
      if (w->lhs.kind == IR_OPERAND_TEMP) {
        const IRInstruction *src =
            ir_find_temp_producer_before(function, write, w->lhs.name);
        /* A cast counts: `(int32)bytes[i]` is still the element that was
         * tested, only widened. Arithmetic does not. */
        if (src && (src->op == IR_OP_LOAD || src->op == IR_OP_CAST)) {
          return IR_BRANCH_SHAPE_EXTREMUM;
        }
      }
    }
    if (writes_tested_operand) {
      return IR_BRANCH_SHAPE_CLAMP_STORE;
    }
    return IR_BRANCH_SHAPE_OTHER;
  }
}

/* The dominant shape among the region's data-dependent diamonds, with the
 * symbol the arm writes. A clamp's two diamonds (a low bound and a high one)
 * report as one clamp; a mixed body reports OTHER. */
static IRBranchShape ir_region_branch_shape(const IRFunction *function,
                                            size_t begin, size_t end,
                                            const char **written_out) {
  IRBranchShape shape = IR_BRANCH_SHAPE_OTHER;
  int seen = 0;
  int has_store = 0;
  size_t body_lo = begin + 1;
  size_t exit_test = (size_t)-1;

  /* The loop's own exit test is the first branch after its header. It is the
   * trip count, not a decision about an element, and counting it as one leaves
   * every real diamond looking like a mixed body. */
  for (size_t i = begin; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_label_is_loop_header(ins->text)) {
      for (size_t j = i + 1; j < end; j++) {
        if (function->instructions[j].op == IR_OP_BRANCH_ZERO ||
            function->instructions[j].op == IR_OP_BRANCH_EQ) {
          exit_test = j;
          break;
        }
      }
      body_lo = (exit_test == (size_t)-1) ? i + 1 : exit_test + 1;
      break;
    }
  }
  for (size_t i = body_lo; i < end; i++) {
    if (function->instructions[i].op == IR_OP_STORE) {
      has_store = 1;
    }
  }
  for (size_t i = body_lo; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IRInstruction *compare = NULL;
    const char *written = NULL;
    IRBranchShape one;
    if (ins->op != IR_OP_BRANCH_ZERO || ir_label_is_runtime_check(ins->text)) {
      continue;
    }
    for (size_t j = i; j > body_lo; j--) {
      const IRInstruction *c = &function->instructions[j - 1];
      if (c->op == IR_OP_BINARY && c->dest.kind == IR_OPERAND_TEMP &&
          c->dest.name && ir_operand_is_temp_named(&ins->lhs, c->dest.name)) {
        compare = c;
        break;
      }
    }
    one = ir_classify_one_diamond(function, i, end, compare, &written);
    if (one == IR_BRANCH_SHAPE_EXTREMUM && has_store) {
      /* A tested value written back and then stored is a clamp, not a
       * reduction: the running state is the element, not an accumulator. */
      one = IR_BRANCH_SHAPE_CLAMP_STORE;
    } else if (one == IR_BRANCH_SHAPE_CLAMP_STORE && !has_store) {
      one = IR_BRANCH_SHAPE_OTHER; /* nothing stored: not a clamp */
    }
    if (!seen) {
      shape = one;
      *written_out = written;
      seen = 1;
    } else if (one != shape) {
      return IR_BRANCH_SHAPE_OTHER;
    }
  }
  return seen ? shape : IR_BRANCH_SHAPE_OTHER;
}

/* The loop counter of the while-loop whose header opens this region, or NULL.
 * The header's compare is `iv < bound`, and its left side names the counter. */
static const char *ir_region_loop_counter(const IRFunction *function,
                                          size_t begin, size_t end) {
  for (size_t i = begin; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    size_t cmp = 0;
    if (ins->op != IR_OP_LABEL || !ir_label_is_loop_header(ins->text)) {
      continue;
    }
    if (!ir_find_next_non_nop(function, i + 1, &cmp) || cmp >= end) {
      return NULL;
    }
    {
      const IRInstruction *c = &function->instructions[cmp];
      if (c->op != IR_OP_BINARY || c->is_float || !c->text ||
          strcmp(c->text, "<") != 0 || c->lhs.kind != IR_OPERAND_SYMBOL ||
          !c->lhs.name) {
        return NULL;
      }
      /* Pointer induction can rewrite the exit test to walk a pointer while
       * the source's counter lives on, still indexing the arrays the test no
       * longer mentions. The index math is what these diagnoses read, so take
       * the counter the body increments by one. */
      if (!ir_symbol_contains(c->lhs.name, "__ptr_")) {
        return c->lhs.name;
      }
      for (size_t j = cmp + 1; j < end; j++) {
        const IRInstruction *step = &function->instructions[j];
        if (step->op == IR_OP_BINARY && !step->is_float && step->text &&
            strcmp(step->text, "+") == 0 &&
            step->dest.kind == IR_OPERAND_SYMBOL && step->dest.name &&
            !ir_symbol_contains(step->dest.name, "__ptr_") &&
            ir_operand_is_symbol_named(&step->lhs, step->dest.name) &&
            ir_operand_is_int_value(&step->rhs, 1)) {
          return step->dest.name;
        }
      }
    }
    return NULL;
  }
  return NULL;
}

/* The element stride of an address index, in elements, or 0 when the index is
 * not an affine function of the counter this understands. `elem_size` is the
 * access width in bytes, so a byte offset of `iv << 3` on a 4-byte access is a
 * stride of two. */
static long long ir_index_element_stride(const IRFunction *function,
                                         size_t before, const IROperand *index,
                                         const char *iv, long long elem_size,
                                         int depth) {
  const IRInstruction *p = NULL;
  if (!index || !iv || elem_size <= 0 || depth > 4) {
    return 0;
  }
  if (ir_operand_is_symbol_named(index, iv)) {
    return elem_size == 1 ? 1 : 0; /* raw counter: one element only at width 1 */
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name) {
    return 0;
  }
  p = ir_find_temp_producer_before(function, before, index->name);
  if (!p || p->op != IR_OP_BINARY || p->is_float || !p->text) {
    return 0;
  }
  /* `X << k` and `X * k` both scale; `X + c` shifts the start without changing
   * the stride. */
  if ((strcmp(p->text, "<<") == 0 || strcmp(p->text, "*") == 0) &&
      p->rhs.kind == IR_OPERAND_INT) {
    long long scale = strcmp(p->text, "<<") == 0
                          ? (p->rhs.int_value < 40 ? (1LL << p->rhs.int_value)
                                                   : 0)
                          : p->rhs.int_value;
    if (scale <= 0) {
      return 0;
    }
    if (ir_operand_is_symbol_named(&p->lhs, iv)) {
      return (scale % elem_size) ? 0 : scale / elem_size;
    }
    {
      long long inner = ir_index_element_stride(function, before, &p->lhs, iv,
                                                1, depth + 1);
      if (!inner) {
        return 0;
      }
      return ((inner * scale) % elem_size) ? 0 : (inner * scale) / elem_size;
    }
  }
  if (strcmp(p->text, "+") == 0) {
    if (p->rhs.kind == IR_OPERAND_INT) {
      return ir_index_element_stride(function, before, &p->lhs, iv, elem_size,
                                     depth + 1);
    }
    if (p->lhs.kind == IR_OPERAND_INT) {
      return ir_index_element_stride(function, before, &p->rhs, iv, elem_size,
                                     depth + 1);
    }
  }
  return 0;
}

/* Does the region access memory with a stride greater than one element? The
 * kernels all walk their bases by one vector per iteration, so `a[i*3]` is
 * outside every one of them -- and saying so beats the catch-all, which tells
 * a reader whose index is already what they meant to write to go make it
 * unit-stride. */
static long long ir_region_strided_access(const IRFunction *function,
                                          size_t begin, size_t end) {
  const char *iv = ir_region_loop_counter(function, begin, end);
  long long worst = 0;
  if (!iv) {
    return 0;
  }
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *addr = NULL;
    const IRInstruction *sum = NULL;
    long long size = 0;
    long long stride = 0;
    if (ins->op == IR_OP_LOAD) {
      addr = &ins->lhs;
    } else if (ins->op == IR_OP_STORE) {
      addr = &ins->dest;
    } else {
      continue;
    }
    if (addr->kind != IR_OPERAND_TEMP || !addr->name ||
        ins->rhs.kind != IR_OPERAND_INT) {
      continue;
    }
    size = ins->rhs.int_value;
    sum = ir_find_temp_producer_before(function, i, addr->name);
    if (!sum || sum->op != IR_OP_BINARY || !sum->text ||
        strcmp(sum->text, "+") != 0 || sum->lhs.kind != IR_OPERAND_SYMBOL) {
      continue;
    }
    stride = ir_index_element_stride(function, i, &sum->rhs, iv, size, 0);
    if (stride > worst) {
      worst = stride;
    }
  }
  return worst > 1 ? worst : 0;
}

/* Does an access index the counter PLUS something that does not vary -- the
 * `m[row * cols + c]` of every row-major inner loop? The access is perfectly
 * unit-stride; it just is not `base[i]`, which is the only address form the
 * kernels index off. Naming the invariant term turns the catch-all into an
 * edit: bind it to a pointer before the loop. Returns 1 and fills `element`
 * with the base array's name when it finds one. */
static int ir_region_invariant_index_term(const IRFunction *function,
                                          size_t begin, size_t end,
                                          const char **base_out) {
  const char *iv = ir_region_loop_counter(function, begin, end);
  if (!iv) {
    return 0;
  }
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *addr = NULL;
    const IRInstruction *sum = NULL;
    const IRInstruction *scale = NULL;
    const IRInstruction *inner = NULL;
    if (ins->op == IR_OP_LOAD) {
      addr = &ins->lhs;
    } else if (ins->op == IR_OP_STORE) {
      addr = &ins->dest;
    } else {
      continue;
    }
    if (addr->kind != IR_OPERAND_TEMP || !addr->name) {
      continue;
    }
    sum = ir_find_temp_producer_before(function, i, addr->name);
    if (!sum || sum->op != IR_OP_BINARY || !sum->text ||
        strcmp(sum->text, "+") != 0 || sum->lhs.kind != IR_OPERAND_SYMBOL ||
        sum->rhs.kind != IR_OPERAND_TEMP || !sum->rhs.name) {
      continue;
    }
    scale = ir_find_temp_producer_before(function, i, sum->rhs.name);
    if (!scale || scale->op != IR_OP_BINARY || !scale->text ||
        strcmp(scale->text, "<<") != 0 || scale->lhs.kind != IR_OPERAND_TEMP ||
        !scale->lhs.name) {
      continue;
    }
    inner = ir_find_temp_producer_before(function, i, scale->lhs.name);
    if (!inner || inner->op != IR_OP_BINARY || !inner->text ||
        strcmp(inner->text, "+") != 0) {
      continue;
    }
    /* One side the counter, the other a runtime value -- not a literal, which
     * the address folder handles on its own. */
    if ((ir_operand_is_symbol_named(&inner->lhs, iv) &&
         inner->rhs.kind != IR_OPERAND_INT) ||
        (ir_operand_is_symbol_named(&inner->rhs, iv) &&
         inner->lhs.kind != IR_OPERAND_INT)) {
      *base_out = sum->lhs.name;
      return 1;
    }
  }
  return 0;
}

/* Forward decl: the dependence-analysis recurrence finder lives further down
 * (next to the deeper --explain diagnosis that shares it). */
static const char *ir_region_find_serial_recurrence(const IRFunction *function,
                                                    size_t begin, size_t end,
                                                    const char **ops,
                                                    size_t *n_ops);

/* Best-effort explanation of why a loop the user marked `@simd` did not
 * vectorize, derived from the surviving scalar IR between the markers. A clean
 * counted loop has exactly one exit test (branch) and one back-edge (jump);
 * extras mean the body carries its own control flow (a nested loop or an `if`),
 * which the recognizers don't handle. */
/* Does this region carry a compiler-inserted null or bounds check? Those are
 * absent under --release, so their presence is the difference between a build
 * that vectorizes and one that does not. */
static int ir_region_has_runtime_check(const IRFunction *function, size_t begin,
                                       size_t end) {
  for (size_t i = begin + 1; i < end && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if ((ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT) &&
        ins->text && strstr(ins->text, "crash_trap") != NULL) {
      return 1;
    }
    if ((ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ ||
         ins->op == IR_OP_JUMP || ins->op == IR_OP_LABEL) &&
        ir_label_is_runtime_check(ins->text)) {
      return 1;
    }
  }
  return 0;
}

static const char *ir_simd_bail_reason(const IRFunction *function, size_t begin,
                                       size_t end) {
  static char reason_buffer[512];
  int has_call = 0, has_new = 0, has_asm = 0;
  int branch_count = 0, jump_count = 0;
  int has_i16 = 0, has_i64 = 0; /* unsupported memory element widths */
  int past_header = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    /* Skip the once-only preamble between the begin marker and the header
     * label (a for-loop's initializer, hoisted pure calls): it is not the
     * loop and must not drive the diagnosis. */
    if (!past_header) {
      continue;
    }
    switch (ins->op) {
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
      /* Ignore compiler-injected runtime-check traps (null/bounds checks emit
       * a guarded call to mettle_crash_trap_ex at -O; they're absent at
       * --release). Only user calls should drive the diagnosis. */
      if (!(ins->text && strstr(ins->text, "crash_trap"))) {
        has_call = 1;
      }
      break;
    case IR_OP_NEW:
      has_new = 1;
      break;
    case IR_OP_INLINE_ASM:
      has_asm = 1;
      break;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!ir_label_is_runtime_check(ins->text)) {
        branch_count++;
      }
      break;
    case IR_OP_JUMP:
      if (!ir_label_is_runtime_check(ins->text)) {
        jump_count++;
      }
      break;
    case IR_OP_LOAD:
    case IR_OP_STORE: {
      /* Vectorizable element widths: 1 (int8/uint8), 4 (int32/float32),
       * 8-float (float64). 16-bit ints and 64-bit ints have no kernel. The
       * load/store size lives in rhs; is_float distinguishes f64 from i64. */
      long long sz = (ins->rhs.kind == IR_OPERAND_INT) ? ins->rhs.int_value : 4;
      if (!ins->is_float) {
        if (sz == 2) {
          has_i16 = 1;
        } else if (sz == 8) {
          has_i64 = 1;
        }
      }
      break;
    }
    default:
      break;
    }
  }
  /* A kernel replaces the loop wholesale, so it has nowhere to put a
   * per-element trap. When this build keeps the runtime checks, that is the
   * difference between it and a build that would vectorize, and it belongs in
   * the message whatever else the loop is doing: the reader's next move is
   * --release, not rewriting a loop that is already fine. */
  const char *keeps_checks =
      ir_region_has_runtime_check(function, begin, end)
          ? "; this build also keeps the runtime checks inside the loop, which "
            "no kernel can carry (--release drops them)"
          : "";
#define IR_SIMD_BAIL(text)                                                       do {                                                                             if (!*keeps_checks) {                                                            return (text);                                                               }                                                                              snprintf(reason_buffer, sizeof(reason_buffer), "%s%s", (text),                          keeps_checks);                                                        return reason_buffer;                                                        } while (0)

  if (has_call) {
    IR_SIMD_BAIL("the loop body contains a function call (only call-free or "
                 "fully-inlined loops vectorize)");
  }
  if (has_new) {
    IR_SIMD_BAIL("the loop body allocates memory (new)");
  }
  if (has_asm) {
    IR_SIMD_BAIL("the loop body contains inline assembly");
  }
  if (ir_region_inner_loop_line(function, begin, end)) {
    /* Check the nest BEFORE the branch count: an inner loop's own exit test
     * and back-edge are extra branches, and calling those a data-dependent
     * branch sends the reader looking for an `if` that is not there. */
    IR_SIMD_BAIL("the loop body contains a nested loop (possibly from an "
                 "inlined call); only the innermost loop of a nest vectorizes");
  }
  if (branch_count > 1 || jump_count > 1) {
    IR_SIMD_BAIL("the loop body branches on data (an `if` or `&&`/`||` per "
                 "iteration); only straight-line loop bodies vectorize");
  }
  if (has_i16) {
    return "the loop accesses 16-bit integers, which have no vectorizer "
           "(use int32/int8, or float32/float64)";
  }
  if (has_i64) {
    return "the loop accesses 64-bit integers, which have no vectorizer "
           "(use int32/int8, or float32/float64)";
  }
  {
    /* A non-reassociable loop-carried recurrence (dependence analysis): a
     * scalar computed from its own previous value through *, /, a shift, or a
     * bitwise/xor op. The iterations form a dependency chain -- a genuine
     * scalar floor, not a missing kernel. */
    const char *ops[6];
    size_t n_ops = 0;
    if (ir_region_find_serial_recurrence(function, begin, end, ops, &n_ops)) {
      return "the loop carries a serial recurrence: a value is computed from "
             "its own previous value through a non-reassociable operation "
             "(`*`, `/`, a shift, or a bitwise/xor op), so the iterations form "
             "a dependency chain; '+'/'-' reductions vectorize, but these do "
             "not";
    }
  }
  /* Nothing about the loop itself disqualifies it, but this build keeps the
   * runtime checks, and a kernel replaces the loop wholesale so it has nowhere
   * to put a per-element trap. Saying so names the difference between this
   * build and one that would vectorize, which "no kernel claimed this shape"
   * does not. */
  /* Honest fallback: we've ruled out the disqualifiers we can detect, so the
   * truthful statement is that no kernel claimed this shape -- NOT an assertion
   * of a specific cause we haven't verified. The checks are appended, never
   * substituted: a non-unit stride does not vectorize under --release either,
   * and naming the checks first would send the reader after a flag that
   * changes nothing. */
  IR_SIMD_BAIL("no vectorizer recognized this loop's shape (e.g. a non-unit "
               "stride, a loop-carried dependence, or a reduction/operation "
               "no kernel covers)");
#undef IR_SIMD_BAIL
}

/* Stable names for the IRSimdBailId schema (internal header). Used by future
 * structured output; kept in one place so the enum and names stay in sync. */
const char *ir_simd_bail_id_name(int id) {
  switch ((IRSimdBailId)id) {
  case IR_SIMD_BAIL_NONE:                return "none";
  case IR_SIMD_BAIL_CALL_IN_BODY:        return "call-in-body";
  case IR_SIMD_BAIL_EXTERN_CALL_IN_BODY: return "extern-call-in-body";
  case IR_SIMD_BAIL_SAFETY_IN_BODY: return "memory-safety-in-body";
  case IR_SIMD_BAIL_INDIRECT_CALL:       return "indirect-call";
  case IR_SIMD_BAIL_ALLOC_IN_BODY:       return "alloc-in-body";
  case IR_SIMD_BAIL_INLINE_ASM:          return "inline-asm";
  case IR_SIMD_BAIL_CONTROL_FLOW:        return "control-flow";
  case IR_SIMD_BAIL_EARLY_EXIT:          return "early-exit";
  case IR_SIMD_BAIL_INT16_ELEMENTS:      return "int16-elements";
  case IR_SIMD_BAIL_INT64_ELEMENTS:      return "int64-elements";
  case IR_SIMD_BAIL_RELOADED_BASE:       return "reloaded-base";
  case IR_SIMD_BAIL_SERIAL_RECURRENCE:   return "serial-recurrence";
  case IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS:  return "mixed-float-widths";
  case IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC: return "byte-sum-narrow-acc";
  case IR_SIMD_BAIL_I32_SUM_NARROW_ACC:  return "int32-sum-narrow-acc";
  case IR_SIMD_BAIL_INLINED_PARAM_LOCAL: return "inlined-param-local";
  case IR_SIMD_BAIL_BODY_LOCAL:          return "body-local";
  case IR_SIMD_BAIL_DOT_SHAPE_ADDRESS:   return "dot-shape-address";
  case IR_SIMD_BAIL_STORE_ONLY_FILL:     return "store-only-fill";
  case IR_SIMD_BAIL_EXTREMUM_SHAPE:      return "extremum-shape";
  case IR_SIMD_BAIL_PREDICATED_COUNT:    return "predicated-count";
  case IR_SIMD_BAIL_CLAMP_STORE:         return "clamp-store";
  case IR_SIMD_BAIL_STRIDED_ACCESS:      return "strided-access";
  case IR_SIMD_BAIL_UNBOUNDED_SHIFT:     return "unbounded-shift";
  case IR_SIMD_BAIL_VARIABLE_SHIFT:      return "variable-shift";
  case IR_SIMD_BAIL_UNRECOGNIZED_SHAPE:  return "unrecognized-shape";
  }
  return "unknown";
}

#define IR_SIMD_SET_DIAG(value)                                                \
  do {                                                                         \
    if (diagnosis_out) {                                                       \
      *diagnosis_out = (value);                                                \
    }                                                                          \
  } while (0)

/* The advice just written tells the reader there is nothing to change: the
 * loop is at its floor, or the gap is the compiler's. It is still worth
 * printing, since "this is fine" is an answer, but it is not a fix and the
 * report's triage must not rank it as one. */
#define IR_SIMD_MARK_ADVISORY()                                                \
  do {                                                                         \
    if (advisory_out) {                                                        \
      *advisory_out = 1;                                                       \
    }                                                                          \
  } while (0)

/* ---- loop-carried recurrence detection (dependence analysis) ---------------
 * A reduction whose only carried operation reassociates ('+'/'-') is NOT a
 * serial bottleneck -- the lanes can sum partials and combine at the end, and
 * the reduction kernels do exactly that. Any other carried operation (*, /, %,
 * the shifts, the bitwise ops, xor, and float '*'/'/') makes each iteration
 * genuinely depend on the previous result, so no lane can start before the one
 * before it finishes. That distinction is the whole diagnosis below. */
static int ir_recur_op_is_reassociable(const char *text) {
  return text && text[0] && !text[1] && (text[0] == '+' || text[0] == '-');
}

/* Record a distinct carried operator (for the human-readable "through `*`,
 * `>>`" list); silently caps the set. The stored pointers are the instruction
 * texts, valid for the lifetime of the diagnosis. */
#define IR_RECUR_MAX_OPS 6
static void ir_recur_note_op(const char **ops, size_t *n_ops, const char *t) {
  if (!t || !t[0]) {
    return;
  }
  for (size_t i = 0; i < *n_ops; i++) {
    if (strcmp(ops[i], t) == 0) {
      return;
    }
  }
  if (*n_ops < IR_RECUR_MAX_OPS) {
    ops[(*n_ops)++] = t;
  }
}

/* Does `op` -- an operand feeding a symbol's in-body definition -- transitively
 * read the PRIOR-iteration value of `sym`? Walks temp producers backward inside
 * the loop region; sets *nonreassoc when any operation on a reaching path does
 * not reassociate, and collects the reaching operators into `ops`. Conservative
 * by construction: an operand it cannot resolve ends that path as "does not
 * reach", so a reported recurrence is always a real one (it never invents a
 * dependence that isn't in the IR). The depth bound also keeps it cheap on the
 * pathological deeply-nested temp chain. */
static int ir_recur_operand_reaches(const IRFunction *function, size_t before,
                                    const IROperand *op, const char *sym,
                                    int *nonreassoc, const char **ops,
                                    size_t *n_ops, int depth) {
  if (!op || depth > 32) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, sym) == 0) {
    return 1; /* a read of the symbol's value from before this iteration */
  }
  if (op->kind != IR_OPERAND_TEMP || !op->name) {
    return 0;
  }
  const IRInstruction *p =
      ir_find_temp_producer_before(function, before, op->name);
  if (!p) {
    return 0;
  }
  /* Only straight-line data ops carry a value chain. A CALL/LOAD result is not
   * the symbol's prior value (a call-in-body is diagnosed separately; a load is
   * fresh array data), so those paths correctly do not reach. */
  if (p->op != IR_OP_BINARY && p->op != IR_OP_CAST && p->op != IR_OP_ASSIGN) {
    return 0;
  }
  size_t pidx = (size_t)(p - function->instructions);
  int found = 0;
  if (ir_recur_operand_reaches(function, pidx, &p->lhs, sym, nonreassoc, ops,
                               n_ops, depth + 1)) {
    found = 1;
  }
  if (ir_recur_operand_reaches(function, pidx, &p->rhs, sym, nonreassoc, ops,
                               n_ops, depth + 1)) {
    found = 1;
  }
  if (found && p->op == IR_OP_BINARY && p->text) {
    ir_recur_note_op(ops, n_ops, p->text);
    if (!ir_recur_op_is_reassociable(p->text)) {
      *nonreassoc = 1;
    }
  }
  return found;
}

/* The loop's first non-reassociable loop-carried recurrence: a scalar symbol
 * whose in-body value is computed from its own previous value through at least
 * one operation that does not reassociate. Returns the symbol (NULL when none),
 * filling `ops` with the distinct carried operators for the message. Handles
 * both the direct form (`s = s <op> x`) and the temp+ASSIGN form
 * (`%t = s <op> x; s <- %t`). The induction variable's own `i = i + 1` is a
 * reassociable '+' recurrence and is therefore never reported. */
static const char *ir_region_find_serial_recurrence(const IRFunction *function,
                                                    size_t begin, size_t end,
                                                    const char **ops,
                                                    size_t *n_ops) {
  int past_header = 0;
  *n_ops = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (!past_header || ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    int nonreassoc = 0, reaches = 0;
    const char *cand_ops[IR_RECUR_MAX_OPS];
    size_t cand_n = 0;
    if (ins->op == IR_OP_BINARY) {
      if (ir_recur_operand_reaches(function, i, &ins->lhs, ins->dest.name,
                                   &nonreassoc, cand_ops, &cand_n, 0)) {
        reaches = 1;
      }
      if (ir_recur_operand_reaches(function, i, &ins->rhs, ins->dest.name,
                                   &nonreassoc, cand_ops, &cand_n, 0)) {
        reaches = 1;
      }
      if (reaches && ins->text) {
        ir_recur_note_op(cand_ops, &cand_n, ins->text);
        if (!ir_recur_op_is_reassociable(ins->text)) {
          nonreassoc = 1;
        }
      }
    } else if (ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_TEMP) {
      reaches = ir_recur_operand_reaches(function, i, &ins->lhs, ins->dest.name,
                                         &nonreassoc, cand_ops, &cand_n, 0);
    }
    if (reaches && nonreassoc) {
      for (size_t k = 0; k < cand_n; k++) {
        ops[k] = cand_ops[k];
      }
      *n_ops = cand_n;
      return ins->dest.name;
    }
  }
  return NULL;
}

/* Which float width dominates the region's memory traffic (defined with the
 * fix simulations below, and used by the diagnosis to name it). */
static long long ir_simd_majority_float_width(const IRFunction *function,
                                              size_t begin, size_t end);

/* The element type of a declared array type: "float32[64]" -> "float32".
 * Falls back to a readable placeholder so the advice still parses as English
 * when the declaration is a shape this does not model. */
static void ir_type_element_name(const char *declared, char *out, size_t cap) {
  const char *bracket = declared ? strchr(declared, '[') : NULL;
  size_t n = bracket ? (size_t)(bracket - declared) : 0;
  while (n > 0 && declared[n - 1] == ' ') {
    n--;
  }
  if (n == 0 || n >= cap) {
    snprintf(out, cap, "T");
    return;
  }
  memcpy(out, declared, n);
  out[n] = '\0';
}

static const char *ir_simd_field_base_symbol(const IRFunction *function,
                                             size_t begin, size_t at,
                                             const IROperand *addr) {
  if (addr->kind == IR_OPERAND_SYMBOL && addr->name) {
    return addr->name;
  }
  if (addr->kind != IR_OPERAND_TEMP || !addr->name) {
    return NULL;
  }
  for (size_t k = at; k-- > begin;) {
    const IRInstruction *def = &function->instructions[k];
    if (def->dest.kind != IR_OPERAND_TEMP || !def->dest.name ||
        strcmp(def->dest.name, addr->name) != 0) {
      continue;
    }
    if (def->op == IR_OP_BINARY && !def->is_float && def->text &&
        strcmp(def->text, "+") == 0 && def->lhs.kind == IR_OPERAND_SYMBOL &&
        def->lhs.name && def->rhs.kind == IR_OPERAND_INT) {
      return def->lhs.name;
    }
    return NULL;
  }
  return NULL;
}

/* --explain: a deeper diagnosis than ir_simd_bail_reason, split into a reason
 * (what blocked vectorization), a fix (what the user can change), and a
 * machine-readable IRSimdBailId every branch must set. Best-effort but never
 * speculative: each claim is derived from instructions actually present in
 * the region. Empty fix = nothing actionable. */
static void ir_simd_explain_bail(const IRFunction *function, size_t begin,
                                 size_t end, char *reason, size_t reason_cap,
                                 char *fix, size_t fix_cap, int *diagnosis_out,
                                 int *advisory_out) {
  reason[0] = '\0';
  fix[0] = '\0';
  if (advisory_out) {
    *advisory_out = 0;
  }
  IR_SIMD_SET_DIAG(IR_SIMD_BAIL_UNRECOGNIZED_SHAPE);

  const char *callee = NULL;
  int has_indirect_call = 0, has_new = 0, has_asm = 0;
  int branch_count = 0, jump_count = 0;
  const char *branch_targets[8];
  size_t branch_target_count = 0;
  int has_return_in_body = 0;
  int has_i16 = 0, has_i64 = 0, has_f32 = 0, has_f64 = 0;
  int has_byte_load = 0, has_i32_load = 0, has_int_accum = 0;
  const char *int_accum_sym = NULL;
  int has_float_accum = 0, has_float_mul = 0;
  int load_count = 0, store_count = 0, byte_store_count = 0;
  int reloaded_base_count = 0;
  const char *reloaded_base_sym = NULL;
  int past_header = 0; /* seen the loop's own header label yet? */
  const char *body_local = NULL;
  /* A stack array whose address the body re-takes every iteration
   * (`%t <- &@a` inside the loop). The kernels index off an invariant base
   * SYMBOL, so a base that is a fresh temp each iteration never matches --
   * even though the source reads `a[i]`, which is exactly the shape the
   * generic advice would tell the writer to produce. Naming the array lets
   * the fill diagnosis below give advice that can actually be followed. */
  const char *rebased_array = NULL;

  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    /* The marker region starts BEFORE a for-loop's initializer, and hoisted
     * preamble code (pure-call LICM results, pointer setup) lands there too.
     * Everything before the header label runs ONCE -- it is not the loop, so
     * it must not drive the diagnosis. */
    if (!past_header) {
      continue;
    }
    switch (ins->op) {
    case IR_OP_DECLARE_LOCAL:
      /* A local declared INSIDE the loop body (after the header label -- a
       * range-for's induction local sits between the markers but BEFORE the
       * header and is fine) is one no recognizer's load->compute->store
       * matching can see through. The common source is an inlined callee's
       * parameter copy that copy-propagation couldn't fold (float32 narrowing
       * keeps the copy alive). */
      if (past_header && !body_local && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name) {
        body_local = ins->dest.name;
      }
      break;
    case IR_OP_ADDRESS_OF:
      /* `%t <- &@a` in the body: record the array it names, once. */
      if (!rebased_array && ins->lhs.kind == IR_OPERAND_SYMBOL &&
          ins->lhs.name) {
        const char *declared =
            ir_function_local_declared_type(function, ins->lhs.name);
        if (declared && strchr(declared, '[')) {
          rebased_array = ins->lhs.name;
        }
      }
      break;
    case IR_OP_CALL:
      if (!(ins->text && strstr(ins->text, "crash_trap")) && !callee) {
        callee = ins->text ? ins->text : "?";
      }
      break;
    case IR_OP_CALL_INDIRECT:
      has_indirect_call = 1;
      break;
    case IR_OP_NEW:
      has_new = 1;
      break;
    case IR_OP_INLINE_ASM:
      has_asm = 1;
      break;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!ir_label_is_runtime_check(ins->text)) {
        branch_count++;
        if (ins->text && branch_target_count < 8) {
          branch_targets[branch_target_count++] = ins->text;
        }
      }
      break;
    case IR_OP_JUMP:
      if (!ir_label_is_runtime_check(ins->text)) {
        jump_count++;
      }
      break;
    case IR_OP_RETURN:
      /* A return INSIDE the loop body is the definitive early-exit marker
       * (`if (...) return x;` -- the find/compare/parse shape). */
      has_return_in_body = 1;
      break;
    case IR_OP_LOAD:
    case IR_OP_STORE: {
      long long sz = (ins->rhs.kind == IR_OPERAND_INT) ? ins->rhs.int_value : 4;
      if (ins->op == IR_OP_LOAD) {
        load_count++;
      } else {
        store_count++;
        if (sz == 1) {
          byte_store_count++;
        }
      }
      if (ins->is_float) {
        if (sz == 4) {
          has_f32 = 1;
        } else if (sz == 8) {
          has_f64 = 1;
        }
      } else {
        if (sz == 1 && ins->op == IR_OP_LOAD) {
          has_byte_load = 1;
        } else if (sz == 2) {
          has_i16 = 1;
        } else if (sz == 4 && ins->op == IR_OP_LOAD) {
          has_i32_load = 1;
        } else if (sz == 8) {
          const char *base =
              (ins->op == IR_OP_LOAD && ins->dest.kind == IR_OPERAND_TEMP)
                  ? ir_simd_field_base_symbol(function, begin, i, &ins->lhs)
                  : NULL;
          if (base) {
            reloaded_base_count++;
            if (!reloaded_base_sym) {
              reloaded_base_sym = base;
            }
          } else {
            has_i64 = 1;
          }
        }
      }
      break;
    }
    case IR_OP_BINARY: {
      /* Integer '+' accumulation of a computed value into a symbol
       * (`total = total + %t`): together with byte loads this identifies the
       * vpsadbw byte-sum shape, whose kernel requires an int64 accumulator.
       * The added operand must be a temp -- `i = i + 1` (a constant) is an
       * induction variable, not a data sum. */
      if (!ins->is_float && ins->text && ins->text[0] == '+' &&
          !ins->text[1] && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ((ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
            strcmp(ins->lhs.name, ins->dest.name) == 0 &&
            ins->rhs.kind == IR_OPERAND_TEMP) ||
           (ins->rhs.kind == IR_OPERAND_SYMBOL && ins->rhs.name &&
            strcmp(ins->rhs.name, ins->dest.name) == 0 &&
            ins->lhs.kind == IR_OPERAND_TEMP))) {
        has_int_accum = 1;
        int_accum_sym = ins->dest.name;
      }
      /* Float multiply + float '+' accumulation = a dot-product-shaped
       * reduction (used below to give an address-pattern diagnosis when no
       * kernel claimed it). */
      if (ins->is_float && ins->text && ins->text[0] == '*' && !ins->text[1]) {
        has_float_mul = 1;
      }
      if (ins->is_float && ins->text && ins->text[0] == '+' &&
          !ins->text[1] &&
          ((ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name) ||
           (ins->rhs.kind == IR_OPERAND_SYMBOL && ins->rhs.name))) {
        has_float_accum = 1;
      }
      break;
    }
    default:
      break;
    }
  }

  if (callee) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (!in->text || strcmp(in->text, callee) ||
          ir_safety_intrinsic(in) == IR_SAFETY_INTRINSIC_NONE) continue;
      snprintf(reason, reason_cap,
               "each iteration retains a memory safety operation; its bounds "
               "or pointer origin could not be proved independent of the iteration");
      snprintf(fix, fix_cap,
               "keep the checked scalar loop, or expose a fixed array extent "
               "and an affine index so the compiler can prove the range");
      IR_SIMD_MARK_ADVISORY();
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_SAFETY_IN_BODY);
      return;
    }
    /* Advice differs fundamentally by what the callee IS: a program-defined
     * function can be inlined (and that fix can be simulated); an extern has
     * no body, so "mark it @inline" would be advice that cannot work. */
    IRFunction *callee_fn =
        g_explain_program ? ir_program_find_function(g_explain_program, callee)
                          : NULL;
    if (g_explain_program && !callee_fn) {
      snprintf(reason, reason_cap,
               "each iteration calls `%s`, an external function with no body "
               "this compiler can see, so it can never be inlined and this "
               "loop cannot vectorize as written",
               callee);
      snprintf(fix, fix_cap,
               "none needed if the call IS the work (I/O, OS calls): the "
               "scalar loop is the right code; if the call is loop-invariant, "
               "hoist it; if it is hot compute, replace it with Mettle code "
               "so the inliner can take it");
      IR_SIMD_MARK_ADVISORY();
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_EXTERN_CALL_IN_BODY);
      return;
    }
    snprintf(reason, reason_cap,
             "each iteration calls `%s`; loops vectorize only after every "
             "call in the body has been inlined away",
             callee);
    snprintf(fix, fix_cap,
             "make `%s` inline-eligible (small body, or mark it @inline), or "
             "hoist the call out of the loop",
             callee);
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_CALL_IN_BODY);
    return;
  }
  if (has_indirect_call) {
    snprintf(reason, reason_cap,
             "each iteration calls through a function pointer, which can "
             "never be inlined away");
    snprintf(fix, fix_cap,
             "call the target directly if it is known at compile time");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INDIRECT_CALL);
    return;
  }
  if (has_new) {
    snprintf(reason, reason_cap, "the loop body allocates memory (`new`) "
                                 "every iteration");
    snprintf(fix, fix_cap, "hoist the allocation out of the loop");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_ALLOC_IN_BODY);
    return;
  }
  if (has_asm) {
    snprintf(reason, reason_cap,
             "the loop body contains inline assembly, which is opaque to the "
             "vectorizer");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INLINE_ASM);
    return;
  }
  if (branch_count > 1 || jump_count > 1) {
    /* Early exit vs internal branching deserve OPPOSITE advice. A branch
     * whose target label is not inside the region leaves the loop before the
     * trip count -- a compare/search shape where the early exit IS the
     * algorithm and scalar code is the right output. A branch that stays
     * inside the region is a data-dependent diamond, where branchless
     * rewriting is real advice. (The first body branch is usually the
     * loop's own exit test, so it is expected to leave the region; only an
     * ADDITIONAL outward branch marks an early exit.) */
    int outward_branches = 0;
    for (size_t t = 0; t < branch_target_count; t++) {
      int inside = 0;
      for (size_t i = begin + 1; i < end && !inside; i++) {
        const IRInstruction *lab = &function->instructions[i];
        if (lab->op == IR_OP_LABEL && lab->text &&
            strcmp(lab->text, branch_targets[t]) == 0) {
          inside = 1;
        }
      }
      if (!inside) {
        outward_branches++;
      }
    }
    if (has_return_in_body || outward_branches > 1) {
      snprintf(reason, reason_cap,
               "the loop can exit before its trip count, and its body does "
               "more than test the exit condition; the search skip-ahead "
               "kernel only covers pure find/mismatch loops (it must be safe "
               "to skip the iterations before the first hit)");
      snprintf(fix, fix_cap,
               "pure searches DO vectorize: `if (a[i] == key) ...` (or != < > "
               "<= >=, key a constant/variable, or a[i] != b[i]) with nothing "
               "else in the body becomes an 8-wide compare+movemask scan. "
               "Split any per-iteration work out of this loop, or hoist the "
               "search into its own loop and process from the found index");
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_EARLY_EXIT);
      return;
    }
    {
      const char *written = NULL;
      IRBranchShape shape =
          ir_region_branch_shape(function, begin, end, &written);
      if (shape == IR_BRANCH_SHAPE_EXTREMUM) {
        /* A running extremum HAS a kernel (vmaxps/vmaxpd/vpmaxsd), so the
         * branch is not what stopped this one. Say what did, or the reader
         * rewrites a loop whose branch was never the problem. */
        const char *acc_type =
            written ? ir_function_local_declared_type(function, written) : NULL;
        snprintf(reason, reason_cap,
                 "this is a running %s, a shape that does vectorize, but "
                 "the kernel did not claim it%s%s%s",
                 "minimum/maximum",
                 acc_type ? " (`" : "", acc_type ? written : "",
                 acc_type ? "` is the accumulator)" : "");
        if (acc_type && strcmp(acc_type, "float64") != 0 &&
            strcmp(acc_type, "float32") != 0 && strcmp(acc_type, "int32") != 0) {
          /* The lane width comes from the ELEMENTS, so widening the
             accumulator alone moves nothing: the loop comes back refused for
             the widening it now does in the body. Name the array. */
          snprintf(fix, fix_cap,
                   "widen the array to int32 elements (float32 and float64 "
                   "work too): the extremum kernel carries those lane widths "
                   "and these are %s. Declaring `%s` alone does not move it, "
                   "because the width the kernel reads is the element's",
                   acc_type, written);
        } else {
          snprintf(fix, fix_cap,
                   "the extremum kernel needs the compared value to come from "
                   "`base[i]` over float32, float64 or int32 elements, and the "
                   "counter to start at 0 or at 1 after seeding from `a[0]`; "
                   "one that widens narrower elements, or indexes anything "
                   "else, falls outside it");
        }
        IR_SIMD_SET_DIAG(IR_SIMD_BAIL_EXTREMUM_SHAPE);
        return;
      }
      if (shape == IR_BRANCH_SHAPE_COUNT) {
        snprintf(reason, reason_cap,
                 "the body accumulates into `%s` under a condition. Over int32 "
                 "elements that is made unconditional and vectorized, so this "
                 "one is out for one of three reasons: the elements are float "
                 "(no kernel, and the branchless form has none either), the "
                 "guard is not a comparison (`if (x & 6)` fires on 2, 4 and 6 "
                 "alike, and the addend would be multiplied by those), or an "
                 "else arm writes `%s` too, which is a select rather than an "
                 "accumulate",
                 written ? written : "the accumulator",
                 written ? written : "the accumulator");
        /* Respelling the guard does not help: a guard on a computed value is
           refused whatever it is compared against. Adding the comparison is
           what the kernel takes, and it takes every comparison. */
        snprintf(fix, fix_cap,
                 "over int32 elements, add the comparison rather than "
                 "branching on it: `%s = %s + ((a[i] & 6) != 0);` vectorizes, "
                 "and so does any other comparison written that way. Over "
                 "float elements there is nothing to change here",
                 written ? written : "count", written ? written : "count");
        IR_SIMD_SET_DIAG(IR_SIMD_BAIL_PREDICATED_COUNT);
        return;
      }
      if (shape == IR_BRANCH_SHAPE_CLAMP_STORE) {
        snprintf(reason, reason_cap,
                 "the body chooses `%s` under a condition before storing it. "
                 "That shape does vectorize over int32 elements, as vpminsd, "
                 "vpmaxsd or a lane select, so what stopped this one is either "
                 "the element type (no float select kernel yet) or a nest too "
                 "deep for the six ymm registers an int32 map has",
                 written ? written : "a local");
        snprintf(fix, fix_cap,
                 "if the elements are int32, split the deepest arm into its own "
                 "loop so fewer values are live at once; otherwise this is a "
                 "gap in the compiler, not a problem with the loop");
        IR_SIMD_MARK_ADVISORY();
        IR_SIMD_SET_DIAG(IR_SIMD_BAIL_CLAMP_STORE);
        return;
      }
    }
    snprintf(reason, reason_cap,
             "the loop body branches on data (an `if` or `&&`/`||` per "
             "iteration). An `if` that only chooses a value is converted to a "
             "lane select and does vectorize, so this one does something a "
             "masked lane cannot: a store, a call, or a nest deeper than the "
             "kernel has registers for");
    snprintf(fix, fix_cap,
             "keep the arms to choosing a value, or split the work into two "
             "simpler loops");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_CONTROL_FLOW);
    return;
  }
  if (has_i16) {
    snprintf(reason, reason_cap,
             "the loop reads/writes 16-bit integers, and no 16-bit kernels "
             "exist");
    if (has_int_accum) {
      /* The int32 sum kernel additionally requires an int64 accumulator, so
       * for a sum loop the honest fix names every needed change (and the
       * paired hypothesis transform simulates exactly this). When the
       * accumulator is already int64, retyping the elements is the whole
       * fix. */
      const char *acc_type =
          int_accum_sym ? ir_function_local_declared_type(function,
                                                          int_accum_sym)
                        : NULL;
      if (acc_type && strcmp(acc_type, "int64") == 0) {
        snprintf(fix, fix_cap, "use int32 elements");
      } else {
        snprintf(fix, fix_cap,
                 "use int32 elements and declare the accumulator as int64");
      }
    } else {
      snprintf(fix, fix_cap, "use int32 (or int8 if the values fit)");
    }
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INT16_ELEMENTS);
    return;
  }
  if (has_i64) {
    snprintf(reason, reason_cap,
             "the loop reads/writes 64-bit integer arrays, and no 64-bit "
             "integer kernels exist");
    snprintf(fix, fix_cap, "use int32 arrays if the values fit");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INT64_ELEMENTS);
    return;
  }
  if (reloaded_base_count > 0 &&
      load_count + store_count > reloaded_base_count) {
    snprintf(reason, reason_cap,
             "the body re-reads the array's base pointer out of `%s` every "
             "iteration; a store in the body could change that pointer, so the "
             "read cannot be lifted out and the kernels have no base to hold "
             "fixed for the whole loop",
             reloaded_base_sym ? reloaded_base_sym : "a struct");
    snprintf(fix, fix_cap,
             "bind the base pointer to a local before the loop and index that "
             "local in the body");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_RELOADED_BASE);
    return;
  }
  {
    /* Loop-carried serial recurrence (dependence analysis): a scalar computed
     * from its own previous value through a non-reassociable operation -- the
     * leaf_call hash/RNG shape (`acc = (acc*K + C) ^ (i + (acc>>7))`), a float
     * product/quotient chain, an IIR filter. The lanes form a dependency chain
     * and cannot run independently, so this is a genuine scalar floor, not a
     * missing kernel. */
    const char *ops[IR_RECUR_MAX_OPS];
    size_t n_ops = 0;
    const char *recur_symbol =
        ir_region_find_serial_recurrence(function, begin, end, ops, &n_ops);
    if (recur_symbol) {
      char op_list[64];
      size_t w = 0;
      op_list[0] = '\0';
      for (size_t i = 0; i < n_ops && w + 10 < sizeof(op_list); i++) {
        int n = snprintf(op_list + w, sizeof(op_list) - w, "%s`%s`",
                         i ? ", " : "", ops[i]);
        if (n < 0) {
          break;
        }
        w += (size_t)n;
      }
      snprintf(reason, reason_cap,
               "`%s` carries a loop-carried recurrence: each iteration computes "
               "it from its own previous value (through %s), so the iterations "
               "form a dependency chain that cannot run as independent SIMD "
               "lanes",
               recur_symbol, n_ops ? op_list : "a non-reassociable operation");
      snprintf(fix, fix_cap,
               "'+'/'-' reductions reassociate and DO vectorize; multiply, "
               "divide, shift, and bitwise/xor recurrences are inherently "
               "serial. If this running state IS the algorithm (a hash, an "
               "RNG, an IIR filter), the loop is already at its scalar floor");
      IR_SIMD_MARK_ADVISORY();
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_SERIAL_RECURRENCE);
      return;
    }
  }
  if (has_f32 && has_f64) {
    /* Naming the width turns "pick one" into an instruction, and it is the
     * width the paired simulation converges on, so the kernel the report
     * promises is the one this edit produces. The minority is what a reader
     * would retype: it is the odd array out. */
    long long keep = ir_simd_majority_float_width(function, begin, end);
    snprintf(reason, reason_cap,
             "the loop mixes float32 and float64 elements; each kernel "
             "handles one width");
    if (keep == 4) {
      snprintf(fix, fix_cap,
               "keep the loop in float32: the float64 accesses are the "
               "minority, so retype those arrays (or convert outside the "
               "loop)");
    } else if (keep == 8) {
      snprintf(fix, fix_cap,
               "keep the loop in float64: the float32 accesses are the "
               "minority, so retype those arrays (or convert outside the "
               "loop)");
    } else {
      snprintf(fix, fix_cap, "keep the loop in a single float width");
    }
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS);
    return;
  }
  if (has_byte_load && has_int_accum) {
    snprintf(reason, reason_cap,
             "this is a byte-sum loop, but the vpsadbw kernel accumulates "
             "into int64 and this loop's accumulator is narrower");
    snprintf(fix, fix_cap,
             "declare the accumulator as int64 (sum bytes as "
             "`total = total + (int64)data[i]`)");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC);
    return;
  }
  if (has_i32_load && int_accum_sym) {
    const char *acc_type =
        ir_function_local_declared_type(function, int_accum_sym);
    if (!acc_type || strcmp(acc_type, "int64") != 0) {
      snprintf(reason, reason_cap,
               "this loop sums int32 values into `%s`, but the int32 "
               "reduction kernel accumulates into int64 (eight lanes are "
               "summed without overflow only there) and `%s` is %s",
               int_accum_sym, int_accum_sym, acc_type ? acc_type : "narrower");
      snprintf(fix, fix_cap, "declare the accumulator `%s` as int64",
               int_accum_sym);
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_I32_SUM_NARROW_ACC);
      return;
    }
  }
  if (body_local) {
    if (strstr(body_local, "__inl_") != NULL) {
      snprintf(reason, reason_cap,
               "the body's data flow passes through the local `%s`, left over "
               "from an inlined call; the recognizers' "
               "load\xE2\x86\x92" "compute\xE2\x86\x92" "store matching "
               "cannot see through it",
               body_local);
      snprintf(fix, fix_cap,
               "a compiler limitation, not a code problem; write the "
               "expression directly in the loop body to vectorize today");
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INLINED_PARAM_LOCAL);
    } else {
      snprintf(reason, reason_cap,
               "the body declares the local `%s` each iteration; the "
               "recognizers' load\xE2\x86\x92" "compute\xE2\x86\x92" "store "
               "matching cannot see through it",
               body_local);
      snprintf(fix, fix_cap,
               "declare `%s` before the loop, or fold the expression in "
               "directly",
               body_local);
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_BODY_LOCAL);
    }
    return;
  }
  if (has_float_mul && has_float_accum && load_count >= 2) {
    snprintf(reason, reason_cap,
             "this is a float multiply-accumulate (dot-product shape), but no "
             "kernel matched its address pattern. The bases must be plain "
             "pointers indexed by the loop counter (base[i])");
    snprintf(fix, fix_cap,
             "hoist invariant index math into a pointer before the loop "
             "(e.g. `var row: float32* = &m[r * cols];` then `row[c]`)");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_DOT_SHAPE_ADDRESS);
    return;
  }
  if (store_count > 0 && load_count == 0) {
    /* Three causes are distinguishable here, and the generic address message
     * is wrong for all of them: it tells a writer who already wrote `a[i]` to
     * make the index unit-stride. Check the specific ones first. */
    if (store_count > 1) {
      snprintf(reason, reason_cap,
               "the body writes %d destinations; the fill kernel fills one "
               "region per loop, so a body with several stores has no single "
               "region to fill",
               store_count);
      snprintf(fix, fix_cap,
               "split it into one loop per destination; each becomes its own "
               "fill kernel");
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_STORE_ONLY_FILL);
      return;
    }
    if (byte_store_count == store_count) {
      snprintf(reason, reason_cap,
               "the loop fills 1-byte elements, and the fill kernel covers "
               "2-, 4- and 8-byte elements only");
      snprintf(fix, fix_cap,
               "nothing to change here: this is a gap in the compiler, not a "
               "problem with the loop");
      IR_SIMD_MARK_ADVISORY();
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_STORE_ONLY_FILL);
      return;
    }
    if (rebased_array) {
      /* The writer already wrote `a[i]`. Telling them to make the index
       * unit-stride would be advice they have followed, so name the real
       * obstacle: a stack array's address is retaken each iteration, and the
       * kernel needs one invariant base. Binding a pointer once fixes it and
       * costs nothing at runtime. */
      char element[64];
      ir_type_element_name(
          ir_function_local_declared_type(function, rebased_array), element,
          sizeof(element));
      snprintf(reason, reason_cap,
               "the loop fills the stack array `%s`, whose address is retaken "
               "on every iteration; the fill kernel indexes off one invariant "
               "base pointer, and a fresh base each iteration is not one",
               rebased_array);
      snprintf(fix, fix_cap,
               "bind the array to a pointer once before the loop "
               "(`var p: %s* = &%s[0];`) and write `p[i]` in the body",
               element, rebased_array);
    } else {
      snprintf(reason, reason_cap,
               "the loop only writes an invariant value (a fill/init pattern), "
               "but its store address did not match the fill vectorizer's "
               "shapes: a unit-stride element `a[i]`, `a[c + i]` with `c` a "
               "loop-invariant scalar, or a pointer walked by a constant "
               "stride");
      snprintf(fix, fix_cap,
               "hoist the invariant part of the index into a base pointer "
               "before the loop (`var row = &a[c]; ... row[i] = v;`) so the "
               "write is a plain unit-stride `row[i]`");
    }
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_STORE_ONLY_FILL);
    return;
  }
  {
    /* Before the catch-all: a stride the reader wrote deliberately. Telling
     * them to "use unit-stride `a[i]`" is not advice for `rgb[i*3+1]` -- the
     * layout is the point -- so name the stride and say it is a gap. */
    long long stride = ir_region_strided_access(function, begin, end);
    if (stride > 1) {
      snprintf(reason, reason_cap,
               "the loop steps %lld elements at a time (`a[i*%lld]` or "
               "similar); every kernel walks its arrays one contiguous vector "
               "per iteration, so no gather/scatter shape is covered",
               stride, stride);
      snprintf(fix, fix_cap,
               "nothing to change here unless the layout can change: %s "
               "arrays (one per component) make each loop unit-stride and all "
               "of them vectorize",
               "separate");
      IR_SIMD_MARK_ADVISORY();
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_STRIDED_ACCESS);
      return;
    }
  }
  {
    /* A `>>` the lanes cannot take. Every other integer op is congruent mod
     * 2^32, so 32-bit lanes reproduce it whatever width the scalar used; a
     * right shift reads bits back DOWN, and a lane that wrapped where the
     * scalar did not would shift different ones. The recognizer takes the
     * shift only where the shifted value is provably inside int32, and a
     * runtime factor in the expression makes that unprovable. */
    int shift_count = 0;
    int variable_shift = 0;
    for (size_t i = begin + 1; i < end; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op != IR_OP_BINARY || ins->is_float || !ins->text) {
        continue;
      }
      int is_shift = strcmp(ins->text, ">>") == 0 ||
                     strcmp(ins->text, "<<") == 0;
      if (!is_shift) {
        continue;
      }
      if (ins->rhs.kind == IR_OPERAND_INT) {
        if (strcmp(ins->text, ">>") == 0) {
          shift_count++;
        }
      } else {
        variable_shift++;
      }
    }
    /* A shift by a value the loop reads has no kernel at all, and no source
       rewrite reaches one. Saying so beats dropping through to the catch-all,
       which offers a shape checklist this loop already satisfies. */
    if (variable_shift > 0 && load_count > 0) {
      snprintf(reason, reason_cap,
               "the body shifts by a value that is not a constant; the kernels "
               "carry a shift only when the distance is written in the source, "
               "so a distance read at run time has none");
      snprintf(fix, fix_cap,
               "nothing to change here unless the distance can be a constant: "
               "a loop per distance, or a constant, both vectorize. This is a "
               "gap in the compiler rather than a problem with the loop");
      IR_SIMD_MARK_ADVISORY();
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_VARIABLE_SHIFT);
      return;
    }
    if (shift_count > 0 && load_count > 0 && store_count > 0) {
      snprintf(reason, reason_cap,
               "the body shifts right, and the value being shifted cannot be "
               "shown to stay inside int32; the lanes are 32 bits wide, so a "
               "shift is only reproduced exactly when no wider intermediate "
               "could have been shifted");
      /* The old text here offered `(r*77 + g*150 + b*29) >> 8` as the shape
         that works. It is the shape being refused, so it read as the compiler
         citing the reader's own code back as the counterexample. Masking is
         the change that moves it, and it is the only one. */
      snprintf(fix, fix_cap,
               "mask the value down to the bits the shift needs: "
               "`(x & 65535) >> 8` bounds it inside int32 and the kernel then "
               "takes it. A constant multiplier is not enough on its own");
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_UNBOUNDED_SHIFT);
      return;
    }
  }
  {
    /* The row-major inner loop. Unit-stride, but indexed off `base + invariant`
     * rather than `base`, which is the one address form the kernels walk. */
    const char *base = NULL;
    if (ir_region_invariant_index_term(function, begin, end, &base)) {
      snprintf(reason, reason_cap,
               "the accesses into `%s` add a loop-invariant term to the "
               "counter (`%s[k + i]`); the kernels walk one base pointer from "
               "element 0, so the index must be the counter alone",
               base, base);
      snprintf(fix, fix_cap,
               "bind the row to a pointer before the loop (`var row = &%s[k];`) "
               "and index it with the counter (`row[i]`): same addresses, and "
               "the shape the kernels read",
               base);
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_DOT_SHAPE_ADDRESS);
      return;
    }
  }
  /* Reaching here means every disqualifier above was checked and none held,
   * so the honest statement is that no recognizer claimed the loop, not a
   * guess at which requirement it missed.
   *
   * "by the time the vectorizers ran" is load-bearing. The report quotes the
   * source line right above this, and that line may still show a call the
   * inliner has since removed; without the qualifier the sentence reads as a
   * denial of what the reader can see. */
  snprintf(reason, reason_cap,
           "no vectorizer recognized this loop's shape: by the time the "
           "vectorizers ran, its body had no call, branch, unsupported "
           "element width or carried dependence left to blame");
  /* A checklist of what the kernels take, which is a description rather than
     an instruction: it names no edit to this loop. Advisory keeps it out of
     "where to start", where it used to outrank nothing and get read as the
     compiler's best idea. The reader still sees it under the loop. */
  snprintf(fix, fix_cap,
           "compare the loop against the shapes that do vectorize: "
           "unit-stride `a[i]` (not `a[i*k]`) over int8/int32/float32/float64, "
           "a straight-line body, and one of a map (`a[i] = expr`), a '+' "
           "reduction (`s = s + expr`), or a dot product");
  IR_SIMD_MARK_ADVISORY();
}

/* ---- fix hypothesis simulation ---------------------------------------------
 * "Verified" fix suggestions: apply the suggested source change as an
 * equivalent IR rewrite on a scratch clone, re-run the real vectorization
 * stages on it, and check whether a kernel claimed the loop. Only then does
 * the report print `verified: with that change ...` -- the claim is the
 * optimizer's own acceptance, not a prediction. */

/* Marker id of the loop beginning at `begin` (-1 when unparsable). The clone's
 * instruction indexes shift when passes rewrite it, so the loop is re-located
 * by this id afterwards. */
static int ir_simd_marker_id_at(const IRFunction *function, size_t begin) {
  const IRInstruction *marker = &function->instructions[begin];
  char which = 0;
  int id = 0, mode = 0;
  if (!ir_instruction_is_simd_marker(marker) ||
      sscanf(marker->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d", &which,
             &id, &mode) != 3) {
    return -1;
  }
  return id;
}

/* Find the B/E marker pair with `id` in `function`; returns 1 and fills the
 * region bounds on success. */
static int ir_simd_find_marker_region(const IRFunction *function, int id,
                                      size_t *begin_out, size_t *end_out) {
  size_t begin = (size_t)-1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    char which = 0;
    int marker_id = 0, mode = 0;
    if (!ir_instruction_is_simd_marker(ins) ||
        sscanf(ins->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d", &which,
               &marker_id, &mode) != 3 ||
        marker_id != id) {
      continue;
    }
    if (which == 'B') {
      begin = i;
    } else if (begin != (size_t)-1) {
      *begin_out = begin;
      *end_out = i;
      return 1;
    }
  }
  return 0;
}

/* A fix mutator applies one suggested source fix to the CLONE as the
 * equivalent IR rewrite, scoped to the loop region [begin, end]. Returns 1
 * when the rewrite was applied (the simulation may proceed), 0 when the
 * expected shape wasn't found (no claim is made), and -1 when the mutator
 * POSITIVELY established the suggested fix cannot be written for this loop
 * (the report then replaces the advice instead of printing it). The clone is
 * disposable: it only has to convince the recognizers, not execute -- which
 * is what keeps mutators small. */
#define IR_SIMD_FIX_INAPPLICABLE (-1)
typedef int (*IRSimdFixMutator)(IRFunction *clone, size_t begin, size_t end);

/* Widen the loop's integer accumulator to int64 on the clone, the way the
 * suggested source fix would: retype its DECLARE_LOCAL, and retarget the
 * widening cast feeding the accumulation when one exists. The byte-sum kernel
 * insists on the cast form (`s += (int64)a[i]`); the int32 sum kernel also
 * accepts the cast-free widening form, so `cast_required` distinguishes them. */
static int ir_simd_widen_accumulator(IRFunction *clone, size_t begin,
                                     size_t end, int cast_required) {
  int rewrote_cast = 0, rewrote_decl = 0;
  const char *acc_symbol = NULL;
  /* Locate the accumulation `S = S + %t` in the region. */
  for (size_t i = begin + 1; i < end && !acc_symbol; i++) {
    const IRInstruction *ins = &clone->instructions[i];
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name &&
        ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
        strcmp(ins->lhs.name, ins->dest.name) == 0) {
      acc_symbol = ins->dest.name;
      /* The widening cast that produces %t, scanning backwards. */
      for (size_t j = i; j-- > begin;) {
        IRInstruction *cast = &clone->instructions[j];
        if (cast->op == IR_OP_CAST && cast->dest.kind == IR_OPERAND_TEMP &&
            cast->dest.name && strcmp(cast->dest.name, ins->rhs.name) == 0) {
          mettle_free_string(cast->text);
          cast->text = mettle_strdup("int64");
          rewrote_cast = cast->text != NULL;
          break;
        }
      }
    }
  }
  if (!acc_symbol) {
    return 0;
  }
  for (size_t i = 0; i < clone->instruction_count; i++) {
    IRInstruction *decl = &clone->instructions[i];
    if (decl->op == IR_OP_DECLARE_LOCAL &&
        decl->dest.kind == IR_OPERAND_SYMBOL && decl->dest.name &&
        strcmp(decl->dest.name, acc_symbol) == 0) {
      mettle_free_string(decl->text);
      decl->text = mettle_strdup("int64");
      rewrote_decl = decl->text != NULL;
      break;
    }
  }
  return rewrote_decl && (rewrote_cast || !cast_required);
}

/* Mutator for IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC ("declare the accumulator as
 * int64"). */
static int ir_simd_mutate_byte_sum_int64(IRFunction *clone, size_t begin,
                                         size_t end) {
  return ir_simd_widen_accumulator(clone, begin, end, 1);
}

/* Mutator for IR_SIMD_BAIL_I32_SUM_NARROW_ACC: the int32 sum kernel admits
 * the cast-free widening form whenever the accumulator is declared int64, so
 * only the declaration needs retyping. */
static int ir_simd_mutate_i32_sum_int64(IRFunction *clone, size_t begin,
                                        size_t end) {
  return ir_simd_widen_accumulator(clone, begin, end, 0);
}

/* Retype the loop's integer memory accesses from `from_size` bytes to int32
 * (loads/stores to size 4, the matching address scale shift to <<2), the IR
 * image of "use int32 elements". When the loop accumulates, the accumulator
 * is also widened to int64 (the int32 sum kernel's requirement, and what the
 * paired fix text tells the user). */
static int ir_simd_retype_int_elems_to_i32(IRFunction *clone, size_t begin,
                                           size_t end, long long from_size,
                                           long long from_shift) {
  int rewrote = 0;
  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *ins = &clone->instructions[i];
    if ((ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) && !ins->is_float &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == from_size) {
      ins->rhs.int_value = 4;
      rewrote = 1;
      continue;
    }
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "<<") == 0 && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == from_shift &&
        ins->lhs.kind == IR_OPERAND_SYMBOL) {
      ins->rhs.int_value = 2;
    }
  }
  if (!rewrote) {
    return 0;
  }
  /* Best-effort: maps have no accumulator, sums need theirs widened. */
  ir_simd_widen_accumulator(clone, begin, end, 0);
  return 1;
}

/* Mutator for IR_SIMD_BAIL_INT16_ELEMENTS ("use int32 elements ..."). */
static int ir_simd_mutate_int16_to_i32(IRFunction *clone, size_t begin,
                                       size_t end) {
  return ir_simd_retype_int_elems_to_i32(clone, begin, end, 2, 1);
}

/* Mutator for IR_SIMD_BAIL_INT64_ELEMENTS ("use int32 arrays ..."). */
static int ir_simd_mutate_int64_to_i32(IRFunction *clone, size_t begin,
                                       size_t end) {
  return ir_simd_retype_int_elems_to_i32(clone, begin, end, 8, 3);
}

/* Which float width dominates the region's memory traffic, or 0 when the
 * region is not mixed. The minority is what a reader would retype, and it is
 * what the mutator below converges, so the kernel the simulation names is the
 * kernel that edit produces. */
static long long ir_simd_majority_float_width(const IRFunction *function,
                                              size_t begin, size_t end) {
  int f32 = 0, f64 = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if ((ins->op != IR_OP_LOAD && ins->op != IR_OP_STORE) || !ins->is_float ||
        ins->rhs.kind != IR_OPERAND_INT) {
      continue;
    }
    if (ins->rhs.int_value == 4) {
      f32++;
    } else if (ins->rhs.int_value == 8) {
      f64++;
    }
  }
  if (!f32 || !f64) {
    return 0;
  }
  return (f32 >= f64) ? 4 : 8;
}

/* Mutator for IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS ("keep the loop in one float
 * width"). Retypes the minority width onto the majority, scale shifts and
 * all, and lets the real vectorizer say whether one width was the only thing
 * in the way. */
static int ir_simd_mutate_single_float_width(IRFunction *clone, size_t begin,
                                             size_t end) {
  long long keep = ir_simd_majority_float_width(clone, begin, end);
  if (!keep) {
    return 0;
  }
  long long drop = (keep == 4) ? 8 : 4;
  long long keep_shift = (keep == 4) ? 2 : 3;
  long long drop_shift = (keep == 4) ? 3 : 2;

  /* Temps loaded at the dropped width. Retyping the load leaves the width
   * cast that consumed it behind, and the recognizers still see it; the
   * reader's edit deletes that cast along with the type. Only casts fed by a
   * load this pass retyped are touched, so an int-to-float conversion (real
   * work) is never mistaken for width bookkeeping. */
  char retyped[32][64];
  size_t retyped_count = 0;

  int rewrote = 0;
  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *ins = &clone->instructions[i];
    if ((ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) && ins->is_float &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == drop) {
      ins->rhs.int_value = keep;
      rewrote = 1;
      if (ins->op == IR_OP_LOAD && ins->dest.kind == IR_OPERAND_TEMP &&
          ins->dest.name && retyped_count < 32) {
        snprintf(retyped[retyped_count], sizeof(retyped[0]), "%s",
                 ins->dest.name);
        retyped_count++;
      }
      continue;
    }
    /* The index scale that went with the dropped width. */
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "<<") == 0 && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == drop_shift &&
        ins->lhs.kind == IR_OPERAND_SYMBOL) {
      ins->rhs.int_value = keep_shift;
      continue;
    }
    if (ins->op == IR_OP_CAST && ins->is_float &&
        ins->lhs.kind == IR_OPERAND_TEMP && ins->lhs.name) {
      for (size_t r = 0; r < retyped_count; r++) {
        if (strcmp(retyped[r], ins->lhs.name) == 0) {
          ins->op = IR_OP_ASSIGN; /* both sides are `keep` wide now */
          break;
        }
      }
    }
  }
  return rewrote;
}

/* True when some instruction in (begin, end) writes the symbol `sym`. */
static int ir_simd_symbol_written_in_region(const IRFunction *function,
                                            size_t begin, size_t end,
                                            const char *sym) {
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, sym) == 0) {
      return 1;
    }
  }
  return 0;
}

/* True when the value of `sym`/`temp` is invariant across the loop region:
 * not the iv, and (transitively) reading no symbol the body writes.
 * Conservative: an unresolvable producer or a chain deeper than the budget
 * counts as variant, so a "yes" is trustworthy. */
static int ir_simd_symbol_is_region_invariant(const IRFunction *function,
                                              size_t begin, size_t end,
                                              const char *sym,
                                              const char *iv) {
  if (!sym || strcmp(sym, iv) == 0) {
    return 0;
  }
  return !ir_simd_symbol_written_in_region(function, begin, end, sym);
}

static int ir_simd_temp_is_region_invariant(const IRFunction *function,
                                            size_t begin, size_t end,
                                            size_t before, const char *temp,
                                            const char *iv, int depth) {
  if (depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, temp);
  if (!producer) {
    return 0;
  }
  const IROperand *sides[2] = {&producer->lhs, &producer->rhs};
  for (int s = 0; s < 2; s++) {
    if (sides[s]->kind == IR_OPERAND_SYMBOL && sides[s]->name &&
        !ir_simd_symbol_is_region_invariant(function, begin, end,
                                            sides[s]->name, iv)) {
      return 0;
    }
    if (sides[s]->kind == IR_OPERAND_TEMP &&
        (!sides[s]->name ||
         !ir_simd_temp_is_region_invariant(function, begin, end, before,
                                           sides[s]->name, iv, depth + 1))) {
      return 0;
    }
  }
  return 1;
}

/* Mutator for IR_SIMD_BAIL_DOT_SHAPE_ADDRESS ("hoist invariant index math
 * into a pointer before the loop"): for each access whose index is
 * `invariant + iv`, retarget it to a fresh row-pointer symbol indexed by the
 * iv alone -- exactly the IR a hoisted `var row: T* = &m[inv];` + `row[iv]`
 * produces inside the loop. All in place: the scale shift's operand becomes
 * the iv, the address add's base becomes `__hypo_rowN`, and the dead
 * index-add slot becomes that symbol's DECLARE_LOCAL (the recognizers consult
 * only the declared type; the clone never executes). The invariance of the
 * hoisted half IS checked (conservatively) -- otherwise the simulation could
 * prove a fix the user cannot actually write. */
/* The loop's induction variable: lhs of the header's `iv < bound` compare. */
static const char *ir_simd_region_induction_variable(const IRFunction *clone,
                                                     size_t begin,
                                                     size_t end) {
  int past_header = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &clone->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (past_header && ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "<") == 0 && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ins->lhs.name) {
      return ins->lhs.name;
    }
  }
  return NULL;
}

static IRInstruction *ir_simd_index_producer(IRFunction *clone, size_t begin,
                                             size_t at, const char *want) {
  IRInstruction *idx = NULL;
  for (;;) {
    idx = NULL;
    for (size_t j = at; j-- > begin;) {
      IRInstruction *cand = &clone->instructions[j];
      if (ir_instruction_writes_destination(cand) &&
          cand->dest.kind == IR_OPERAND_TEMP && cand->dest.name &&
          strcmp(cand->dest.name, want) == 0) {
        idx = cand;
        break;
      }
    }
    if (!idx || idx->op != IR_OP_CAST || idx->is_float ||
        idx->lhs.kind != IR_OPERAND_TEMP || !idx->lhs.name) {
      return idx;
    }
    want = idx->lhs.name;
  }
}

static const IROperand *ir_simd_row_invariant_half(IRFunction *clone,
                                                   size_t begin, size_t end,
                                                   size_t at,
                                                   const IRInstruction *idx,
                                                   const char *iv,
                                                   int *variant_index_seen) {
  const IROperand *other = NULL;
  if (idx->lhs.kind == IR_OPERAND_SYMBOL && idx->lhs.name &&
      strcmp(idx->lhs.name, iv) == 0) {
    other = &idx->rhs;
  } else if (idx->rhs.kind == IR_OPERAND_SYMBOL && idx->rhs.name &&
             strcmp(idx->rhs.name, iv) == 0) {
    other = &idx->lhs;
  } else {
    return NULL;
  }
  if (other->kind == IR_OPERAND_SYMBOL) {
    if (!ir_simd_symbol_is_region_invariant(clone, begin, end, other->name,
                                            iv)) {
      *variant_index_seen = 1;
      return NULL;
    }
  } else if (other->kind == IR_OPERAND_TEMP) {
    if (!other->name ||
        !ir_simd_temp_is_region_invariant(clone, begin, end, at, other->name,
                                          iv, 0)) {
      *variant_index_seen = 1;
      return NULL;
    }
  } else if (other->kind != IR_OPERAND_INT) {
    return NULL;
  }
  return other;
}

/* The address add consuming the shifted index: `addr = base + %shifted`. */
static IRInstruction *ir_simd_address_add(IRFunction *clone, size_t at,
                                          size_t end, const char *shifted) {
  for (size_t k = at + 1; k < end; k++) {
    IRInstruction *cand = &clone->instructions[k];
    if (cand->op == IR_OP_BINARY && !cand->is_float && cand->text &&
        strcmp(cand->text, "+") == 0 && cand->lhs.kind == IR_OPERAND_SYMBOL &&
        cand->lhs.name && cand->rhs.kind == IR_OPERAND_TEMP && cand->rhs.name &&
        strcmp(cand->rhs.name, shifted) == 0) {
      return cand;
    }
  }
  return NULL;
}

static int ir_simd_is_index_scale(const IRInstruction *shl) {
  return shl->op == IR_OP_BINARY && !shl->is_float && shl->text &&
         strcmp(shl->text, "<<") == 0 && shl->rhs.kind == IR_OPERAND_INT &&
         (shl->rhs.int_value == 2 || shl->rhs.int_value == 3) &&
         shl->lhs.kind == IR_OPERAND_TEMP && shl->lhs.name &&
         shl->dest.kind == IR_OPERAND_TEMP && shl->dest.name;
}

static int ir_simd_mutate_dot_row_pointer(IRFunction *clone, size_t begin,
                                          size_t end) {
  const char *iv = ir_simd_region_induction_variable(clone, begin, end);
  int rewrites = 0;
  int variant_index_seen = 0;

  if (!iv) {
    return 0;
  }

  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *shl = &clone->instructions[i];
    IRInstruction *idx;
    IRInstruction *addr;
    const char *elem_type;
    char row_name[32];

    if (!ir_simd_is_index_scale(shl)) {
      continue;
    }
    idx = ir_simd_index_producer(clone, begin, i, shl->lhs.name);
    if (!idx || idx->op != IR_OP_BINARY || idx->is_float || !idx->text ||
        strcmp(idx->text, "+") != 0) {
      continue;
    }
    if (!ir_simd_row_invariant_half(clone, begin, end, i, idx, iv,
                                    &variant_index_seen)) {
      continue;
    }
    addr = ir_simd_address_add(clone, i, end, shl->dest.name);
    if (!addr) {
      continue;
    }

    snprintf(row_name, sizeof(row_name), "__hypo_row%d", rewrites);
    elem_type = (shl->rhs.int_value == 3) ? "float64*" : "float32*";

    ir_operand_destroy(&shl->lhs);
    shl->lhs = ir_operand_symbol(iv);
    ir_operand_destroy(&addr->lhs);
    addr->lhs = ir_operand_symbol(row_name);
    /* The index add is now dead; its slot becomes the row pointer's
     * DECLARE_LOCAL so ir_symbol_is_float_array_base accepts the base. */
    ir_instruction_destroy_storage(idx);
    memset(idx, 0, sizeof(*idx));
    idx->op = IR_OP_DECLARE_LOCAL;
    idx->dest = ir_operand_symbol(row_name);
    idx->text = mettle_strdup(elem_type);
    rewrites++;
  }
  if (rewrites > 0) {
    return 1;
  }
  /* The dot shape was there but its index half changes every iteration:
   * the suggested hoist is positively unwritable, not merely unmatched. */
  return variant_index_seen ? IR_SIMD_FIX_INAPPLICABLE : 0;
}

/* "bind the array to a pointer once before the loop" (store-only-fill on a
 * stack array). The fill kernel indexes off an invariant base SYMBOL; a stack
 * array's address is retaken inside the body, so the store's base is a fresh
 * temp every iteration and no kernel claims it.
 *
 * The rewrite is the source change: the in-body `%t <- &@a` becomes a pointer
 * local declared before the loop, and the address add reads that symbol. Like
 * the row-pointer mutator, this reshapes the clone rather than executing it;
 * the claim being tested is whether the recognizer accepts the shape the user
 * would produce, and the clone is discarded either way.
 *
 * Returns 0 for the other store-only-fill causes (several stores, byte
 * elements), whose loops have no in-body address-of to hoist, so they never
 * pick up a proof they have not earned. */
static int ir_simd_mutate_fill_base_pointer(IRFunction *clone, size_t begin,
                                            size_t end) {
  int rewrites = 0;
  int past_header = 0;

  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *addr_of = &clone->instructions[i];
    if (addr_of->op == IR_OP_LABEL) {
      if (addr_of->text && (strstr(addr_of->text, "ir_while_") != NULL ||
                            strstr(addr_of->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (!past_header || addr_of->op != IR_OP_ADDRESS_OF ||
        addr_of->lhs.kind != IR_OPERAND_SYMBOL || !addr_of->lhs.name ||
        addr_of->dest.kind != IR_OPERAND_TEMP || !addr_of->dest.name) {
      continue;
    }
    const char *declared =
        ir_function_local_declared_type(clone, addr_of->lhs.name);
    if (!declared || !strchr(declared, '[')) {
      continue; /* not a stack array: nothing to bind */
    }

    /* The address add that consumes it: `%addr = %base + %scaled`. */
    IRInstruction *addr = NULL;
    for (size_t k = i + 1; k < end; k++) {
      IRInstruction *cand = &clone->instructions[k];
      if (cand->op == IR_OP_BINARY && !cand->is_float && cand->text &&
          strcmp(cand->text, "+") == 0 &&
          cand->lhs.kind == IR_OPERAND_TEMP && cand->lhs.name &&
          strcmp(cand->lhs.name, addr_of->dest.name) == 0) {
        addr = cand;
        break;
      }
    }
    if (!addr) {
      continue;
    }

    /* Point the address add at the array symbol itself and drop the
     * per-iteration address-of. That is what binding a pointer once achieves
     * from the kernel's side: one invariant base for the whole loop instead
     * of a fresh temp each time round. */
    ir_operand_destroy(&addr->lhs);
    addr->lhs = ir_operand_symbol(addr_of->lhs.name);
    ir_instruction_destroy_storage(addr_of);
    memset(addr_of, 0, sizeof(*addr_of));
    addr_of->op = IR_OP_NOP;
    rewrites++;
  }
  return rewrites > 0;
}

/* Hoist the body's local declaration above the loop header, which is what
 * "declare it before the loop" does to the instruction stream. A DECLARE_LOCAL
 * names storage and carries no value (the initializer lowers to a separate
 * store), so moving it changes nothing the loop computes. */
static int ir_simd_mutate_hoist_body_local(IRFunction *clone, size_t begin,
                                           size_t end) {
  size_t header = 0, decl = 0;
  for (size_t i = begin + 1; i < end && i < clone->instruction_count; i++) {
    IRInstruction *ins = &clone->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (!header && ir_label_is_loop_header(ins->text)) {
        header = i;
      }
      continue;
    }
    if (header && ins->op == IR_OP_DECLARE_LOCAL &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name) {
      decl = i;
      break;
    }
  }
  if (!header || !decl) {
    return 0;
  }
  IRInstruction saved = clone->instructions[decl];
  memmove(&clone->instructions[header + 1], &clone->instructions[header],
          (decl - header) * sizeof(IRInstruction));
  clone->instructions[header] = saved;
  return 1;
}

/* The transform table: which diagnoses have a paired fix simulation. Growing
 * the hypothesis engine = adding a mutator and one row here.
 * `inapplicable_fix` (optional) replaces the advice when the mutator returns
 * IR_SIMD_FIX_INAPPLICABLE -- proven-useless advice must not be printed. */
static const struct {
  IRSimdBailId diagnosis;
  IRSimdFixMutator mutate;
  const char *inapplicable_fix;
} g_simd_fix_transforms[] = {
    {IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC, ir_simd_mutate_byte_sum_int64, NULL},
    {IR_SIMD_BAIL_I32_SUM_NARROW_ACC, ir_simd_mutate_i32_sum_int64, NULL},
    {IR_SIMD_BAIL_INT16_ELEMENTS, ir_simd_mutate_int16_to_i32, NULL},
    {IR_SIMD_BAIL_INT64_ELEMENTS, ir_simd_mutate_int64_to_i32, NULL},
    {IR_SIMD_BAIL_STORE_ONLY_FILL, ir_simd_mutate_fill_base_pointer, NULL},
    {IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS, ir_simd_mutate_single_float_width, NULL},
    {IR_SIMD_BAIL_BODY_LOCAL, ir_simd_mutate_hoist_body_local, NULL},
    {IR_SIMD_BAIL_DOT_SHAPE_ADDRESS, ir_simd_mutate_dot_row_pointer,
     "none via hoisting. Re-checked: the index half that is not the loop "
     "counter changes every iteration, so it cannot be hoisted out; this "
     "access is genuinely non-unit-stride"},
};

/* The shared simulation driver: clone the function, apply the mutator, re-run
 * the real optimization stages (remark recording suppressed), re-locate the
 * loop by marker id (indexes shift), and check whether a kernel claimed it.
 * On success fills `desc` with the kernel description and returns 1; returns
 * IR_SIMD_FIX_INAPPLICABLE when the mutator proved the fix unwritable.
 *
 * When the fix applied but the loop stayed scalar, the run also holds the
 * answer to the question the reader asks next: what blocks it NOW. Diagnosing
 * the mutated clone yields that, and the caller reports the advice as a first
 * step instead of implying it finishes the job. IR_SIMD_FIX_PARTIAL says
 * `next_reason` was filled. */
#define IR_SIMD_FIX_PARTIAL 2
static int ir_explain_simulate_fix(const IRFunction *function, size_t begin,
                                   size_t end, IRSimdFixMutator mutate,
                                   int diagnosis, char *desc, size_t desc_cap,
                                   char *next_reason, size_t next_reason_cap) {
  int marker_id = ir_simd_marker_id_at(function, begin);
  if (marker_id < 0) {
    return 0;
  }

  IRFunction *clone = ir_explain_clone_function(function);
  if (!clone) {
    return 0;
  }
  int mutated = mutate(clone, begin, end);
  if (mutated <= 0) {
    ir_function_destroy(clone);
    return mutated;
  }

  ir_explain_set_hypothesis(1);
  int ran = ir_optimize_function_revectorize(clone);
  ir_explain_set_hypothesis(0);

  int verified = 0;
  size_t new_begin = 0, new_end = 0;
  if (ran && ir_simd_find_marker_region(clone, marker_id, &new_begin,
                                        &new_end)) {
    const IRInstruction *kernel =
        ir_region_vectorized_ins(clone, new_begin, new_end, 0);
    if (kernel) {
      ir_explain_kernel_desc(kernel, desc, desc_cap);
      verified = 1;
    } else if (next_reason && next_reason_cap) {
      int next_diagnosis = IR_SIMD_BAIL_NONE, next_advisory = 0;
      char scratch[320];
      ir_simd_explain_bail(clone, new_begin, new_end, next_reason,
                           next_reason_cap, scratch, sizeof(scratch),
                           &next_diagnosis, &next_advisory);
      /* Only a NEW obstacle is worth reporting. The same diagnosis coming back
       * means the mutator did not move the loop at all, and repeating it as
       * though it were the next step would send the reader in a circle. */
      if (next_diagnosis != IR_SIMD_BAIL_NONE && next_diagnosis != diagnosis) {
        verified = IR_SIMD_FIX_PARTIAL;
      } else {
        next_reason[0] = '\0';
      }
    }
  }
  ir_function_destroy(clone);
  return verified;
}

/* Run the fix simulation paired with `diagnosis`, if any. Returns 1 and fills
 * `desc` when the simulated fix was accepted by the optimizer; returns
 * IR_SIMD_FIX_INAPPLICABLE (and sets *inapplicable_fix_out to the replacement
 * advice) when the mutator proved the suggested fix unwritable; returns
 * IR_SIMD_FIX_PARTIAL (filling `next_reason`/`next_fix`) when the fix applied
 * cleanly and the loop still did not vectorize. */
static int ir_explain_try_fix_for_diagnosis(const IRFunction *function,
                                            size_t begin, size_t end,
                                            int diagnosis, char *desc,
                                            size_t desc_cap,
                                            const char **inapplicable_fix_out,
                                            char *next_reason,
                                            size_t next_reason_cap) {
  size_t transform_count =
      sizeof(g_simd_fix_transforms) / sizeof(g_simd_fix_transforms[0]);
  for (size_t t = 0; t < transform_count; t++) {
    if ((int)g_simd_fix_transforms[t].diagnosis == diagnosis) {
      int result = ir_explain_simulate_fix(
          function, begin, end, g_simd_fix_transforms[t].mutate, diagnosis,
          desc, desc_cap, next_reason, next_reason_cap);
      if (result == IR_SIMD_FIX_INAPPLICABLE && inapplicable_fix_out) {
        *inapplicable_fix_out = g_simd_fix_transforms[t].inapplicable_fix;
      }
      return result;
    }
  }
  return 0;
}

/* The CALL_IN_BODY fix simulation. It has no table row because it is
 * program-level: pretend the loop's callee is @inline, re-run the INLINER on
 * a caller clone, then revectorize and check the region. Honesty guards: the
 * pretend flag cannot bypass the inliner's structural rejections, remark
 * recording is suppressed for the whole nested run, and the claim requires
 * the loop itself to have collapsed into a kernel (no surviving loop header
 * label) -- a vectorized loop INLINED FROM THE CALLEE inside a still-scalar
 * outer loop must not be passed off as "this loop vectorizes".
 *
 * Tri-state result:
 *   1  verified: the @inline (or @noinline-removal) advice WORKS; `desc` has
 *      the kernel the loop becomes.
 *   -1 proven inapplicable: the simulation ran and showed the advice cannot
 *      help. *decline_reason_out is the inliner's own structural refusal
 *      when that was the cause (e.g. the callee contains loops), or NULL
 *      when the callee DID inline but the loop still stayed scalar; in the
 *      latter case *nest_after_out says whether a loop nest remains (the
 *      callee's loops landed in the body -- only innermost loops vectorize).
 *   0  couldn't tell (no program/marker/callee, clone failure, or the forced
 *      inliner run made no change for reasons the simulation cannot see);
 *      the caller keeps its generic advice.
 * Fills `callee_out` (the advice target) and *was_noinline_out (1 = the
 * simulated change was REMOVING `@noinline`, not adding @inline). */
static int ir_explain_simulate_inline_fix(const IRFunction *function,
                                          size_t begin, size_t end,
                                          char *callee_out, size_t callee_cap,
                                          char *desc, size_t desc_cap,
                                          int *was_noinline_out,
                                          const char **decline_reason_out,
                                          int *nest_after_out) {
  *decline_reason_out = NULL;
  *nest_after_out = 0;
  if (!g_explain_program) {
    return 0;
  }
  int marker_id = ir_simd_marker_id_at(function, begin);
  if (marker_id < 0) {
    return 0;
  }

  /* The diagnosed callee: first user call past the loop header (the same walk
   * the diagnosis made). */
  const char *callee = NULL;
  int past_header = 0;
  for (size_t i = begin + 1; i < end && !callee; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (past_header && ins->op == IR_OP_CALL && ins->text &&
        strstr(ins->text, "crash_trap") == NULL) {
      callee = ins->text;
    }
  }
  if (!callee) {
    return 0;
  }
  snprintf(callee_out, callee_cap, "%s", callee);

  IRFunction *clone = ir_explain_clone_function(function);
  if (!clone) {
    return 0;
  }

  ir_explain_set_hypothesis(1);
  int verified = 0;
  if (ir_inline_explain_simulate_force_inline(g_explain_program, clone, callee,
                                              was_noinline_out,
                                              decline_reason_out)) {
    if (ir_optimize_function_revectorize(clone)) {
      size_t new_begin = 0, new_end = 0;
      if (ir_simd_find_marker_region(clone, marker_id, &new_begin, &new_end)) {
        /* Loop HEADERS, not any label carrying a loop's name. `while` emits
         * `ir_while_N` and `ir_while_end_N`, and a substring test counts the
         * exit label as a surviving loop. That read every collapsed loop as
         * still looping, so a verified fix was reported as disproven and the
         * reader was told inlining is not the blocker when removing
         * `@noinline` is the whole fix. */
        size_t before = ir_region_loop_header_count(function, begin, end);
        size_t after = ir_region_loop_header_count(clone, new_begin, new_end);
        const IRInstruction *kernel =
            ir_region_vectorized_ins(clone, new_begin, new_end, 1);
        if (after == 0 && kernel) {
          ir_explain_kernel_desc(kernel, desc, desc_cap);
          verified = 1;
        } else {
          /* The callee inlined cleanly, yet the loop is still scalar: the
           * @inline advice is disproven. Only a RISE in headers means the
           * callee brought loops in; the loop's own header surviving just
           * means it stayed scalar. */
          *nest_after_out = (after > before);
          verified = -1;
        }
      }
    }
  } else if (*decline_reason_out) {
    /* The inliner refused the callee even with the pretend flags set --
     * structural, so no decorator the user adds can change the outcome. */
    verified = -1;
  }
  ir_explain_set_hypothesis(0);
  ir_function_destroy(clone);
  return verified;
}

static void ir_clear_simd_markers(IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (ir_instruction_is_simd_marker(instruction)) {
      mettle_free_string(instruction->text);
      instruction->text = NULL; /* op stays IR_OP_NOP -- inert everywhere */
    }
  }
}

#define IR_SIMD_MAX_NESTING 64
#define IR_SIMD_MAX_LOOPS 256

/* One marker-bracketed loop region, collected during the contract walk so the
 * --explain pass can reason about nests (which loop contains which). */
typedef struct {
  size_t begin;
  size_t end;
  int mode;
  SourceLocation location;
} IRSimdLoopRecord;

/* The last source line the loop covers, for an editor that wants to highlight
 * the construct rather than its header line. Inlined bodies carry their own
 * file's lines, so only instructions from the loop's own file count. */
static size_t ir_simd_loop_end_line(const IRFunction *function,
                                    const IRSimdLoopRecord *loop) {
  size_t last = loop->location.line;
  if (!function) {
    return last;
  }
  for (size_t i = loop->begin;
       i <= loop->end && i < function->instruction_count; i++) {
    const SourceLocation *at = &function->instructions[i].location;
    if (at->line <= last) {
      continue;
    }
    if (at->filename && loop->location.filename &&
        strcmp(at->filename, loop->location.filename) != 0) {
      continue;
    }
    last = at->line;
  }
  return last;
}

/* --explain: one remark per recorded loop, nest-aware:
 *   - vectorized at its own level        -> "vectorized -> <kernel>"
 *   - scalar but a nested loop vectorized -> "vectorized inner, scalar outer"
 *   - scalar with a scalar nested loop    -> NOT vectorized (points inward)
 *   - no loop left between the markers    -> fully unrolled / removed
 *   - scalar leaf                         -> NOT vectorized + reason + fix */
static void ir_explain_report_loops(const IRFunction *function,
                                    const IRSimdLoopRecord *loops,
                                    size_t loop_count) {
  for (size_t k = 0; k < loop_count; k++) {
    const IRSimdLoopRecord *L = &loops[k];
    /* Focus filter FIRST: remarks outside the focus file are dropped at
     * record time anyway, but the fix simulations below (clone + re-run the
     * optimizer, and for call-in-body the inliner too) are the expensive
     * part -- without this gate every imported module's loops were being
     * simulated and then discarded (5+ seconds of --explain on a real
     * application; ~100ms with it). */
    if (!ir_explain_location_enabled(&L->location)) {
      continue;
    }
    const IRInstruction *own =
        ir_region_vectorized_ins(function, L->begin, L->end, 0);
    if (!own) {
      own = ir_region_skipahead_ins(function, L->begin, L->end);
    }
    const IRInstruction *any =
        own ? own : ir_region_vectorized_ins(function, L->begin, L->end, 1);

    /* Nest depth (1 = top level): how many recorded loops contain this one.
     * Stamped on the remark for the JSON sidecar -- a deeply nested scalar
     * loop is a better optimization target than a top-level one. */
    size_t nest_depth = 1;
    for (size_t m = 0; m < loop_count; m++) {
      if (m != k && loops[m].begin < L->begin && loops[m].end > L->end) {
        nest_depth++;
      }
    }

    int has_inner = 0;
    size_t inner_line = 0;
    for (size_t m = 0; m < loop_count; m++) {
      if (m == k || loops[m].begin <= L->begin || loops[m].end >= L->end) {
        continue;
      }
      has_inner = 1;
      if (inner_line == 0) {
        inner_line = loops[m].location.line;
      }
      /* Point the message at a vectorized inner loop when one exists. */
      if (!own && any &&
          ir_region_vectorized_ins(function, loops[m].begin, loops[m].end, 1)) {
        inner_line = loops[m].location.line;
      }
    }
    /* A nest the records cannot see: the inlined callee's loops carry no
     * markers. Fall back to the labels, and name the call that brought the
     * loop in so the advice matches what the reader wrote. */
    size_t inlined_loop_from_line = 0;
    char inlined_loop_callee[128];
    inlined_loop_callee[0] = '\0';
    if (!has_inner) {
      size_t structural = ir_region_inner_loop_line(function, L->begin, L->end);
      if (structural) {
        has_inner = 1;
        inner_line = structural;
        size_t last = ir_simd_loop_end_line(function, L);
        if (ir_explain_inlined_calls_in_range(
                function->name, L->location.line, last ? last : L->location.line,
                &inlined_loop_from_line, inlined_loop_callee,
                sizeof(inlined_loop_callee)) != 1) {
          inlined_loop_from_line = 0;
          inlined_loop_callee[0] = '\0';
        }
      }
    }

    char headline[192], reason[320], fix[320];
    if (own) {
      char desc[128];
      ir_explain_kernel_desc(own, desc, sizeof(desc));
      snprintf(headline, sizeof(headline), "vectorized \xE2\x86\x92 %s", desc);
      ir_explain_remark(function->name, "loop", L->location, 1, headline, NULL,
                        NULL, NULL);
      ir_explain_remark_code("vectorized");
      ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
    } else if (any) {
      snprintf(reason, sizeof(reason),
               "only the innermost loop of a nest is vectorized; this loop "
               "drives the vectorized inner loop (line %zu)",
               inner_line);
      ir_explain_remark(function->name, "loop", L->location, 1,
                        "vectorized inner, scalar outer", reason, NULL, NULL);
      ir_explain_remark_code("vectorized-inner");
      ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
    } else if (has_inner) {
      if (inlined_loop_callee[0]) {
        /* The reader's source shows a call here, not a loop. Say where the
         * loop came from, or the advice reads as being about code they never
         * wrote. */
        snprintf(reason, sizeof(reason),
                 "the call to `%s` on line %zu was inlined, so that callee's "
                 "loop (line %zu) now sits in this body; only innermost loops "
                 "are vectorized",
                 inlined_loop_callee, inlined_loop_from_line, inner_line);
        snprintf(fix, sizeof(fix),
                 "nothing to change on this line: this loop drives the work, "
                 "and the vectorizable part is `%s`'s loop. See the remark "
                 "on line %zu",
                 inlined_loop_callee, inner_line);
      } else {
        snprintf(reason, sizeof(reason),
                 "the body contains a nested loop (line %zu), and only "
                 "innermost loops are vectorized; the inner loop did not "
                 "vectorize either. See its remark",
                 inner_line);
        snprintf(fix, sizeof(fix),
                 "nothing to change on this line: this loop drives the nest, "
                 "so the fix belongs on the inner loop at line %zu",
                 inner_line);
      }
      ir_explain_remark(function->name, "loop", L->location, 0,
                        "NOT vectorized", reason, fix, NULL);
      ir_explain_remark_code("outer-of-nest");
      ir_explain_remark_advisory();
      ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
    } else if (!ir_region_has_loop_label(function, L->begin, L->end)) {
      /* The unroller records a definitive "fully unrolled (N iterations)"
       * remark when it was the cause; only guess when nothing claimed it. */
      if (!ir_explain_has_remark_at(L->location.line, "loop")) {
        ir_explain_remark(function->name, "loop", L->location, 1,
                          "eliminated: no loop remains after "
                          "optimization (fully unrolled or folded away)",
                          NULL, NULL, NULL);
        ir_explain_remark_code("eliminated");
        ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
      }
    } else {
      int diagnosis = IR_SIMD_BAIL_NONE;
      int advisory = 0;
      ir_simd_explain_bail(function, L->begin, L->end, reason, sizeof(reason),
                           fix, sizeof(fix), &diagnosis, &advisory);
      /* When the diagnosis has a paired hypothesis transform, simulate the
       * suggested fix and let the vectorizer itself confirm it. A proven-
       * inapplicable fix is REPLACED, never printed -- bad advice with a
       * confident tone is the failure mode this whole report exists to end. */
      char verified[512], partial[512];
      verified[0] = partial[0] = '\0';
      char kernel_desc[128];
      if (diagnosis == IR_SIMD_BAIL_CALL_IN_BODY) {
        /* Program-level simulation: pretend-@inline the callee, re-run the
         * inliner on a clone, revectorize. */
        char callee[128];
        int was_noinline = 0;
        const char *decline_reason = NULL;
        int nest_after = 0;
        int sim = ir_explain_simulate_inline_fix(
            function, L->begin, L->end, callee, sizeof(callee), kernel_desc,
            sizeof(kernel_desc), &was_noinline, &decline_reason, &nest_after);
        if (sim == -1) {
          /* The simulation DISPROVED the @inline advice; printing it anyway
           * (and letting an editor offer it as a one-click fix) is exactly
           * the confident-bad-advice failure mode this report exists to end.
           * Replace reason and fix with what the simulation learned. */
          if (decline_reason) {
            snprintf(reason, sizeof(reason),
                     "each iteration calls `%s`, and `@inline` cannot help: "
                     "%s",
                     callee, decline_reason);
          } else if (nest_after) {
            snprintf(reason, sizeof(reason),
                     "each iteration calls `%s`; even with it inlined, its "
                     "loops would land in this body, making this the outer "
                     "loop of a nest, and only innermost loops vectorize",
                     callee);
          } else {
            snprintf(reason, sizeof(reason),
                     "each iteration calls `%s`; the compiler simulated "
                     "inlining it, and this loop still does not vectorize, "
                     "so inlining is not the blocker",
                     callee);
          }
          snprintf(fix, sizeof(fix),
                   "nothing to change on this line: this loop is a driver "
                   "and scalar is the right code for it. The vectorizable "
                   "work is inside `%s`, so check the remarks on its loops",
                   callee);
          advisory = 1;
        } else if (sim == 1) {
          if (was_noinline) {
            /* The right advice for a vetoed callee is removing the veto;
             * also correct the fix line, which suggested @inline. */
            snprintf(fix, sizeof(fix),
                     "remove `@noinline` from `%s` (it blocks this loop's "
                     "vectorization), or hoist the call out of the loop",
                     callee);
            snprintf(verified, sizeof(verified),
                     "simulated removing `@noinline` from `%s` and re-ran "
                     "the inliner and the optimizer: this loop then "
                     "vectorizes \xE2\x86\x92 %s",
                     callee, kernel_desc);
          } else {
            snprintf(verified, sizeof(verified),
                     "simulated marking `%s` @inline and re-ran the inliner "
                     "and the optimizer: this loop then vectorizes "
                     "\xE2\x86\x92 %s",
                     callee, kernel_desc);
          }
        }
      } else {
        const char *inapplicable_fix = NULL;
        char next_reason[320];
        next_reason[0] = '\0';
        int sim = ir_explain_try_fix_for_diagnosis(
            function, L->begin, L->end, diagnosis, kernel_desc,
            sizeof(kernel_desc), &inapplicable_fix, next_reason,
            sizeof(next_reason));
        if (sim == 1) {
          snprintf(verified, sizeof(verified),
                   "simulated that fix and re-ran the optimizer: this loop "
                   "then vectorizes \xE2\x86\x92 %s",
                   kernel_desc);
        } else if (sim == IR_SIMD_FIX_INAPPLICABLE && inapplicable_fix) {
          /* The simulation disproved the advice, so what is left describes
           * the loop rather than instructing anyone. */
          snprintf(fix, sizeof(fix), "%s", inapplicable_fix);
          advisory = 1;
        } else if (sim == IR_SIMD_FIX_PARTIAL && next_reason[0]) {
          /* The fix is right and it is not enough. Saying so, and naming what
           * surfaces next, beats letting the reader make the edit and find the
           * verdict unchanged. The next obstacle goes on its own line: spliced
           * into the fix it buries the one thing to do first. */
          size_t used = strlen(fix);
          snprintf(fix + used, sizeof(fix) - used, " (first step only)");
          snprintf(partial, sizeof(partial),
                   "re-checked with that change applied: the loop still does "
                   "not vectorize, because %s",
                   next_reason);
        }
      }
      ir_explain_remark(function->name, "loop", L->location, 0,
                        "NOT vectorized", reason, fix[0] ? fix : NULL,
                        verified[0] ? verified : NULL);
      if (partial[0]) {
        ir_explain_remark_partial(partial);
      }
      if (advisory) {
        ir_explain_remark_advisory();
      }
      ir_explain_remark_code(ir_simd_bail_id_name(diagnosis));
      ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
    }
    ir_explain_remark_loop_depth(L->location.line, nest_depth);
  }
}

int ir_verify_simd_contracts(IRFunction *function) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  struct {
    int mode;
    size_t begin_index;
    SourceLocation location;
  } open[IR_SIMD_MAX_NESTING];
  IRSimdLoopRecord loops[IR_SIMD_MAX_LOOPS];
  size_t loop_count = 0;
  int depth = 0;
  int had_fatal = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!ir_instruction_is_simd_marker(instruction)) {
      continue;
    }

    char which = 0;
    int id = 0, mode = 0;
    if (sscanf(instruction->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d",
               &which, &id, &mode) != 3) {
      continue;
    }

    if (which == 'B') {
      if (depth < IR_SIMD_MAX_NESTING) {
        open[depth].mode = mode;
        open[depth].begin_index = i;
        open[depth].location = instruction->location;
      }
      depth++;
      continue;
    }

    /* which == 'E' */
    if (depth <= 0) {
      continue; /* unbalanced; should not happen */
    }
    depth--;
    if (depth >= IR_SIMD_MAX_NESTING) {
      continue; /* its matching begin was past the nesting cap */
    }

    size_t begin_index = open[depth].begin_index;
    int loop_mode = open[depth].mode;
    SourceLocation loc = open[depth].location;
    const char *file = loc.filename ? loc.filename : "<input>";

    if (loop_count < IR_SIMD_MAX_LOOPS) {
      loops[loop_count].begin = begin_index;
      loops[loop_count].end = i;
      loops[loop_count].mode = loop_mode;
      loops[loop_count].location = loc;
      loop_count++;
    }

    if (loop_mode == SIMD_ATTR_REPORT) {
      continue; /* --explain bookkeeping only; no contract to enforce */
    }

    int vec_op = ir_region_vectorized_op(function, begin_index, i);
    if (vec_op < 0 && ir_region_skipahead_ins(function, begin_index, i)) {
      vec_op = (int)IR_OP_SIMD_FIND;
    }
    if (vec_op >= 0) {
      if (g_simd_report) {
        fprintf(stderr, "%s:%zu:%zu: note: @simd loop vectorized (%s)\n", file,
                loc.line, loc.column, ir_opcode_name((IROpcode)vec_op));
      }
      continue; /* contract honored */
    }

    const char *reason = ir_simd_bail_reason(function, begin_index, i);
    if (loop_mode == SIMD_ATTR_CONTRACT) {
      fprintf(stderr, "%s:%zu:%zu: error: @simd! loop was not vectorized: %s\n",
              file, loc.line, loc.column, reason);
      g_simd_contract_user_error = 1;
      had_fatal = 1;
    } else {
      fprintf(stderr,
              "%s:%zu:%zu: warning: @simd loop was not vectorized: %s\n", file,
              loc.line, loc.column, reason);
    }
  }

  if (ir_explain_enabled()) {
    ir_explain_report_loops(function, loops, loop_count);
  }

  ir_clear_simd_markers(function);
  return had_fatal ? 0 : 1;
}

void ir_note_simd_contracts_unverified(IRProgram *program) {
  if (!program) {
    return;
  }
  int marker_count = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (!ir_instruction_is_simd_marker(&function->instructions[i])) {
        continue;
      }
      char which = 0;
      int id = 0, mode = 0;
      if (sscanf(function->instructions[i].text +
                     strlen(IR_SIMD_MARKER_PREFIX),
                 "%c:%d:%d", &which, &id, &mode) == 3 &&
          which == 'B' && mode != SIMD_ATTR_REPORT) {
        /* Report-only markers come from --explain, not from a user `@simd`;
         * they don't represent an unverified contract. */
        marker_count++;
      }
    }
    ir_clear_simd_markers(function);
  }
  if (marker_count > 0) {
    fprintf(stderr,
            "note: %d `@simd` loop%s present but not verified; vectorization "
            "contracts are only checked with -O/--release\n",
            marker_count, marker_count == 1 ? "" : "s");
  }

  int contract_count = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (function && (function->is_inline_contract || function->is_noalloc)) {
      contract_count++;
    }
  }
  if (contract_count > 0) {
    fprintf(stderr,
            "note: %d `@inline!`/`@noalloc` contract%s present but not "
            "verified; contracts are only checked with -O/--release\n",
            contract_count, contract_count == 1 ? "" : "s");
  }
}
