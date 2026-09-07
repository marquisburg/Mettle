#ifndef MIR_ANNOTATE_H
#define MIR_ANNOTATE_H

/* Codegen provenance annotator (Design A, forward-annotated).
 *
 * When --annotate-asm is set, the MIR encoder records, for every instruction it
 * emits, the byte range it occupies, a rendered assembly line (Intel and AT&T),
 * the originating source line, and the codegen decision behind it (spill, SIMD
 * kernel, strength-reduced divide, ...). The decision narration is enriched by
 * joining the per-function/per-line remarks the --explain optimizer already
 * produces, so the annotator reuses that verified vocabulary instead of
 * inventing a parallel one.
 *
 * Output: a human-readable listing to stdout, and a machine-readable
 * <output-stem>.annot.json sidecar the VS Code extension renders side-by-side
 * with the source. This is NOT a disassembler: it never decodes bytes back to
 * instructions; it captures intent at emit time, when the provenance still
 * exists. The raw bytes are recorded alongside each line so nothing is hidden.
 */

#include <stddef.h>

#include "codegen/binary/mir.h" /* MirFunction, MirInst */
#include "ir/ir.h"              /* IRFunction */

typedef enum {
  MIR_ANNOT_SYNTAX_INTEL = 0,
  MIR_ANNOT_SYNTAX_ATT = 1,
  MIR_ANNOT_SYNTAX_BOTH = 2
} MirAnnotSyntax;

/* Enable the annotator and choose the output stem (the -o path; the sidecar is
 * derived as <stem>.annot.json). Called once from main, mirroring the way
 * --explain wires its globals. Passing enabled=0 is the default no-op state. */
void mir_annotate_set_enabled(int enabled);
int mir_annotate_enabled(void);
/* Run the analysis for its numbers alone: --explain-json arms the annotator to
 * get per-loop cycles and port bottlenecks, but wants no asm listing and no
 * .annot.json sidecar. */
void mir_annotate_set_cost_only(int cost_only);
void mir_annotate_set_output_path(const char *output_path);
void mir_annotate_set_syntax(MirAnnotSyntax syntax);
void mir_annotate_set_source_file(const char *source_file);

/* LLM-facing focused queries. When a line query is set (lo>0), mir_annotate_flush
 * prints a compact report scoped to source lines [lo,hi] (asm + cost + covering
 * loops + live registers + decisions) instead of the full listing and sidecar;
 * fn (or NULL) restricts to one function. lo==0 with fn != NULL scopes the whole
 * function. A hot query (n>0) prints the program's top-n codegen hotspots. Both
 * are compact and structured for tools, not the human listing. */
void mir_annotate_set_line_query(int lo, int hi, const char *fn);
void mir_annotate_set_hot_query(int n);

/* Begin/finish capturing a function. begin() must be called before mir_encode
 * so per-instruction records land in the right function; ir_fn supplies the
 * source-line lookup (MirInst.ir_index -> ir_fn->instructions[i].location). */
void mir_annotate_begin_function(const char *name, const IRFunction *ir_fn,
                                 const char *filename, size_t decl_line);
void mir_annotate_end_function(void);

/* Record one emitted MIR instruction occupying [byte_off, byte_off+byte_len) in
 * the function's code buffer. `bytes` points at the just-emitted machine bytes.
 * No-op unless the annotator is enabled and a function is open. */
void mir_annotate_record(const MirFunction *fn, const MirInst *in,
                         int mir_index, size_t byte_off, size_t byte_len,
                         const unsigned char *bytes);

/* Record a synthetic span (prologue/epilogue padding) with a fixed label. */
void mir_annotate_record_synthetic(const char *label, const char *decision,
                                   size_t byte_off, size_t byte_len,
                                   const unsigned char *bytes);

/* Note which backend emitted the open function: "register-allocated" (MIR) or
 * "baseline (fallback)". reason is the MIR-gate bail code for the fallback (e.g.
 * "op:12"), or NULL. Shown in the function header of the listing. */
void mir_annotate_note_backend(const char *backend, const char *reason);

/* Record one span emitted by the BASELINE (fallback) backend, which works at IR
 * granularity (no register allocation). The asm column shows the IR operation as
 * a pseudo-op; the decision is derived from the IR opcode (vectorized kernel,
 * Fibonacci rotate idiom, inlined memcpy, ...). This is where the optimized
 * idioms the MIR gate rejects actually live, so it carries the most interesting
 * decisions. No-op unless enabled and a function is open. */
void mir_annotate_record_ir(const IRFunction *ir_fn, int ir_index,
                            size_t byte_off, size_t byte_len,
                            const unsigned char *bytes);

/* Record a zero-byte label marker (the baseline backend emits labels as no
 * bytes, so they never reach record_ir; without them, loop recovery cannot find
 * a backward branch's target). No-op unless enabled and a function is open. */

/* Flush everything captured so far: write <stem>.annot.json and print the
 * human-readable listing to stdout. Called once after codegen. */
void mir_annotate_flush(void);

#endif /* MIR_ANNOTATE_H */
