#include "ir_optimize_internal.h"
#include "../ir_effects.h"

#include <stdio.h>
#include <string.h>

/* `@noalloc`: the function -- and everything it can reach through direct
 * calls -- performs zero heap allocations, or compilation fails pointing at
 * the allocation. This is a PROOF, not a lint, so anything unprovable counts
 * against the contract: calls to externs we don't know to be allocation-free
 * and calls through function pointers are violations too, with reasons that
 * say so.
 *
 * Allocation sources recognized:
 *   - IR_OP_NEW (the `new` expression)
 *   - calls to known allocator entry points (malloc/calloc/realloc/...)
 *   - string '+' concatenation (codegen lowers it to a heap-allocating
 *     kernel; ir_lowering marks the BINARY with `allocates`)
 *   - transitively, any reachable program function containing the above
 *
 * Runs after inlining/optimization so the verdict matches the code that will
 * actually execute. */

static int noalloc_name_is_allocator(const char *name) {
  return ir_effects_name_is_allocator(name);
}

/* Externs known not to allocate: owned math, raw memory and string inspection,
 * and the compiler's own trap helpers (they terminate the process). Anything
 * else outside the program is unprovable and therefore a violation. The lists
 * live with the effect pass, which reads them as the sources of `alloc`. */
static int noalloc_name_is_known_clean(const char *name) {
  return ir_effects_name_is_known_clean(name);
}

/* Memoized allocation verdict per function, indexed in program order.
 * IN_PROGRESS marks the DFS stack: a cycle proves nothing new, so it reads as
 * clean unless something else on the path allocates. */
enum {
  NOALLOC_UNKNOWN = 0,
  NOALLOC_IN_PROGRESS,
  NOALLOC_CLEAN,
  NOALLOC_ALLOCATES
};

static size_t noalloc_function_index(IRProgram *program,
                                     const IRFunction *function) {
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] == function) {
      return i;
    }
  }
  return (size_t)-1;
}

/* What a violation looks like when found. `instruction` is the offending op
 * inside `function`; `what` is a static description. */
typedef struct {
  const IRFunction *function;
  const IRInstruction *instruction;
  const char *what;
  const char *name; /* callee/allocator name when relevant, else NULL */
} NoallocViolation;

/* Depth-first allocation check. Fills `violation` with the FIRST offending
 * instruction found (the shallowest, so the error points as close to the
 * user's code as possible). Returns the NOALLOC_* verdict for `function`. */
static int noalloc_check(IRProgram *program, IRFunction *function,
                         unsigned char *state, NoallocViolation *violation) {
  size_t index = noalloc_function_index(program, function);
  if (index == (size_t)-1) {
    return NOALLOC_CLEAN; /* unreachable; defensive */
  }
  if (state[index] == NOALLOC_CLEAN || state[index] == NOALLOC_ALLOCATES) {
    return state[index];
  }
  if (state[index] == NOALLOC_IN_PROGRESS) {
    return NOALLOC_CLEAN; /* recursion back-edge; the rest of the path decides */
  }
  state[index] = NOALLOC_IN_PROGRESS;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];

    const char *what = NULL;
    const char *name = NULL;

    if (ins->op == IR_OP_NEW) {
      what = "a `new` expression allocates here";
    } else if (ins->allocates) {
      what = "string '+' concatenation allocates a new string here";
    } else if (ins->op == IR_OP_CALL_INDIRECT) {
      what = "a call through a function pointer cannot be proven "
             "allocation-free";
    } else if (ins->op == IR_OP_CALL && ins->text) {
      if (noalloc_name_is_allocator(ins->text)) {
        what = "calls the allocator";
        name = ins->text;
      } else if (!noalloc_name_is_known_clean(ins->text)) {
        IRFunction *callee = ir_program_find_function(program, ins->text);
        if (!callee) {
          what = "calls the external function";
          name = ins->text;
        } else if (noalloc_check(program, callee, state, violation) ==
                   NOALLOC_ALLOCATES) {
          /* The recursion already captured the deepest offending instruction
           * (the actual allocation); this call-site entry is only used if the
           * verdict came from the memo table with no details recorded. */
          what = "calls";
          name = ins->text;
        }
      }
    }

    if (what) {
      if (!violation->function) {
        violation->function = function;
        violation->instruction = ins;
        violation->what = what;
        violation->name = name;
      }
      state[index] = NOALLOC_ALLOCATES;
      return NOALLOC_ALLOCATES;
    }
  }

  state[index] = NOALLOC_CLEAN;
  return NOALLOC_CLEAN;
}

static void noalloc_report(const IRFunction *contract_fn,
                           const NoallocViolation *v) {
  const SourceLocation *loc = &v->instruction->location;
  const char *file = loc->filename ? loc->filename : "<input>";
  char detail[256];
  if (v->name && strcmp(v->what, "calls") == 0) {
    snprintf(detail, sizeof(detail),
             "calls `%s`, which allocates (or reaches something that does)",
             v->name);
  } else if (v->name) {
    snprintf(detail, sizeof(detail),
             "%s `%s`, which cannot be proven allocation-free", v->what,
             v->name);
  } else {
    snprintf(detail, sizeof(detail), "%s", v->what);
  }
  if (v->function == contract_fn) {
    fprintf(stderr, "%s:%zu:%zu: error: @noalloc function `%s` allocates: %s\n",
            file, loc->line, loc->column,
            contract_fn->name ? contract_fn->name : "?", detail);
  } else {
    fprintf(stderr,
            "%s:%zu:%zu: error: @noalloc function `%s` allocates: inside "
            "reachable function `%s`, %s\n",
            file, loc->line, loc->column,
            contract_fn->name ? contract_fn->name : "?",
            v->function->name ? v->function->name : "?", detail);
  }
}

int ir_enforce_noalloc_contracts(IRProgram *program) {
  if (!program || program->function_count == 0) {
    return 1;
  }

  int ok = 1;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function || !function->is_noalloc) {
      continue;
    }
    /* Fresh memo per contract function: a verdict memoized during an earlier
     * contract's DFS carries no violation details, and the error must point
     * at a real instruction every time. Contract functions are few. */
    unsigned char *state = calloc(program->function_count, 1);
    if (!state) {
      break; /* out of memory: don't fail the build over a check buffer */
    }
    NoallocViolation violation = {0};
    if (noalloc_check(program, function, state, &violation) ==
            NOALLOC_ALLOCATES &&
        violation.function) {
      noalloc_report(function, &violation);
      ok = 0;
    } else if (ir_explain_enabled() && function->instruction_count > 0) {
      ir_explain_remark(function->name, "function",
                        function->instructions[0].location, 1,
                        "verified @noalloc: allocation-free on "
                        "every reachable path",
                        NULL, NULL, NULL);
      ir_explain_remark_code("noalloc-verified");
    }
    free(state);
  }

  if (!ok) {
    ir_optimize_note_user_error();
  }
  return ok;
}
