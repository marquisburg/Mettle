#ifndef METTLE_RUNTIME_TRACE_H
#define METTLE_RUNTIME_TRACE_H

/* Recording what a compiled run did, so a `@rule` over a `Trace` can be asked
 * about a run in production and not only about the runs the tests drive.
 *
 * `--record-trace` instruments the program to call these at the same places
 * the compile-time interpreter records an event: entering a function,
 * allocating, freeing, taking a lock and releasing one. Each becomes one line
 * of text, and `mettle check-trace <file.mettle> <trace>` reads those lines
 * back into the same collector the interpreter fills and runs the trace rules
 * against them. The rule does not know which run it is reading, which is the
 * point: one rule, checked against a test and against a shipped process.
 *
 * The file is written where METTLE_TRACE says, or to mettle-trace.txt. A run
 * stops recording after METTLE_TRACE_MAX events (200000 by default) so a
 * long-lived process writes a bounded file rather than filling a disk, and
 * the last line says how many it dropped.
 */

#include <stdint.h>

void mettle_trace_enter(const char *name, const char *file, int64_t line,
                        int64_t column);
void mettle_trace_leave(void);
void mettle_trace_event(const char *kind, const char *name, const char *file,
                        int64_t line, int64_t column, int64_t value);
void mettle_trace_flush(void);

#endif /* METTLE_RUNTIME_TRACE_H */
