#include "ir_optimize_internal.h"
#include "../ir_verify.h"
#include "../../common.h"

#include <time.h>

const char *g_ir_pass_names[IR_OPT_PASS_COUNT] = {
#define IR_OPT_PASS_NAME(id, name) [IR_OPT_PASS_##id] = name,
    IR_OPT_PASS_LIST(IR_OPT_PASS_NAME)
#undef IR_OPT_PASS_NAME
};

const char *ir_opt_pass_name(IROptPassId pass_id) {
  if (pass_id < 0 || pass_id >= IR_OPT_PASS_COUNT ||
      !g_ir_pass_names[pass_id]) {
    return "<unnamed_ir_pass>";
  }
  return g_ir_pass_names[pass_id];
}

/* METTLE_TIME_IR_PASSES=1: accumulate wall time per pass across the whole
 * compile and dump a sorted table at the end of optimization. The cheap way
 * to answer "which pass is eating the build" without a sampling profiler. */
static int ir_pass_time_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_TIME_IR_PASSES");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static int ir_pass_time_covers(const IRFunction *function) {
  const char *only = getenv("METTLE_TIME_ONE_FUNCTION");

  if (!only || !only[0]) {
    return 1;
  }
  return function && function->name && strcmp(function->name, only) == 0;
}

static double g_ir_pass_ms[IR_OPT_PASS_COUNT];
static unsigned long long g_ir_pass_runs[IR_OPT_PASS_COUNT];
/* Named-sequence passes (vectorizers etc.) keyed by name, small fixed table. */
#define IR_PASS_TIME_NAMED_MAX 96
static struct {
  const char *name;
  double ms;
  unsigned long long runs;
} g_ir_named_ms[IR_PASS_TIME_NAMED_MAX];
static size_t g_ir_named_count = 0;

static double ir_pass_now_ticks(void) {
  return mettle_now_ms();
}

static void ir_pass_time_add_named(const char *name, double ms) {
  for (size_t i = 0; i < g_ir_named_count; i++) {
    if (g_ir_named_ms[i].name == name ||
        strcmp(g_ir_named_ms[i].name, name) == 0) {
      g_ir_named_ms[i].ms += ms;
      g_ir_named_ms[i].runs++;
      return;
    }
  }
  if (g_ir_named_count < IR_PASS_TIME_NAMED_MAX) {
    g_ir_named_ms[g_ir_named_count].name = name;
    g_ir_named_ms[g_ir_named_count].ms = ms;
    g_ir_named_ms[g_ir_named_count].runs = 1;
    g_ir_named_count++;
  }
}

/* Timing hooks for program-level passes (the inliner, pure-call LICM) that
 * don't go through the per-function drivers. begin returns 0 when disabled. */
double ir_pass_time_begin(void) {
  return ir_pass_time_enabled() ? ir_pass_now_ticks() : 0.0;
}

void ir_pass_time_end(const char *name, double begin_ms) {
  if (!ir_pass_time_enabled() || begin_ms == 0.0) {
    return;
  }
  ir_pass_time_add_named(name, ir_pass_now_ticks() - begin_ms);
}

void ir_pass_time_report(void) {
  if (!ir_pass_time_enabled()) {
    return;
  }
  fprintf(stderr, "-- IR pass times (cumulative ms) --\n");
  for (int dumped = 0; dumped < 40; dumped++) {
    double best = 0.5; /* drop sub-tick noise */
    int best_fix = -1;
    size_t best_named = (size_t)-1;
    for (int i = 0; i < IR_OPT_PASS_COUNT; i++) {
      if (g_ir_pass_ms[i] > best) {
        best = g_ir_pass_ms[i];
        best_fix = i;
        best_named = (size_t)-1;
      }
    }
    for (size_t i = 0; i < g_ir_named_count; i++) {
      if (g_ir_named_ms[i].ms > best) {
        best = g_ir_named_ms[i].ms;
        best_named = i;
        best_fix = -1;
      }
    }
    if (best_fix >= 0) {
      fprintf(stderr, "  %-32s %12.0f  (%llu runs)\n",
              ir_opt_pass_name((IROptPassId)best_fix), g_ir_pass_ms[best_fix],
              g_ir_pass_runs[best_fix]);
      g_ir_pass_ms[best_fix] = 0.0;
    } else if (best_named != (size_t)-1) {
      fprintf(stderr, "  %-32s %12.0f  (%llu runs)\n",
              g_ir_named_ms[best_named].name, g_ir_named_ms[best_named].ms,
              g_ir_named_ms[best_named].runs);
      g_ir_named_ms[best_named].ms = 0.0;
    } else {
      break;
    }
  }
}

static int ir_skip_delimiter(char c) {
  return c == ',' || c == ' ' || c == '\t';
}

static int ir_skip_token_equals(const char *token, size_t token_len,
                                const char *value) {
  return value && strlen(value) == token_len &&
         strncmp(token, value, token_len) == 0;
}

static int ir_pass_trace_enabled(void) {
  /* Cached: this is consulted per pass EVENT (hundreds of thousands of times
   * on big programs) and getenv is not cheap on Windows. */
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_TRACE_IR_PASSES");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static void ir_trace_pass_event(const char *pass_name, const char *event,
                                const unsigned long long *version,
                                int changed) {
  if (!ir_pass_trace_enabled()) {
    return;
  }

  MettleCompilerContext *ctx = mettle_compiler_ctx();
  fprintf(stderr, "[ir-opt] function=%s",
          ctx->function_name ? ctx->function_name : "<anonymous>");
  if (ctx->fixpoint_iteration > 0) {
    fprintf(stderr, " iteration=%d", ctx->fixpoint_iteration);
  }
  if (version) {
    fprintf(stderr, " version=%llu", *version);
  }
  fprintf(stderr, " pass=%s event=%s", pass_name, event);
  if (changed >= 0) {
    fprintf(stderr, " changed=%d", changed);
  }
  fputc('\n', stderr);
  fflush(stderr); /* the trace exists to locate hangs; keep it ordered */
}

/* Diagnostic: METTLE_SKIP_PASS="sroa,16" disables the listed pass names or
 * numeric pass IDs so a miscompile can be bisected to a single pass. Names
 * cover both fixpoint passes and named-sequence passes (the pre-inline and
 * post-fixpoint stages: vectorizers, SLP, induction-pointer, ...). */
static int ir_skip_spec_matches(const char *id_text, const char *pass_name) {
  /* Snapshot once: consulted per pass run, and getenv per call was real
   * compile time on big programs. The env cannot change mid-process for a
   * diagnostic knob. */
  static const char *spec = NULL;
  static int fetched = 0;
  if (!fetched) {
    /* Own the string: POSIX lets a later getenv overwrite the buffer, and the
     * freestanding runtime's used to. */
    const char *raw = getenv("METTLE_SKIP_PASS");
    spec = raw ? mettle_strdup(raw) : NULL;
    fetched = 1;
  }
  if (!spec || !*spec) {
    return 0;
  }

  const char *p = spec;
  while (*p) {
    while (ir_skip_delimiter(*p)) {
      p++;
    }
    const char *token = p;
    while (*p && !ir_skip_delimiter(*p)) {
      p++;
    }
    size_t token_len = (size_t)(p - token);
    if (token_len == 0) {
      continue;
    }
    if (ir_skip_token_equals(token, token_len, id_text) ||
        ir_skip_token_equals(token, token_len, pass_name)) {
      return 1;
    }
  }
  return 0;
}

int ir_pass_name_is_skipped(const char *pass_name) {
  return ir_skip_spec_matches(NULL, pass_name);
}

int ir_pass_is_skipped(IROptPassId pass_id) {
  if (pass_id < 0 || pass_id >= IR_OPT_PASS_COUNT) {
    return 0;
  }

  char id_text[16];
  int id_len = snprintf(id_text, sizeof(id_text), "%d", (int)pass_id);
  if (id_len <= 0) {
    return 0;
  }

  return ir_skip_spec_matches(id_text, ir_opt_pass_name(pass_id));
}

/* METTLE_NO_SIMD: build a baseline (SSE2-only) binary by skipping every
 * vectorizer / SLP / SIMD named pass. Those are the only passes that emit
 * AVX/AVX2/FMA instructions, so a binary built with this set runs on any
 * x86-64 CPU (SSE2 is mandatory in the x86-64 baseline). Scalar float codegen
 * already uses legacy SSE2 encodings, so nothing else needs AVX. Use for
 * distributable builds that must run on older machines. */
static int ir_no_simd_enabled(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("METTLE_NO_SIMD");
    v = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
  }
  return v;
}

/* The passes METTLE_NO_SIMD turns off: the ones that emit a vector kernel, all
 * of which are named for what they are. It used to skip EVERY pass, which is
 * not what it says and not what the docs promise. Skipping the whole pipeline
 * also left the loop canonical form unestablished while the checker that
 * enforces it still ran, so `while (...) { var t: int64 = b; ... }` -- a local
 * declared inside a loop, which hoist_body_locals exists to lift out -- turned
 * -O and --release into an internal compiler error. */
static int ir_pass_is_vectorizer(const char *name) {
  return name && (strncmp(name, "simd_", 5) == 0 ||
                  strncmp(name, "auto_vectorize", 14) == 0 ||
                  strncmp(name, "outer_vectorize", 15) == 0);
}

/* NO_SLP: the pair vectorizer only.
 *
 * This lived inside the two passes as an early `return 0`, which is the
 * failure code, so asking to disable the pass reported an internal compiler
 * error instead. METTLE_NO_SIMD had the same bug before it. A pass function
 * has one return value carrying two meanings -- did it work, and did it do
 * anything -- and there is no third value for "I declined", so declining had
 * to borrow one of the two and borrowed the wrong one, twice.
 *
 * The driver already has a place to say "not this one" and already says it for
 * METTLE_NO_SIMD, a skip list and a quarantine list. A knob belongs there,
 * where declining is expressible, rather than inside a pass where it is not. */
static int ir_no_slp_enabled(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("NO_SLP");
    v = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
  }
  return v;
}

static int ir_pass_is_slp(const char *name) {
  return name && strncmp(name, "simd_slp_", 9) == 0;
}

/* A signature over the function's volatile accesses: how many there are, in
 * what order, and at what width. A pass that drops one, invents one, or swaps
 * two of them has broken the one guarantee `volatile` makes, and the program
 * would be silently wrong. Comparing the signature across a pass turns that
 * into a compiler error naming the pass. Only functions that hold a volatile
 * access pay for this. */
static unsigned long long ir_volatile_signature(const IRFunction *function) {
  unsigned long long signature = 1469598103934665603ULL;
  size_t i;
  if (!function) {
    return signature;
  }
  for (i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    unsigned long long token;
    if (!instruction->is_volatile) {
      continue;
    }
    token = (unsigned long long)instruction->op * 131ULL +
            (unsigned long long)instruction->alias_class * 17ULL +
            (instruction->rhs.kind == IR_OPERAND_INT
                 ? (unsigned long long)instruction->rhs.int_value
                 : 0ULL);
    signature = (signature ^ token) * 1099511628211ULL;
  }
  return signature;
}

static int ir_run_named_pass(IRFunction *function, const IROptNamedPass *pass,
                             const char *failure_message, int *changed_out) {
  int changed = 0;
  int audit_volatile = function && function->has_volatile_access;
  unsigned long long volatile_before =
      audit_volatile ? ir_volatile_signature(function) : 0;

  if (changed_out) {
    *changed_out = 0;
  }

  if (!pass || !pass->name || !pass->run) {
    return 0;
  }

  if (ir_no_simd_enabled() && ir_pass_is_vectorizer(pass->name)) {
    ir_trace_pass_event(pass->name, "skipped", NULL, -1);
    return 1;                 /* baseline build: no vectorization, no AVX */
  }

  if (ir_no_slp_enabled() && ir_pass_is_slp(pass->name)) {
    ir_trace_pass_event(pass->name, "skipped", NULL, -1);
    return 1;
  }

  if (ir_pass_name_is_skipped(pass->name)) {
    ir_trace_pass_event(pass->name, "skipped", NULL, -1);
    return 1;
  }
  if (ir_verify_pass_quarantined(function, pass->name)) {
    ir_trace_pass_event(pass->name, "quarantined", NULL, -1);
    return 1;
  }

  IRVerifySnapshot *verify_snapshot = ir_verify_snapshot_take(function);

  mettle_compiler_ctx_set_pass_name(pass->name);
  ir_explain_pass_begin(function);
  double t0 = ir_pass_time_begin();
  if (!pass->run(function, &changed)) {
    ir_trace_pass_event(pass->name, "failed", NULL, -1);
    mettle_compiler_ice(failure_message);
  }
  ir_pass_time_end(pass->name, t0);
  ir_explain_pass_end(function, pass->name, changed);

  if (audit_volatile && ir_volatile_signature(function) != volatile_before) {
    char message[256];
    snprintf(message, sizeof(message),
             "optimization pass '%s' changed the volatile accesses in '%s'; a "
             "volatile load or store may not be removed, duplicated or "
             "reordered against another",
             pass->name, function->name ? function->name : "<unnamed>");
    mettle_compiler_ice(message);
  }

  if (verify_snapshot) {
    ir_verify_maybe_sabotage(function, pass->name, &changed);
    ir_verify_check_pass(function, verify_snapshot, pass->name, &changed);
    ir_verify_snapshot_free(verify_snapshot);
  }

  ir_trace_pass_event(pass->name, changed ? "changed" : "clean", NULL,
                      changed);
  if (changed_out) {
    *changed_out = changed;
  }
  return 1;
}

int ir_run_named_pass_sequence(IRFunction *function,
                               const IROptNamedPass *passes,
                               size_t pass_count,
                               const char *failure_message) {
  for (size_t i = 0; i < pass_count; i++) {
    if (!ir_run_named_pass(function, &passes[i], failure_message, NULL)) {
      return 0;
    }
  }

  return 1;
}

/* Worklist driver for named-pass stages (see the header comment). Cleanliness
 * is tracked per unique pass function at the stage's IR version, exactly as
 * ir_run_fixpoint_pass tracks the fixpoint passes: a pass whose clean version
 * is still current is looking at the same instruction array it already
 * declined, so it is skipped without running its matcher. */
#define IR_NAMED_STAGE_MAX_PASSES 64

int ir_run_named_stage_fixpoint(IRFunction *function,
                                const IROptNamedPass *passes,
                                size_t pass_count, int max_iterations,
                                const char *stage_name,
                                const char *failure_message,
                                int require_convergence) {
  if (!function || !passes || pass_count == 0 || max_iterations <= 0 ||
      pass_count > IR_NAMED_STAGE_MAX_PASSES) {
    return 0;
  }

  /* Duplicate entries (same run pointer) share one cleanliness slot, so an
   * array that still lists a pass twice behaves as one pass offered twice. */
  size_t slot[IR_NAMED_STAGE_MAX_PASSES];
  unsigned long long clean_version[IR_NAMED_STAGE_MAX_PASSES];
  for (size_t i = 0; i < pass_count; i++) {
    slot[i] = i;
    clean_version[i] = 0;
    for (size_t j = 0; j < i; j++) {
      if (passes[j].run == passes[i].run) {
        slot[i] = slot[j];
        break;
      }
    }
  }

  unsigned long long version = 1;
  int converged = 0;

  for (int iteration = 0; iteration < max_iterations && !converged;
       iteration++) {
    int iteration_changed = 0;
    IROptFunctionFeatures features;

    mettle_compiler_ctx_set_fixpoint_iteration(iteration + 1);
    ir_collect_function_features(function, &features);
    unsigned feature_flags = ir_opt_feature_flags(&features);

    for (size_t i = 0; i < pass_count; i++) {
      const IROptNamedPass *pass = &passes[i];
      unsigned all = pass->gate.all;
      unsigned any = pass->gate.any;

      if ((feature_flags & all) != all ||
          (any != 0 && (feature_flags & any) == 0)) {
        ir_trace_pass_event(pass->name, "disabled", &version, -1);
        clean_version[slot[i]] = version;
        continue;
      }
      if (clean_version[slot[i]] == version) {
        ir_trace_pass_event(pass->name, "already_clean", &version, -1);
        continue;
      }

      int changed = 0;
      if (!ir_run_named_pass(function, pass, failure_message, &changed)) {
        return 0;
      }
      if (changed) {
        version++;
        iteration_changed = 1;
        /* A structural change can remove features (a claimed loop loses its
         * labels); refresh so later gates read the truth. A saturated mask
         * needs no refresh: re-scanning could only clear bits, and a stale
         * set bit costs one matcher call that declines, where a stale clear
         * bit would skip a pass that had work. Erring toward running is both
         * cheaper here and the safe direction. */
        if (feature_flags != IR_OPT_FEATURE_ALL) {
          ir_collect_function_features(function, &features);
          feature_flags = ir_opt_feature_flags(&features);
        }
      } else {
        clean_version[slot[i]] = version;
      }
    }

    if (!iteration_changed) {
      converged = 1;
    }
  }

  mettle_compiler_ctx_set_fixpoint_iteration(0);

  if (require_convergence && !converged) {
    /* The stage's output is a normal form the passes behind it rely on.
     * Still changing at the cap means the form does not hold: some pass is
     * oscillating or feeding another, and every recognizer downstream would
     * be matching against shapes it cannot trust. Stop loudly. */
    fprintf(stderr,
            "mettle: internal error: stage '%s' did not converge on function "
            "'%s' after %d iterations\n",
            stage_name ? stage_name : "<unnamed>",
            function->name ? function->name : "<anonymous>", max_iterations);
    mettle_compiler_ice("IR normal-form stage failed to converge");
  }

  return 1;
}

/* Fixpoint pass driver with redundant-run skipping.
 *
 * The IR has a monotonically increasing version that bumps whenever any pass
 * changes it. Each pass records the version at which it last reported no
 * change. If that version is still current, the instruction array is identical
 * to what the pass already inspected, so the pass cannot change anything.
 */
int ir_run_fixpoint_pass(IRFunction *function, IROptPassId pass_id,
                         IROptFunctionPass pass, int enabled,
                         unsigned long long *version,
                         unsigned long long *clean_version, int *changed) {
  if (!version || !clean_version || !changed || pass_id < 0 ||
      pass_id >= IR_OPT_PASS_COUNT) {
    return 0;
  }

  const char *pass_name = ir_opt_pass_name(pass_id);
  if (!enabled) {
    ir_trace_pass_event(pass_name, "disabled", version, -1);
    clean_version[pass_id] = *version;
    return 1;
  }

  if (ir_pass_is_skipped(pass_id)) {
    ir_trace_pass_event(pass_name, "skipped", version, -1);
    clean_version[pass_id] = *version;
    return 1;
  }

  if (clean_version[pass_id] == *version) {
    ir_trace_pass_event(pass_name, "already_clean", version, -1);
    return 1;
  }

  if (ir_verify_pass_quarantined(function, pass_name)) {
    ir_trace_pass_event(pass_name, "quarantined", version, -1);
    clean_version[pass_id] = *version;
    return 1;
  }

  IRVerifySnapshot *verify_snapshot = ir_verify_snapshot_take(function);

  int pass_changed = 0;
  mettle_compiler_ctx_set_pass_name(pass_name);
  ir_explain_pass_begin(function);
  double t0 = ir_pass_time_begin();
  if (!pass || !pass(function, &pass_changed)) {
    ir_verify_snapshot_free(verify_snapshot);
    ir_trace_pass_event(pass_name, "failed", version, -1);
    return 0;
  }
  if (ir_pass_time_enabled() && ir_pass_time_covers(function)) {
    g_ir_pass_ms[pass_id] += ir_pass_now_ticks() - t0;
    g_ir_pass_runs[pass_id]++;
  }
  ir_explain_pass_end(function, pass_name, pass_changed);

  if (verify_snapshot) {
    ir_verify_maybe_sabotage(function, pass_name, &pass_changed);
    if (!ir_verify_check_pass(function, verify_snapshot, pass_name,
                              &pass_changed)) {
      /* Divergence: IR restored; the pass is quarantined for this function,
       * so mark it clean at this version rather than re-running it. */
      clean_version[pass_id] = *version;
    }
    ir_verify_snapshot_free(verify_snapshot);
  }

  if (pass_changed) {
    *changed = 1;
    (*version)++;
  } else {
    clean_version[pass_id] = *version;
  }

  ir_trace_pass_event(pass_name, pass_changed ? "changed" : "clean", version,
                      pass_changed);
  return 1;
}
