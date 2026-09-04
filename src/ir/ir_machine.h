#ifndef IR_MACHINE_H
#define IR_MACHINE_H

#include <stddef.h>

typedef struct {
  size_t line;
  size_t column;
  int vectorized;
  const char *kind;
} IRMachineLoop;

typedef struct {
  const char *name;
  const char *file;
  size_t line;
  size_t column;
  long long frame_bytes;
  long long spills;
  long long instructions;
  int register_allocated;
  long long inlined_calls;
  long long calls_left;
  IRMachineLoop *loops;
  size_t loop_count;
  size_t loop_capacity;
} IRMachineFunction;

void ir_explain_set_quiet(int quiet);
void ir_machine_set_collect(int on);
int ir_machine_collecting(void);
void ir_machine_note_frame(const char *name, long long frame_bytes,
                           long long spills);
void ir_machine_note_backend(const char *name, const char *file,
                             long long instructions, int register_allocated);
void ir_machine_note_loop(const char *function, const char *file, size_t line,
                          size_t column, int vectorized, const char *kind);
void ir_machine_note_call(const char *function, const char *file, int inlined);
size_t ir_machine_function_count(void);
const IRMachineFunction *ir_machine_function_at(size_t index);
void ir_machine_reset(void);

#endif
