#ifndef METTLE_SCHEDULE_EXPAND_H
#define METTLE_SCHEDULE_EXPAND_H

#include "parser/ast.h"
#include "error/error_reporter.h"

typedef struct {
  size_t schedules;
  size_t phases;
  size_t generated;
} ScheduleStats;

int schedule_expand(ASTNode *program, ErrorReporter *reporter,
                    ScheduleStats *stats);

#endif
