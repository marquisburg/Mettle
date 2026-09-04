#ifndef IR_DEADLINE_H
#define IR_DEADLINE_H

/* Deadlines as declared facts.
 *
 * `fn tick() where cycles < 5000` is a claim about the longest path through a
 * function, and this pass proves it or refuses the build. The cost of one
 * instruction comes from a model keyed on the target, the cost of a call is
 * the callee's own proven cost, and the cost of a loop is its body times a
 * trip count the compiler already bounds. A path it cannot bound is a
 * refusal, not a silence: under `--pgo` a measured trip count stands in, and
 * the verdict is reported as evidence rather than proof.
 *
 * `--check-deadlines` re-asks the question from the other side. Every basic
 * block adds its own model cost to a per-thread counter while the program
 * runs, and returning past the bound traps. It costs a program with no
 * deadline nothing, because nothing is emitted where nothing was claimed.
 */

#include "ir.h"
#include "../error/error_reporter.h"
#include <stdio.h>

typedef struct {
  size_t declared;
  size_t proven;
  size_t on_evidence;
  long long worst_slack;
  const char *worst_function;
} IRDeadlineStats;

int ir_deadline_run(IRProgram *program, ErrorReporter *reporter,
                    const char *target_name, int instrument, FILE *report,
                    IRDeadlineStats *stats);

#endif /* IR_DEADLINE_H */
