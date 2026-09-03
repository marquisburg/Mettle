#ifndef IR_EXPLAIN_SAFETY_H
#define IR_EXPLAIN_SAFETY_H

#include <stddef.h>

/* --explain surfacing of what --safe did to each memory access.
 *
 * The point of the mode is that most checks are gone by the time the program
 * runs, so the report has to say how many, and which ones are left and why.
 * Without that the overhead is a number nobody can act on; with it, a survivor
 * is a specific line with a specific reason, and usually a fixable one.
 *
 * Same shape and the same reason as ir_explain_memory.h: the safety pass runs
 * before the optimizer, in a different layer from the report, so it pushes
 * facts through a header that pulls in nothing from either side.
 *
 * Collection is off unless the driver enabled it, which it does only for
 * --explain builds. When off, every call here is a cheap no-op. */

/* Why a check is still in the program. */
typedef enum {
  IR_SAFETY_SURVIVOR_EXTENT = 0, /* compare against a constant extent */
  IR_SAFETY_SURVIVOR_REGION = 1, /* a call asking the runtime shadow map */
  IR_SAFETY_SURVIVOR_SPAN = 2    /* compare against a once-resolved allocation */
} IRSafetySurvivorKind;

/* Enable collection. `focus_file`, when non-NULL, limits notes to accesses in
 * that file by basename, mirroring how optimizer remarks are scoped to the
 * main input so imported modules do not flood the report. Must be called
 * before the safety pass runs, which is earlier than the optimizer's own
 * --explain state comes up. */
void ir_explain_safety_set_collect(int enabled, const char *focus_file);

/* Record one check that survived. `function` and `file` may be NULL. */
void ir_explain_safety_note(const char *file, size_t line,
                            const char *function_name,
                            IRSafetySurvivorKind kind);

/* Record the totals for the whole program. Called once, after the pass. Every
 * access lowering marked ends up in exactly one of these, so they must add up:
 * a summary that quietly drops an outcome is worse than none, because the
 * arithmetic looks wrong and the reader cannot tell which number to trust. */
void ir_explain_safety_totals(size_t emitted, size_t proved, size_t hoisted,
                              size_t spanned, size_t exempt,
                              size_t extent_tests, size_t region_calls);

void ir_explain_safety_typed_note(const char *file, size_t line,
                                  const char *function_name,
                                  const char *type_name, long long min,
                                  long long max, size_t length);

#endif /* IR_EXPLAIN_SAFETY_H */
