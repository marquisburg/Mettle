#include "codegen/binary/mir.h"
#include "../../common.h" /* mettle_fnv1a_hash */
#include "internal.h"     /* BINARY_SAFETY_GRANULE */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Bytes of stack home for an address-taken value, advancing `*base` first when
 * the home has to start on a granule boundary. Homes grow downward from a
 * 16-byte-aligned rbp and `spill_offset` is the far end, so an offset that is a
 * multiple of the granule puts the object's first byte on one. See
 * MirVreg::home_granule. */
static int mir_home_bytes_for(const MirVreg *vr, int *base) {
  int home = vr->home_bytes > 0 ? vr->home_bytes : 8;
  if (vr->home_granule) {
    home = (home + BINARY_SAFETY_GRANULE - 1) / BINARY_SAFETY_GRANULE *
           BINARY_SAFETY_GRANULE;
    *base = (*base + BINARY_SAFETY_GRANULE - 1) / BINARY_SAFETY_GRANULE *
            BINARY_SAFETY_GRANULE;
  }
  return home;
}

/* Linear-scan register allocation over MIR.
 *
 * Design choices that buy correctness cheaply:
 *  - RAX/RCX/RDX are NOT allocatable; they are left as encoder scratch. This
 *    means fixed-physreg ops (IDIV/DIV need RDX:RAX, variable shifts need CL)
 *    never have to be modeled as interval constraints, the encoder just moves
 *    vreg operands through the scratch regs. 11 GP regs remain allocatable,
 *    more than the legacy promoter's 7.
 *  - Allocatable GP, in preference order: volatile R8..R11 first (a leaf
 *    function need not save them), then nonvolatile RBX,RSI,RDI,R12..R15
 *    (saved/restored by the encoder's prologue/epilogue when used).
 *  - Liveness is computed over the linear MIR order, then conservatively
 *    extended across every backward branch to a fixpoint so a value live around
 *    a loop stays allocated across the back-edge. Over-extension only costs
 *    register pressure, never correctness.
 *  - When no register is free, the longest-remaining interval is spilled to a
 *    fresh rbp-relative slot (classic linear-scan "spill at interval"). */

/* Preference-ordered GP allocation pool. Volatile first.
 *
 * The incoming/outgoing argument registers are excluded so that no allocatable
 * register is ever an ABI argument register on EITHER calling convention:
 *   - Win64 args: RCX, RDX, R8, R9 (RCX/RDX are already scratch).
 *   - SysV  args: RDI, RSI, RDX, RCX, R8, R9.
 * Excluding R8/R9 AND RSI/RDI means parameter homing (prologue) and outgoing
 * call-argument moves can never clobber a not-yet-consumed argument that still
 * lives in one of those registers, the parallel-move hazard cannot arise on
 * Windows or Linux. The remaining pool is R10/R11 (volatile) plus the
 * universally callee-saved RBX/R12..R15. */
static const BinaryGpRegister MIR_GP_POOL[] = {
    BINARY_GP_RBX, BINARY_GP_R12, BINARY_GP_R13, BINARY_GP_R14, BINARY_GP_R15};
#define MIR_GP_POOL_COUNT (sizeof(MIR_GP_POOL) / sizeof(MIR_GP_POOL[0]))
/* Reclaimable registers, tried after the callee-saved base pool. RAX/RCX/RDX
 * are now allocatable (the encoder scratch moved to R10/R11): they are volatile
 *: so a cross-call value never lands in them (it uses MIR_GP_CROSSCALL_POOL),
 * and they carry implicit clobbers from the divide family, setcc, and variable
 * shifts, which mir_reg_clobbered_in_range keeps a live value out of. The ABI
 * argument registers (RCX/RDX/R8/R9 on Win64) are reclaimed unless they hold an
 * incoming parameter (mir_reg_poolable): outgoing-argument homing writes are
 * explicit phys-dst MOVs, which the same clobber index keeps live values out
 * of. RSI/RDI are callee-saved on Win64. */
static const BinaryGpRegister MIR_GP_EXTRA[] = {
    BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_RSI,
    BINARY_GP_RDI, BINARY_GP_R8,  BINARY_GP_R9};
#define MIR_GP_EXTRA_COUNT (sizeof(MIR_GP_EXTRA) / sizeof(MIR_GP_EXTRA[0]))
/* Upper bound on the leaf pool: the static base plus every extra. */
#define MIR_GP_LEAF_POOL_MAX (MIR_GP_POOL_COUNT + MIR_GP_EXTRA_COUNT)

/* True if the function makes any call (so caller-saved regs are unsafe to hold
 * values across, and outgoing-arg registers must not be reclaimed).
 *
 * The frame-pointer-omission decision depends on inline kernels counting here,
 * not only calls: a MIR_IR_KERNEL addresses its staged operands off the frame
 * base, and several of those kernels borrow stack with a balanced `sub rsp` or
 * push a register mid-body. rsp is therefore not stable across them, and an
 * rsp-relative slot address would land inside the kernel's own scratch. */
/* Everything that clobbers the caller-saved set without naming it in operands,
 * so a value living across one must be in a register the clobber cannot reach.
 * The inline block copies belong here for the same reason a call does: they
 * leave the destination in RAX and run the counter down in RCX, and neither is
 * an operand the clobber index can see. */
static int mir_op_is_call_barrier(MirOpcode op) {
  return op == MIR_CALL || op == MIR_CALL_INDIRECT || op == MIR_HEAP_NEW ||
         op == MIR_REP_MOVSB || op == MIR_REP_STOSB || op == MIR_SYSCALL ||
         op == MIR_INLINE_ASM || mir_op_is_inline_kernel(op);
}

static int mir_fn_has_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    /* An inline kernel clobbers the ABI argument/caller-saved registers, so the
     * function must be treated as non-leaf: RCX/RDX/R8/R9 are unsafe to reclaim
     * into the general pool (the kernel overwrites them). */
    if (mir_op_is_call_barrier(fn->insns[i].op)) {
      return 1;
    }
  }
  return 0;
}

/* Mark every value that spans a clobber barrier, and note whether all of the
 * barriers it spans were RAX-preserving calls. Shared by both allocators so the
 * two cannot drift on what counts as a call. */
static void mir_mark_crosses_call(MirFunction *fn) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    fn->vregs[v].crosses_call = 0;
    fn->vregs[v].crosses_preserving_only = 1;
    fn->vregs[v].crosses_xmm_preserving_only = 1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (!mir_op_is_call_barrier(fn->insns[i].op)) {
      continue;
    }
    int is_call = fn->insns[i].op == MIR_CALL;
    int keeps_rax = is_call && fn->insns[i].preserves_rax;
    int keeps_xmm = is_call && fn->insns[i].preserves_xmm;
    int c = (int)i;
    for (size_t v = 0; v < fn->vreg_count; v++) {
      MirVreg *vr = &fn->vregs[v];
      if (vr->live_start != MIR_LIVE_NONE && vr->live_start < c &&
          vr->live_end > c) {
        vr->crosses_call = 1;
        if (!keeps_rax) {
          vr->crosses_preserving_only = 0;
        }
        if (!keeps_xmm) {
          vr->crosses_xmm_preserving_only = 0;
        }
      }
    }
  }
}

/* Drop the save and restore wherever the allocator did not, in the end, put
 * anything in a preserved register.
 *
 * The slots have to be reserved before colouring and the answer is only known
 * after it, so this is the earliest the question can be asked. It matters: a
 * function with no float work at all would otherwise store and reload four XMM
 * lanes at every check, including the ones the compiler hoisted in front of a
 * loop and which therefore really run. */
static void mir_drop_unused_preserves(MirFunction *fn) {
  int need_gp = 0;
  int need_xmm = 0;
  for (size_t v = 0; v < fn->vreg_count; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (!vr->in_register || !vr->crosses_call) {
      continue;
    }
    if (vr->rclass == MIR_RC_GP && vr->phys == (int)BINARY_GP_RAX) {
      need_gp = 1;
    } else if (vr->rclass == MIR_RC_XMM) {
      for (size_t i = 0; i < MIR_XMM_POOL_COUNT; i++) {
        if (vr->phys == (int)MIR_XMM_POOL[i]) {
          need_xmm = 1;
        }
      }
    }
  }
  if (!need_gp) {
    fn->preserve_slot = 0;
  }
  if (!need_xmm) {
    fn->preserve_xmm_slot = 0;
  }
}

/* The cross-call pool as `vr` sees it: the callee-saved base, plus RAX when
 * every call its range spans preserves RAX. RAX goes last so a value still
 * prefers a register that costs the prologue nothing. `buffer` holds
 * MIR_GP_CROSSCALL_POOL_EXT entries. */
static size_t mir_cross_pool_for(const MirVreg *vr,
                                 const BinaryGpRegister *base, size_t base_n,
                                 BinaryGpRegister *buffer) {
  size_t n = 0;
  for (; n < base_n; n++) {
    buffer[n] = base[n];
  }
  if (vr->crosses_preserving_only) {
    buffer[n++] = BINARY_GP_RAX;
  }
  return n;
}

/* Does this function hold a call the encoder saves registers around? Each kind
 * needs its own frame slots to park them in. */
static int mir_fn_has_preserving_call(const MirFunction *fn, int xmm) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op != MIR_CALL) {
      continue;
    }
    if (xmm ? fn->insns[i].preserves_xmm : fn->insns[i].preserves_rax) {
      return 1;
    }
  }
  return 0;
}

/* True if the function makes a REAL call (MIR_CALL / MIR_CALL_INDIRECT), as
 * opposed to an inline SIMD kernel. Used for LEAF-POOL BUILDING: a real call
 * clobbers all caller-saved registers with no per-clobber PHYS write for the
 * allocator to see, so arg registers stay out of the general pool. An inline
 * kernel is different -- it marshals its operands through explicit
 * `MIR_MOV phys(RCX/RDX/R8/R9/RAX), value` writes (which mir_reg_clobbered_in_range
 * detects) and is itself a crosses_call barrier (which bars spanning values from
 * the arg registers). Both per-vreg mechanisms run regardless of the pool, so a
 * kernel-only function can keep the full leaf pool; the histogram/RMW loops that
 * run AFTER a one-time `simd_fill` init were needlessly spilling because the fill
 * shrank the whole function's pool (radix_sort). */
static int mir_fn_has_real_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_CALL || fn->insns[i].op == MIR_CALL_INDIRECT ||
        fn->insns[i].op == MIR_HEAP_NEW ||
        fn->insns[i].op == MIR_REP_MOVSB ||
        fn->insns[i].op == MIR_REP_STOSB ||
        fn->insns[i].op == MIR_SYSCALL) {
      return 1;
    }
  }
  return 0;
}

/* True if the function contains an inline SLP vector kernel. Such kernels
 * marshal through fixed registers without representing every clobber as a MIR
 * PHYS write, so when the frame pointer is omitted we conservatively keep rbp
 * out of the allocatable pool for these functions (they are matmul/dot kernels
 * that already beat gcc and gain nothing from one more GP register). */
static int mir_fn_uses_slp(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_op_is_inline_kernel(fn->insns[i].op)) {
      return 1;
    }
  }
  return 0;
}

/* Integer argument-register index of `reg` under the active ABI, or -1 if it is
 * not an argument register at all. */
static int mir_reg_arg_index(BinaryGpRegister reg) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  if (abi && abi->int_param_registers) {
    for (size_t i = 0; i < abi->int_param_count; i++) {
      if (abi->int_param_registers[i] == reg) {
        return (int)i;
      }
    }
  }
  return -1;
}

/* Whether `reg` may join the non-cross-call allocatable pool.
 * An arg register is poolable when it carries NO incoming parameter (its arg
 * index >= param_count, so prologue homing never reads it). Outgoing calls do
 * NOT bar it, even though the pre-call homing sequence writes the arg
 * registers: every explicit `MIR_MOV phys(reg), value` is a clobber event in
 * mir_reg_clobbered_in_range's per-register index, so a value whose interval
 * contains a homing write is never placed in that register; a value that
 * SPANS the call is cross-call and barred from all volatiles. (Verified by
 * tests/test_regalloc_argreg_call_pressure.mettle, which forces enough
 * pressure that argument sources land in R8/R9 around a 4-arg call.)
 * Non-arg callee-saved registers (RSI/RDI on Win64) are always poolable. */
static int mir_reg_poolable(BinaryGpRegister reg, size_t param_count,
                            int is_leaf) {
  (void)is_leaf;
  (void)param_count;
  (void)reg;
  /* Even a register holding an incoming parameter is poolable. The homing
   * hazard it used to be excluded for is a hazard only among the PARAMETERS --
   * one parameter's home overwriting an argument register another parameter has
   * not been read out of yet -- so it is enough to bar arg registers from the
   * entry-live values themselves, which mir_color_reg_mask does per vreg. Every
   * other value is defined after the prologue has finished homing, when the
   * argument registers hold nothing anyone still needs.
   *
   * Excluding them wholesale cost real registers: a 5-parameter function lost
   * ALL FOUR Win64 argument registers, leaving eight for its whole body, and a
   * merge loop with eight live values then spilled its array reads. */
  return 1;
}

/* Build the non-cross-call GP pool: the universally-safe base, then any
 * arg-capable register the function does not need for its own parameters. On
 * Win64 this reclaims RSI/RDI (callee-saved, never args) plus trailing unused
 * arg registers; on SysV it reclaims every arg register past the parameter
 * count. Reclaimed nonvolatiles are saved by the used-nonvolatile machinery;
 * caller-saved regs are used only for values that do not cross calls. */
static size_t mir_build_gp_leaf_pool(BinaryGpRegister *out, size_t param_count,
                                     int is_leaf) {
  size_t n = 0;
  for (size_t i = 0; i < MIR_GP_POOL_COUNT; i++) {
    out[n++] = MIR_GP_POOL[i];
  }
  for (size_t i = 0; i < MIR_GP_EXTRA_COUNT; i++) {
    if (mir_reg_poolable(MIR_GP_EXTRA[i], param_count, is_leaf)) {
      out[n++] = MIR_GP_EXTRA[i];
    }
  }
  return n;
}

/* Max cross-call GP pool: Win64 can also use RSI/RDI (nonvolatile there), while
 * SysV must keep them out because calls may clobber argument registers. */
#define MIR_GP_CROSSCALL_POOL_MAX 7
/* Room for the one register a preserving call gives back on top of the pool. */
#define MIR_GP_CROSSCALL_POOL_EXT (MIR_GP_CROSSCALL_POOL_MAX + 1)

static size_t mir_build_gp_crosscall_pool(BinaryGpRegister *out) {
  size_t n = 0;
  const BinaryAbi *abi = code_generator_binary_active_abi();
  int sysv = abi && abi->counts_classes_separately;
  out[n++] = BINARY_GP_RBX;
  if (!sysv) {
    out[n++] = BINARY_GP_RSI;
    out[n++] = BINARY_GP_RDI;
  }
  out[n++] = BINARY_GP_R12;
  out[n++] = BINARY_GP_R13;
  out[n++] = BINARY_GP_R14;
  out[n++] = BINARY_GP_R15;
  return n;
}

/* XMM pool: Win64 volatile lanes XMM0..XMM3. XMM4/XMM5 are reserved as the two
 * float scratch registers the encoder uses (analogous to RAX/RCX for GP), for
 * staging spilled/immediate float operands and breaking non-commutative
 * aliasing. All are caller-saved, so a leaf function need not preserve them. */
const BinaryXmmRegister MIR_XMM_POOL[MIR_XMM_POOL_COUNT] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3};

/* Second-tier XMM pool: xmm8..xmm15. These are callee-saved on Win64 (the
 * prologue saves/restores the ones used) and caller-saved on SysV; either way a
 * value placed here that does NOT live across a call is correct. They are
 * argument registers on neither ABI (Win64 floats: xmm0-3; SysV: xmm0-7), so no
 * parameter-homing or call-marshalling hazard arises. Tried only after the
 * volatile xmm0-3 are exhausted, so leaf code with light float pressure pays no
 * save/restore. */
static const BinaryXmmRegister MIR_XMM_NONVOL_POOL[] = {
    BINARY_XMM8,  BINARY_XMM9,  BINARY_XMM10, BINARY_XMM11,
    BINARY_XMM12, BINARY_XMM13, BINARY_XMM14, BINARY_XMM15};
#define MIR_XMM_NONVOL_POOL_COUNT \
  (sizeof(MIR_XMM_NONVOL_POOL) / sizeof(MIR_XMM_NONVOL_POOL[0]))

static int mir_gp_is_nonvolatile(BinaryGpRegister reg) {
  return code_generator_binary_gp_register_is_win64_nonvolatile(reg);
}

/* Record each vreg use/def site into the vreg's [live_start, live_end]. */
static void mir_note_operand_liveness(MirFunction *fn, const MirOperand *op,
                                      int index) {
  if (!op) {
    return;
  }
  MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
  if (op->kind == MIR_OPK_VREG) {
    ids[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    ids[0] = op->mem.base;
    ids[1] = op->mem.index;
  }
  for (int k = 0; k < 2; k++) {
    MirVregId v = ids[k];
    if (v < 0 || (size_t)v >= fn->vreg_count) {
      continue;
    }
    MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE || index < vr->live_start) {
      vr->live_start = index;
    }
    if (vr->live_end == MIR_LIVE_NONE || index > vr->live_end) {
      vr->live_end = index;
    }
  }
}

/* Find the MIR index of a label definition, or -1. */
static int mir_find_label(const MirFunction *fn, const char *name) {
  if (!name) {
    return -1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/* All of these carry the branch-target label in dst. A backward target makes
 * the instruction a loop back-edge (e.g. a rotated loop's bottom-test CMPBR). */
static int mir_inst_is_branch(const MirInst *in) {
  return in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
         in->op == MIR_FCMPBR;
}

typedef struct {
  int l; /* label (loop header) instruction index */
  int b; /* backward-branch instruction index; l < b */
} MirBackEdge;

/* Every loop back-edge of the function, in instruction order. Resolves labels
 * through a name table built in one pass, instead of a mir_find_label scan per
 * branch. Returns 1 on success (with *edges_out possibly NULL when there are
 * no back-edges) and 0 on allocation failure, which the caller must handle by
 * falling back to per-pass edge derivation, skipping extension entirely
 * would let loop-carried vregs share registers with loop-body temps. */
static int mir_collect_back_edges(const MirFunction *fn,
                                  MirBackEdge **edges_out, size_t *count_out) {
  size_t label_count = 0;
  size_t branch_count = 0;
  *edges_out = NULL;
  *count_out = 0;

  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      label_count++;
    } else if (mir_inst_is_branch(in) && in->dst.kind == MIR_OPK_LABEL) {
      branch_count++;
    }
  }
  if (branch_count == 0) {
    return 1; /* nothing to extend */
  }

  /* label name -> index, open addressing, sized for load factor <= 0.5 */
  size_t slot_count = 16;
  while (slot_count < label_count * 2) {
    slot_count *= 2;
  }
  size_t *slots = calloc(slot_count, sizeof(*slots)); /* insn index + 1 */
  MirBackEdge *edges = malloc(branch_count * sizeof(*edges));
  if (!slots || !edges) {
    free(slots);
    free(edges);
    return 0;
  }

  size_t mask = slot_count - 1;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op != MIR_LABEL || in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      continue;
    }
    size_t h = mettle_fnv1a_hash(in->dst.sym) & mask;
    while (slots[h]) {
      /* mir_find_label returns the FIRST label with a name; keep that. */
      if (strcmp(fn->insns[slots[h] - 1].dst.sym, in->dst.sym) == 0) {
        if (getenv("METTLE_MIR_DUPLABEL")) {
          fprintf(stderr, "MIR-DUPLABEL %s at %zu (first at %zu)\n",
                  in->dst.sym, i, slots[h] - 1);
        }
        h = SIZE_MAX;
        break;
      }
      h = (h + 1) & mask;
    }
    if (h != SIZE_MAX) {
      slots[h] = i + 1;
    }
  }

  size_t n = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (!mir_inst_is_branch(in) || in->dst.kind != MIR_OPK_LABEL ||
        !in->dst.sym) {
      continue;
    }
    int l = -1;
    size_t h = mettle_fnv1a_hash(in->dst.sym) & mask;
    while (slots[h]) {
      if (strcmp(fn->insns[slots[h] - 1].dst.sym, in->dst.sym) == 0) {
        l = (int)(slots[h] - 1);
        break;
      }
      h = (h + 1) & mask;
    }
    if (l < 0 || l >= (int)i) {
      continue; /* forward branch or unknown label: no loop back-edge */
    }
    edges[n].l = l;
    edges[n].b = (int)i;
    n++;
  }

  free(slots);
  if (n == 0) {
    free(edges);
    return 1;
  }
  *edges_out = edges;
  *count_out = n;
  return 1;
}

#define MIR_LIVE_CFG_MAX_WORK 8000000u

typedef struct {
  size_t *slots;
  size_t mask;
} MirLabelMap;

typedef struct {
  int *block_of;
  int *block_start;
  unsigned long long *live_in;
  unsigned long long *live_out;
  size_t block_count;
  size_t words;
} MirLiveCfg;

static int mir_label_map_build(const MirFunction *fn, MirLabelMap *map) {
  size_t label_count = 0;
  size_t slot_count = 16;
  map->slots = NULL;
  map->mask = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      label_count++;
    }
  }
  while (slot_count < label_count * 2) {
    slot_count *= 2;
  }
  map->slots = (size_t *)calloc(slot_count, sizeof(*map->slots));
  if (!map->slots) {
    return 0;
  }
  map->mask = slot_count - 1;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    size_t h;
    int duplicate = 0;
    if (in->op != MIR_LABEL || in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      continue;
    }
    h = mettle_fnv1a_hash(in->dst.sym) & map->mask;
    while (map->slots[h]) {
      if (strcmp(fn->insns[map->slots[h] - 1].dst.sym, in->dst.sym) == 0) {
        duplicate = 1;
        break;
      }
      h = (h + 1) & map->mask;
    }
    if (!duplicate) {
      map->slots[h] = i + 1;
    }
  }
  return 1;
}

static int mir_label_map_find(const MirFunction *fn, const MirLabelMap *map,
                              const char *name) {
  size_t h;
  if (!name || !map->slots) {
    return -1;
  }
  h = mettle_fnv1a_hash(name) & map->mask;
  while (map->slots[h]) {
    if (strcmp(fn->insns[map->slots[h] - 1].dst.sym, name) == 0) {
      return (int)(map->slots[h] - 1);
    }
    h = (h + 1) & map->mask;
  }
  return -1;
}

static int mir_inst_ends_block(const MirInst *in) {
  return in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
         in->op == MIR_FCMPBR || in->op == MIR_JMP_TABLE || in->op == MIR_RET ||
         in->op == MIR_TRAP;
}

static void mir_live_bit_set(unsigned long long *set, size_t v) {
  set[v >> 6] |= 1ull << (v & 63);
}

static int mir_live_bit_get(const unsigned long long *set, size_t v) {
  return (set[v >> 6] & (1ull << (v & 63))) != 0;
}

static void mir_live_note_uses(const MirFunction *fn, const MirOperand *op,
                               unsigned long long *use_set,
                               const unsigned long long *def_set,
                               int skip_plain_vreg) {
  MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
  if (op->kind == MIR_OPK_VREG) {
    if (skip_plain_vreg) {
      return;
    }
    ids[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    ids[0] = op->mem.base;
    ids[1] = op->mem.index;
  } else {
    return;
  }
  for (int k = 0; k < 2; k++) {
    MirVregId v = ids[k];
    if (v < 0 || (size_t)v >= fn->vreg_count) {
      continue;
    }
    if (!mir_live_bit_get(def_set, (size_t)v)) {
      mir_live_bit_set(use_set, (size_t)v);
    }
  }
}

/* Add every vreg read by one operand to a live set. Unlike
 * mir_live_note_uses, this runs while walking a block backwards, after the
 * instruction's definition has already been removed from the live set. */
static void mir_live_add_operand(const MirFunction *fn, const MirOperand *op,
                                 unsigned long long *live,
                                 int skip_plain_vreg) {
  MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
  if (op->kind == MIR_OPK_VREG) {
    if (skip_plain_vreg) {
      return;
    }
    ids[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    ids[0] = op->mem.base;
    ids[1] = op->mem.index;
  } else {
    return;
  }
  for (int k = 0; k < 2; k++) {
    MirVregId v = ids[k];
    if (v >= 0 && (size_t)v < fn->vreg_count) {
      mir_live_bit_set(live, (size_t)v);
    }
  }
}

static int mir_live_cfg_build(const MirFunction *fn, MirLiveCfg *cfg,
                              int precise_defs) {
  MirLabelMap map;
  unsigned char *leader = NULL;
  int *block_start = NULL;
  int *succ_head = NULL;
  int *succ_next = NULL;
  int *succ_block = NULL;
  unsigned long long *use_set = NULL;
  unsigned long long *def_set = NULL;
  unsigned char *killable = NULL;
  int *first_dst = NULL;
  int *first_any = NULL;
  size_t words = (fn->vreg_count + 63) / 64;
  size_t nblocks = 0;
  size_t succ_capacity = 0;
  size_t succ_count = 0;
  int ok = 0;
  int changed = 1;

  cfg->block_of = NULL;
  cfg->block_start = NULL;
  cfg->live_in = NULL;
  cfg->live_out = NULL;
  cfg->block_count = 0;
  cfg->words = words;
  if (fn->insn_count == 0 || fn->vreg_count == 0 || words == 0) {
    return 0;
  }
  if ((unsigned long long)fn->insn_count * (unsigned long long)words >
      MIR_LIVE_CFG_MAX_WORK) {
    return 0;
  }
  if (!mir_label_map_build(fn, &map)) {
    return 0;
  }

  leader = (unsigned char *)calloc(fn->insn_count, sizeof(*leader));
  cfg->block_of = (int *)malloc(fn->insn_count * sizeof(*cfg->block_of));
  if (!precise_defs) {
    killable = (unsigned char *)calloc(fn->vreg_count, sizeof(*killable));
    first_dst = (int *)malloc(fn->vreg_count * sizeof(*first_dst));
    first_any = (int *)malloc(fn->vreg_count * sizeof(*first_any));
  }
  if (!leader || !cfg->block_of ||
      (!precise_defs && (!killable || !first_dst || !first_any))) {
    goto done;
  }
  if (!precise_defs) {
    for (size_t v = 0; v < fn->vreg_count; v++) {
      first_dst[v] = -1;
      first_any[v] = -1;
    }
  }

  leader[0] = 1;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL) {
      leader[i] = 1;
    }
    if (mir_inst_ends_block(in) && i + 1 < fn->insn_count) {
      leader[i + 1] = 1;
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (leader[i]) {
      nblocks++;
    }
    cfg->block_of[i] = (int)nblocks - 1;
  }
  block_start = (int *)malloc(nblocks * sizeof(*block_start));
  succ_head = (int *)malloc(nblocks * sizeof(*succ_head));
  if (!block_start || !succ_head) {
    goto done;
  }
  {
    size_t b = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      if (leader[i]) {
        block_start[b++] = (int)i;
      }
    }
  }
  for (size_t b = 0; b < nblocks; b++) {
    succ_head[b] = -1;
  }

  succ_capacity = nblocks * 2 + 8;
  succ_next = (int *)malloc(succ_capacity * sizeof(*succ_next));
  succ_block = (int *)malloc(succ_capacity * sizeof(*succ_block));
  if (!succ_next || !succ_block) {
    goto done;
  }
  for (size_t b = 0; b < nblocks; b++) {
    size_t last = (b + 1 < nblocks) ? (size_t)block_start[b + 1] - 1
                                    : fn->insn_count - 1;
    const MirInst *in = &fn->insns[last];
    int targets[2];
    int target_count = 0;
    int fall_through = 1;
    const MirJumpTable *table = NULL;
    if (in->op == MIR_JMP) {
      fall_through = 0;
      targets[target_count++] = mir_label_map_find(fn, &map, in->dst.sym);
    } else if (in->op == MIR_JCC || in->op == MIR_CMPBR ||
               in->op == MIR_FCMPBR) {
      targets[target_count++] = mir_label_map_find(fn, &map, in->dst.sym);
    } else if (in->op == MIR_JMP_TABLE) {
      fall_through = 0;
      table = (const MirJumpTable *)in->aux;
      if (!table) {
        goto done;
      }
    } else if (in->op == MIR_RET || in->op == MIR_TRAP) {
      fall_through = 0;
    }
    for (int t = 0; t < target_count; t++) {
      if (targets[t] < 0) {
        goto done;
      }
    }
    if (fall_through && b + 1 >= nblocks) {
      fall_through = 0;
    }
    {
      size_t need = succ_count + (size_t)target_count + (fall_through ? 1u : 0u);
      if (table) {
        need += table->count;
      }
      if (need > succ_capacity) {
        int *n1;
        int *n2;
        size_t grown = succ_capacity * 2 + need;
        n1 = (int *)realloc(succ_next, grown * sizeof(*succ_next));
        if (!n1) {
          goto done;
        }
        succ_next = n1;
        n2 = (int *)realloc(succ_block, grown * sizeof(*succ_block));
        if (!n2) {
          goto done;
        }
        succ_block = n2;
        succ_capacity = grown;
      }
    }
    for (int t = 0; t < target_count; t++) {
      succ_block[succ_count] = cfg->block_of[targets[t]];
      succ_next[succ_count] = succ_head[b];
      succ_head[b] = (int)succ_count;
      succ_count++;
    }
    if (table) {
      for (size_t t = 0; t < table->count; t++) {
        int target = mir_label_map_find(fn, &map, table->labels[t]);
        if (target < 0) {
          goto done;
        }
        succ_block[succ_count] = cfg->block_of[target];
        succ_next[succ_count] = succ_head[b];
        succ_head[b] = (int)succ_count;
        succ_count++;
      }
    }
    if (fall_through) {
      succ_block[succ_count] = (int)b + 1;
      succ_next[succ_count] = succ_head[b];
      succ_head[b] = (int)succ_count;
      succ_count++;
    }
  }

  if (!precise_defs) {
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      const MirOperand *ops[3] = {&in->dst, &in->a, &in->b};
      for (int k = 0; k < 3; k++) {
        MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
        if (ops[k]->kind == MIR_OPK_VREG) {
          ids[0] = ops[k]->vreg;
        } else if (ops[k]->kind == MIR_OPK_MEM) {
          ids[0] = ops[k]->mem.base;
          ids[1] = ops[k]->mem.index;
        }
        for (int j = 0; j < 2; j++) {
          MirVregId v = ids[j];
          if (v < 0 || (size_t)v >= fn->vreg_count) {
            continue;
          }
          if (first_any[v] < 0) {
            first_any[v] = (int)i;
          }
          if (k == 0 && ops[k]->kind == MIR_OPK_VREG && first_dst[v] < 0) {
            first_dst[v] = (int)i;
          }
        }
      }
    }
    for (size_t v = 0; v < fn->vreg_count; v++) {
      killable[v] = (first_dst[v] >= 0 && first_dst[v] == first_any[v] &&
                     !fn->vregs[v].entry_live)
                        ? 1
                        : 0;
    }
  }

  use_set = (unsigned long long *)calloc(nblocks * words, sizeof(*use_set));
  def_set = (unsigned long long *)calloc(nblocks * words, sizeof(*def_set));
  cfg->live_in =
      (unsigned long long *)calloc(nblocks * words, sizeof(*cfg->live_in));
  if (!use_set || !def_set || !cfg->live_in) {
    goto done;
  }
  for (size_t b = 0; b < nblocks; b++) {
    size_t lo = (size_t)block_start[b];
    size_t hi = (b + 1 < nblocks) ? (size_t)block_start[b + 1] : fn->insn_count;
    unsigned long long *u = use_set + b * words;
    unsigned long long *d = def_set + b * words;
    for (size_t i = lo; i < hi; i++) {
      const MirInst *in = &fn->insns[i];
      mir_live_note_uses(fn, &in->dst, u, d, 1);
      mir_live_note_uses(fn, &in->a, u, d, 0);
      mir_live_note_uses(fn, &in->b, u, d, 0);
      if (in->dst.kind == MIR_OPK_VREG) {
        MirVregId v = in->dst.vreg;
        if (v >= 0 && (size_t)v < fn->vreg_count) {
          if (precise_defs) {
            mir_live_bit_set(d, (size_t)v);
          } else if (killable[v] && first_dst[v] == (int)i) {
            mir_live_bit_set(d, (size_t)v);
          } else if (!mir_live_bit_get(d, (size_t)v)) {
            mir_live_bit_set(u, (size_t)v);
          }
        }
      }
    }
  }

  while (changed) {
    changed = 0;
    for (size_t bi = nblocks; bi > 0; bi--) {
      size_t b = bi - 1;
      const unsigned long long *u = use_set + b * words;
      const unsigned long long *d = def_set + b * words;
      unsigned long long *in_set = cfg->live_in + b * words;
      for (int e = succ_head[b]; e >= 0; e = succ_next[e]) {
        const unsigned long long *s =
            cfg->live_in + (size_t)succ_block[e] * words;
        for (size_t w = 0; w < words; w++) {
          unsigned long long next = in_set[w] | (s[w] & ~d[w]);
          if (next != in_set[w]) {
            in_set[w] = next;
            changed = 1;
          }
        }
      }
      for (size_t w = 0; w < words; w++) {
        unsigned long long next = in_set[w] | u[w];
        if (next != in_set[w]) {
          in_set[w] = next;
          changed = 1;
        }
      }
    }
  }

  if (precise_defs) {
    cfg->live_out =
        (unsigned long long *)calloc(nblocks * words, sizeof(*cfg->live_out));
    if (!cfg->live_out) {
      goto done;
    }
    for (size_t b = 0; b < nblocks; b++) {
      unsigned long long *out_set = cfg->live_out + b * words;
      for (int e = succ_head[b]; e >= 0; e = succ_next[e]) {
        const unsigned long long *s =
            cfg->live_in + (size_t)succ_block[e] * words;
        for (size_t w = 0; w < words; w++) {
          out_set[w] |= s[w];
        }
      }
    }

    cfg->block_start = block_start;
    block_start = NULL;
  }
  cfg->block_count = nblocks;
  ok = 1;

done:
  free(map.slots);
  free(leader);
  free(block_start);
  free(succ_head);
  free(succ_next);
  free(succ_block);
  free(use_set);
  free(def_set);
  free(killable);
  free(first_dst);
  free(first_any);
  if (!ok) {
    free(cfg->block_of);
    free(cfg->block_start);
    free(cfg->live_in);
    free(cfg->live_out);
    cfg->block_of = NULL;
    cfg->block_start = NULL;
    cfg->live_in = NULL;
    cfg->live_out = NULL;
    cfg->block_count = 0;
  }
  return ok;
}

static void mir_live_cfg_free(MirLiveCfg *cfg) {
  free(cfg->block_of);
  free(cfg->block_start);
  free(cfg->live_in);
  free(cfg->live_out);
  cfg->block_of = NULL;
  cfg->block_start = NULL;
  cfg->live_in = NULL;
  cfg->live_out = NULL;
  cfg->block_count = 0;
}

/* One back-edge's worth of interval extension: any vreg whose interval crosses
 * the [l,b] boundary must stay live across the whole loop. */
static void mir_extend_across_edge(MirFunction *fn, int l, int b,
                                   const unsigned long long *header_live,
                                   int *changed) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE) {
      continue;
    }
    if (header_live && !mir_live_bit_get(header_live, v)) {
      continue;
    }
    /* interval overlaps [l,b]? */
    if (vr->live_end < l || vr->live_start > b) {
      continue;
    }
    /* crosses a boundary (defined before l, or used after b)? An entry-live
     * vreg (param / hidden out-pointer) is defined by the prologue BEFORE
     * instruction 0, so when the loop header is at index 0 (tail-recursion
     * loops) it crosses even though live_start == l. */
    int crosses = (vr->live_start < l) || (vr->live_end > b) ||
                  (vr->entry_live && l == 0);
    if (!crosses) {
      continue;
    }
    vr->loop_carried = 1; /* reused across this loop's back-edge */
    if (vr->live_start > l) {
      vr->live_start = l;
      *changed = 1;
    }
    if (vr->live_end < b) {
      vr->live_end = b;
      *changed = 1;
    }
  }
}

static void mir_compute_liveness(MirFunction *fn) {
  for (size_t i = 0; i < fn->vreg_count; i++) {
    fn->vregs[i].live_start = MIR_LIVE_NONE;
    fn->vregs[i].live_end = MIR_LIVE_NONE;
    fn->vregs[i].loop_carried = 0;
    fn->vregs[i].entry_live = 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    mir_note_operand_liveness(fn, &in->dst, (int)i);
    mir_note_operand_liveness(fn, &in->a, (int)i);
    mir_note_operand_liveness(fn, &in->b, (int)i);
  }

  /* Parameters are defined by the prologue, before any MIR instruction, so they
   * are live from index 0. This MUST happen before the loop-extension below: a
   * param used only inside a loop would otherwise have an interval sitting
   * entirely within the loop and be (wrongly) judged not to cross the loop
   * boundary, so it would not be extended across the back-edge and could share
   * a register with a loop-body temp, clobbering the param every iteration. */
  for (size_t i = 0; i < fn->param_count; i++) {
    MirVreg *pv = &fn->vregs[fn->params[i].vreg];
    if (pv->live_end != MIR_LIVE_NONE) {
      pv->live_start = 0;
      pv->entry_live = 1;
    }
  }
  /* The hidden INDIRECT-return out-pointer is also defined by the prologue, so
   * it is live from entry to its last use (the struct copy at each RETURN). */
  if (fn->returns_indirect && fn->indirect_return_vreg != MIR_VREG_NONE) {
    MirVreg *rv = &fn->vregs[fn->indirect_return_vreg];
    if (rv->live_end != MIR_LIVE_NONE) {
      rv->live_start = 0;
      rv->entry_live = 1;
    }
  }

  /* Conservatively extend intervals across backward branches (loops) to a
   * fixpoint. For each branch at B targeting a label at L < B, any vreg whose
   * interval crosses the [L,B] boundary must stay live across the whole loop.
   *
   * The back-edge set never changes during the fixpoint, only the intervals
   * do, so it is collected once up front. Re-deriving it every pass (with
   * mir_find_label's linear scan per branch) made the fixpoint
   * O(passes x insns x (labels + vregs)), which dominated regalloc on large
   * straight-from-the-frontend functions. */
  MirBackEdge *edges = NULL;
  size_t edge_count = 0;
  int changed = 1;
  if (mir_collect_back_edges(fn, &edges, &edge_count)) {
    MirLiveCfg cfg;
    int have_cfg = edge_count > 0 && mir_live_cfg_build(fn, &cfg, 0);
    while (changed) {
      changed = 0;
      for (size_t e = 0; e < edge_count; e++) {
        const unsigned long long *header_live = NULL;
        if (have_cfg) {
          header_live =
              cfg.live_in + (size_t)cfg.block_of[edges[e].l] * cfg.words;
        }
        mir_extend_across_edge(fn, edges[e].l, edges[e].b, header_live,
                               &changed);
      }
    }
    if (have_cfg) {
      mir_live_cfg_free(&cfg);
    }
    free(edges);
    return;
  }

  /* Fallback: edge collection failed to allocate; derive edges per pass. */
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (!mir_inst_is_branch(in) || in->dst.kind != MIR_OPK_LABEL) {
        continue;
      }
      int l = mir_find_label(fn, in->dst.sym);
      int b = (int)i;
      if (l < 0 || l >= b) {
        continue; /* forward branch: no loop back-edge */
      }
      mir_extend_across_edge(fn, l, b, NULL, &changed);
    }
  }
}

/* Order vregs by ascending live_start for the scan. Returns a malloc'd array of
 * vreg ids (caller frees), or NULL on OOM / when there are no live vregs. */
static MirVregId *mir_order_by_start(MirFunction *fn, size_t *count_out) {
  size_t live = 0;
  for (size_t i = 0; i < fn->vreg_count; i++) {
    if (fn->vregs[i].live_start != MIR_LIVE_NONE) {
      live++;
    }
  }
  *count_out = live;
  if (live == 0) {
    return NULL;
  }
  MirVregId *order = (MirVregId *)malloc(live * sizeof(MirVregId));
  if (!order) {
    fn->has_error = 1;
    return NULL;
  }
  size_t n = 0;
  for (size_t i = 0; i < fn->vreg_count; i++) {
    if (fn->vregs[i].live_start != MIR_LIVE_NONE) {
      order[n++] = (MirVregId)i;
    }
  }
  /* insertion sort by (live_start, then id), vreg counts are small. */
  for (size_t i = 1; i < live; i++) {
    MirVregId key = order[i];
    int ks = fn->vregs[key].live_start;
    size_t j = i;
    while (j > 0) {
      int prev_s = fn->vregs[order[j - 1]].live_start;
      if (prev_s < ks || (prev_s == ks && order[j - 1] <= key)) {
        break;
      }
      order[j] = order[j - 1];
      j--;
    }
    order[j] = key;
  }
  return order;
}

/* Compute two-address coalescing hints: for each commutative 2-address op whose
 * result is a GP vreg, if a source operand is a GP vreg that DIES at this op,
 * record it as the destination's coalesce hint. The allocator then tries to
 * place the destination in that dying source's register, after which the
 * encoder emits the op in place (no `mov dst, a` copy). SUB is non-commutative,
 * so only its minuend (a) qualifies; IMUL-by-immediate is 3-operand already. */
static void mir_compute_coalesce_hints(MirFunction *fn) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    fn->vregs[v].coalesce_hint = MIR_VREG_NONE;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    int commutative;
    switch (in->op) {
    case MIR_ADD:
    case MIR_AND:
    case MIR_OR:
    case MIR_XOR:
    case MIR_IMUL:
    /* Float ops are two-address too (e.g. addsd D,b computes D=D+b), so the
     * encoder copies operand a into D unless the allocator already placed a (or,
     * for commutative ops, b) there. Hinting the dying source elides that
     * per-op movaps -- the dominant overhead in tight scalar-float loops. */
    case MIR_FADD:
    case MIR_FMUL:
    case MIR_FXOR:
      commutative = 1;
      break;
    case MIR_SUB:
    case MIR_FSUB:
    case MIR_FDIV:
    /* NEG/NOT are one-source two-address ops (`neg D` computes D = -D), so the
     * encoder copies the source into the destination unless the allocator put
     * it there already -- the same copy the arithmetic cases above elide. */
    case MIR_NEG:
    case MIR_NOT:
    /* A shift by a constant is two-address the same way (`shr D, 1` computes
     * D = D >> 1). Only the shifted value is a candidate, never the count --
     * which the `commutative = 0` below already ensures, since the count is
     * operand b. A VARIABLE shift stages through a fixed scratch register no
     * matter where its input lives, so hinting it would bias the allocator for
     * nothing; those are filtered out below. */
    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR:
    /* A plain register copy `dst <- a` is the most basic coalescing target: if a
     * dies at the copy, dst and a never overlap, so they can share a register and
     * the move disappears entirely (store_from/materialize elide a `mov R,R`).
     * This removes loop-carried rotation copies (e.g. an unrolled `a=b; b=next`)
     * that the per-op ALU/float coalescing above never sees. Loads/stores/
     * immediates (a not a vreg, or dst a memory store) fall out via the vreg/
     * same-class checks below. */
    case MIR_MOV:
      commutative = 0;
      break;
    default:
      continue;
    }
    if (in->dst.kind != MIR_OPK_VREG) {
      continue;
    }
    MirRegClass dcls = fn->vregs[in->dst.vreg].rclass;
    if (in->op == MIR_IMUL && in->b.kind == MIR_OPK_IMM) {
      continue; /* imul r, a, imm32 needs no copy */
    }
    if ((in->op == MIR_SHL || in->op == MIR_SHR || in->op == MIR_SAR) &&
        in->b.kind != MIR_OPK_IMM) {
      continue; /* variable shift: the value goes through a scratch anyway */
    }
    MirVregId d = in->dst.vreg;
    MirVregId cand = MIR_VREG_NONE;
    /* Prefer the left operand when it dies here, even for commutative ops. The
     * frontend naturally lowers accumulator chains as left-associated adds
     * (`acc + t1`, then previous + `t2`); preserving that left register carries
     * the running value across fixed-register ops like MULHI and avoids spilling
     * the partial sum. If a is not a candidate, fall back to b. */
    if (in->a.kind == MIR_OPK_VREG && in->a.vreg != d &&
        fn->vregs[in->a.vreg].rclass == dcls &&
        fn->vregs[in->a.vreg].live_end == (int)i) {
      cand = in->a.vreg;
    } else if (commutative && in->b.kind == MIR_OPK_VREG && in->b.vreg != d &&
               fn->vregs[in->b.vreg].rclass == dcls &&
               fn->vregs[in->b.vreg].live_end == (int)i) {
      cand = in->b.vreg;
    }
    fn->vregs[d].coalesce_hint = cand;
  }
}

/* True if `reg` is unavailable to a value whose interval is (s, e) -- because an
 * instruction strictly inside it needs that register to carry a FIXED value.
 * Two ways that happens:
 *
 *  - The register is named explicitly as an instruction's destination or source
 *    (MIR_OPK_PHYS). A fixed destination would overwrite the value; a fixed
 *    source needs the register to hold something else at that point, so a value
 *    parked there has displaced it.
 *  - RAX/RCX/RDX additionally carry IMPLICIT clobbers: the divide family
 *    (IDIV/DIV/MULHI) writes RAX:RDX, setcc writes RAX, and a variable-count
 *    shift routes its count through CL (RCX).
 *
 * Boundary instructions (k == s or k == e) are the value's own def/last-use as a
 * div/shift operand or result, which the encoder places correctly, so they are
 * not conflicts. Calls are handled separately by crosses_call (a value spanning
 * a call is barred from all volatiles, including these three). */
/* Positions of every clobber event in one function, sorted ascending, so a
 * clobbered-in-range query is two binary searches instead of a scan of the
 * interval. mir_color_reg_mask asks this question for every register of the
 * pool for every vreg; on a function large enough (a frontend can emit a
 * module initializer with 10^6 instructions) the interval scans made regalloc
 * quadratic. Cached per function, keyed the way g_binary_ir_function_index
 * keys its cache; rebuilt in one pass whenever the function changes. */
typedef struct {
  int *pos;
  size_t count;
  size_t cap;
} MirClobberList;

typedef struct {
  const MirFunction *fn;
  const MirInst *insns;
  size_t insn_count;
  int valid; /* 0 after an allocation failure: callers use the linear scan */
  /* Per physical GP register: every index where that register carries a FIXED
   * value, as either the destination or a source of the instruction. Both
   * directions bar the same thing -- a vreg assigned that register whose live
   * range strictly spans the index. A fixed WRITE would overwrite the vreg; a
   * fixed READ means the register is holding the vreg instead of the value the
   * instruction needs, which is how the divide's remainder used to be lost (see
   * mir_clobber_index_ensure). */
  MirClobberList explicit_fixed[16];
  MirClobberList rax_implicit;
  MirClobberList rcx_implicit;
  MirClobberList rdx_implicit;
} MirClobberIndex;

static MirClobberIndex g_mir_clobber_index = {0};

static void mir_clobber_list_free(MirClobberList *l) {
  free(l->pos);
  l->pos = NULL;
  l->count = 0;
  l->cap = 0;
}

static int mir_clobber_list_push(MirClobberList *l, int k) {
  if (l->count == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 16;
    int *pos = realloc(l->pos, cap * sizeof(*pos));
    if (!pos) {
      return 0;
    }
    l->pos = pos;
    l->cap = cap;
  }
  l->pos[l->count++] = k; /* k only grows across the build pass: sorted */
  return 1;
}

/* Any position strictly inside (s, e)? */
static int mir_clobber_list_hit(const MirClobberList *l, int s, int e) {
  size_t lo = 0;
  size_t hi = l->count;
  while (lo < hi) { /* first position > s */
    size_t mid = lo + (hi - lo) / 2;
    if (l->pos[mid] <= s) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < l->count && l->pos[lo] < e;
}

static void mir_clobber_index_reset(void) {
  for (size_t r = 0; r < 16; r++) {
    mir_clobber_list_free(&g_mir_clobber_index.explicit_fixed[r]);
  }
  mir_clobber_list_free(&g_mir_clobber_index.rax_implicit);
  mir_clobber_list_free(&g_mir_clobber_index.rcx_implicit);
  mir_clobber_list_free(&g_mir_clobber_index.rdx_implicit);
  g_mir_clobber_index.fn = NULL;
  g_mir_clobber_index.insns = NULL;
  g_mir_clobber_index.insn_count = 0;
  g_mir_clobber_index.valid = 0;
}

static int mir_clobber_index_ensure(const MirFunction *fn) {
  MirClobberIndex *ix = &g_mir_clobber_index;
  if (ix->fn == fn && ix->insns == fn->insns &&
      ix->insn_count == fn->insn_count) {
    return ix->valid;
  }

  mir_clobber_index_reset();
  ix->fn = fn;
  ix->insns = fn->insns;
  ix->insn_count = fn->insn_count;
  ix->valid = 1;

  for (size_t k = 0; k < fn->insn_count; k++) {
    const MirInst *in = &fn->insns[k];
    int ok = 1;
    /* Destination AND sources. Recording only destinations left a fixed-register
     * READ invisible, and the divmod fusion depends on one: the divide leaves
     * the quotient in RAX and the remainder in RDX, and the very next
     * instruction is `mov <vreg>, phys(RDX)` to capture the remainder. Nothing
     * stopped the allocator giving the QUOTIENT vreg RDX, whereupon the
     * encoder's `mov rdx, rax` overwrote the remainder before it was read and
     * `x % d` quietly evaluated to `x / d`. */
    const MirOperand *fixed[3] = {&in->dst, &in->a, &in->b};
    for (int f = 0; ok && f < 3; f++) {
      if (fixed[f]->kind == MIR_OPK_PHYS && fixed[f]->rclass == MIR_RC_GP &&
          fixed[f]->phys >= 0 && fixed[f]->phys < 16) {
        ok = mir_clobber_list_push(&ix->explicit_fixed[fixed[f]->phys], (int)k);
      }
    }
    if (ok) {
      switch (in->op) {
      case MIR_IDIV:
      case MIR_DIV:
      case MIR_MULHI:
        ok = mir_clobber_list_push(&ix->rax_implicit, (int)k) &&
             mir_clobber_list_push(&ix->rdx_implicit, (int)k);
        break;
      case MIR_CQO:
      case MIR_XOR_RDX:
        ok = mir_clobber_list_push(&ix->rdx_implicit, (int)k);
        break;
      case MIR_SETCC:
      case MIR_FSETCC:
        ok = mir_clobber_list_push(&ix->rax_implicit, (int)k);
        if (ok && in->op == MIR_FSETCC &&
            mir_fsetcc_unordered_cc(in->cc) >= 0) {
          ok = mir_clobber_list_push(&ix->rcx_implicit, (int)k);
        }
        break;
      case MIR_SHL:
      case MIR_SHR:
      case MIR_SAR:
        if (in->b.kind != MIR_OPK_IMM) {
          ok = mir_clobber_list_push(&ix->rcx_implicit, (int)k);
        }
        break;
      case MIR_IR_KERNEL: {
        /* Caller-saved registers are already barred across a kernel by
         * crosses_call. What that does not cover is a kernel writing a
         * CALLEE-saved register without preserving it, which the allocator would
         * otherwise consider a safe home for a value spanning the kernel. Each
         * such register is declared by the kernel's table row; record it as an
         * explicit clobber here so an interval containing the kernel avoids
         * it. */
        const MirKernelAux *ka = (const MirKernelAux *)in->aux;
        const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
        unsigned clobbers = kern ? kern->gp_clobbers : 0u;
        for (int r = 0; clobbers && ok; r++, clobbers >>= 1) {
          if (clobbers & 1u) {
            ok = mir_clobber_list_push(&ix->explicit_fixed[r], (int)k);
          }
        }
        break;
      }
      case MIR_INLINE_ASM:
        for (int r = 0; ok && r < 16; r++) {
          ok = mir_clobber_list_push(&ix->explicit_fixed[r], (int)k);
        }
        break;
      default:
        break;
      }
    }
    if (!ok) {
      mir_clobber_index_reset();
      ix->fn = fn;
      ix->insns = fn->insns;
      ix->insn_count = fn->insn_count;
      ix->valid = 0; /* remember the failure; do not rebuild per query */
      return 0;
    }
  }

  return 1;
}

static int mir_reg_clobbered_in_range(const MirFunction *fn,
                                      BinaryGpRegister reg, int s, int e) {
  int constrained = (reg == BINARY_GP_RAX || reg == BINARY_GP_RCX ||
                     reg == BINARY_GP_RDX);

  if ((int)reg >= 0 && (int)reg < 16 && mir_clobber_index_ensure(fn)) {
    const MirClobberIndex *ix = &g_mir_clobber_index;
    if (mir_clobber_list_hit(&ix->explicit_fixed[reg], s, e)) {
      return 1;
    }
    if (!constrained) {
      return 0;
    }
    if (reg == BINARY_GP_RAX) {
      return mir_clobber_list_hit(&ix->rax_implicit, s, e);
    }
    if (reg == BINARY_GP_RCX) {
      return mir_clobber_list_hit(&ix->rcx_implicit, s, e);
    }
    return mir_clobber_list_hit(&ix->rdx_implicit, s, e);
  }

  /* Fallback: index unavailable; scan the interval as before. */
  for (int k = s + 1; k < e; k++) {
    const MirInst *in = &fn->insns[k];
    /* This physical register carrying a fixed value here, as the destination
     * (return value into RAX, ABI argument setup, hidden-return pointer, ...)
     * or as a source (capturing the divide's remainder out of RDX). Either way
     * a vreg cannot also be living in it across this point. Mirrors the index
     * build above. */
    const MirOperand *fixed[3] = {&in->dst, &in->a, &in->b};
    for (int f = 0; f < 3; f++) {
      if (fixed[f]->kind == MIR_OPK_PHYS && fixed[f]->rclass == MIR_RC_GP &&
          fixed[f]->phys == (int)reg) {
        return 1;
      }
    }
    /* Callee-saved registers an inline kernel writes without preserving (see
     * the index build above); unlike the cases below these are not limited to
     * RAX/RCX/RDX, so they are checked before the `constrained` filter. */
    if (in->op == MIR_IR_KERNEL && (int)reg >= 0 && (int)reg < 16) {
      const MirKernelAux *ka = (const MirKernelAux *)in->aux;
      const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
      if (kern && (kern->gp_clobbers & (1u << (unsigned)reg))) {
        return 1;
      }
    }
    if (in->op == MIR_INLINE_ASM) {
      return 1;
    }
    if (!constrained) {
      continue;
    }
    switch (in->op) {
    case MIR_IDIV:
    case MIR_DIV:
    case MIR_MULHI:
      if (reg == BINARY_GP_RAX || reg == BINARY_GP_RDX) {
        return 1;
      }
      break;
    case MIR_CQO:
    case MIR_XOR_RDX:
      if (reg == BINARY_GP_RDX) {
        return 1;
      }
      break;
    case MIR_SETCC:
    case MIR_FSETCC:
      if (reg == BINARY_GP_RAX) {
        return 1;
      }
      if (in->op == MIR_FSETCC && reg == BINARY_GP_RCX &&
          mir_fsetcc_unordered_cc(in->cc) >= 0) {
        return 1;
      }
      break;
    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR:
      if (reg == BINARY_GP_RCX && in->b.kind != MIR_OPK_IMM) {
        return 1;
      }
      break;
    default:
      break;
    }
  }
  return 0;
}

/* ---- graph-coloring allocator (Chaitin-Briggs, optimistic) -----------------
 *
 * A second, higher-quality allocator that replaces the greedy linear scan's
 * local decisions with a global view: it builds the interference graph, picks
 * spill victims by a dynamic cost model (use density x loop weight) rather than
 * "farthest live_end", and biases copy-related values onto the same register so
 * the encoder elides the move. It reuses every correctness primitive the linear
 * scan established -- the same liveness, the same crosses_call / clobber-range /
 * address-taken / ABI-pool constraints -- so it can only differ in QUALITY, not
 * legality. Two simplifications make it a single pass with no spill-rewrite
 * loop: (1) a "spilled" vreg is simply memory-resident (the encoder loads/stores
 * it per access, exactly like an address-taken local), so a node that fails to
 * color is just marked in_register=0; (2) interference uses STRICT interval
 * overlap (`a.start < b.end && b.start < a.end`), which models "an instruction
 * reads its sources before writing its dest", so a value and the result that
 * consumes-and-overwrites it at the same point do NOT interfere -- preserving
 * the two-address sharing the linear scan got from its coalesce hint, now for
 * any copy, not just the def-point one. The SELECT phase assigns only a color
 * absent from every interfering neighbour and present in the node's allowed-
 * register mask, so the result is always a legal allocation. */

/* Allowed physical registers for `v`, as a bitmask over phys 0..15: the ABI
 * pool for its class/cross-call status, minus any register clobbered somewhere
 * inside its live range. Empty when the value must be memory-resident. */
static uint32_t mir_color_reg_mask(const MirFunction *fn, MirVregId v,
                                   const BinaryGpRegister *gp_leaf_pool,
                                   size_t gp_leaf_n,
                                   const BinaryGpRegister *gp_cross_pool,
                                   size_t gp_cross_n, int allow_rbp) {
  const MirVreg *vr = &fn->vregs[v];
  uint32_t m = 0;
  BinaryGpRegister cross_ext[MIR_GP_CROSSCALL_POOL_EXT];
  if (vr->rclass == MIR_RC_GP) {
    if (vr->crosses_call) {
      gp_cross_n = mir_cross_pool_for(vr, gp_cross_pool, gp_cross_n, cross_ext);
      gp_cross_pool = cross_ext;
    }
    const BinaryGpRegister *pool =
        vr->crosses_call ? gp_cross_pool : gp_leaf_pool;
    size_t n = vr->crosses_call ? gp_cross_n : gp_leaf_n;
    /* The prologue homes each parameter out of its incoming argument register.
     * Those moves happen in sequence, so a parameter whose home IS an argument
     * register another parameter has not been read out of yet would clobber it.
     * Barring the entry-live values -- the parameters and the hidden indirect-
     * return pointer -- from every argument register the function actually
     * receives in removes that hazard at its source. Values defined later are
     * free to use those registers: by then homing is finished. */
    size_t incoming = fn->incoming_arg_slots
                          ? fn->incoming_arg_slots
                          : fn->param_count + (fn->returns_indirect ? 1 : 0);
    for (size_t i = 0; i < n; i++) {
      BinaryGpRegister reg = pool[i];
      if (fn->reserve_rbx && reg == BINARY_GP_RBX) {
        continue;
      }
      if (vr->entry_live) {
        int ai = mir_reg_arg_index(reg);
        if (ai >= 0 && (size_t)ai < incoming) {
          continue;
        }
      }
      if (!mir_reg_clobbered_in_range(fn, reg, vr->live_start, vr->live_end)) {
        m |= 1u << reg;
      }
    }
    /* With the frame pointer omitted, rbp is a free callee-saved register,
     * usable for cross-call and leaf values alike, provided nothing writes it
     * in the live range. This is the FPO payoff: an extra register that removes
     * a spill (and, in call-heavy code, the rsp-relative stack access that would
     * otherwise force a stack-engine sync uop). */
    if (allow_rbp &&
        !mir_reg_clobbered_in_range(fn, BINARY_GP_RBP, vr->live_start,
                                    vr->live_end)) {
      m |= 1u << BINARY_GP_RBP;
    }
  } else if (vr->rclass == MIR_RC_XMM &&
             (!vr->crosses_call || vr->crosses_xmm_preserving_only)) {
    /* Volatile xmm0-3 then callee-saved xmm8-15. A cross-call XMM ordinarily
     * has no register that survives the call on BOTH ABIs, so it stays memory
     * (m=0) -- unless every call it spans is a preserving one, which saves and
     * restores the volatile lanes along with RAX. That is what keeps the
     * accumulator of a checked float reduction in a register instead of loading
     * and storing it once per element (`matvec`).
     * When the function homes a float argument into an XMM register (xmm0-3),
     * those volatile lanes are excluded from the pool, exactly as the GP arg
     * registers always are, so no allocated value sits in an outgoing argument
     * register and the pre-call homing moves cannot clobber one another. */
    if (!fn->has_xmm_arg_call) {
      for (size_t i = 0; i < MIR_XMM_POOL_COUNT; i++) {
        m |= 1u << MIR_XMM_POOL[i];
      }
    }
    /* xmm8-15 only where no call is in range at all: they are callee-saved on
     * Win64 but caller-saved on SysV, and the preserving call saves the four
     * lanes above, not these. */
    if (!vr->crosses_call) {
      for (size_t i = 0; i < MIR_XMM_NONVOL_POOL_COUNT; i++) {
        if (mir_xmm_is_encoder_scratch(MIR_XMM_NONVOL_POOL[i])) {
          continue;
        }
        m |= 1u << MIR_XMM_NONVOL_POOL[i];
      }
    }
  }
  return m;
}

/* STRICT live-interval overlap (see header): touching at a single point is NOT
 * overlap, so a dying source and the result that overwrites it can share a
 * register. */
static int mir_color_interferes(const MirVreg *a, const MirVreg *b) {
  if (a->rclass != b->rclass) {
    return 0;
  }
  /* Two prologue-defined values (parameters / hidden return pointer) are both
   * live from entry, so they always interfere -- even when each is used at only
   * a single shared instruction index, where the strict-overlap test below would
   * (wrongly) judge their point intervals disjoint and let them share a register.
   * This only ADDS edges that genuinely exist; a non-degenerate param pair
   * already interferes via the interval test. */
  if (a->entry_live && b->entry_live) {
    return 1;
  }
  return a->live_start < b->live_end && b->live_start < a->live_end;
}

/* The Chaitin-Briggs core. Returns 1 on success (every vreg has assigned set,
 * to a register or a fresh stack slot), 0 on OOM. `*next_spill` is advanced for
 * each value that ends up memory-resident. */
/* For each vreg, the vreg it is narrowed from by a MOVZX/MOVSX, or
 * MIR_VREG_NONE. Colouring the two alike makes the extend a same-register
 * `mov r32, r32`, which -- unlike the cross-register form -- the hardware
 * cannot rename away, so it costs a full cycle on the dependence chain. Both
 * allocators consult this to prefer a different register. One pass over the
 * instructions; NULL on OOM, which callers treat as "no preference". */
static MirVregId *mir_build_narrowing_extend_map(const MirFunction *fn) {
  if (fn->vreg_count == 0) {
    return NULL;
  }
  MirVregId *map = (MirVregId *)malloc(fn->vreg_count * sizeof(MirVregId));
  if (!map) {
    return NULL;
  }
  for (size_t v = 0; v < fn->vreg_count; v++) {
    map[v] = MIR_VREG_NONE;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if ((in->op != MIR_MOVZX && in->op != MIR_MOVSX) || in->width >= 8 ||
        in->dst.kind != MIR_OPK_VREG || in->a.kind != MIR_OPK_VREG ||
        in->dst.vreg == in->a.vreg) {
      continue;
    }
    map[in->dst.vreg] = in->a.vreg;
  }
  return map;
}

/* The physical register a narrowing extend's destination should avoid, or -1. */
static int mir_narrowing_avoid_reg(const MirFunction *fn, const MirVregId *map,
                                   MirVregId v) {
  if (!map || map[v] == MIR_VREG_NONE) {
    return -1;
  }
  const MirVreg *sv = &fn->vregs[map[v]];
  return sv->in_register ? (int)sv->phys : -1;
}

#define MIR_MAX_WEIGHTED_DEPTH 3
#define MIR_MAX_SPILL_COST (1 << 24)

static unsigned char *mir_build_loop_depths(const MirFunction *fn) {
  MirBackEdge *edges = NULL;
  size_t edge_count = 0;
  unsigned char *depth;
  if (fn->insn_count == 0) {
    return NULL;
  }
  depth = (unsigned char *)calloc(fn->insn_count, sizeof(*depth));
  if (!depth) {
    return NULL;
  }
  if (!mir_collect_back_edges(fn, &edges, &edge_count) || edge_count == 0) {
    free(edges);
    return depth;
  }
  for (size_t e = 0; e < edge_count; e++) {
    int l = edges[e].l;
    int b = edges[e].b;
    int seen = 0;
    if (l < 0 || b < l || (size_t)b >= fn->insn_count) {
      continue;
    }
    for (size_t k = 0; k < e; k++) {
      if (edges[k].l == l) {
        seen = 1;
        break;
      }
    }
    if (seen) {
      continue;
    }
    for (size_t k = e + 1; k < edge_count; k++) {
      if (edges[k].l == l && edges[k].b > b && (size_t)edges[k].b < fn->insn_count) {
        b = edges[k].b;
      }
    }
    for (int i = l; i <= b; i++) {
      if (depth[i] < MIR_MAX_WEIGHTED_DEPTH) {
        depth[i]++;
      }
    }
  }
  free(edges);
  return depth;
}

static int mir_scale_cost(int cost, int factor) {
  long long scaled = (long long)cost * factor;
  return scaled > MIR_MAX_SPILL_COST ? MIR_MAX_SPILL_COST : (int)scaled;
}

static void mir_note_operand_depth(const MirOperand *op, unsigned char depth,
                                   unsigned char *use_depth, size_t n) {
  MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
  if (op->kind == MIR_OPK_VREG) {
    ids[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    ids[0] = op->mem.base;
    ids[1] = op->mem.index;
  }
  for (int j = 0; j < 2; j++) {
    if (ids[j] >= 0 && (size_t)ids[j] < n && use_depth[ids[j]] < depth) {
      use_depth[ids[j]] = depth;
    }
  }
}

static int mir_spill_rank(const MirFunction *fn, const unsigned char *use_depth,
                          MirVregId v) {
  (void)fn;
  return (int)use_depth[v];
}

const char *g_mir_ra_trace_name = NULL;

static int mir_env_regalloc_trace(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_REGALLOC_TRACE") ? 1 : 0;
  }
  return cached;
}

static const char *mir_ra_trace_name(void) {
  return g_mir_ra_trace_name ? g_mir_ra_trace_name : "?";
}

static int mir_color_graph(MirFunction *fn, const BinaryGpRegister *gp_leaf_pool,
                           size_t gp_leaf_n,
                           const BinaryGpRegister *gp_cross_pool,
                           size_t gp_cross_n, int *next_spill, int allow_rbp) {
  size_t N = fn->vreg_count;
  if (N == 0) {
    return 1;
  }
  size_t words = (N + 63) / 64;
  uint64_t *inter = (uint64_t *)calloc(N * words, sizeof(uint64_t));
  uint32_t *mask = (uint32_t *)calloc(N, sizeof(uint32_t));
  int *degree = (int *)calloc(N, sizeof(int));
  int *cost = (int *)calloc(N, sizeof(int));
  int *colorable = (int *)calloc(N, sizeof(int));
  int *removed = (int *)calloc(N, sizeof(int));
  /* Register count per vreg: popcount(mask[v]). mask never changes once built,
   * and the simplify loop tests this once per node per round, so computing it
   * here takes the popcount out of an O(N^2) path. */
  int *reg_count = (int *)calloc(N, sizeof(int));
  /* Spill-preference metric, cost per unit of relief. Both selection scans below
   * read it for every remaining node every round -- O(N^2) reads -- while it only
   * changes when a node's degree does, so it is cached and refreshed at the point
   * of change. That keeps the 64-bit divide off the scan, and the cached value is
   * the same truncated integer the scan used to compute inline, so the choices
   * (including ties) are unchanged. */
  long long *metric = (long long *)calloc(N, sizeof(long long));
  MirVregId *stack = (MirVregId *)malloc(N * sizeof(MirVregId));
  MirVregId *narrow_src = mir_build_narrowing_extend_map(fn);
  if (!inter || !mask || !degree || !cost || !colorable || !removed ||
      !reg_count || !metric || !stack) {
    free(inter); free(mask); free(degree); free(cost); free(colorable);
    free(removed); free(reg_count); free(metric); free(stack);
    free(narrow_src);
    return 0;
  }
#define MIR_METRIC(v) ((long long)cost[v] * 1000 / (degree[v] + 1))
#define MIR_INTER_SET(a, b)                                                    \
  (inter[(size_t)(a) * words + ((size_t)(b) >> 6)] |= (uint64_t)1                \
                                                       << ((size_t)(b) & 63))
#define MIR_INTER_GET(a, b)                                                    \
  ((inter[(size_t)(a) * words + ((size_t)(b) >> 6)] >>                          \
    ((size_t)(b) & 63)) & 1u)
#define MIR_INTER_ADD(a, b)                                                     \
  do {                                                                          \
    if ((a) != (b) && !MIR_INTER_GET((a), (b))) {                               \
      MIR_INTER_SET((a), (b));                                                  \
      MIR_INTER_SET((b), (a));                                                  \
      degree[(a)]++;                                                            \
      degree[(b)]++;                                                            \
    }                                                                           \
  } while (0)
/* Walk vreg `a`'s neighbours. Popping set bits word by word skips the runs of
 * non-neighbours that testing 0..N one index at a time would visit, which is
 * what the three O(N) neighbour scans below used to spend their time on. Only
 * colourable vregs ever get an edge, so no colorable[] re-check is needed. */
#define MIR_INTER_FOR_EACH(a, bvar)                                            \
  for (size_t w_ = 0; w_ < words; w_++)                                        \
    for (uint64_t bits_ = inter[(size_t)(a) * words + w_], bvar;               \
         bits_ && ((bvar = w_ * 64 + (size_t)__builtin_ctzll(bits_)), 1);       \
         bits_ &= bits_ - 1)

  /* Colorable set: live, not address-taken (those are already memory-resident).
   * Mask + per-operand access count (the spill cost, weighted up for loop-
   * carried values so the recurrence's hot registers are kept). */
  for (size_t v = 0; v < N; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE || vr->address_taken) {
      continue;
    }
    colorable[v] = 1;
    mask[v] = mir_color_reg_mask(fn, (MirVregId)v, gp_leaf_pool, gp_leaf_n,
                                 gp_cross_pool, gp_cross_n, allow_rbp);
    reg_count[v] = __builtin_popcount(mask[v]);
    cost[v] = 1;
  }
  unsigned char *loop_depth = mir_build_loop_depths(fn);
  unsigned char *use_depth = (unsigned char *)calloc(N, sizeof(*use_depth));
  if (!use_depth) {
    free(loop_depth);
    free(inter); free(mask); free(degree); free(cost); free(colorable);
    free(removed); free(reg_count); free(metric); free(stack);
    free(narrow_src);
    return 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    const MirOperand *ops[3] = {&in->dst, &in->a, &in->b};
    unsigned char d = loop_depth ? loop_depth[i] : 0;
    int weight = 1;
    for (int k = 0; k < d; k++) {
      weight *= 10;
    }
    for (int k = 0; k < 3; k++) {
      const MirOperand *op = ops[k];
      MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
      mir_note_operand_depth(op, d, use_depth, N);
      if (op->kind == MIR_OPK_VREG) {
        ids[0] = op->vreg;
      } else if (op->kind == MIR_OPK_MEM) {
        ids[0] = op->mem.base;
        ids[1] = op->mem.index;
      }
      for (int j = 0; j < 2; j++) {
        MirVregId id = ids[j];
        if (id >= 0 && (size_t)id < N && colorable[id]) {
          cost[id] = mir_scale_cost(cost[id] + weight, 1);
        }
      }
    }
  }
  free(loop_depth);
  for (size_t i = 0; i < fn->iconst_count; i++) {
    MirVregId v = fn->iconsts[i].vreg;
    if (v >= 0 && (size_t)v < N && colorable[v]) {
      cost[v] = mir_scale_cost(cost[v], 64);
    }
  }
  for (size_t i = 0; i < fn->fconst_count; i++) {
    MirVregId v = fn->fconsts[i].vreg;
    if (v >= 0 && (size_t)v < N && colorable[v]) {
      cost[v] = mir_scale_cost(cost[v], 32);
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LEA_FUNC && in->dst.kind == MIR_OPK_VREG) {
      MirVregId v = in->dst.vreg;
      if (v >= 0 && (size_t)v < N && colorable[v]) {
        cost[v] = mir_scale_cost(cost[v], 128);
      }
    }
  }

  /* Build interference from the control-flow liveness sets. The old interval
   * graph made every value between its first and last textual use overlap. In a
   * branch-heavy function that joined values from mutually exclusive arms into
   * one large clique, even though no execution can keep those values live at the
   * same time. Walking each block backwards adds the standard def-versus-live
   * edges and keeps those arms separate. If the bounded CFG analysis cannot run,
   * retain the interval graph as the conservative fallback. */
  {
    MirLiveCfg cfg;
    /* The exact walk adds one dataflow solve and one interference matrix to
     * what the interval graph already pays, so it is bounded in the same
     * currency mir_live_cfg_build refuses past. A vreg count in place of that
     * work bound cut off application-sized functions: a 560-vreg main is
     * ordinary, and the interval graph spilled 99 of its values where the
     * exact graph spills 3. */
    size_t branch_count = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      if (mir_inst_ends_block(&fn->insns[i])) {
        branch_count++;
      }
    }
    static int interval_only = -1;
    if (interval_only < 0) {
      interval_only = getenv("METTLE_INTERVAL_INTERFERENCE") ? 1 : 0;
    }
    int use_cfg = !interval_only && fn->insn_count >= 8 && branch_count >= 1 &&
                  (unsigned long long)fn->insn_count *
                          (unsigned long long)words <=
                      MIR_LIVE_CFG_MAX_WORK;
    if (mir_env_regalloc_trace()) {
      fprintf(stderr, "RA\t%s\tvregs=%zu\tinsns=%zu\tblocks=%zu\tcfg=%d\n",
              mir_ra_trace_name(), N, fn->insn_count, branch_count, use_cfg);
    }
    int have_cfg = use_cfg && mir_live_cfg_build(fn, &cfg, 1);
    unsigned long long *live =
        have_cfg ? (unsigned long long *)malloc(words * sizeof(*live)) : NULL;
    int exact_graph = have_cfg && live;
    if (exact_graph) {
      for (size_t block = 0; block < cfg.block_count; block++) {
        size_t lo = (size_t)cfg.block_start[block];
        size_t hi = block + 1 < cfg.block_count
                        ? (size_t)cfg.block_start[block + 1]
                        : fn->insn_count;
        memcpy(live, cfg.live_out + block * words,
               words * sizeof(*live));
        for (size_t at = hi; at-- > lo;) {
          const MirInst *in = &fn->insns[at];
          if (in->dst.kind == MIR_OPK_VREG) {
            MirVregId d = in->dst.vreg;
            if (d >= 0 && (size_t)d < N) {
              if (colorable[d]) {
                for (size_t w = 0; w < words; w++) {
                  uint64_t bits = live[w];
                  while (bits) {
                    size_t v = w * 64 + (size_t)__builtin_ctzll(bits);
                    bits &= bits - 1;
                    if (v < N && colorable[v] &&
                        fn->vregs[d].rclass == fn->vregs[v].rclass) {
                      MIR_INTER_ADD((size_t)d, v);
                    }
                  }
                }
              }
              live[(size_t)d >> 6] &= ~(1ull << ((size_t)d & 63));
            }
          }
          mir_live_add_operand(fn, &in->dst, live, 1);
          mir_live_add_operand(fn, &in->a, live, 0);
          mir_live_add_operand(fn, &in->b, live, 0);
        }
      }

      /* Parameters, constants, and the hidden return pointer can be live before
       * the first MIR instruction. They need the pairwise edges that ordinary
       * definitions inside the stream create. */
      for (size_t a = 0; a < N; a++) {
        if (!colorable[a] || !mir_live_bit_get(cfg.live_in, a)) {
          continue;
        }
        for (size_t b = a + 1; b < N; b++) {
          if (colorable[b] && mir_live_bit_get(cfg.live_in, b) &&
              fn->vregs[a].rclass == fn->vregs[b].rclass) {
            MIR_INTER_ADD(a, b);
          }
        }
      }
    } else {
      for (size_t a = 0; a < N; a++) {
        if (!colorable[a]) {
          continue;
        }
        for (size_t b = a + 1; b < N; b++) {
          if (colorable[b] &&
              mir_color_interferes(&fn->vregs[a], &fn->vregs[b])) {
            MIR_INTER_ADD(a, b);
          }
        }
      }
    }
    int exact_pressure = 0;
    int interval_pressure = 0;
    if (exact_graph) {
      uint64_t *interval_inter =
          (uint64_t *)calloc(N * words, sizeof(*interval_inter));
      int *interval_degree = (int *)calloc(N, sizeof(*interval_degree));
      for (size_t a = 0; a < N; a++) {
        if (!colorable[a]) {
          continue;
        }
        if (degree[a] >= reg_count[a]) {
          exact_pressure++;
        }
        if (interval_inter && interval_degree) {
          for (size_t b = a + 1; b < N; b++) {
            if (colorable[b] &&
                mir_color_interferes(&fn->vregs[a], &fn->vregs[b])) {
              interval_inter[a * words + (b >> 6)] |= 1ull << (b & 63);
              interval_inter[b * words + (a >> 6)] |= 1ull << (a & 63);
              interval_degree[a]++;
              interval_degree[b]++;
            }
          }
        }
      }
      if (interval_inter && interval_degree) {
        for (size_t a = 0; a < N; a++) {
          if (colorable[a] && interval_degree[a] >= reg_count[a]) {
            interval_pressure++;
          }
        }
        /* Exact liveness changes allocation only when it relieves broad
         * pressure. A small edge change can reshuffle otherwise sound colors
         * without removing enough contention to pay for that disruption. One
         * full leaf register set is a stable, target-derived cutoff. */
        if (mir_env_regalloc_trace()) {
          fprintf(stderr, "RA-PRESSURE\t%s\tinterval=%d\texact=%d\n",
                  mir_ra_trace_name(), interval_pressure, exact_pressure);
        }
        if (interval_pressure - exact_pressure <
            (int)MIR_GP_LEAF_POOL_MAX) {
          free(inter);
          free(degree);
          inter = interval_inter;
          degree = interval_degree;
          interval_inter = NULL;
          interval_degree = NULL;
        }
      }
      free(interval_inter);
      free(interval_degree);
    }
    free(live);
    if (have_cfg) {
      mir_live_cfg_free(&cfg);
    }
  }

  /* Anti-affinity edge across a narrowing MOVZX/MOVSX. The two ends do NOT
   * overlap (the source dies at the extend), so nothing above makes them
   * interfere and they are free to share a register -- which is precisely the
   * placement to avoid. `mov r8d, r9d` is renamed away by the hardware and
   * costs nothing; `mov r8d, r8d`, the same instruction with both ends coloured
   * alike, cannot be (it zeroes the upper half in place) and so lands a full
   * cycle on the dependence chain. In a serial recurrence such as a
   * bit-at-a-time CRC, one of these sits on the loop-carried path per step and
   * that cycle is a quarter of the whole loop.
   *
   * An edge is the right mechanism because the SELECT order is arbitrary: a
   * one-sided preference only works when the source happens to be coloured
   * first. Recorded only when both ends are still register candidates, so the
   * pressure this adds is one extra neighbour on values that are already
   * short-lived. */
  if (narrow_src) {
    for (size_t v = 0; v < N; v++) {
      MirVregId s = narrow_src[v];
      if (!colorable[v] || s == MIR_VREG_NONE || (size_t)s >= N ||
          !colorable[s] || (size_t)s == v || MIR_INTER_GET(v, s)) {
        continue;
      }
      MIR_INTER_SET(v, s);
      MIR_INTER_SET(s, v);
      degree[v]++;
      degree[s]++;
    }
  }

  /* Simplify / optimistic-spill: repeatedly remove a node whose current degree
   * is below the number of registers it could take (trivially colorable),
   * pushing it on the stack; when none qualifies, optimistically remove the
   * node with the lowest spill cost per unit of relief (cost/degree). The order
   * only affects quality -- SELECT guarantees legality either way. */
  size_t sp = 0;
  size_t remaining = 0;
  for (size_t v = 0; v < N; v++) {
    if (colorable[v]) {
      remaining++;
      metric[v] = MIR_METRIC(v);
    }
  }
  while (remaining > 0) {
    MirVregId pick = MIR_VREG_NONE;
    long long best_simplify = -1;
    int best_simplify_rank = MIR_MAX_WEIGHTED_DEPTH + 1;
    for (size_t v = 0; v < N; v++) {
      if (colorable[v] && !removed[v] && degree[v] < reg_count[v]) {
        int rank = mir_spill_rank(fn, use_depth, (MirVregId)v);
        int better;
        if (pick == MIR_VREG_NONE) {
          better = 1;
        } else if (rank != best_simplify_rank) {
          better = (rank < best_simplify_rank);
        } else {
          better = (metric[v] < best_simplify);
        }
        if (better) {
          best_simplify = metric[v];
          best_simplify_rank = rank;
          pick = (MirVregId)v;
        }
      }
    }
    if (pick == MIR_VREG_NONE) {
      long long best = -1;
      int best_rank = MIR_MAX_WEIGHTED_DEPTH + 1;
      for (size_t v = 0; v < N; v++) {
        int rank;
        int better;
        if (!colorable[v] || removed[v]) {
          continue;
        }
        rank = mir_spill_rank(fn, use_depth, (MirVregId)v);
        if (pick == MIR_VREG_NONE) {
          better = 1;
        } else if (rank != best_rank) {
          better = (rank < best_rank);
        } else {
          better = (metric[v] < best);
        }
        if (better) {
          best = metric[v];
          best_rank = rank;
          pick = (MirVregId)v;
        }
      }
    }
    removed[pick] = 1;
    stack[sp++] = pick;
    remaining--;
    MIR_INTER_FOR_EACH(pick, b) {
      if (!removed[b]) {
        degree[b]--;
        metric[b] = MIR_METRIC(b);
      }
    }
  }

  /* SELECT: pop in reverse and assign a legal colour, or spill to memory. A
   * copy partner's colour (the coalesce hint, i.e. a dying two-address source)
   * is preferred when free, so the encoder's store_from elides the move. */
  while (sp > 0) {
    MirVregId v = stack[--sp];
    MirVreg *vr = &fn->vregs[v];
    uint32_t used = 0;
    MIR_INTER_FOR_EACH(v, b) {
      if (fn->vregs[b].in_register) {
        used |= 1u << fn->vregs[b].phys;
      }
    }
    uint32_t avail = mask[v] & ~used;
    if (avail == 0) {
      *next_spill += vr->width > 8 ? 16 : 8;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = *next_spill;
      continue;
    }
    /* Anti-preference for a narrowing extend's destination: keep it OFF its
     * source's register. `mov r8d, r9d` is renamed away by the hardware and
     * costs nothing, but `mov r8d, r8d` -- the same instruction with both
     * operands coloured alike -- is not eliminable (it has to zero the upper
     * half in place) and so lands a full cycle on the dependence chain. In a
     * serial recurrence like a bit-at-a-time CRC, where one of these sits on
     * the loop-carried path per step, that single cycle is ~25% of the loop.
     * Only a preference: if the source's register is the only one left, taking
     * it still beats spilling. */
    uint32_t preferred = avail;
    int avoid = mir_narrowing_avoid_reg(fn, narrow_src, v);
    if (avoid >= 0 && (preferred & ~(1u << (unsigned)avoid)) != 0) {
      preferred &= ~(1u << (unsigned)avoid);
    }
    int chosen = -1;
    /* Bias toward a copy partner's register to elide the move. */
    if (vr->coalesce_hint != MIR_VREG_NONE) {
      MirVreg *hv = &fn->vregs[vr->coalesce_hint];
      if (hv->in_register && (preferred & (1u << hv->phys))) {
        chosen = hv->phys;
      }
    }
    if (chosen < 0) {
      /* Otherwise the lowest-numbered available register: keeps volatile/low
       * regs busy first and is deterministic. */
      for (int r = 0; r < 16; r++) {
        if (preferred & (1u << r)) {
          chosen = r;
          break;
        }
      }
    }
    vr->assigned = 1;
    vr->in_register = 1;
    vr->phys = chosen;
  }

  /* Post-colouring coalescing: eliminate a register-to-register copy `dst <- src`
   * by giving dst the SAME register as src, when src dies at the copy, the two do
   * not interfere, and src's register is unused among dst's interfering
   * neighbours. The encoder then emits nothing for a `mov R,R`. This catches move
   * chains (e.g. an unrolled `a=b; b=next` rotation, or a value's last copy into a
   * loop-carried home) that the SELECT-time bias misses because the partner had
   * not been coloured yet. It only rewrites a register ASSIGNMENT -- both vregs
   * hold the same value at the copy point and the freed-register check preserves
   * graph legality -- so it can never change behaviour, only remove a move.
   * Iterated to a fixpoint so a recoloured dst can in turn feed the next copy. */
  int coalesced = 1;
  int coalesce_rounds = 0;
  while (coalesced && coalesce_rounds++ < 16) {
    coalesced = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (in->op != MIR_MOV || in->dst.kind != MIR_OPK_VREG ||
          in->a.kind != MIR_OPK_VREG) {
        continue;
      }
      MirVregId d = in->dst.vreg;
      MirVregId s = in->a.vreg;
      if (d < 0 || s < 0 || (size_t)d >= N || (size_t)s >= N || d == s ||
          !colorable[d] || !colorable[s]) {
        continue;
      }
      MirVreg *dv = &fn->vregs[d];
      MirVreg *sv = &fn->vregs[s];
      if (!dv->in_register || !sv->in_register || dv->rclass != sv->rclass ||
          dv->phys == sv->phys || sv->live_end != (int)i ||
          MIR_INTER_GET(d, s) || !(mask[d] & (1u << sv->phys))) {
        continue;
      }
      uint32_t used = 0;
      MIR_INTER_FOR_EACH(d, b) {
        if (fn->vregs[b].in_register) {
          used |= 1u << fn->vregs[b].phys;
        }
      }
      if (used & (1u << sv->phys)) {
        continue;
      }
      dv->phys = sv->phys;
      coalesced = 1;
    }
  }

#undef MIR_METRIC
#undef MIR_INTER_SET
#undef MIR_INTER_GET
#undef MIR_INTER_ADD
  free(inter); free(mask); free(degree); free(cost); free(colorable);
  free(removed); free(reg_count); free(metric); free(stack);
  free(narrow_src); free(use_depth);
  return 1;
}

/* Shared tail: tell the context which callee-saved GP/XMM registers the
 * allocation used so the prologue/epilogue preserve them. */
static int mir_regalloc_report_saved(MirFunction *fn) {
  if (!fn->context) {
    return 1;
  }
  int used_nonvol[16];
  memset(used_nonvol, 0, sizeof(used_nonvol));
  for (size_t i = 0; i < fn->vreg_count; i++) {
    MirVreg *vr = &fn->vregs[i];
    /* RBP is callee-saved on both ABIs but is excluded from the global Win64
     * nonvolatile classifier (it is normally the frame pointer). When the frame
     * pointer is omitted the allocator may place a value in it, and that value
     * must be preserved like any other callee-saved register -- otherwise the
     * caller's frame pointer is destroyed. */
    if (vr->in_register && vr->rclass == MIR_RC_GP &&
        (mir_gp_is_nonvolatile((BinaryGpRegister)vr->phys) ||
         vr->phys == BINARY_GP_RBP)) {
      used_nonvol[vr->phys] = 1;
    }
  }
  /* An inline kernel writing a callee-saved register is preserving nothing on
   * its own, so the function has to preserve it for its caller exactly as if
   * the allocator had placed a value there. Keeping a value OUT of that
   * register across the kernel is a separate matter, handled by the clobber
   * index -- prologue save/restore only covers entry and exit, not the middle
   * of the body. (The fallback emitter never hit this because its promoter
   * claims R12..R15 up front, so they are always in its saved set.) */
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_INLINE_ASM) {
      for (int reg = 0; reg < 16; reg++) {
        if (mir_gp_is_nonvolatile((BinaryGpRegister)reg)) {
          used_nonvol[reg] = 1;
        }
      }
      continue;
    }
    if (fn->insns[i].op != MIR_IR_KERNEL) {
      continue;
    }
    const MirKernelAux *ka = (const MirKernelAux *)fn->insns[i].aux;
    const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
    unsigned clobbers = kern ? kern->gp_clobbers : 0u;
    for (int reg = 0; clobbers; reg++, clobbers >>= 1) {
      if ((clobbers & 1u) && (mir_gp_is_nonvolatile((BinaryGpRegister)reg) ||
                              reg == BINARY_GP_RBP)) {
        used_nonvol[reg] = 1;
      }
    }
  }
  for (int reg = 0; reg < 16; reg++) {
    if (used_nonvol[reg] && !code_generator_binary_context_add_saved_register(
                                fn->context, (BinaryGpRegister)reg)) {
      return 0;
    }
  }
  int used_xmm[16];
  memset(used_xmm, 0, sizeof(used_xmm));
  for (size_t i = 0; i < fn->vreg_count; i++) {
    MirVreg *vr = &fn->vregs[i];
    if (vr->in_register && vr->rclass == MIR_RC_XMM && vr->phys >= 8) {
      used_xmm[vr->phys] = 1;
    }
  }
  for (int reg = 8; reg < 16; reg++) {
    if (used_xmm[reg] && !code_generator_binary_context_add_saved_xmm_register(
                             fn->context, (BinaryXmmRegister)reg)) {
      return 0;
    }
  }
  return 1;
}

/* Graph-coloring entry: shares the linear scan's setup (liveness, crosses_call,
 * address-taken homing, ABI pool) then colours. */
static int mir_regalloc_color(MirFunction *fn) {
  mir_compute_liveness(fn);
  mir_compute_coalesce_hints(fn);
  mir_mark_crosses_call(fn);

  int next_spill = fn->context ? fn->context->raw_frame_size : 0;
  fn->preserve_slot = 0;
  fn->preserve_xmm_slot = 0;
  if (mir_fn_has_preserving_call(fn, 0)) {
    next_spill += 8;
    fn->preserve_slot = next_spill;
  }
  if (mir_fn_has_preserving_call(fn, 1)) {
    next_spill += (int)MIR_XMM_POOL_COUNT * 8;
    fn->preserve_xmm_slot = next_spill;
  }
  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->address_taken) {
      int home = mir_home_bytes_for(vr, &next_spill);
      next_spill += home;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = next_spill;
    }
  }

  BinaryGpRegister gp_leaf_pool[MIR_GP_LEAF_POOL_MAX];
  /* An indirect (>8-byte) struct return passes a hidden out-pointer in the
   * first arg slot, shifting the real parameters up by one; count it so a
   * register still holding an incoming parameter is never reclaimed. */
  size_t gp_leaf_n = mir_build_gp_leaf_pool(
      gp_leaf_pool, fn->param_count + (fn->returns_indirect ? 1 : 0),
      !mir_fn_has_real_calls(fn));
  BinaryGpRegister gp_cross_pool[MIR_GP_CROSSCALL_POOL_MAX];
  size_t gp_cross_n = mir_build_gp_crosscall_pool(gp_cross_pool);

  int allow_rbp = fn->context && fn->context->omit_frame_pointer &&
                  !mir_fn_uses_slp(fn);
  if (!mir_color_graph(fn, gp_leaf_pool, gp_leaf_n, gp_cross_pool, gp_cross_n,
                       &next_spill, allow_rbp)) {
    fn->has_error = 1;
    return 0;
  }
  mir_drop_unused_preserves(fn);
  if (mir_env_regalloc_trace()) {
    size_t spilled = 0, kept = 0;
    for (size_t v = 0; v < fn->vreg_count; v++) {
      const MirVreg *vr = &fn->vregs[v];
      if (vr->live_start == MIR_LIVE_NONE || vr->address_taken) {
        continue;
      }
      if (vr->assigned && vr->in_register) {
        kept++;
      } else if (vr->assigned) {
        spilled++;
      }
    }
    fprintf(stderr, "RA-DONE\t%s\tkept=%zu\tspilled=%zu\n",
            mir_ra_trace_name(), kept, spilled);
  }
  fn->spill_bytes = next_spill - (fn->context ? fn->context->raw_frame_size : 0);
  if (!mir_regalloc_report_saved(fn)) {
    fn->has_error = 1;
    return 0;
  }
  return 1;
}

/* True if `op` solely produces its vreg dst with no other observable effect, so
 * the instruction is removable when that dst is never read. Excludes anything
 * that can fault or has a side effect a later instruction may depend on: loads
 * (handled at the call site via a MEM source check), stores, divides (trap on
 * zero), calls, branches, returns, traps, flag/compare producers consumed
 * elsewhere, and SIMD/SLP/vector-memory ops. Flags set by these ALU ops are
 * never relied upon (MIR reads flags only via explicit CMP/TEST/UCOMIS), so
 * dropping a dead one cannot change a branch outcome. */
static int mir_op_pure_def(MirOpcode op) {
  switch (op) {
  case MIR_MOV:
  case MIR_MOVZX:
  case MIR_MOVSX:
  case MIR_LEA:
  case MIR_LEA_LOCAL:
  case MIR_LEA_GLOBAL:
  case MIR_LEA_FUNC:
  case MIR_LEA_CSTR:
  case MIR_LEA_STRLIT:
  case MIR_ADD:
  case MIR_SUB:
  case MIR_AND:
  case MIR_OR:
  case MIR_XOR:
  case MIR_IMUL:
  case MIR_NEG:
  case MIR_NOT:
  case MIR_POPCNT:
  case MIR_SHL:
  case MIR_SHR:
  case MIR_SAR:
  case MIR_SETCC:
  case MIR_CMOVCC:
  case MIR_FADD:
  case MIR_FSUB:
  case MIR_FMUL:
  case MIR_FDIV:
  case MIR_FXOR:
  case MIR_FDUP:
  case MIR_FEXTHI:
  case MIR_CVTSI2F:
  case MIR_CVTF2SI:
  case MIR_CVTF2F:
  case MIR_MOVD_TO_XMM:
  case MIR_MOVD_TO_GP:
  case MIR_CVTPH2PS:
  case MIR_CVTPS2PH:
    return 1;
  default:
    return 0;
  }
}

static void mir_dce_add_read(MirVregId v, int *reads, size_t n) {
  if (v >= 0 && (size_t)v < n) {
    reads[v]++;
  }
}

/* Count the vregs READ by one operand: a plain vreg, or the base/index of a
 * memory address. (A vreg dst is a definition, never a read, MIR is
 * three-address: dst = a OP b.) */
static void mir_dce_count_operand(const MirOperand *op, int *reads, size_t n) {
  if (op->kind == MIR_OPK_VREG) {
    mir_dce_add_read(op->vreg, reads, n);
  } else if (op->kind == MIR_OPK_MEM) {
    mir_dce_add_read(op->mem.base, reads, n);
    mir_dce_add_read(op->mem.index, reads, n);
  }
}

/* Dead-code elimination: drop pure value-producing ops whose vreg dst is never
 * read. Iterates to a fixpoint, since removing one dead def can orphan the
 * sources that fed it. This cleans up dead register shuffles left behind by
 * IR-level constant folding -- e.g. a source-unrolled `next=a+b; a=b; b=next`
 * whose result folds to a constant leaves the `a=b; b=next` rotation copies
 * running every loop iteration. */
static void mir_dce(MirFunction *fn) {
  if (fn->vreg_count == 0 || fn->insn_count == 0) {
    return;
  }
  int *reads = (int *)malloc(fn->vreg_count * sizeof(int));
  if (!reads) {
    return;
  }
  int changed = 1;
  while (changed) {
    changed = 0;
    memset(reads, 0, fn->vreg_count * sizeof(int));
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (in->op == MIR_NOP) {
        continue;
      }
      mir_dce_count_operand(&in->a, reads, fn->vreg_count);
      mir_dce_count_operand(&in->b, reads, fn->vreg_count);
      if (in->dst.kind == MIR_OPK_MEM) {
        mir_dce_add_read(in->dst.mem.base, reads, fn->vreg_count);
        mir_dce_add_read(in->dst.mem.index, reads, fn->vreg_count);
      }
    }
    for (size_t i = 0; i < fn->insn_count; i++) {
      MirInst *in = &fn->insns[i];
      if (in->op == MIR_NOP || !mir_op_pure_def(in->op)) {
        continue;
      }
      if (in->dst.kind != MIR_OPK_VREG) {
        continue; /* a store (dst MEM) is not a pure def */
      }
      if (in->op == MIR_MOV && in->a.kind == MIR_OPK_MEM) {
        continue; /* a load may fault; leave it */
      }
      MirVregId d = in->dst.vreg;
      if (d < 0 || (size_t)d >= fn->vreg_count ||
          d == fn->indirect_return_vreg) {
        continue;
      }
      if (reads[d] == 0) {
        in->op = MIR_NOP;
        changed = 1;
      }
    }
  }
  free(reads);
}

int mir_regalloc(MirFunction *fn) {
  if (!fn) {
    return 0;
  }
  if (fn->vreg_count == 0) {
    return 1;
  }

  /* Invalidate the clobber-event cache unconditionally: it is keyed on the
   * MirFunction and insns POINTERS plus the instruction count, but MirFunction
   * is a stack local reused at the same address for every function, and a
   * freed insns array is routinely handed back by the allocator for the next
   * function. Two same-length functions could then false-hit the cache and
   * colour against the PREVIOUS function's clobber positions - heap-layout
   * dependent register choices (the opt_ptr_induction determinism flake) and,
   * in the worst case, a value placed in a register a homing move clobbers.
   * Resetting here keeps the full intra-function benefit: the index is built
   * once on the first query below and stays valid for the whole allocation. */
  mir_clobber_index_reset();

  /* Strip dead pure defs before allocation so they neither consume registers nor
   * emit instructions. */
  mir_dce(fn);

  /* Finalize the frame-pointer-omission decision now that the MIR body exists.
   * The mir_lower stage cleared it for feature gates (stack traces / debug);
   * here we additionally require the function to be a LEAF. In a leaf, rsp is
   * never perturbed by call/push in the body, so rsp-relative slot addressing
   * pays no stack-engine sync uop -- the freed rbp and the shorter prologue are
   * a clean win. In a call-heavy function, rsp-relative spill accesses near each
   * call would each force a sync, which cancels the prologue saving (measured
   * slightly negative on rec_fib), so those keep the rbp frame. */
  if (fn->context && fn->context->omit_frame_pointer && mir_fn_has_calls(fn)) {
    fn->context->omit_frame_pointer = 0;
  }

  /* Graph coloring is the default; METTLE_LINEAR_ALLOC forces the legacy
   * linear scan (an escape hatch for differential debugging). Snapshotted
   * because this runs once per function and getenv on Windows takes a lock and
   * scans the whole environment -- measured at 2.5% of total compile time on a
   * 13k-function input. The env cannot change mid-process for a diagnostic
   * knob. */
  {
    static int linear = -1;
    if (linear < 0) {
      linear = getenv("METTLE_LINEAR_ALLOC") ? 1 : 0;
    }
    if (!linear) {
      return mir_regalloc_color(fn);
    }
  }

  mir_compute_liveness(fn);
  mir_compute_coalesce_hints(fn);

  /* A value is "cross-call" if its live interval strictly spans a MIR_CALL
   * (defined before the call, used after it). Such values must survive the
   * callee's clobber of caller-saved registers. (A value defined by the call's
   * return, or whose last use is feeding an argument, does not span it.) */
  /* An inline kernel clobbers the caller-saved set (RAX/RCX/RDX/R8/R9/R10/R11
   * + xmm0..) exactly like a call, so a value spanning one must also live in a
   * callee-saved register or spill. */
  mir_mark_crosses_call(fn);

  size_t order_count = 0;
  MirVregId *order = mir_order_by_start(fn, &order_count);
  if (fn->has_error) {
    free(order);
    return 0;
  }
  MirVregId *narrow_src = mir_build_narrowing_extend_map(fn);

  /* Per-class free pools, tracked as "register r is free / held by vreg". */
  int gp_held_by[16];  /* index by BinaryGpRegister -> vreg id or -1 */
  int xmm_held_by[16]; /* index by BinaryXmmRegister -> vreg id or -1 */
  for (int i = 0; i < 16; i++) {
    gp_held_by[i] = -1;
    xmm_held_by[i] = -1;
  }
  /* XMM4/XMM5 are encoder scratch (see MIR_XMM_POOL), never allocate them. */
  xmm_held_by[BINARY_XMM4] = -2;
  xmm_held_by[BINARY_XMM5] = -2;
  /* Leaf pool for this ABI/shape (base + any arg-capable reg this function does
   * not need for its own params or outgoing calls). */
  BinaryGpRegister gp_leaf_pool[MIR_GP_LEAF_POOL_MAX];
  /* +1 for the hidden out-pointer of an indirect struct return (it occupies the
   * first incoming arg slot, shifting the real parameters up). */
  size_t gp_leaf_pool_count = mir_build_gp_leaf_pool(
      gp_leaf_pool, fn->param_count + (fn->returns_indirect ? 1 : 0),
      !mir_fn_has_real_calls(fn));
  BinaryGpRegister gp_cross_pool[MIR_GP_CROSSCALL_POOL_MAX];
  size_t gp_cross_pool_count = mir_build_gp_crosscall_pool(gp_cross_pool);
  /* Start every GP register reserved, then open exactly the leaf-pool members.
   * RAX/RCX/RDX (encoder scratch) and RSP/RBP (stack/frame) are never in the
   * pool, so they stay reserved. */
  for (int r = 0; r < 16; r++) {
    gp_held_by[r] = -2;
  }
  for (size_t i = 0; i < gp_leaf_pool_count; i++) {
    gp_held_by[gp_leaf_pool[i]] = -1;
  }

  /* Spill slots grow downward below the existing frame. The encoder adds
   * fn->spill_bytes to the prologue allocation; slot k lives at
   * [rbp - (base_frame + (k+1)*8)]. We record only the running total here and
   * store each vreg's own positive offset. */
  int next_spill_offset = fn->context ? fn->context->raw_frame_size : 0;
  fn->preserve_slot = 0;
  fn->preserve_xmm_slot = 0;
  if (mir_fn_has_preserving_call(fn, 0)) {
    next_spill_offset += 8;
    fn->preserve_slot = next_spill_offset;
  }
  if (mir_fn_has_preserving_call(fn, 1)) {
    next_spill_offset += (int)MIR_XMM_POOL_COUNT * 8;
    fn->preserve_xmm_slot = next_spill_offset;
  }

  /* Address-taken values must be memory-resident; give each a stack slot up
   * front (independent of liveness, one may be written only through its alias
   * pointer and never appear in the interval order). The main scan then skips
   * them so they never occupy a register. */
  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->address_taken) {
      /* A struct local owns a multi-slot home (home_bytes); the slot offset is
       * the FAR (highest) end since homes grow downward from rbp, so the home
       * spans [rbp - offset .. rbp - offset + home_bytes). */
      int home = mir_home_bytes_for(vr, &next_spill_offset);
      next_spill_offset += home;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = next_spill_offset;
    }
  }

  /* Active intervals, kept as a simple array we scan/expire each step. */
  MirVregId *active = (MirVregId *)malloc(order_count * sizeof(MirVregId));
  if (!active && order_count > 0) {
    free(order);
    free(narrow_src);
    fn->has_error = 1;
    return 0;
  }
  size_t active_count = 0;

  for (size_t oi = 0; oi < order_count; oi++) {
    MirVregId cur = order[oi];
    MirVreg *cv = &fn->vregs[cur];
    int point = cv->live_start;

    /* Expire intervals that ended before this start. */
    size_t w = 0;
    for (size_t r = 0; r < active_count; r++) {
      MirVregId a = active[r];
      MirVreg *av = &fn->vregs[a];
      if (av->live_end < point) {
        if (av->in_register) {
          if (av->rclass == MIR_RC_XMM) {
            xmm_held_by[av->phys] = -1;
          } else {
            gp_held_by[av->phys] = -1;
          }
        }
      } else {
        active[w++] = a;
      }
    }
    active_count = w;

    /* Address-taken values are memory-resident (their stack slot was assigned
     * up front, below): never give them a register, so every use loads and
     * every def stores through the home, keeping a by-name access and an
     * aliasing-pointer access on the same memory. */
    if (cv->address_taken) {
      continue;
    }

    /* Two-address coalescing: reuse the register of a source that dies exactly
     * here, so the encoder writes the result in place. Only for non-cross-call
     * GP values (a cross-call dst needs a callee-saved reg, which the dying
     * source may not be in). The source is still `active` (its live_end == this
     * point, so the expire above kept it); steal its register and drop it from
     * the active set so the next expire does not free what is now ours. */
    int got_reg = 0;
    if (cv->rclass == MIR_RC_GP && !cv->crosses_call &&
        cv->coalesce_hint != MIR_VREG_NONE) {
      MirVreg *hv = &fn->vregs[cv->coalesce_hint];
      if (hv->in_register && hv->rclass == MIR_RC_GP &&
          hv->live_end == point && gp_held_by[hv->phys] == cv->coalesce_hint &&
          !mir_reg_clobbered_in_range(fn, (BinaryGpRegister)hv->phys,
                                      cv->live_start, cv->live_end)) {
        cv->phys = hv->phys;
        cv->assigned = 1;
        cv->in_register = 1;
        gp_held_by[hv->phys] = cur;
        for (size_t r = 0; r < active_count; r++) {
          if (active[r] == cv->coalesce_hint) {
            active[r] = active[--active_count];
            break;
          }
        }
        got_reg = 1;
      }
    }
    /* Try to grab a free physical register. Cross-call values may only use the
     * callee-saved pool (GP), or must spill (XMM has no callee-saved lane in our
     * allocatable set). */
    if (!got_reg && cv->rclass == MIR_RC_XMM) {
      /* A value spanning only preserving calls may take the volatile lanes:
       * those are exactly what such a call saves and restores. It may not take
       * xmm8-15, which are callee-saved on Win64 but not on SysV. */
      if (!cv->crosses_call || cv->crosses_xmm_preserving_only) {
        /* Skip the volatile xmm0-3 when they serve as outgoing float-argument
         * registers (see has_xmm_arg_call / mir_color_reg_mask). */
        for (size_t p = 0; !fn->has_xmm_arg_call && p < MIR_XMM_POOL_COUNT; p++) {
          BinaryXmmRegister reg = MIR_XMM_POOL[p];
          if (xmm_held_by[reg] == -1) {
            xmm_held_by[reg] = cur;
            cv->assigned = 1;
            cv->in_register = 1;
            cv->phys = reg;
            got_reg = 1;
            break;
          }
        }
        /* Spill to the callee-saved xmm8..15 tier before the stack. */
        for (size_t p = 0;
             !got_reg && !cv->crosses_call && p < MIR_XMM_NONVOL_POOL_COUNT;
             p++) {
          BinaryXmmRegister reg = MIR_XMM_NONVOL_POOL[p];
          if (xmm_held_by[reg] == -1) {
            xmm_held_by[reg] = cur;
            cv->assigned = 1;
            cv->in_register = 1;
            cv->phys = reg;
            got_reg = 1;
            break;
          }
        }
      }
    } else if (!got_reg) {
      BinaryGpRegister cross_ext[MIR_GP_CROSSCALL_POOL_EXT];
      size_t cross_ext_n =
          mir_cross_pool_for(cv, gp_cross_pool, gp_cross_pool_count, cross_ext);
      const BinaryGpRegister *pool =
          cv->crosses_call ? cross_ext : gp_leaf_pool;
      size_t pool_n = cv->crosses_call ? cross_ext_n : gp_leaf_pool_count;
      /* A narrowing extend wants a register OTHER than the one it extends:
       * `mov r8d, r9d` is renamed away by the hardware for free, while
       * `mov r8d, r8d` cannot be (it zeroes the upper half in place) and costs
       * a cycle on the dependence chain. Pass 0 skips that one register; pass 1
       * reconsiders it, since taking it still beats spilling. */
      int avoid = mir_narrowing_avoid_reg(fn, narrow_src, cur);
      for (int relax = 0; !got_reg && relax < 2; relax++) {
        for (size_t p = 0; p < pool_n; p++) {
          BinaryGpRegister reg = pool[p];
          if (relax == 0 && avoid >= 0 && (int)reg == avoid) {
            continue;
          }
          if (gp_held_by[reg] == -1 &&
              !mir_reg_clobbered_in_range(fn, reg, cv->live_start,
                                          cv->live_end)) {
            gp_held_by[reg] = cur;
            cv->assigned = 1;
            cv->in_register = 1;
            cv->phys = reg;
            got_reg = 1;
            break;
          }
        }
        if (avoid < 0) {
          break; /* nothing was skipped, so the second pass would repeat */
        }
      }
    }

    if (got_reg) {
      active[active_count++] = cur;
      continue;
    }

    /* Cross-call values that found no callee-saved register simply spill, they
     * must not steal a volatile register (it would be clobbered by the call). */
    if (cv->crosses_call) {
      next_spill_offset += cv->width > 8 ? 16 : 8;
      cv->assigned = 1;
      cv->in_register = 0;
      cv->spill_offset = next_spill_offset;
      continue;
    }

    /* No free register: choose a spill victim. The classic linear-scan choice
     * is the farthest live_end, but in a loop that is exactly a loop-carried
     * value (base pointer / accumulator / induction var) reused every
     * iteration -- spilling it reloads it each pass. So prefer a NON-loop-
     * carried victim (a body temp, often a cold sub-path's value that costs one
     * reload); only fall back to farthest-live_end within the same loop-carried
     * category. Same class, not clobbered inside cur's interval. */
    MirVregId spill_victim = MIR_VREG_NONE;
    int victim_end = -1;
    int victim_lc = 1;
    for (size_t r = 0; r < active_count; r++) {
      MirVregId a = active[r];
      MirVreg *av = &fn->vregs[a];
      if (av->rclass != cv->rclass || !av->in_register) {
        continue;
      }
      /* Don't steal a register that would be clobbered inside cur's interval. */
      if (av->rclass == MIR_RC_GP &&
          mir_reg_clobbered_in_range(fn, (BinaryGpRegister)av->phys,
                                     cv->live_start, cv->live_end)) {
        continue;
      }
      int better;
      if (spill_victim == MIR_VREG_NONE) {
        better = 1;
      } else if (av->loop_carried != victim_lc) {
        better = (av->loop_carried < victim_lc); /* prefer non-loop-carried */
      } else {
        better = (av->live_end > victim_end);
      }
      if (better) {
        victim_end = av->live_end;
        victim_lc = av->loop_carried;
        spill_victim = a;
      }
    }

    /* Spill the victim instead of `cur` when the victim is the worse one to
     * keep: a loop-carried `cur` should evict a non-loop-carried victim
     * regardless of live_end; otherwise the standard farthest-live_end rule. */
    int prefer_victim = 0;
    if (spill_victim != MIR_VREG_NONE) {
      if (victim_lc != cv->loop_carried) {
        prefer_victim = (cv->loop_carried && !victim_lc);
      } else {
        prefer_victim = fn->vregs[spill_victim].live_end > cv->live_end;
      }
    }
    if (prefer_victim) {
      /* Steal the victim's register; spill the victim. */
      MirVreg *vv = &fn->vregs[spill_victim];
      int reg = vv->phys;
      next_spill_offset += vv->width > 8 ? 16 : 8;
      vv->in_register = 0;
      vv->assigned = 1;
      vv->spill_offset = next_spill_offset;
      cv->assigned = 1;
      cv->in_register = 1;
      cv->phys = reg;
      if (cv->rclass == MIR_RC_XMM) {
        xmm_held_by[reg] = cur;
      } else {
        gp_held_by[reg] = cur;
      }
      /* Replace victim with cur in the active set. */
      for (size_t r = 0; r < active_count; r++) {
        if (active[r] == spill_victim) {
          active[r] = cur;
          break;
        }
      }
    } else {
      /* Spill current. */
      next_spill_offset += cv->width > 8 ? 16 : 8;
      cv->assigned = 1;
      cv->in_register = 0;
      cv->spill_offset = next_spill_offset;
    }
  }

  mir_drop_unused_preserves(fn);
  fn->spill_bytes =
      next_spill_offset - (fn->context ? fn->context->raw_frame_size : 0);

  /* Tell the function context which nonvolatile registers the allocation used,
   * so the encoder's prologue/epilogue saves and restores them. */
  if (fn->context) {
    int used_nonvol[16];
    memset(used_nonvol, 0, sizeof(used_nonvol));
    for (size_t i = 0; i < fn->vreg_count; i++) {
      MirVreg *vr = &fn->vregs[i];
      if (vr->in_register && vr->rclass == MIR_RC_GP &&
          mir_gp_is_nonvolatile((BinaryGpRegister)vr->phys)) {
        used_nonvol[vr->phys] = 1;
      }
    }
    for (int reg = 0; reg < 16; reg++) {
      if (used_nonvol[reg] &&
          !code_generator_binary_context_add_saved_register(
              fn->context, (BinaryGpRegister)reg)) {
        free(order);
        free(active);
        free(narrow_src);
        fn->has_error = 1;
        return 0;
      }
    }

    /* Callee-saved XMM (xmm8..15) the allocation used: the prologue/epilogue
     * preserve them (a no-op cost on SysV where they are caller-saved). */
    int used_xmm[16];
    memset(used_xmm, 0, sizeof(used_xmm));
    for (size_t i = 0; i < fn->vreg_count; i++) {
      MirVreg *vr = &fn->vregs[i];
      if (vr->in_register && vr->rclass == MIR_RC_XMM && vr->phys >= 8) {
        used_xmm[vr->phys] = 1;
      }
    }
    for (int reg = 8; reg < 16; reg++) {
      if (used_xmm[reg] &&
          !code_generator_binary_context_add_saved_xmm_register(
              fn->context, (BinaryXmmRegister)reg)) {
        free(order);
        free(active);
        free(narrow_src);
        fn->has_error = 1;
        return 0;
      }
    }
  }

  free(order);
  free(active);
  free(narrow_src);
  return 1;
}
