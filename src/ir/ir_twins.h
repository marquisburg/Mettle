#ifndef IR_TWINS_H
#define IR_TWINS_H

#include "ir.h"
#include "../error/error_reporter.h"
#include <stddef.h>
#include <stdio.h>

typedef struct {
  size_t pairs;
  size_t validated;
  size_t diverged;
  size_t gapped;
  size_t input_sets;
} IRTwinStats;

typedef struct IRTwinSnapshots IRTwinSnapshots;

int ir_program_has_twins(const IRProgram *program);
int ir_twins_check(IRProgram *program, ErrorReporter *reporter, FILE *report,
                   const char *stage, IRTwinStats *stats);

/* Snapshot every reference before the optimizer runs, so the pair can be
   re-checked afterwards even when the reference itself was swept as dead. */
IRTwinSnapshots *ir_twins_capture(IRProgram *program);
void ir_twins_snapshots_free(IRTwinSnapshots *snapshots);
int ir_twins_recheck(IRProgram *program, const IRTwinSnapshots *snapshots,
                     ErrorReporter *reporter, FILE *report, const char *stage,
                     IRTwinStats *stats);

#endif
