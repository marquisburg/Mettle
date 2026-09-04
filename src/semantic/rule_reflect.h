#ifndef RULE_REFLECT_H
#define RULE_REFLECT_H

#include "type_checker.h"
#include "../ir/ir_rules.h"
#include "../ir/ir_effects.h"

int rule_reflect_build(TypeChecker *checker, ASTNode *program,
                       const char *root_file, const char *target,
                       const IREffectResults *effects, IRRuleImage *out,
                       char **error_message);

/* The snapshot a `@rule fn (m: Machine)` reads, built after code generation
   from what the passes recorded as they decided. */
/* The snapshot a `@rule fn (t: Trace)` reads, built from the events the
   interpreter recorded while `mettle test` ran. */
int rule_reflect_build_trace(TypeChecker *checker, const char *root_file,
                             IRRuleImage *out, char **error_message);

int rule_reflect_build_machine(TypeChecker *checker, const IRProgram *program,
                               const char *root_file, const char *target,
                               const IREffectResults *effects,
                               IRRuleImage *out, char **error_message);

#endif
