#ifndef PTX_EMITTER_H
#define PTX_EMITTER_H

#include "code_generator.h"
#include "ir/ir.h"
#include <stdio.h>

typedef struct {
  const char *target; /* PTX target name, for example "sm_121a" */
  int isa_major;      /* `.version` major component */
  int isa_minor;      /* `.version` minor component */
  int tensor_tuple_budget; /* 0 = architecture default; otherwise 1..4096 */
  /* --gpu-checks: emit the trap for `gpu_assert`. Off by default, so a
   * shipped kernel pays nothing for assertions left in the source. */
  int checks;
  /* --report-gpu-types: print what the address-space, alignment, layout and
   * uniformity analyses concluded, and what they cost. */
  int report_types;
} PtxEmitOptions;

/* Lower an IR program to NVIDIA PTX text (one `.visible .entry` per kernel),
 * targeting the CUDA Driver API's cuModuleLoadData JIT. Ordinary functions are
 * not exported as GPU entry points. Type resolution for symbol operands uses
 * `generator` (its symbol table / type checker); temp/SSA value types are
 * inferred from each defining instruction. PTX has unlimited typed virtual
 * registers, so there is no register allocation -- each IR value maps to a
 * fresh %r/%rd/%f/%fd/%p register of the appropriate class.
 *
 * GPU operations reach this backend as semantic MtlcIntrinsic identities.
 * The Mettle frontend maps its legacy gpu_* source aliases at the IR boundary;
 * other frontends use mtlc_intrinsic() without depending on those spellings.
 *
 * Returns 1 on success. On failure returns 0 and, if error is non-NULL, sets
 * *error to a malloc'd message the caller must free. */
int ptx_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                     const PtxEmitOptions *options, char **error);

#endif /* PTX_EMITTER_H */
