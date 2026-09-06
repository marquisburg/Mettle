#ifndef IR_OPTIMIZE_H
#define IR_OPTIMIZE_H

#include "ir.h"

/* A global integer `var` with a compile-time constant initializer. The
 * optimizer folds reads of such a global to its initializer value when no
 * function in the program writes it or takes its address (IR has no global
 * table of its own, so main.c collects these from the AST). */
typedef struct IRGlobalIntConst {
  const char *name;
  long long value;
} IRGlobalIntConst;

typedef struct {
  /* Reserved for future IR optimization controls. */
  int preserve_function_boundaries;
  /* --simd-report: emit a note describing what each `@simd` loop became. */
  int simd_report;
  /* --explain: report every optimization decision (loop vectorization, call
   * inlining) as a note, with a reason whenever the optimizer declined. */
  int explain;
  /* When set, --explain remarks are limited to source locations in this file
   * (the main input), so imported stdlib modules don't flood the report. */
  const char *explain_focus_file;
  /* Non-extern global integer vars with literal initializers (see above). */
  const IRGlobalIntConst *global_int_consts;
  size_t global_int_const_count;
  /* Set when this compile feeds an executable link with `main` as the single
   * entry point. Gates whole-program transforms whose soundness needs every
   * call site visible (e.g. allocation-site layout factorization, which
   * rewrites callee bodies to a new pool layout). */
  int whole_program;
  /* Restrict the pipeline to scalar/control-flow transforms that retain the
   * shared IR instruction set. This excludes x86 SIMD idiom formation. */
  int target_neutral_only;
  /* With target_neutral_only, optimize only kernel-reachable device functions
   * and validate the shared GPU call graph before transforming them. */
  int gpu_device_only;
} IROptimizeOptions;

// Runs optimization passes on the generated IR program.
// Currently implements:
// - Small-function inlining (including control flow, no calls in callee)
// - Copy/constant propagation for temporaries and stack locals
// - Fibonacci-style rotate_add fusion and loop-body rotate fusion
// - Small constant-bound counted-loop unrolling (<= 64 trips)
// - Integer constant/algebraic folding and strength reduction
// - CSE, dead temp elimination, branch/jump CFG cleanup
// Returns 1 on success, 0 on error.
/* Scalar analysis keeps safety marks and loop shapes until resolution. */
int ir_optimize_safety_analysis(IRProgram *program, int preserve_boundaries);

int ir_optimize_program(IRProgram *program,
                        const IROptimizeOptions *options);

// True when the most recent ir_optimize_program() failed because a `@simd!`
// vectorization contract was violated (a user error already reported to
// stderr), as opposed to an internal compiler error. Lets the driver skip the
// generic ICE report in that case.
int ir_optimize_had_user_error(void);

/* --explain (see ir_optimize_explain.c). The codegen MIR gate records, per
 * focus-file function, whether it got the register-allocating backend (and
 * its non-nop IR size, so the report can rank where baseline codegen costs);
 * the driver flushes that section after codegen, which also routes the whole
 * buffered report: small reports go to stderr, large ones to a
 * `<output-stem>.explain.txt` sidecar with a digest on stderr. No-ops unless
 * --explain is on. */
int ir_explain_enabled(void);
void ir_explain_backend_function(const char *function_name,
                                 const char *filename, int ok,
                                 const char *detail, size_t instructions);
void ir_explain_backend_flush(void);
/* The codegen cost model, published by the MIR annotator once every function is
 * encoded. The optimizer's half of the report says what it decided; this says
 * what the decision costs, which is the difference between "this loop did not
 * vectorize" and "and it runs 7.2 cycles an iteration, bottlenecked on p23".
 * Cycles are centicycles. No-ops unless --explain is on. */
void ir_explain_backend_loop(const char *function_name, const char *filename,
                             size_t head_line, size_t tail_line, int depth,
                             int cycles_per_iter, const char *bottleneck,
                             int has_kernel, int estimated);
void ir_explain_backend_cost(const char *function_name, const char *filename,
                             int spills, int regs_used, int total_rthru,
                             long hot_cost, int vec_ops, int estimated_spans);
/* Finish a report for a non-native backend. The target-neutral optimizer has
 * already flushed its decisions; this adds an honest backend boundary instead
 * of pretending the native MIR eligibility report applies to PTX/SPIR-V. */
void ir_explain_target_flush(const char *target_name);
/* The -o path the sidecar is derived from; set before compilation. */
void ir_explain_set_output_path(const char *path);
/* --explain-json: also write a machine-readable <output-stem>.explain.json. */
void ir_explain_set_json(int enabled);
/* --explain=SELECTOR: narrow the PROSE report to one slice of it. Accepts
 * `missed`, `fixable`, `proven`, `loops`, `calls`, a function name, or a
 * decision code. The JSON sidecar ignores it and stays whole-file. The string
 * must outlive the compile (argv does). */
void ir_explain_set_filter(const char *selector);
/* --annotate-asm: keep the optimization remark table alive past the
 * optimization-stage flush so the codegen annotator (which runs later) can read
 * it. The remarks then leak at process exit -- fine for a one-shot compile. */
void ir_explain_set_retain_remarks(int enabled);
/* Read-only access to the collected --explain remarks, for the codegen
 * annotator's per-function/per-line decision join. Returns 0 past the end. */
size_t ir_explain_remark_count(void);
int ir_explain_remark_at(size_t index, const char **function_name,
                         const char **entity, size_t *line, int *positive,
                         const char **headline, const char **reason,
                         const char **fix, const char **verified, size_t *depth);
/* Render the --ml-opt model-driven optimizations (from the TSV the native pass
 * wrote) to stderr, styled like the main report. Called after the ML pass. */
void ir_explain_ml_opt(const char *path);

// When optimization is NOT run (no -O/--release), `@simd` markers are never
// verified. This prints one note saying so (if any are present) and strips the
// markers from the program. Safe to call unconditionally on the debug path.
void ir_note_simd_contracts_unverified(IRProgram *program);

#endif // IR_OPTIMIZE_H
