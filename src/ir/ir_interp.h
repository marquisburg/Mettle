#ifndef IR_INTERP_H
#define IR_INTERP_H

/* In-compiler reference interpreter for optimized IR.
 *
 * This is the semantic arbiter behind --verify translation validation: it
 * executes an IRFunction directly over the in-memory instruction array with a
 * bounds-checked synthetic address space, and its behavior for every opcode -
 * including the SIMD kernel ops - is the documented scalar semantics from
 * ir.h. Two runs of the same machine configuration are bit-deterministic, so
 * "interpret the function before pass P and after pass P on identical inputs
 * and compare observations" is a sound equivalence check: any divergence means
 * the pass changed observable behavior.
 *
 * Observables: the return value, the final bytes of every registered buffer,
 * the ordered trace of extern calls (unknown externs are modeled as pure
 * "return 0" but traced, so deleting or reordering one is still visible), and
 * the final values of touched globals.
 */

#include "ir.h"
#include <stdint.h>

typedef enum {
  IR_INTERP_OK = 0,
  IR_INTERP_UNSUPPORTED, /* opcode/type outside the interpretable subset */
  IR_INTERP_TRAP,        /* div by zero, out-of-bounds, use-after-free */
  IR_INTERP_FUEL,        /* step budget exhausted */
  IR_INTERP_DEPTH,       /* call depth cap exceeded */
  /* The program called its own runtime guard (mettle_crash_trap*): a clean,
   * deliberate abort. Two runs that both guard-trap are considered
   * equivalent regardless of the exact crash point, matching how debug
   * checks behave under optimization. */
  IR_INTERP_GUARD_TRAP,
  /* assert()/assert_eq() builtin failed (mettle test). Details via
   * ir_interp_assert_info. */
  IR_INTERP_ASSERT_FAIL
} IRInterpStatus;

typedef struct {
  long long i;
  double f;
  int is_float;
  /* The value came from memory nothing had written, or was computed from one
     that did. A real run reads whatever the allocator or the stack left
     there, so nothing may be concluded from it. */
  int undefined;
} IRInterpValue;

/* Bytes captured per pointer argument at extern-call time. */
#define IR_INTERP_EXTERN_MEM_CAP 96

typedef struct {
  char name[64];
  IRInterpValue args[8];
  size_t arg_count;
  /* What each pointer argument addressed AT CALL TIME (capped): the memory an
   * extern could observe through the pointer. Length 0 = not a pointer into
   * interpreter memory. Without this, a pass that corrupts a locally-built
   * buffer handed to an extern is invisible - the pointer VALUE is identical
   * in both machines, and only final buffer bytes are otherwise compared. */
  unsigned char arg_mem[8][IR_INTERP_EXTERN_MEM_CAP];
  unsigned short arg_mem_len[8];
  /* The argument resolved to an address inside interpreter memory. Its
   * numeric value is an allocation address, which a pass is free to change by
   * changing how many objects a function builds; what the extern can observe
   * is the bytes, compared above. */
  unsigned char arg_is_pointer[8];
  unsigned char modelled;
} IRInterpExternCall;

typedef struct IRInterpMachine IRInterpMachine;

IRInterpMachine *ir_interp_create(IRProgram *program);
void ir_interp_destroy(IRInterpMachine *machine);

/* Route calls to `name` to `fn` instead of the program's copy (used to run a
 * self-recursive function's BEFORE snapshot against itself). */
void ir_interp_set_override(IRInterpMachine *machine, const char *name,
                            IRFunction *fn);

/* Register an input buffer; returns its synthetic address (or 0 on failure).
 * `init` may be NULL for a zeroed buffer. Buffers registered in the same
 * order with the same contents give two machines an identical address space. */
unsigned long long ir_interp_add_buffer(IRInterpMachine *machine,
                                        const void *init, long long size);

IRInterpStatus ir_interp_run(IRInterpMachine *machine, IRFunction *function,
                             const IRInterpValue *args, size_t arg_count,
                             IRInterpValue *result, long long fuel);

/* Observation accessors (valid after ir_interp_run). */
size_t ir_interp_buffer_count(const IRInterpMachine *machine);
const unsigned char *ir_interp_buffer_data(const IRInterpMachine *machine,
                                           size_t index, long long *size);
/* The run chose a path from a value it could not know -- uninitialized memory,
   or a call with no model. Everything after that branch is one arbitrary
   execution out of several, so no answer may be reported from it. */
int ir_interp_branched_on_undefined(const IRInterpMachine *machine);

size_t ir_interp_extern_trace_count(const IRInterpMachine *machine);
const IRInterpExternCall *ir_interp_extern_trace(const IRInterpMachine *machine,
                                                 size_t index);
/* Iterate final global values: returns count; name/value for index. */
size_t ir_interp_global_count(const IRInterpMachine *machine);
const char *ir_interp_global_name(const IRInterpMachine *machine, size_t index);
IRInterpValue ir_interp_global_value(const IRInterpMachine *machine,
                                     size_t index);

long long ir_interp_pointee_window(IRInterpMachine *machine,
                                   unsigned long long value,
                                   unsigned char *out, size_t capacity);

unsigned long long ir_interp_function_address(IRInterpMachine *machine,
                                              const char *name);

int ir_interp_buffer_is_literal(const IRInterpMachine *machine, size_t index);

unsigned long long
ir_interp_next_buffer_address(const IRInterpMachine *machine);

long long ir_interp_read_bytes(IRInterpMachine *machine,
                               unsigned long long address, unsigned char *out,
                               size_t length);

long long ir_interp_fuel_remaining(const IRInterpMachine *machine);

/* Human-readable reason for the last non-OK status ("call_indirect",
 * "extern trace overflow", "local type 'string'", ...). */
const char *ir_interp_status_detail(const IRInterpMachine *machine);

/* After IR_INTERP_ASSERT_FAIL: the assert call site and operand values.
 * Returns 0 when no assertion failed. */
int ir_interp_assert_info(const IRInterpMachine *machine, size_t *line,
                          size_t *column, IRInterpValue *left,
                          IRInterpValue *right, int *is_eq);

/* Source line of the NEW/malloc that created buffer `index` (0 = unknown /
 * harness-registered input buffer). For leak reporting. */
size_t ir_interp_buffer_alloc_line(const IRInterpMachine *machine,
                                   size_t index);
int ir_interp_buffer_freed(const IRInterpMachine *machine, size_t index);

/* Line-level value tracing: after each executed instruction inside
 * `only_in` frames that writes a named destination, the hook receives the
 * source line, destination name, and new value. */
/* `expansion_note` names the `comptime for` iteration that generated the
 * instruction, or is NULL for code the programmer wrote directly. Several
 * expansions of one written line otherwise report as one indistinguishable
 * run of values. */
typedef void (*IRInterpValueHook)(void *ctx, size_t line, const char *name,
                                  IRInterpValue value,
                                  const char *expansion_note);
void ir_interp_set_value_hook(IRInterpMachine *machine, IRInterpValueHook hook,
                              void *ctx, const IRFunction *only_in);

/* Execution counting (zero-run PGO): when enabled, every executed
 * instruction increments a per-function counter array indexed by instruction
 * position. Query after the run. */
void ir_interp_recheck_inferred_purity(IRInterpMachine *machine, int on);
void ir_interp_set_purity_fault(int on);
int ir_interp_purity_fault_enabled(void);
void ir_interp_enable_counting(IRInterpMachine *machine);
const long long *ir_interp_get_counts(const IRInterpMachine *machine,
                                      const IRFunction *function,
                                      size_t *count_out);

#endif /* IR_INTERP_H */
