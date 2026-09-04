#ifndef IR_RULES_H
#define IR_RULES_H

#include "ir.h"
#include "../error/error_reporter.h"
#include <stddef.h>
#include <stdio.h>

typedef struct {
  const char *file;
  size_t line;
  size_t column;
} IRRuleSite;

typedef struct {
  unsigned char *bytes;
  size_t size;
  size_t *pointer_offsets;
  size_t pointer_count;
  IRRuleSite *sites;
  size_t site_count;
  size_t function_count;
  size_t type_count;
} IRRuleImage;

void ir_rule_image_free(IRRuleImage *image);

typedef struct {
  size_t rules;
  size_t passed;
  size_t failed;
  size_t gaps;
  size_t malformed;
  long long steps;
} IRRuleStats;

typedef enum {
  IR_RULE_OVER_PROGRAM,
  IR_RULE_OVER_MACHINE,
  IR_RULE_OVER_TRACE
} IRRuleKind;

IRRuleKind ir_rule_kind(const IRFunction *rule);
int ir_program_has_rules(const IRProgram *program);
int ir_program_has_rules_of(const IRProgram *program, IRRuleKind kind);

int ir_rules_run(IRProgram *program, const IRRuleImage *image,
                 ErrorReporter *reporter, FILE *report, long long budget,
                 IRRuleStats *stats);

/* As above, and when `cross_check` is set every rule is run twice more over a
   freshly placed image and a verdict that moves is reported as R0005. */
int ir_rules_run_checked(IRProgram *program, const IRRuleImage *image,
                         ErrorReporter *reporter, FILE *report,
                         long long budget, int cross_check,
                         IRRuleStats *stats);

/* Run only the rules that ask for `kind`. The image has to be the one that
   kind names; nothing here checks that, because the caller built both. */
int ir_rules_run_kind(IRProgram *program, const IRRuleImage *image,
                      ErrorReporter *reporter, FILE *report, long long budget,
                      int cross_check, IRRuleKind kind, IRRuleStats *stats);


#endif
