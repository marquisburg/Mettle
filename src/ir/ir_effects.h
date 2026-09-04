#ifndef IR_EFFECTS_H
#define IR_EFFECTS_H

#include "ir.h"
#include "../error/error_reporter.h"
#include <stddef.h>
#include <stdio.h>

typedef struct {
  const char *name;
  SourceLocation site;
  int is_builtin;
  int is_exported;
} IREffectDecl;

typedef struct {
  const char *function;
  const char *signature;
  SourceLocation location;
} IREffectObligation;

typedef struct {
  const IREffectDecl *effects;
  size_t effect_count;
  const IREffectObligation *obligations;
  size_t obligation_count;
  int instrument;
  int library_build;
  FILE *report;
} IREffectInput;

typedef struct IREffectResults IREffectResults;

int ir_effects_run(IRProgram *program, const IREffectInput *input,
                   ErrorReporter *reporter, IREffectResults **out_results,
                   long long *out_steps);
void ir_effect_results_free(IREffectResults *results);
int ir_effect_results_lookup(const IREffectResults *results,
                             const char *function, const char ***performs,
                             size_t *perform_count, const char ***needs,
                             size_t *need_count);
int ir_program_declares_effects(const IRProgram *program);
int ir_effects_name_is_allocator(const char *name);
int ir_effects_name_is_known_clean(const char *name);

#endif
