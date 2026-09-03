#ifndef RULE_REFLECT_H
#define RULE_REFLECT_H

#include "type_checker.h"
#include "../ir/ir_rules.h"
#include "../ir/ir_effects.h"

int rule_reflect_build(TypeChecker *checker, ASTNode *program,
                       const char *root_file, const char *target,
                       const IREffectResults *effects, IRRuleImage *out,
                       char **error_message);

#endif
