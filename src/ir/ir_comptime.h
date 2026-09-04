#ifndef IR_COMPTIME_H
#define IR_COMPTIME_H

/* Compile-time execution surfaces built on the reference interpreter:
 *
 *   mettle test <file>            run every @test function inside the
 *                                 compiler - no codegen, no linking, no
 *                                 process. Assertion failures render as
 *                                 rich diagnostics; every test doubles as a
 *                                 leak check because the interpreter owns
 *                                 the heap.
 *
 *   mettle trace <file> <fn> [args...]
 *                                 interpret one function on concrete
 *                                 arguments and print its source annotated
 *                                 line by line with the values that flowed
 *                                 through it.
 */

#include "ir.h"
#include "../error/error_reporter.h"

/* A hook the driver installs to run the program's trace rules after each
   test. Returning 0 fails that test. */
void ir_comptime_set_trace_rules(int (*hook)(void *, const char *),
                                 void *context);

/* Returns the process exit code (0 = all tests passed). */
int ir_comptime_run_tests(IRProgram *program, ErrorReporter *reporter,
                          const char *filename, const char *filter);

/* Returns the process exit code (0 = trace printed). */
int ir_comptime_trace(IRProgram *program, ErrorReporter *reporter,
                      const char *filename, const char *source,
                      const char *function_name, const char *const *args,
                      size_t arg_count);

#endif /* IR_COMPTIME_H */
