#ifndef IR_VERIFY_H
#define IR_VERIFY_H

/* --verify: per-pass translation validation.
 *
 * When enabled, the pass driver snapshots a function's IR before every
 * optimization pass. If the pass reports a change, the BEFORE and AFTER IR
 * are executed by the reference interpreter (ir_interp.c) on identical
 * generated inputs and their observations compared: return value, final
 * buffer bytes, extern-call trace, touched globals. On divergence the
 * compiler prints the offending pass, function, and a concrete
 * counterexample, RESTORES the pre-pass IR, quarantines that (pass,
 * function) pair for the rest of the compile, and carries on - the emitted
 * binary is always built from validated IR.
 *
 * A wrong quarantine (a false positive from the input generator) costs only
 * optimization, never correctness.
 */

#include "ir.h"

void ir_verify_set_enabled(int enabled);
int ir_verify_enabled(void);

/* Called at the start/end of ir_optimize_program_pipeline. end prints the
 * validation summary to stderr and resets state. */
void ir_verify_begin_program(IRProgram *program);
void ir_verify_end_program(void);

/* True when a previous divergence quarantined `pass_name` for `function`;
 * the driver must skip the pass. */
int ir_verify_pass_quarantined(const IRFunction *function,
                               const char *pass_name);

typedef struct IRVerifySnapshot IRVerifySnapshot;

/* Deep-copy the function's instruction stream. Returns NULL when verification
 * is disabled/not begun (callers treat NULL as "skip validation"). */
IRVerifySnapshot *ir_verify_snapshot_take(IRFunction *function);
void ir_verify_snapshot_free(IRVerifySnapshot *snapshot);

/* METTLE_VERIFY_BREAK="pass_name[:function_name]": deliberately corrupt the
 * function's IR right after the named pass runs (once per compile), to prove
 * end-to-end that validation detects and quarantines a miscompiling pass.
 * Sets *changed when it fires. */
void ir_verify_maybe_sabotage(IRFunction *function, const char *pass_name,
                              int *changed);

/* Validate the pass application. Returns 1 when the pass is kept (validated,
 * or unverifiable and given the benefit of the doubt), 0 when a divergence
 * was found: the function's IR has been restored from the snapshot, the
 * (pass, function) pair quarantined, and *changed cleared. */
int ir_verify_check_pass(IRFunction *function, IRVerifySnapshot *snapshot,
                         const char *pass_name, int *changed);

/* Exit code contribution: number of divergences found this compile. */
int ir_verify_divergence_count(void);

/* How many generated input sets the fixed table holds. Callers that report a
 * passing verdict need this: the check is a differential test over these
 * sets, and a verdict that does not say so reads as a proof it is not. */
int ir_verify_input_run_count(void);

/* How many input sets the most recent ir_verify_check_rewrite actually ran:
 * the fixed table plus the boundary values harvested from the two functions,
 * which varies per function. */
int ir_verify_last_input_run_count(void);

/* ---- standalone rewrite gate ----
 *
 * The differential check is also usable OUTSIDE the --verify lifecycle, by
 * callers that gate their own rewrites (--ml-opt validates every model
 * disposition through this). No quarantine, no counters, no reporting: the
 * caller owns the policy. */

typedef enum {
  IR_VERIFY_REWRITE_VALIDATED,    /* equivalent on every usable input set */
  IR_VERIFY_REWRITE_DIVERGED,     /* counterexample found; why/cex filled */
  IR_VERIFY_REWRITE_UNVERIFIABLE  /* signature/construct/inputs not runnable */
} IRVerifyRewriteVerdict;

/* Snapshot capture/restore that do not require --verify to be active. */
IRVerifySnapshot *ir_verify_snapshot_capture(IRFunction *function);
int ir_verify_snapshot_restore(IRFunction *function,
                               const IRVerifySnapshot *snapshot);

/* Compare `function` (after) against `snapshot` (before) on generated inputs.
 * On DIVERGED, `why` holds the divergence description and `counterexample`
 * the formatted call. On UNVERIFIABLE, `skip_reason` says what blocked the
 * check. Any output pointer may be NULL. */
IRVerifyRewriteVerdict ir_verify_check_rewrite(
    IRProgram *program, IRFunction *function, const IRVerifySnapshot *snapshot,
    char *why, size_t why_capacity, char *counterexample, size_t cex_capacity,
    char *skip_reason, size_t skip_capacity);

/* The same check restricted to inputs on which `guard` (a function over the
 * same parameters) returns nonzero. `guard_hits` receives how many of the
 * generated input sets the guard admitted; zero admitted sets is reported as
 * UNVERIFIABLE with a skip reason. */
IRVerifyRewriteVerdict ir_verify_check_rewrite_guarded(
    IRProgram *program, IRFunction *function, const IRVerifySnapshot *snapshot,
    IRFunction *guard, int *guard_hits, char *why, size_t why_capacity,
    char *counterexample, size_t cex_capacity, char *skip_reason,
    size_t skip_capacity);

/* As above, with extra integer probe values appended to the generated input
 * sets: every integer parameter takes each probe in turn, then the probes are
 * staggered across parameters, and float parameters cycle through a fixed
 * table of awkward values on those runs. For small functions whose whole
 * meaning is a few operations, where a boundary the fixed table never reaches
 * is the whole question. */
IRVerifyRewriteVerdict ir_verify_check_rewrite_probed(
    IRProgram *program, IRFunction *function, const IRVerifySnapshot *snapshot,
    IRFunction *guard, int *guard_hits, const long long *probes,
    int probe_count, char *why, size_t why_capacity, char *counterexample,
    size_t cex_capacity, char *skip_reason, size_t skip_capacity);

#endif /* IR_VERIFY_H */
