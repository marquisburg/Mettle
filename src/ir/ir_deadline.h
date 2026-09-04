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

/* What one instruction of each kind costs. This comes from the target
 * description, so a machine a program described is costed with the model that
 * description carries and not with one the compiler kept to itself. */
typedef struct {
  long long op;
  long long load;
  long long store;
  long long branch;
  long long multiply;
  long long multiply_float;
  long long divide;
  long long divide_float;
  long long call;
  long long allocate;
  int described;
} IRDeadlineCosts;

typedef struct {
  size_t declared;
  size_t proven;
  size_t on_evidence;
  long long worst_slack;
  const char *worst_function;
} IRDeadlineStats;

/* `instrumented` says this build carries run-time checks the deadline was not
 * declared against: `--safe`, `--record-trace`, `--check-overflow`,
 * `--check-tasks`, `--check-effects`, `--check-proofs` or `--verify`. Their cost is real
 * and is counted, so the number the report prints is what that build costs;
 * what changes is the verdict. A deadline missed only because the checks are
 * on is said and not enforced, because a claim about the plain build is not
 * evidence about the checked one, either way. */
int ir_deadline_run(IRProgram *program, ErrorReporter *reporter,
                    const IRDeadlineCosts *costs, int instrument,
                    int instrumented, FILE *report, IRDeadlineStats *stats);

#endif /* IR_DEADLINE_H */
