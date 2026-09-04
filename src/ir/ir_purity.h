#ifndef IR_PURITY_H
#define IR_PURITY_H

#include "ir.h"
#include "../error/error_reporter.h"
#include <stddef.h>

typedef struct {
  size_t functions;
  size_t readonly;
  size_t speculatable;
  size_t declared_pure;
  size_t declared_pure_proven;
} IRPurityStats;

void ir_purity_infer(IRProgram *program);
int ir_purity_check_contracts(IRProgram *program, ErrorReporter *reporter,
                              IRPurityStats *stats);
const char *ir_purity_proof_name(const IRFunction *function);

#endif
