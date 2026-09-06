#ifndef IR_SAFETY_H
#define IR_SAFETY_H

/* `--safe`: turning unproven accesses into either nothing or a check.
 *
 * Lowering marks accesses with IR_OP_SAFETY_CHECK. Lifetime instrumentation
 * appends their pointer origins before scalar rewrites or inlining. The
 * optimizer propagates constants and recognizes induction variables with
 * these marks still present. Resolution then proves or widens each check
 * before vector recognition. Residual checks retain known intrinsic effects
 * and use the runtime call ABI when emitted by the backend.
 */

#include "ir.h"
#include <stddef.h>

typedef struct {
  size_t emitted;      /* accesses lowering marked */
  size_t proved;       /* deleted: the access cannot be out of bounds */
  size_t hoisted;      /* folded into one check covering a whole loop's range */
  size_t spanned;      /* kept, but as a compare against a once-resolved span */
  size_t exempt;       /* dropped: inside the allocator, which is not checked */
  size_t extent_tests; /* survivors that compile to a compare and branch */
  size_t region_calls; /* survivors that have to ask the runtime */
} IRSafetyStats;

/* Safety intrinsics keep the call ABI at the backend boundary. Optimizers
 * must use their effects rather than treat them as unknown external code. */
typedef enum {
  IR_SAFETY_INTRINSIC_NONE,
  IR_SAFETY_INTRINSIC_CHECK,
  IR_SAFETY_INTRINSIC_READ_ORIGIN,
  IR_SAFETY_INTRINSIC_WRITE_ORIGIN,
  IR_SAFETY_INTRINSIC_LIFETIME
} IRSafetyIntrinsic;
IRSafetyIntrinsic ir_safety_intrinsic(const IRInstruction *instruction);

/* Prove which private storage cannot contain pointer origins. */
int ir_safety_analyze_origins(IRProgram *program);

/* Resolve every check in the program. Returns zero on failure. A caller must
 * discard a failed compilation; resolution can have rewritten earlier
 * functions. `stats` may be NULL. */
int ir_safety_resolve_program(IRProgram *program, IRSafetyStats *stats);

/* Tell the runtime where the heap is: register what each allocation call
 * returns, and retire it before the matching free. Without this the shadow map
 * describes nothing, every region check finds unowned memory, and pointer
 * accesses pass unexamined.
 *
 * Runs after --native-heap has chosen the allocator, so it sees whichever
 * names the calls ended up with, and before the optimizer, so the bookkeeping
 * inlines and moves like any other call. Returns zero only on allocation
 * failure. */
int ir_safety_register_allocations(IRProgram *program);

/* Drop the stack notes whose local the optimizer removed. Runs after
 * optimization: a note is emitted at function entry for a local declared
 * anywhere in the body, so folding away the block that declared it leaves the
 * note addressing a slot that is no longer there. */
int ir_safety_retire_dangling_notes(IRProgram *program);

#endif /* IR_SAFETY_H */
