#ifndef IR_TRACE_RECORD_H
#define IR_TRACE_RECORD_H

/* `--record-trace`: instrument a compiled program to write down what it did.
 *
 * The events are the ones the compile-time interpreter already records, at
 * the same places and in the same vocabulary: entering a function, an
 * allocation, a free, taking a lock and releasing one. A run writes them as
 * lines of text, and `mettle check-trace` reads them back into the collector
 * the interpreter fills, so a `@rule` over a `Trace` can be asked about a run
 * in production and not only about the runs the tests drive.
 *
 * A program built without the flag has none of this in it.
 */

#include "ir.h"

int ir_trace_record_instrument(IRProgram *program, size_t *out_sites);

#endif /* IR_TRACE_RECORD_H */
