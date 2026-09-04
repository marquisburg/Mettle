#include "ir_twins.h"
#include "ir_verify.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IRFunction *twins_find(IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->name && strcmp(fn->name, name) == 0) {
      return fn;
    }
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    const char *base;
    if (!fn || !fn->name) {
      continue;
    }
    base = strrchr(fn->name, '_');
    if (base && strcmp(base + 1, name) == 0) {
      return fn;
    }
  }
  return NULL;
}

int ir_program_has_twins(const IRProgram *program) {
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->reference_twin) {
      return 1;
    }
  }
  return 0;
}

static int twins_signature_matches(const IRFunction *fast,
                                   const IRFunction *slow, char *why,
                                   size_t capacity) {
  if (fast->parameter_count != slow->parameter_count) {
    snprintf(why, capacity,
             "parameter count differs: `%s` takes %zu, `%s` takes %zu",
             fast->name, fast->parameter_count, slow->name,
             slow->parameter_count);
    return 0;
  }
  for (size_t i = 0; i < fast->parameter_count; i++) {
    const char *a = fast->parameter_types ? fast->parameter_types[i] : NULL;
    const char *b = slow->parameter_types ? slow->parameter_types[i] : NULL;
    if ((a == NULL) != (b == NULL) || (a && b && strcmp(a, b) != 0)) {
      snprintf(why, capacity,
               "parameter %zu differs: `%s` takes '%s', `%s` takes '%s'",
               i + 1, fast->name, a ? a : "?", slow->name, b ? b : "?");
      return 0;
    }
  }
  {
    const char *ra = fast->return_type_name;
    const char *rb = slow->return_type_name;
    if ((ra == NULL) != (rb == NULL) || (ra && rb && strcmp(ra, rb) != 0)) {
      snprintf(why, capacity,
               "return type differs: `%s` returns '%s', `%s` returns '%s'",
               fast->name, ra ? ra : "?", slow->name, rb ? rb : "?");
      return 0;
    }
  }
  return 1;
}

static void twins_report(ErrorReporter *reporter, const IRFunction *fn,
                         const char *code, const char *message,
                         const char *label, int is_error) {
  SourceSpan span = source_span_from_location(fn->location, 2);
  if (!reporter) {
    fprintf(stderr, "%s[%s]: %s\n", is_error ? "error" : "warning", code,
            message);
    return;
  }
  if (is_error) {
    error_reporter_add_error_with_span(reporter, ERROR_SEMANTIC, span,
                                       message);
  } else {
    error_reporter_add_warning_with_span(reporter, ERROR_SEMANTIC, span,
                                         message);
  }
  error_reporter_set_last_label(reporter, label);
  error_reporter_set_last_code(reporter, code);
}

struct IRTwinSnapshots {
  struct {
    const char *fast;
    const char *slow;
    IRVerifySnapshot *reference;
  } *pairs;
  size_t count;
};

IRTwinSnapshots *ir_twins_capture(IRProgram *program) {
  IRTwinSnapshots *snapshots;
  if (!program) {
    return NULL;
  }
  snapshots = calloc(1, sizeof(IRTwinSnapshots));
  if (!snapshots) {
    return NULL;
  }
  snapshots->pairs =
      calloc(program->function_count ? program->function_count : 1,
             sizeof(*snapshots->pairs));
  if (!snapshots->pairs) {
    free(snapshots);
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fast = program->functions[i];
    IRFunction *slow;
    if (!fast || !fast->reference_twin) {
      continue;
    }
    slow = twins_find(program, fast->reference_twin);
    if (!slow || slow == fast) {
      continue;
    }
    snapshots->pairs[snapshots->count].fast = fast->name;
    snapshots->pairs[snapshots->count].slow = slow->name;
    snapshots->pairs[snapshots->count].reference =
        ir_verify_snapshot_capture(slow);
    snapshots->count++;
  }
  return snapshots;
}

void ir_twins_snapshots_free(IRTwinSnapshots *snapshots) {
  if (!snapshots) {
    return;
  }
  for (size_t i = 0; i < snapshots->count; i++) {
    ir_verify_snapshot_free(snapshots->pairs[i].reference);
  }
  free(snapshots->pairs);
  free(snapshots);
}

int ir_twins_recheck(IRProgram *program, const IRTwinSnapshots *snapshots,
                     ErrorReporter *reporter, FILE *report, const char *stage,
                     IRTwinStats *stats) {
  int ok = 1;
  IRTwinStats local;
  memset(&local, 0, sizeof(local));
  if (!program || !snapshots) {
    if (stats) {
      *stats = local;
    }
    return 1;
  }
  for (size_t i = 0; i < snapshots->count; i++) {
    IRFunction *fast = twins_find(program, snapshots->pairs[i].fast);
    char why[512];
    char counterexample[512];
    char skip_reason[256];
    char message[900];
    IRVerifyRewriteVerdict verdict;
    if (!fast || !snapshots->pairs[i].reference) {
      continue;
    }
    local.pairs++;
    why[0] = '\0';
    counterexample[0] = '\0';
    skip_reason[0] = '\0';
    verdict = ir_verify_check_rewrite(
        program, fast, snapshots->pairs[i].reference, why, sizeof(why),
        counterexample, sizeof(counterexample), skip_reason,
        sizeof(skip_reason));
    if (verdict == IR_VERIFY_REWRITE_DIVERGED) {
      local.diverged++;
      ok = 0;
      snprintf(message, sizeof(message),
               "`%s` no longer agrees with its reference `%s` %s%s%s%s%s",
               snapshots->pairs[i].fast, snapshots->pairs[i].slow,
               stage ? stage : "", why[0] ? ": " : "", why,
               counterexample[0] ? "; " : "", counterexample);
      twins_report(reporter, fast, "T0001", message,
                   "diverges from its reference", 1);
    } else if (verdict == IR_VERIFY_REWRITE_VALIDATED) {
      local.validated++;
      local.input_sets += (size_t)ir_verify_last_input_run_count();
      if (report) {
        fprintf(report,
                "twin %s against %s (%s): agreed on %d generated input sets\n",
                snapshots->pairs[i].fast, snapshots->pairs[i].slow,
                stage ? stage : "?", ir_verify_last_input_run_count());
      }
    } else {
      local.gapped++;
      if (report) {
        fprintf(report, "twin %s against %s (%s): gap, %s\n",
                snapshots->pairs[i].fast, snapshots->pairs[i].slow,
                stage ? stage : "?",
                skip_reason[0] ? skip_reason : "no inputs");
      }
    }
  }
  if (stats) {
    *stats = local;
  }
  return ok;
}

int ir_twins_check(IRProgram *program, ErrorReporter *reporter, FILE *report,
                   const char *stage, IRTwinStats *stats) {
  int ok = 1;
  IRTwinStats local;
  memset(&local, 0, sizeof(local));
  if (!program) {
    if (stats) {
      *stats = local;
    }
    return 1;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fast = program->functions[i];
    IRFunction *slow;
    IRVerifySnapshot *reference;
    char why[512];
    char counterexample[512];
    char skip_reason[256];
    char message[900];
    IRVerifyRewriteVerdict verdict;
    if (!fast || !fast->reference_twin) {
      continue;
    }
    local.pairs++;
    slow = twins_find(program, fast->reference_twin);
    if (!slow) {
      snprintf(message, sizeof(message),
               "`%s` names `%s` as its reference, and this program defines no "
               "such function",
               fast->name, fast->reference_twin);
      twins_report(reporter, fast, "T0001", message, "no such reference", 1);
      local.diverged++;
      ok = 0;
      continue;
    }
    if (slow == fast) {
      snprintf(message, sizeof(message),
               "`%s` names itself as its reference, which checks nothing",
               fast->name);
      twins_report(reporter, fast, "T0001", message, "its own reference", 1);
      local.diverged++;
      ok = 0;
      continue;
    }
    why[0] = '\0';
    if (!twins_signature_matches(fast, slow, why, sizeof(why))) {
      snprintf(message, sizeof(message),
               "`%s` and its reference `%s` cannot be compared: %s",
               fast->name, slow->name, why);
      twins_report(reporter, fast, "T0001", message, "signatures differ", 1);
      local.diverged++;
      ok = 0;
      continue;
    }
    reference = ir_verify_snapshot_capture(slow);
    if (!reference) {
      snprintf(message, sizeof(message),
               "`%s` could not be snapshotted, so `%s` was not checked "
               "against it",
               slow->name, fast->name);
      twins_report(reporter, fast, "T0002", message, "gap", 0);
      local.gapped++;
      continue;
    }
    why[0] = '\0';
    counterexample[0] = '\0';
    skip_reason[0] = '\0';
    verdict = ir_verify_check_rewrite(program, fast, reference, why,
                                      sizeof(why), counterexample,
                                      sizeof(counterexample), skip_reason,
                                      sizeof(skip_reason));
    ir_verify_snapshot_free(reference);
    switch (verdict) {
    case IR_VERIFY_REWRITE_VALIDATED:
      local.validated++;
      local.input_sets += (size_t)ir_verify_last_input_run_count();
      if (report) {
        fprintf(report,
                "twin %s against %s (%s): agreed on %d generated input sets\n",
                fast->name, slow->name, stage ? stage : "?",
                ir_verify_last_input_run_count());
      }
      break;
    case IR_VERIFY_REWRITE_DIVERGED:
      local.diverged++;
      ok = 0;
      snprintf(message, sizeof(message),
               "`%s` and its reference `%s` disagree%s%s%s%s",
               fast->name, slow->name, why[0] ? ": " : "", why,
               counterexample[0] ? "; " : "", counterexample);
      twins_report(reporter, fast, "T0001", message, "diverges from its "
                                                     "reference", 1);
      if (report) {
        fprintf(report, "twin %s against %s (%s): diverged\n", fast->name,
                slow->name, stage ? stage : "?");
      }
      break;
    case IR_VERIFY_REWRITE_UNVERIFIABLE:
    default:
      local.gapped++;
      snprintf(message, sizeof(message),
               "`%s` was not checked against its reference `%s`: %s",
               fast->name, slow->name,
               skip_reason[0] ? skip_reason
                              : "the prober could not generate inputs");
      twins_report(reporter, fast, "T0002", message,
                   "the prober could not reach it", 0);
      if (report) {
        fprintf(report, "twin %s against %s (%s): gap, %s\n", fast->name,
                slow->name, stage ? stage : "?",
                skip_reason[0] ? skip_reason : "no inputs");
      }
      break;
    }
  }
  if (report && local.pairs > 0) {
    fprintf(report,
            "twins: %zu pair%s, %zu agreed, %zu diverged, %zu gapped, %zu "
            "input sets\n",
            local.pairs, local.pairs == 1 ? "" : "s", local.validated,
            local.diverged, local.gapped, local.input_sets);
  }
  if (stats) {
    *stats = local;
  }
  return ok;
}
