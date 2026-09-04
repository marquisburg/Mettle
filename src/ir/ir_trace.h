#ifndef IR_TRACE_H
#define IR_TRACE_H

#include <stddef.h>

typedef struct {
  const char *kind;
  const char *name;
  const char *file;
  size_t line;
  size_t column;
  long long value;
} IRTraceEvent;

void ir_trace_set_collect(int on);
int ir_trace_collecting(void);
void ir_trace_begin(const char *test_name);
void ir_trace_record(const char *kind, const char *name, const char *file,
                     size_t line, size_t column, long long value);
const char *ir_trace_test_name(void);
size_t ir_trace_event_count(void);
const IRTraceEvent *ir_trace_event_at(size_t index);
void ir_trace_reset(void);

#endif
