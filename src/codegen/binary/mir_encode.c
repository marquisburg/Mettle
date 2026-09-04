#include "codegen/binary/mir.h"
#include "ir/ir_machine.h"

/* What the last encoded function spilled, read by the caller that knows the
   function's name. Set only while a machine rule is collecting. */
long long mir_encode_last_spills = 0;
#include "codegen/binary/mir_annotate.h"
#include "codegen/binary/simd_internal.h"
#include "codegen/code_generator_internal.h"

#include <stdlib.h>
#include <string.h>

/* MIR (post-allocation) -> machine bytes in fn->context->code.
 *
 * Compute model: RAX is the primary scratch/accumulator and RCX the secondary;
 * RDX is reserved (future divide). Operand values come from their ALLOCATED
 * registers (or are materialized from a spill slot / immediate into a scratch),
 * never from per-temp stack homes, that is the whole point. Each MIR op
 * computes into RAX and writes the destination's register (or spill slot). The
 * extra reg-reg moves vs an optimal in-place scheme are cheap and removable
 * later; correctness first. */

/* Encoder scratch registers. R10/R11 are pure scratch, not allocatable, and
 * not ABI argument registers on EITHER Win64 or SysV, so RAX/RCX/RDX are freed
 * for the register allocator. Ops that need a HARDWARE register (divide's
 * RDX:RAX, variable shift's CL, setcc's byte target) name it explicitly. */
#define SCRATCH_A BINARY_GP_R10
#define SCRATCH_B BINARY_GP_R11
/* Float scratch (see MIR_XMM_POOL): XMM4 primary, XMM5 secondary. */
#define FSCRATCH_A BINARY_XMM4
#define FSCRATCH_B BINARY_XMM5

static int enc_err(MirFunction *fn, const char *msg) {
  if (fn->generator && !fn->generator->has_error) {
    code_generator_set_error(fn->generator, "%s in function '%s'", msg,
                             fn->context->function_name
                                 ? fn->context->function_name
                                 : "?");
  }
  fn->has_error = 1;
  return 0;
}

/* rbp-relative offset of a spilled vreg (mem = [rbp - offset]). */
static int spill_off(const MirVreg *v) { return v->spill_offset; }

/* Spill-home forwarding.
 *
 * A spilled value that is written and then immediately read back costs two
 * memory operations to move a value that is already sitting in the register
 * that just wrote it. The allocator produces long runs of this whenever a
 * chain of instructions shares one coalesced slot: the counter update in a
 * pressured loop stores, reloads, stores and reloads the same word.
 *
 * The only thing that makes forwarding unsound is something happening between
 * the store and the load, and the code buffer answers that exactly: if its
 * size has not moved, no instruction was emitted in between, so the register
 * still holds what the slot holds. A label emits no bytes and would slip
 * through that test, so control flow clears the record explicitly.
 *
 * An address-taken home is never forwarded: a pointer can write those bytes
 * without going through this path at all. */
typedef struct {
  int valid;
  int disp;
  BinaryGpRegister reg;
  size_t code_size;
} MirHomeForward;

static MirHomeForward g_home_fwd;

static void home_fwd_clear(void) { g_home_fwd.valid = 0; }

/* A label emits no bytes, so the code-size invariant cannot see that control
 * can arrive here from a branch with a different register state. Anything that
 * transfers control ends a forwarding window. */
static void home_fwd_note_boundary(MirOpcode op) {
  if (op == MIR_LABEL || op == MIR_JMP || op == MIR_JCC || op == MIR_CMPBR ||
      op == MIR_FCMPBR || op == MIR_CALL || op == MIR_RET) {
    g_home_fwd.valid = 0;
  }
}

static void home_fwd_record(const BinaryCodeBuffer *code, int disp,
                            BinaryGpRegister reg) {
  g_home_fwd.valid = 1;
  g_home_fwd.disp = disp;
  g_home_fwd.reg = reg;
  g_home_fwd.code_size = code->size;
}

static int home_fwd_has(const BinaryCodeBuffer *code, int disp,
                        BinaryGpRegister reg) {
  return g_home_fwd.valid && g_home_fwd.disp == disp &&
         g_home_fwd.reg == reg && g_home_fwd.code_size == code->size;
}

/* Frame base register for stack slots: RSP when the frame pointer is omitted
 * (rbp is then free for allocation), otherwise RBP. */
static BinaryGpRegister frame_base(const MirFunction *fn) {
  return fn->context->omit_frame_pointer ? BINARY_GP_RSP : BINARY_GP_RBP;
}

/* Translate an rbp-relative displacement to the active frame base. With the
 * frame pointer omitted, rsp sits frame_size below where rbp would point, so
 * [rbp+d] == [rsp+frame_size+d]. */
static int frame_disp(const MirFunction *fn, int rbp_disp) {
  return fn->context->omit_frame_pointer ? rbp_disp + fn->context->frame_size
                                         : rbp_disp;
}

/* Load a GP vreg's spilled home into `dst`. An address-taken narrow scalar
 * (home_width 1/2/4) is authoritative only at its declared width: an
 * aliasing pointer writes exactly those bytes, so the load extends from them
 * instead of scooping whatever the rest of the 8-byte slot last held. */
static int gp_home_load(MirFunction *fn, const MirVreg *v,
                        BinaryGpRegister dst) {
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister base = frame_base(fn);
  int disp = frame_disp(fn, -spill_off(v));
  if (!v->address_taken && home_fwd_has(code, disp, dst)) {
    return 1;
  }
  if (v->address_taken) {
    switch (v->home_width) {
    case 4:
      return v->home_signed
                 ? binary_emit_movsxd_reg_mem(code, dst, base, disp)
                 : binary_emit_mov_reg_mem32(code, dst, base, disp);
    case 2:
      return v->home_signed
                 ? binary_emit_movsx_reg_mem16(code, dst, base, disp)
                 : binary_emit_movzx_reg_mem16(code, dst, base, disp);
    case 1:
      return v->home_signed
                 ? binary_emit_movsx_reg_mem8(code, dst, base, disp)
                 : binary_emit_movzx_reg_mem8(code, dst, base, disp);
    default:
      break;
    }
  }
  return binary_emit_mov_reg_mem(code, dst, base, disp);
}

static int gp_home_mem(MirFunction *fn, const MirOperand *op,
                       BinaryGpRegister *base, int *disp) {
  if (op->kind == MIR_OPK_STACKHOME) {
    *base = frame_base(fn);
    *disp = frame_disp(fn, -op->disp);
    return 1;
  }
  if (op->kind != MIR_OPK_VREG) {
    return 0;
  }
  {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register || v->rclass != MIR_RC_GP) {
      return 0;
    }
    if (v->address_taken &&
        (v->home_width == 1 || v->home_width == 2 || v->home_width == 4)) {
      return 0;
    }
    *base = frame_base(fn);
    *disp = frame_disp(fn, -spill_off(v));
  }
  return 1;
}

/* Emit: target <- value of `op`. */
static int materialize_into(MirFunction *fn, const MirOperand *op,
                            BinaryGpRegister target) {
  BinaryCodeBuffer *code = &fn->context->code;
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      if ((BinaryGpRegister)v->phys != target) {
        return binary_emit_mov_reg_reg(code, target, (BinaryGpRegister)v->phys);
      }
      return 1;
    }
    return gp_home_load(fn, v, target);
  }
  case MIR_OPK_PHYS:
    if ((BinaryGpRegister)op->phys != target) {
      return binary_emit_mov_reg_reg(code, target, (BinaryGpRegister)op->phys);
    }
    return 1;
  case MIR_OPK_IMM:
    return binary_emit_mov_reg_imm64(code, target, (uint64_t)op->imm);
  case MIR_OPK_STACKHOME:
    return binary_emit_mov_reg_mem(code, target, frame_base(fn),
                                   frame_disp(fn, -op->disp));
  default:
    return enc_err(fn, "unsupported MIR operand in materialize");
  }
}

static int mir_reg_in(BinaryGpRegister r, const BinaryGpRegister *set, int n) {
  for (int i = 0; i < n; i++) {
    if (set[i] == r) {
      return 1;
    }
  }
  return 0;
}

/* The register `op` already sits in, if any, without emitting anything.
 *
 * Staging picks have to know this up front. An operand resolved LATER may
 * already be resident somewhere, and staging an earlier spilled operand into
 * that register would destroy it before it is read. RDX is the case that bites:
 * it is in the allocator's pool and is also the preferred index scratch. */
static int mir_operand_fixed_reg(const MirFunction *fn, const MirOperand *op,
                                 BinaryGpRegister *out) {
  if (op->kind == MIR_OPK_VREG && op->vreg >= 0 &&
      (size_t)op->vreg < fn->vreg_count && fn->vregs[op->vreg].in_register) {
    *out = (BinaryGpRegister)fn->vregs[op->vreg].phys;
    return 1;
  }
  if (op->kind == MIR_OPK_PHYS) {
    *out = (BinaryGpRegister)op->phys;
    return 1;
  }
  return 0;
}

/* Append `op`'s resident register to `set` if it has one. */
static void mir_note_fixed_reg(const MirFunction *fn, const MirOperand *op,
                               BinaryGpRegister *set, int *n) {
  BinaryGpRegister r;
  if (mir_operand_fixed_reg(fn, op, &r)) {
    set[(*n)++] = r;
  }
}

/* True when some allocated value sitting in `phys` is live at instruction
 * `idx`. Only meaningful for allocatable registers: R10/R11 are outside the
 * pool, so nothing is ever live in them. */
static int mir_phys_live_at(const MirFunction *fn, BinaryGpRegister phys,
                            size_t idx) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (!vr->assigned || !vr->in_register || vr->rclass == MIR_RC_XMM ||
        (BinaryGpRegister)vr->phys != phys) {
      continue;
    }
    if (vr->live_start <= (int)idx && vr->live_end >= (int)idx) {
      return 1;
    }
  }
  return 0;
}

/* Pick a scratch register for staging a spilled operand: `preferred` unless it
 * is already holding one of `avoid`.
 *
 * A [base + index*scale] access stages each spilled operand through a scratch,
 * and two of them landing in the SAME register does not fail loudly: it encodes
 * base and index as one register, so `[base + index*4]` silently becomes
 * `[r11 + r11*4]` and the access reads the wrong address.
 *
 * The candidates are the reserved encoder scratches R10/R11, which are outside
 * the allocator's pool and so can never hold a live value. RDX is allowed only
 * as a last resort AND only when no live value occupies it at this instruction:
 * it is allocatable, and staging over a live one is silent corruption of a
 * value this instruction never mentions. A loop counter in RDX was overwritten
 * by the index staging of a byte load, and the loop then ran until it walked
 * off its array -- `avoid` cannot see that, because the clobbered value is not
 * an operand of the access doing the clobbering.
 *
 * `extra` names registers the caller has vouched for -- an address-forming
 * instruction's own destination, which it writes only after reading the whole
 * address, so staging into it is safe once the operands' resident registers are
 * in `avoid`. Those skip the liveness gate: the destination is defined here, so
 * a scan would always see it as live.
 *
 * Returns 0 (and leaves *out untouched) when nothing is safe, so the caller
 * raises an encoder error rather than emitting a wrong address. */
static int mir_pick_scratch(const MirFunction *fn, size_t idx,
                            BinaryGpRegister preferred,
                            const BinaryGpRegister *avoid, int avoid_n,
                            const BinaryGpRegister *extra, int extra_n,
                            BinaryGpRegister *out) {
  BinaryGpRegister pool[8];
  int vouched[8];
  int n = 0;
  pool[n] = preferred;
  vouched[n++] = 0;
  pool[n] = SCRATCH_B;
  vouched[n++] = 1;
  pool[n] = SCRATCH_A;
  vouched[n++] = 1;
  for (int i = 0; i < extra_n && n < 7; i++) {
    pool[n] = extra[i];
    vouched[n++] = 1;
  }
  pool[n] = BINARY_GP_RDX;
  vouched[n++] = 0;

  for (int i = 0; i < n; i++) {
    if (mir_reg_in(pool[i], avoid, avoid_n)) {
      continue;
    }
    if (!vouched[i] && pool[i] != SCRATCH_A && pool[i] != SCRATCH_B &&
        mir_phys_live_at(fn, pool[i], idx)) {
      continue;
    }
    *out = pool[i];
    return 1;
  }
  return 0;
}

/* Force every scaled store down the form-the-address-first path, which a
 * scaled store only takes when the three-scratch staging runs dry. That needs
 * register pressure deep self-recursion expansion produces and the default
 * inlining caps do not, so the path would otherwise ship untested. getenv is
 * slow on Windows and this sits in the encoder, so snapshot it once. */
static int mir_env_addr_store(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_MIR_ADDR_STORE") ? 1 : 0;
  }
  return cached;
}

/* Return the physical register currently holding `op`'s value, materializing
 * into `scratch` when the operand is a spill/immediate/home. */
static BinaryGpRegister value_reg(MirFunction *fn, const MirOperand *op,
                                  BinaryGpRegister scratch, int *ok) {
  *ok = 1;
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      /* A vreg's phys number means nothing without its bank: XMM0 and RAX
       * are both 0. Reading a float's bits into a GP register is a real
       * request -- interpolation hands mettle_string_from_f64 the raw bits
       * -- and it has to cross with movq. Casting the number straight to a
       * GP register silently read RAX instead. */
      if (v->rclass == MIR_RC_XMM) {
        *ok = binary_emit_movq_reg_xmm(&fn->context->code, scratch,
                                       (BinaryXmmRegister)v->phys);
        if (!*ok) {
          *ok = enc_err(fn, "out of memory moving a float to a GP register");
        }
        return scratch;
      }
      if (v->rclass != MIR_RC_GP) {
        *ok = enc_err(fn, "a packed vector has no GP value form");
        return scratch;
      }
      return (BinaryGpRegister)v->phys;
    }
    /* Spilled: the home slot holds the bits either way, so a GP load of the
     * slot is the same value whichever bank wrote it. */
    *ok = gp_home_load(fn, v, scratch);
    return scratch;
  }
  case MIR_OPK_PHYS:
    return (BinaryGpRegister)op->phys;
  case MIR_OPK_IMM:
  /* A float immediate as a raw VALUE is its IEEE-754 bits (that is the FIMM
   * contract), so a GP materialization is the bits themselves. This is what a
   * memory store of a float constant needs; anything arithmetic goes through
   * the XMM staging path and never lands here. */
  case MIR_OPK_FIMM:
    *ok = binary_emit_mov_reg_imm64(&fn->context->code, scratch,
                                    (uint64_t)op->imm);
    return scratch;
  case MIR_OPK_STACKHOME:
    *ok = binary_emit_mov_reg_mem(&fn->context->code, scratch, frame_base(fn),
                                  frame_disp(fn, -op->disp));
    return scratch;
  default:
    *ok = enc_err(fn, "unsupported MIR operand as value");
    return scratch;
  }
}

/* Emit: dst <- value in src_phys. */
static int store_from(MirFunction *fn, const MirOperand *dst,
                      BinaryGpRegister src_phys) {
  BinaryCodeBuffer *code = &fn->context->code;
  switch (dst->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->in_register) {
      /* Same bank confusion as value_reg, in the other direction. */
      if (v->rclass == MIR_RC_XMM) {
        return binary_emit_movq_xmm_reg(code, (BinaryXmmRegister)v->phys,
                                        src_phys);
      }
      if (v->rclass != MIR_RC_GP) {
        return enc_err(fn, "a packed vector has no GP value form");
      }
      if ((BinaryGpRegister)v->phys != src_phys) {
        return binary_emit_mov_reg_reg(code, (BinaryGpRegister)v->phys,
                                       src_phys);
      }
      return 1;
    }
    {
      int disp = frame_disp(fn, -spill_off(v));
      if (!v->address_taken && home_fwd_has(code, disp, src_phys)) {
        /* The slot already holds this register's value and nothing has run
         * since it was put there. */
        return 1;
      }
      if (!binary_emit_mov_mem_reg(code, frame_base(fn), disp, src_phys)) {
        return 0;
      }
      if (!v->address_taken) {
        home_fwd_record(code, disp, src_phys);
      }
      return 1;
    }
  }
  case MIR_OPK_PHYS:
    if ((BinaryGpRegister)dst->phys != src_phys) {
      return binary_emit_mov_reg_reg(code, (BinaryGpRegister)dst->phys, src_phys);
    }
    return 1;
  default:
    return enc_err(fn, "unsupported MIR destination");
  }
}

/* ALU r/m,reg opcode bytes for the reg-reg ALU forms. */
static int alu_opcode(MirOpcode op, unsigned char *out) {
  switch (op) {
  case MIR_ADD: *out = 0x01; return 1;
  case MIR_SUB: *out = 0x29; return 1;
  case MIR_AND: *out = 0x21; return 1;
  case MIR_OR:  *out = 0x09; return 1;
  case MIR_XOR: *out = 0x31; return 1;
  default: return 0;
  }
}

/* ALU /digit sub-opcodes for the reg,imm forms. */
static int alu_imm_subopcode(MirOpcode op, unsigned char *out) {
  switch (op) {
  case MIR_ADD: *out = 0; return 1;
  case MIR_OR:  *out = 1; return 1;
  case MIR_AND: *out = 4; return 1;
  case MIR_SUB: *out = 5; return 1;
  case MIR_XOR: *out = 6; return 1;
  default: return 0;
  }
}

static int alu_imm(MirFunction *fn, MirOpcode op, BinaryGpRegister reg,
                   long long imm, int width) {
  BinaryCodeBuffer *code = &fn->context->code;
  uint32_t v = (uint32_t)imm;
  if (width == 4) {
    unsigned char sub;
    if (!alu_imm_subopcode(op, &sub)) {
      return 0;
    }
    return binary_emit_alu_reg_imm_w32(code, sub, reg, v);
  }
  switch (op) {
  case MIR_ADD: return binary_emit_add_reg_imm32(code, reg, v);
  case MIR_SUB: return binary_emit_sub_reg_imm32(code, reg, v);
  case MIR_AND: return binary_emit_and_reg_imm32(code, reg, v);
  case MIR_OR:  return binary_emit_or_reg_imm32(code, reg, v);
  case MIR_XOR: return binary_emit_xor_reg_imm32(code, reg, v);
  default: return 0;
  }
}

/* Does `op` currently resolve to physical register D? (A register-resident
 * vreg or a fixed PHYS operand.) Immediates/spills/memory never alias D. */
static int operand_in_phys(MirFunction *fn, const MirOperand *op,
                           BinaryGpRegister D) {
  if (op->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[op->vreg];
    return v->in_register && (BinaryGpRegister)v->phys == D;
  }
  if (op->kind == MIR_OPK_PHYS) {
    return (BinaryGpRegister)op->phys == D;
  }
  return 0;
}

/* True (filling *reg) when `op` is already resident in a GP register, with no
 * spill reload or immediate materialization needed. */
static int operand_gp_reg(MirFunction *fn, const MirOperand *op,
                          BinaryGpRegister *reg) {
  if (op->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register && v->rclass == MIR_RC_GP) {
      *reg = (BinaryGpRegister)v->phys;
      return 1;
    }
    return 0;
  }
  if (op->kind == MIR_OPK_PHYS) {
    *reg = (BinaryGpRegister)op->phys;
    return 1;
  }
  return 0;
}

/* If `dst` is register-resident, write its physical register and return 1;
 * otherwise (spilled) return 0. */
static int dst_is_reg(MirFunction *fn, const MirOperand *dst,
                      BinaryGpRegister *D_out) {
  if (dst->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->rclass != MIR_RC_GP) {
      return 0; /* not a GP register; store_from crosses the bank */
    }
    if (v->in_register) {
      *D_out = (BinaryGpRegister)v->phys;
      return 1;
    }
    return 0;
  }
  if (dst->kind == MIR_OPK_PHYS) {
    *D_out = (BinaryGpRegister)dst->phys;
    return 1;
  }
  return 0;
}

/* Emit `target OP= x` for an integer ALU op. `x` must not alias `target` unless
 * the op is commutative (callers guarantee this). Uses the scratch register
 * that is not `target` to stage a spilled/wide-immediate `x`. */
static int emit_op_eq(MirFunction *fn, MirOpcode mop, unsigned char opc,
                      BinaryGpRegister target, const MirOperand *x, int width) {
  BinaryCodeBuffer *code = &fn->context->code;
  /* At operand size 32 the immediate is not sign-extended, so ANY 32-bit
   * constant folds into the instruction -- including the ones (0x80000000 and
   * up) that the 64-bit form has to stage through a register.
   *
   * A 64-bit AND joins that: the immediate's upper half is zero, so the
   * result's upper half is zero whichever operand size the instruction uses,
   * and operand size 32 zeroes it for free. That rescues the masking constants
   * -- 0xEDB88320, 0xFFFFFF00, 0x9E3779B9 -- which otherwise burn a register
   * and an instruction at every use. OR and XOR do NOT join: they would have
   * preserved the operand's upper half, which operand size 32 discards. */
  if (x->kind == MIR_OPK_IMM &&
      (code_generator_binary_immediate_fits_signed_32(x->imm) ||
       (width == 4 && (unsigned long long)x->imm <= 0xFFFFFFFFULL))) {
    return alu_imm(fn, mop, target, x->imm, width)
               ? 1
               : enc_err(fn, "out of memory in ALU imm");
  }
  if (x->kind == MIR_OPK_IMM && mop == MIR_AND && width == 8 &&
      (unsigned long long)x->imm <= 0xFFFFFFFFULL) {
    return alu_imm(fn, mop, target, x->imm, 4)
               ? 1
               : enc_err(fn, "out of memory in ALU imm");
  }
  {
    BinaryGpRegister mbase;
    int mdisp;
    if (gp_home_mem(fn, x, &mbase, &mdisp)) {
      return binary_emit_alu_reg_mem(code, opc, target, mbase, mdisp,
                                     width == 4 ? 4 : 8)
                 ? 1
                 : enc_err(fn, "out of memory in ALU mem");
    }
  }
  BinaryGpRegister scratch = (target == SCRATCH_A) ? SCRATCH_B : SCRATCH_A;
  int ok;
  BinaryGpRegister xr = value_reg(fn, x, scratch, &ok);
  if (!ok) {
    return 0;
  }
  int emitted = (width == 4)
                    ? binary_emit_alu_reg_reg32(code, opc, target, xr)
                    : binary_emit_alu_reg_reg(code, opc, target, xr);
  return emitted ? 1 : enc_err(fn, "out of memory in ALU");
}

/* dst = -a (MIR_NEG) or dst = ~a (MIR_NOT). One-source two-address: stage a in
 * the destination register (or RAX scratch for a spilled dst), then neg/not in
 * place. */
static int encode_neg_not(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int w32 = (in->width == 4); /* see encode_alu */
  BinaryGpRegister D;
  if (dst_is_reg(fn, &in->dst, &D)) {
    if (!operand_in_phys(fn, &in->a, D) && !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    int ok = (in->op == MIR_NEG)
                 ? (w32 ? binary_emit_neg_reg32(code, D)
                        : binary_emit_neg_reg(code, D))
                 : (w32 ? binary_emit_not_reg32(code, D)
                        : binary_emit_not_reg(code, D));
    return ok ? 1 : enc_err(fn, "out of memory in neg/not");
  }
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  int ok = (in->op == MIR_NEG)
               ? (w32 ? binary_emit_neg_reg32(code, SCRATCH_A)
                      : binary_emit_neg_reg(code, SCRATCH_A))
               : (w32 ? binary_emit_not_reg32(code, SCRATCH_A)
                      : binary_emit_not_reg(code, SCRATCH_A));
  if (!ok) {
    return enc_err(fn, "out of memory in neg/not");
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_alu(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  unsigned char opc;
  if (!alu_opcode(in->op, &opc)) {
    return enc_err(fn, "bad ALU opcode");
  }
  int is_sub = (in->op == MIR_SUB);
  /* Width 4 means the result is defined to be the low 32 bits zero-extended,
   * which is exactly what a 32-bit-operand-size instruction produces for free
   * (see mir_narrow_zero_extended_ops). Staging an operand may still use a
   * 64-bit move: the narrow op reads only the low half and rewrites the top. */
  int w32 = (in->width == 4);
  BinaryGpRegister D;

  if (dst_is_reg(fn, &in->dst, &D)) {
    /* `D = a + b` with neither operand already in D would otherwise be
     * `mov D, a; add D, b` (two instructions). When both operands are live in
     * GP registers, `lea D, [a + b]` does it in one and -- unlike register
     * coalescing -- changes nothing about allocation (D stays D), so it cannot
     * lengthen a live range or cause a spill. ADD only (LEA can't subtract);
     * the SIB index can't be RSP, so swap operands if needed (ADD commutes).
     * MIR consumes condition flags only through explicit CMP, so LEA not
     * setting flags is fine. */
    if (in->op == MIR_ADD && !operand_in_phys(fn, &in->a, D) &&
        !operand_in_phys(fn, &in->b, D)) {
      BinaryGpRegister ra, rb;
      if (operand_gp_reg(fn, &in->a, &ra) && operand_gp_reg(fn, &in->b, &rb)) {
        BinaryGpRegister base = ra, index = rb;
        if (index == BINARY_GP_RSP) {
          base = rb;
          index = ra;
        }
        if (index != BINARY_GP_RSP &&
            (w32 ? binary_emit_lea32_reg_base_index_scale_disp(code, D, base,
                                                               index, 1, 0)
                 : binary_emit_lea_reg_base_index_scale_disp(code, D, base,
                                                             index, 1, 0))) {
          return 1;
        }
      }
    }
    if ((in->op == MIR_ADD || is_sub) &&
        in->b.kind == MIR_OPK_IMM && in->b.imm >= -2147483647LL &&
        in->b.imm <= 2147483647LL && !operand_in_phys(fn, &in->a, D)) {
      BinaryGpRegister ra;
      long long disp = is_sub ? -in->b.imm : in->b.imm;
      if (operand_gp_reg(fn, &in->a, &ra) && ra != BINARY_GP_RSP &&
          (w32 ? binary_emit_lea32_reg_mem(code, D, ra, (int)disp)
               : binary_emit_lea_reg_mem(code, D, ra, (int)disp))) {
        return 1;
      }
    }
    if (operand_in_phys(fn, &in->b, D)) {
      /* b already occupies the destination register. */
      if (is_sub) {
        /* dst = a - b, b in D: stage a in RAX, subtract D, write back. */
        if (!materialize_into(fn, &in->a, SCRATCH_A)) {
          return 0;
        }
        if (!(w32 ? binary_emit_alu_reg_reg32(code, opc, SCRATCH_A, D)
                  : binary_emit_alu_reg_reg(code, opc, SCRATCH_A, D))) {
          return enc_err(fn, "out of memory in sub");
        }
        return store_from(fn, &in->dst, SCRATCH_A);
      }
      /* commutative: D = D OP a == a OP b. */
      return emit_op_eq(fn, in->op, opc, D, &in->a, in->width);
    }
    /* b does not alias D: place a in D, then D OP= b. */
    if (!operand_in_phys(fn, &in->a, D) &&
        !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    return emit_op_eq(fn, in->op, opc, D, &in->b, in->width);
  }

  /* Spilled destination: compute in RAX (no allocatable reg aliases it), store. */
  if (!materialize_into(fn, &in->a, SCRATCH_A) ||
      !emit_op_eq(fn, in->op, opc, SCRATCH_A, &in->b, in->width)) {
    return 0;
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_imul(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int b_imm32 = in->b.kind == MIR_OPK_IMM &&
                code_generator_binary_immediate_fits_signed_32(in->b.imm);
  BinaryGpRegister D;

  if (in->width == 4) {
    int ok;
    int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
    BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
    BinaryGpRegister stage = (target == SCRATCH_A) ? SCRATCH_B : SCRATCH_A;
    if (b_imm32) {
      BinaryGpRegister areg = value_reg(fn, &in->a, stage, &ok);
      BinaryGpRegister mul_scratch =
          (areg == stage) ? ((stage == SCRATCH_A) ? SCRATCH_B : SCRATCH_A)
                          : stage;
      if (!ok || !binary_emit_imul_reg_reg_imm32_scratch_w32(
                     code, target, areg, (uint32_t)in->b.imm,
                     mul_scratch != target, mul_scratch)) {
        return enc_err(fn, "out of memory in imul32 imm");
      }
    } else if (dst_in_reg && operand_in_phys(fn, &in->b, target)) {
      BinaryGpRegister areg = value_reg(fn, &in->a, stage, &ok);
      if (!ok || !binary_emit_imul_reg_reg32(code, target, areg)) {
        return enc_err(fn, "out of memory in imul32");
      }
    } else {
      BinaryGpRegister breg;
      if (!operand_in_phys(fn, &in->a, target) &&
          !materialize_into(fn, &in->a, target)) {
        return 0;
      }
      breg = value_reg(fn, &in->b, stage, &ok);
      if (!ok || !binary_emit_imul_reg_reg32(code, target, breg)) {
        return enc_err(fn, "out of memory in imul32");
      }
    }
    if (!dst_in_reg) {
      return store_from(fn, &in->dst, SCRATCH_A);
    }
    return 1;
  }

  if (dst_is_reg(fn, &in->dst, &D)) {
    int ok;
    if (b_imm32) {
      /* D = a * imm: three-operand imul reads a, writes D (a may equal D).
       * Hand over the scratch that staging `a` did not use, so the shift-and-add
       * expansions stay available when D and a are the same register. */
      BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
      BinaryGpRegister scratch = (areg == SCRATCH_A) ? SCRATCH_B : SCRATCH_A;
      if (!ok || !binary_emit_imul_reg_reg_imm32_scratch(
                     code, D, areg, (uint32_t)in->b.imm, 1, scratch)) {
        return enc_err(fn, "out of memory in imul imm");
      }
      return 1;
    }
    if (operand_in_phys(fn, &in->b, D)) {
      /* D holds b; D *= a (imul is commutative). */
      BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
      if (!ok || !binary_emit_imul_reg_reg(code, D, areg)) {
        return enc_err(fn, "out of memory in imul");
      }
      return 1;
    }
    if (!operand_in_phys(fn, &in->a, D) &&
        !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_A, &ok);
    if (!ok || !binary_emit_imul_reg_reg(code, D, breg)) {
      return enc_err(fn, "out of memory in imul");
    }
    return 1;
  }

  /* Spilled destination. */
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  if (b_imm32) {
    if (!binary_emit_imul_reg_reg_imm32(code, SCRATCH_A, SCRATCH_A,
                                        (uint32_t)in->b.imm)) {
      return enc_err(fn, "out of memory in imul imm");
    }
  } else {
    int ok;
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &ok);
    if (!ok || !binary_emit_imul_reg_reg(code, SCRATCH_A, breg)) {
      return enc_err(fn, "out of memory in imul");
    }
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

/* dst = a / b (quotient) or a % b (remainder when in->cc != 0). Signedness is
 * in->is_unsigned (the dividend's type): signed uses CQO + IDIV, unsigned uses
 * XOR(RDX) + DIV. Always 64-bit on the sign/zero-extended operands, which gives
 * the same result as a narrower divide. The dividend goes in RAX, RDX is the
 * high half, so the divisor must be staged out of RAX/RDX (now allocatable)
 * into a scratch register BEFORE the dividend is loaded into RAX. */
static int encode_div(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int rok;
  /* Resolve the divisor first (it might currently live in RAX or RDX, which the
   * dividend/high-half are about to overwrite); force it into SCRATCH_B then. */
  BinaryGpRegister divisor = value_reg(fn, &in->b, SCRATCH_B, &rok);
  if (!rok) {
    return 0;
  }
  if (divisor == BINARY_GP_RAX || divisor == BINARY_GP_RDX) {
    if (!binary_emit_mov_reg_reg(code, SCRATCH_B, divisor)) {
      return enc_err(fn, "out of memory staging divisor");
    }
    divisor = SCRATCH_B;
  }
  if (!materialize_into(fn, &in->a, BINARY_GP_RAX)) {
    return 0;
  }
  if (in->is_unsigned) {
    if (!binary_emit_xor_reg_reg32(code, BINARY_GP_RDX) ||
        !binary_emit_div_reg(code, divisor)) {
      return enc_err(fn, "out of memory in div");
    }
  } else {
    /* A constant divisor that is not -1 cannot reach the overflow case, so it
     * keeps the bare IDIV; anything else takes the guarded form. */
    int needs_guard = !(in->b.kind == MIR_OPK_IMM && in->b.imm != -1);
    if (needs_guard) {
      if (!binary_emit_idiv_wrapping(code, divisor)) {
        return enc_err(fn, "out of memory in idiv");
      }
    } else if (!binary_emit_cqo(code) || !binary_emit_idiv_reg(code, divisor)) {
      return enc_err(fn, "out of memory in idiv");
    }
  }
  BinaryGpRegister result = in->cc ? BINARY_GP_RDX : BINARY_GP_RAX;
  return store_from(fn, &in->dst, result);
}

/* dst = high 64 bits of (a * b). The multiplicand goes in RAX; the one-operand
 * mul/imul writes the full 128-bit product to RDX:RAX and we keep RDX. b is the
 * magic constant (IMM) or a register staged out of RAX/RDX (now allocatable)
 * into SCRATCH_B before RAX is loaded. is_unsigned selects mul. */
static int encode_mulhi(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister mreg;
  if (in->b.kind == MIR_OPK_IMM) {
    if (!binary_emit_mov_reg_imm64(code, SCRATCH_B, (uint64_t)in->b.imm)) {
      return enc_err(fn, "out of memory in mulhi imm");
    }
    mreg = SCRATCH_B;
  } else {
    int rok;
    mreg = value_reg(fn, &in->b, SCRATCH_B, &rok);
    if (!rok) {
      return 0;
    }
    if (mreg == BINARY_GP_RAX || mreg == BINARY_GP_RDX) {
      if (!binary_emit_mov_reg_reg(code, SCRATCH_B, mreg)) {
        return enc_err(fn, "out of memory staging multiplier");
      }
      mreg = SCRATCH_B;
    }
  }
  if (!materialize_into(fn, &in->a, BINARY_GP_RAX)) {
    return 0;
  }
  if (in->is_unsigned ? !binary_emit_mul_reg(code, mreg)
                      : !binary_emit_imul_reg(code, mreg)) {
    return enc_err(fn, "out of memory in mulhi");
  }
  return store_from(fn, &in->dst, BINARY_GP_RDX);
}

static int encode_shift(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  unsigned char sub = (in->op == MIR_SHL) ? 4 : (in->op == MIR_SHR) ? 5 : 7;
  BinaryGpRegister D;
  int dst_reg = dst_is_reg(fn, &in->dst, &D);
  BinaryGpRegister work = dst_reg ? D : SCRATCH_A;

  if (in->b.kind == MIR_OPK_IMM) {
    if ((dst_reg && !operand_in_phys(fn, &in->a, D) &&
         !materialize_into(fn, &in->a, work)) ||
        (!dst_reg && !materialize_into(fn, &in->a, work))) {
      return 0;
    }
    if (!binary_emit_shift_reg_imm8(code, sub, work,
                                    (unsigned char)(in->b.imm & 63))) {
      return enc_err(fn, "out of memory in shift imm");
    }
    return dst_reg ? 1 : store_from(fn, &in->dst, work);
  }
  /* Variable count: it must end up in CL (RCX). RCX is now allocatable, so the
   * value `a` may itself live in RCX, and the count may live anywhere. Stage the
   * value into SCRATCH_A first (reading it from wherever, RCX included), then
   * move the count into RCX (the value is already safe in SCRATCH_A), shift, and
   * store. The MIR layer marks a variable shift as an RCX clobber. */
  int ok;
  BinaryGpRegister cnt = value_reg(fn, &in->b, SCRATCH_B, &ok);
  if (!ok) {
    return 0;
  }
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  if (cnt != BINARY_GP_RCX &&
      !binary_emit_mov_reg_reg(code, BINARY_GP_RCX, cnt)) {
    return enc_err(fn, "out of memory moving shift count");
  }
  if (!binary_emit_shift_reg_cl(code, sub, SCRATCH_A)) {
    return enc_err(fn, "out of memory in shift");
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_setcc(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  /* The compare reads a and b without modifying them, so use their own
   * registers directly. setcc requires an 8-bit-addressable low reg, so it
   * always targets AL and the result is zero-extended into RAX, then stored. */
  int ok;
  BinaryGpRegister cbase;
  int cdisp;
  BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  /* A 4-byte (int32/uint32) compare must be 32-bit: MIR computes in 64-bit, so a
   * narrow operand can carry garbage in its high 32 bits; a 32-bit cmp ignores
   * them (the low 32 bits are the true value). An immediate is staged into a
   * register first since the 64-bit cmp-imm would sign-extend it. */
  if (in->width == 4) {
    /* A 32-bit immediate folds straight into the 32-bit cmp (no scratch reg);
     * the low 32 bits are the int32/uint32 constant being compared. */
    if (in->b.kind == MIR_OPK_IMM) {
      if (!binary_emit_cmp_reg_imm_w32(code, areg, (uint32_t)in->b.imm)) {
        return enc_err(fn, "out of memory in cmp32 imm");
      }
    } else if (gp_home_mem(fn, &in->b, &cbase, &cdisp)) {
      if (!binary_emit_alu_reg_mem(code, 0x39, areg, cbase, cdisp, 4)) {
        return enc_err(fn, "out of memory in cmp32 mem");
      }
    } else {
      BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &ok);
      if (!ok || !binary_emit_cmp_reg_reg32(code, areg, breg)) {
        return enc_err(fn, "out of memory in cmp32");
      }
    }
  } else if (in->b.kind == MIR_OPK_IMM &&
             code_generator_binary_immediate_fits_signed_32(in->b.imm)) {
    if (!binary_emit_cmp_reg_imm32(code, areg, (uint32_t)in->b.imm)) {
      return enc_err(fn, "out of memory in cmp imm");
    }
  } else if (gp_home_mem(fn, &in->b, &cbase, &cdisp)) {
    if (!binary_emit_alu_reg_mem(code, 0x39, areg, cbase, cdisp, 8)) {
      return enc_err(fn, "out of memory in cmp mem");
    }
  } else {
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &ok);
    if (!ok || !binary_emit_cmp_reg_reg(code, areg, breg)) {
      return enc_err(fn, "out of memory in cmp");
    }
  }
  if (!binary_emit_setcc_reg8(code, in->cc, BINARY_GP_RAX) ||
      !binary_emit_movzx_eax_al(code)) {
    return enc_err(fn, "out of memory in setcc");
  }
  /* setcc/movzx target AL/EAX specifically; the allocator marks SETCC as an RAX
   * clobber so no live value sits in RAX across it. */
  return store_from(fn, &in->dst, BINARY_GP_RAX);
}

/* dst <- extend(low `width` bytes of a) per signedness. Signed extensions emit
 * directly into the destination register (the reg-reg encoders always emit and
 * are correct in place). Unsigned narrowings and spilled destinations use the
 * RAX path with the dedicated AL/AX/EAX encoders (which always emit, unlike
 * mov_reg_reg32 which is a no-op when dst==src and would skip the zeroing). */
static int encode_extend(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int signed_ext = (in->op == MIR_MOVSX);
  BinaryGpRegister D;

  if (dst_is_reg(fn, &in->dst, &D)) {
    int ok;
    BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
    if (!ok) {
      return 0;
    }
    int done = 1;
    /* The width-4 unsigned form must be the ALWAYS-emitting 32-bit mov: the
     * canonicalizing `mov D32, D32` has dst == src, and the skip-when-equal
     * mov_reg_reg32 would silently drop the zero-extension. */
    switch (in->width) {
    case 4:
      done = signed_ext ? binary_emit_movsxd_reg_reg32(code, D, areg)
                        : binary_emit_movzx_reg_reg32(code, D, areg);
      break;
    case 2:
      done = signed_ext ? binary_emit_movsx_reg_reg16(code, D, areg)
                        : binary_emit_movzx_reg_reg16(code, D, areg);
      break;
    case 1:
      done = signed_ext ? binary_emit_movsx_reg_reg8(code, D, areg)
                        : binary_emit_movzx_reg_reg8(code, D, areg);
      break;
    default: return enc_err(fn, "bad extend width");
    }
    return done ? 1 : enc_err(fn, "out of memory in extend");
  }

  /* Scratch path (spilled destination): extend in SCRATCH_A using the general
   * reg-reg forms (no RAX dependency), then store. */
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  BinaryGpRegister S = SCRATCH_A;
  int ok = 1;
  switch (in->width) {
  case 4:
    ok = signed_ext ? binary_emit_movsxd_reg_reg32(code, S, S)
                    : binary_emit_movzx_reg_reg32(code, S, S);
    break;
  case 2:
    ok = signed_ext ? binary_emit_movsx_reg_reg16(code, S, S)
                    : binary_emit_movzx_reg_reg16(code, S, S);
    break;
  case 1:
    ok = signed_ext ? binary_emit_movsx_reg_reg8(code, S, S)
                    : binary_emit_movzx_reg_reg8(code, S, S);
    break;
  default:
    return enc_err(fn, "bad extend width");
  }
  if (!ok) {
    return enc_err(fn, "out of memory in extend");
  }
  return store_from(fn, &in->dst, S);
}

/* ---- float (XMM) operand plumbing -------------------------------------- */

static int dst_is_xmm_reg(MirFunction *fn, const MirOperand *dst,
                          BinaryXmmRegister *D_out) {
  if (dst->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->rclass == MIR_RC_GP) {
      return 0; /* not an XMM register; xmm_store crosses the bank */
    }
    if (v->in_register) {
      *D_out = (BinaryXmmRegister)v->phys;
      return 1;
    }
    return 0;
  }
  if (dst->kind == MIR_OPK_PHYS) {
    *D_out = (BinaryXmmRegister)dst->phys;
    return 1;
  }
  return 0;
}

static int xmm_operand_in_phys(MirFunction *fn, const MirOperand *op,
                               BinaryXmmRegister D) {
  if (op->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[op->vreg];
    return v->in_register && (BinaryXmmRegister)v->phys == D;
  }
  if (op->kind == MIR_OPK_PHYS) {
    return (BinaryXmmRegister)op->phys == D;
  }
  return 0;
}

/* xmm dst <- xmm src, scalar (movss for width 4, movsd for width 8). */
static int xmm_mov(BinaryCodeBuffer *code, BinaryXmmRegister dst,
                   BinaryXmmRegister src, int width) {
  if (dst == src) {
    return 1;
  }
  /* movaps dst, src (0F 28 /r). A reg-reg movss/movsd MERGES into the
   * destination's upper lanes, creating a false dependency on its prior value
   * and defeating the rename-stage move-elimination; movaps copies the whole
   * register, so the copy is dependency-free and typically eliminated. We only
   * use the low lane, so copying all 128 bits is semantically irrelevant. */
  (void)width;
  return binary_emit_rex(code, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(code, 0x0F) &&
         binary_code_buffer_append_u8(code, 0x28) &&
         binary_code_buffer_append_u8(
             code, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* Load a float immediate's raw bits into an XMM register via a GP staging reg. */
static int xmm_load_fimm(MirFunction *fn, uint64_t bits,
                         BinaryXmmRegister target, int width) {
  BinaryCodeBuffer *code = &fn->context->code;
  /* +0.0 is the one constant with no bits to move: pxor is shorter, needs no
   * general register, and breaks the dependence on the target's old value.
   * Comparing against zero and zero-initializing an accumulator are the two
   * places it turns up, and both are common in float code. */
  if (bits == 0) {
    return binary_emit_pxor_xmm_xmm(code, target, target);
  }
  if (width == 4) {
    return binary_emit_mov_reg_imm32_zero_extend(code, SCRATCH_A,
                                                 (uint32_t)bits) &&
           binary_emit_movd_xmm_reg(code, target, SCRATCH_A);
  }
  return binary_emit_mov_reg_imm64(code, SCRATCH_A, bits) &&
         binary_emit_movq_xmm_reg(code, target, SCRATCH_A);
}

/* Park (save=1) or recover (save=0) the registers a preserving call promises to
 * leave alone: RAX and the volatile XMM lanes. These are the registers the
 * allocator hands to values that span only such calls, which is what lets a
 * checked inner loop keep its working set, the loaded element, the float
 * accumulator, out of memory. The cost lands entirely on a path a correct
 * program does not take.
 *
 * Frame slots rather than pushes: the call's stack arguments and shadow space
 * are addressed off rsp, and moving rsp between writing them and making the
 * call would put both in the wrong place. */
static int mir_emit_preserve_volatiles(MirFunction *fn, const MirInst *in,
                                       int save) {
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister base = frame_base(fn);
  if (in->preserves_rax && fn->preserve_slot > 0) {
    if (save) {
      if (!binary_emit_mov_mem_reg(code, base,
                                   frame_disp(fn, -fn->preserve_slot),
                                   BINARY_GP_RAX)) {
        return 0;
      }
    } else if (!binary_emit_mov_reg_mem(code, BINARY_GP_RAX, base,
                                        frame_disp(fn, -fn->preserve_slot))) {
      return 0;
    }
  }
  if (!in->preserves_xmm || fn->preserve_xmm_slot <= 0) {
    return 1;
  }
  for (size_t i = 0; i < MIR_XMM_POOL_COUNT; i++) {
    int disp = frame_disp(fn, -fn->preserve_xmm_slot + (int)i * 8);
    /* movsd, the widest scalar MIR keeps in an XMM lane. */
    if (!simd_emit_prefixed_xmm_mem_disp(code, 0xF2, save ? 0x11 : 0x10,
                                         MIR_XMM_POOL[i], base, disp)) {
      return 0;
    }
  }
  return 1;
}

/* Float spill slots are GP-width stack homes; reload/store via a GP reg so no
 * scalar-memory SSE encoders are needed. */
static int xmm_spill_load(MirFunction *fn, const MirVreg *v,
                          BinaryXmmRegister target) {
  unsigned char prefix =
      (v->width == 16) ? 0x66 : ((v->width == 4) ? 0xF3 : 0xF2);
  return simd_emit_prefixed_xmm_mem_disp(&fn->context->code, prefix, 0x10,
                                         target, frame_base(fn),
                                         frame_disp(fn, -v->spill_offset));
}

static int xmm_spill_store(MirFunction *fn, const MirVreg *v,
                           BinaryXmmRegister src) {
  unsigned char prefix =
      (v->width == 16) ? 0x66 : ((v->width == 4) ? 0xF3 : 0xF2);
  return simd_emit_prefixed_xmm_mem_disp(&fn->context->code, prefix, 0x11, src,
                                         frame_base(fn),
                                         frame_disp(fn, -v->spill_offset));
}

/* Resolve a float operand to the XMM register holding its value, materializing
 * a spill/immediate into `scratch`. */
static BinaryXmmRegister xmm_value(MirFunction *fn, const MirOperand *op,
                                   BinaryXmmRegister scratch, int width,
                                   int *ok) {
  *ok = 1;
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      return (BinaryXmmRegister)v->phys;
    }
    *ok = xmm_spill_load(fn, v, scratch);
    return scratch;
  }
  case MIR_OPK_PHYS:
    return (BinaryXmmRegister)op->phys;
  case MIR_OPK_FIMM:
    *ok = xmm_load_fimm(fn, (uint64_t)op->imm, scratch, width);
    return scratch;
  default:
    *ok = enc_err(fn, "unsupported float operand");
    return scratch;
  }
}

static int materialize_xmm_into(MirFunction *fn, const MirOperand *op,
                                BinaryXmmRegister target, int width) {
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      return xmm_mov(&fn->context->code, target, (BinaryXmmRegister)v->phys,
                     width);
    }
    return xmm_spill_load(fn, v, target);
  }
  case MIR_OPK_PHYS:
    return xmm_mov(&fn->context->code, target, (BinaryXmmRegister)op->phys,
                   width);
  case MIR_OPK_FIMM:
    return xmm_load_fimm(fn, (uint64_t)op->imm, target, width);
  default:
    return enc_err(fn, "unsupported float operand in materialize");
  }
}

static int xmm_store(MirFunction *fn, const MirOperand *dst,
                     BinaryXmmRegister src, int width) {
  switch (dst->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->in_register) {
      return xmm_mov(&fn->context->code, (BinaryXmmRegister)v->phys, src, width);
    }
    return xmm_spill_store(fn, v, src);
  }
  case MIR_OPK_PHYS:
    return xmm_mov(&fn->context->code, (BinaryXmmRegister)dst->phys, src, width);
  default:
    return enc_err(fn, "unsupported float destination");
  }
}

/* target OP= src for a scalar float op. */
static int sse_arith(MirFunction *fn, MirOpcode op, int width,
                     BinaryXmmRegister target, BinaryXmmRegister src) {
  BinaryCodeBuffer *code = &fn->context->code;
  if (width == 4) {
    switch (op) {
    case MIR_FADD: return binary_emit_addss_xmm_xmm(code, target, src);
    case MIR_FSUB: return binary_emit_subss_xmm_xmm(code, target, src);
    case MIR_FMUL: return binary_emit_mulss_xmm_xmm(code, target, src);
    case MIR_FDIV: return binary_emit_divss_xmm_xmm(code, target, src);
    default: return 0;
    }
  }
  switch (op) {
  case MIR_FADD: return binary_emit_addsd_xmm_xmm(code, target, src);
  case MIR_FSUB: return binary_emit_subsd_xmm_xmm(code, target, src);
  case MIR_FMUL: return binary_emit_mulsd_xmm_xmm(code, target, src);
  case MIR_FDIV: return binary_emit_divsd_xmm_xmm(code, target, src);
  default: return 0;
  }
}

/* Scalar float arithmetic in the VEX 3-operand form: v<op>s{s,d} D, A, B.
 * The legacy SSE forms are two-address (addsd D,b computes D = D OP b), which
 * forced a movaps copy of A into D whenever the allocator could not coalesce
 * them; the VEX form names all three registers, so no copy exists to elide.
 * VEX.128/LIG writes zero the upper lanes, so mixing with the surrounding
 * legacy-SSE code carries no transition penalty. */
/* One VEX.128 xmm three-operand instruction: reg = dst, vvvv = a, rm = b. */
static int vex_xmm_3op(MirFunction *fn, int pp, unsigned char opcode,
                       BinaryXmmRegister dst, BinaryXmmRegister a,
                       BinaryXmmRegister b) {
  BinaryCodeBuffer *code = &fn->context->code;
  return wcs_vex3(code, 1, pp, 0, 0, (int)dst, (int)b, (int)a) &&
         binary_code_buffer_append_u8(code, opcode) &&
         binary_code_buffer_append_u8(
             code, (unsigned char)(0xC0 | ((dst & 7) << 3) | (b & 7)));
}

static int vex_scalar_arith(MirFunction *fn, MirOpcode op, int width,
                            BinaryXmmRegister dst, BinaryXmmRegister a,
                            BinaryXmmRegister b) {
  unsigned char opcode;
  switch (op) {
  case MIR_FADD: opcode = 0x58; break;
  case MIR_FSUB: opcode = 0x5C; break;
  case MIR_FMUL: opcode = 0x59; break;
  case MIR_FDIV: opcode = 0x5E; break;
  default: return 0;
  }
  /* pp: F3 (ss) at width 4, F2 (sd) at width 8, 66 (pd, both lanes) at 16. */
  return vex_xmm_3op(fn, width == 16 ? 1 : (width == 4 ? 2 : 3), opcode, dst,
                     a, b);
}

static int encode_fbinop(MirFunction *fn, const MirInst *in) {
  int w = in->width;
  BinaryXmmRegister D;
  int ok;

  int dst_in_reg = dst_is_xmm_reg(fn, &in->dst, &D);
  if (!dst_in_reg) {
    D = FSCRATCH_A;
  }
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
  if (!ok) {
    return enc_err(fn, "out of memory in float op");
  }
  BinaryXmmRegister bval = xmm_value(fn, &in->b, FSCRATCH_B, w, &ok);
  if (!ok || !vex_scalar_arith(fn, in->op, w, D, aval, bval)) {
    return enc_err(fn, "out of memory in float op");
  }
  return dst_in_reg ? 1 : xmm_store(fn, &in->dst, FSCRATCH_A, w);
}

/* int -> float: dst(xmm) = cvtsi2sd/ss(a gp). in->width is the float width. */
static int encode_cvtsi2f(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_A;
  int done;
  if (in->is_unsigned) {
    /* Unsigned source: the machine's conversion is signed, so a value with bit
     * 63 set has to be halved, converted, and doubled. */
    done = code_generator_binary_emit_unsigned_int_to_float(
        fn->context, in->width == 4 ? 32 : 64, target, areg, SCRATCH_A,
        SCRATCH_B);
  } else {
    done = (in->width == 4) ? binary_emit_cvtsi2ss_xmm_reg(code, target, areg)
                            : binary_emit_cvtsi2sd_xmm_reg(code, target, areg);
  }
  if (!done) {
    return enc_err(fn, "out of memory in cvtsi2f");
  }
  return (target == FSCRATCH_A) ? xmm_store(fn, &in->dst, FSCRATCH_A, in->width)
                                : 1;
}

/* float -> int (truncating): dst(gp) = cvtt(a xmm). in->width is float width. */
static int encode_cvtf2si(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryXmmRegister xval = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &ok);
  if (!ok) {
    return 0;
  }
  BinaryGpRegister D;
  BinaryGpRegister target = dst_is_reg(fn, &in->dst, &D) ? D : SCRATCH_A;
  int done;
  if (in->is_unsigned) {
    /* uint64 target: signed truncation answers its sentinel from 2^63 up, so
     * bias the value down, truncate, and put the top bit back. */
    done = code_generator_binary_emit_float_to_unsigned_int(
        fn->context, in->width == 4 ? 32 : 64, target, xval, SCRATCH_B,
        FSCRATCH_B);
  } else {
    done = (in->width == 4)
               ? binary_emit_cvttss2si_reg_xmm(code, target, xval)
               : binary_emit_cvttsd2si_reg_xmm(code, target, xval);
  }
  if (!done) {
    return enc_err(fn, "out of memory in cvtf2si");
  }
  return (target == SCRATCH_A) ? store_from(fn, &in->dst, SCRATCH_A) : 1;
}

/* float -> float width change: in->width is the destination float width. */
static int encode_cvtf2f(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  int srcw = (in->width == 8) ? 4 : 8;
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, srcw, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_B;
  int done = (in->width == 8) ? binary_emit_cvtss2sd_xmm_xmm(code, target, aval)
                              : binary_emit_cvtsd2ss_xmm_xmm(code, target, aval);
  if (!done) {
    return enc_err(fn, "out of memory in cvtf2f");
  }
  return (target == FSCRATCH_B) ? xmm_store(fn, &in->dst, FSCRATCH_B, in->width)
                                : 1;
}

static int encode_cvtph2ps(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, 4, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_B;
  int done = wcs_avx_vcvtph2ps_xmm(code, (int)target, (int)aval);
  if (!done) {
    return enc_err(fn, "out of memory in cvtph2ps");
  }
  return (target == FSCRATCH_B) ? xmm_store(fn, &in->dst, FSCRATCH_B, 4)
                                : 1;
}

static int encode_cvtps2ph(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, 4, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_B;
  int done = wcs_avx_vcvtps2ph_xmm(code, (int)target, (int)aval, 0);
  if (!done) {
    return enc_err(fn, "out of memory in cvtps2ph");
  }
  return (target == FSCRATCH_B) ? xmm_store(fn, &in->dst, FSCRATCH_B, 4)
                                : 1;
}

/* Load `size` bytes from [base (+ index*scale) + disp] straight into `target`,
 * sign/zero-extending to 64 bits in the SAME instruction (movsxd/movsx/movzx
 * from memory, or a plain mov for 8 bytes / unsigned 4). This is the general
 * shape win: every signed sub-word array read drops a separate movsx, and any
 * load whose destination already has a register skips the scratch bounce. */
static int emit_ext_load(BinaryCodeBuffer *code, BinaryGpRegister target,
                         BinaryGpRegister base, int has_index,
                         BinaryGpRegister index, int scale, int disp, int size,
                         int is_signed) {
  int rexw = 0, has2 = 0;
  unsigned char op1 = 0, op2 = 0;
  switch (size) {
  case 1:
    rexw = 1;
    has2 = 1;
    op1 = 0x0F;
    op2 = is_signed ? 0xBE : 0xB6; /* movsx/movzx r64, m8 */
    break;
  case 2:
    rexw = 1;
    has2 = 1;
    op1 = 0x0F;
    op2 = is_signed ? 0xBF : 0xB7; /* movsx/movzx r64, m16 */
    break;
  case 4:
    if (is_signed) {
      rexw = 1;
      op1 = 0x63; /* movsxd r64, m32 */
    } else {
      op1 = 0x8B; /* mov r32, m32 (zero-extends to 64) */
    }
    break;
  case 8:
    rexw = 1;
    op1 = 0x8B; /* mov r64, m64 */
    break;
  default:
    return 0;
  }
  if (has_index) {
    return binary_emit_memory_access_sib(code, 0, rexw, op1, has2, op2, target,
                                         base, index, scale, disp);
  }
  return binary_emit_memory_access_ex(code, 0, rexw, op1, has2, op2, target,
                                      base, disp);
}

static int encode_mov(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;

  /* Float moves: load/store via a GP staging reg (mov [mem]->RAX, movq/movd to
   * xmm and back), and reg-reg / float-immediate copies. */
  if (in->is_float) {
    int ok;
    int w = in->width;
    /* movss / movsd; movupd for a 16-byte pair (unaligned-safe). */
    unsigned char prefix = (w == 16) ? 0x66 : ((w == 4) ? 0xF3 : 0xF2);
    if (in->a.kind == MIR_OPK_MEM) {
      /* float LOAD: movss/movsd dst <- [base + disp], straight into dst's
       * register. This path has no scaled-index form; the lowering never builds
       * one, and mir_fold_address_offsets refuses to create one. */
      if (in->a.mem.index != MIR_VREG_NONE) {
        return enc_err(fn, "scaled index in a float load");
      }
      MirOperand base = mir_op_vreg(in->a.mem.base);
      BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok);
      if (!ok) {
        return 0;
      }
      BinaryXmmRegister target;
      int direct = dst_is_xmm_reg(fn, &in->dst, &target);
      if (!direct) {
        target = FSCRATCH_A;
      }
      if (!simd_emit_prefixed_xmm_mem_disp(&ctx->code, prefix, 0x10, target,
                                           addr, in->a.mem.disp)) {
        return enc_err(fn, "out of memory in float load");
      }
      return direct ? 1 : xmm_store(fn, &in->dst, FSCRATCH_A, w);
    }
    if (in->dst.kind == MIR_OPK_MEM) {
      /* float STORE: movss/movsd [base + disp] <- a. */
      if (in->dst.mem.index != MIR_VREG_NONE) {
        return enc_err(fn, "scaled index in a float store");
      }
      MirOperand base = mir_op_vreg(in->dst.mem.base);
      BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok);
      if (!ok) {
        return 0;
      }
      BinaryXmmRegister val = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
      if (!ok) {
        return 0;
      }
      if (!simd_emit_prefixed_xmm_mem_disp(&ctx->code, prefix, 0x11, val, addr,
                                           in->dst.mem.disp)) {
        return enc_err(fn, "out of memory in float store");
      }
      return 1;
    }
    BinaryXmmRegister sval = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
    if (!ok) {
      return 0;
    }
    return xmm_store(fn, &in->dst, sval, w);
  }

  /* LOAD: dst <- [base (+ index*scale + disp)], width bytes. Load straight into
   * dst's register (extending in the same instruction); only bounce through
   * SCRATCH_A when dst is spilled. */
  if (in->a.kind == MIR_OPK_MEM) {
    int ok;
    int is_signed = !in->is_unsigned;
    BinaryGpRegister D;
    int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
    BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
    if (in->a.mem.index != MIR_VREG_NONE) {
      MirOperand bop = mir_op_vreg(in->a.mem.base);
      MirOperand iop = mir_op_vreg(in->a.mem.index);
      /* Stage the spilled operands clear of the load target, of each other, and
       * of whatever register the other one is already resident in. Avoiding
       * only the target was not enough: a spilled base stages into SCRATCH_B,
       * and a target in RDX then sent the index to SCRATCH_B as well, so base
       * and index read one register. */
      BinaryGpRegister taken[4];
      int tn = 0;
      mir_note_fixed_reg(fn, &bop, taken, &tn);
      mir_note_fixed_reg(fn, &iop, taken, &tn);
      /* The load writes `target` only after reading base and index, so it is a
       * legal staging register once neither operand is resident in it. */
      BinaryGpRegister vouch[1];
      int vn = mir_reg_in(target, taken, tn) ? 0 : 1;
      vouch[0] = target;
      size_t idx = (size_t)(in - fn->insns);
      BinaryGpRegister base_scratch, index_scratch;
      if (!mir_pick_scratch(fn, idx, SCRATCH_B, taken, tn, vouch, vn,
                            &base_scratch)) {
        return enc_err(fn, "no free scratch register for a scaled load base");
      }
      BinaryGpRegister base_reg = value_reg(fn, &bop, base_scratch, &ok);
      if (!ok) {
        return 0;
      }
      taken[tn++] = base_reg;
      if (!mir_pick_scratch(fn, idx, BINARY_GP_RDX, taken, tn, vouch, vn,
                            &index_scratch)) {
        return enc_err(fn, "no free scratch register for a scaled load index");
      }
      BinaryGpRegister index_reg = value_reg(fn, &iop, index_scratch, &ok);
      if (!ok) {
        return 0;
      }
      if (!emit_ext_load(&ctx->code, target, base_reg, 1, index_reg,
                         in->a.mem.scale, in->a.mem.disp, in->width,
                         is_signed)) {
        return enc_err(fn, "out of memory in scaled load");
      }
    } else {
      MirOperand base = mir_op_vreg(in->a.mem.base);
      BinaryGpRegister base_reg = value_reg(fn, &base, SCRATCH_B, &ok);
      if (!ok) {
        return 0;
      }
      if (!emit_ext_load(&ctx->code, target, base_reg, 0, BINARY_GP_RSP, 1,
                         in->a.mem.disp, in->width, is_signed)) {
        return enc_err(fn, "out of memory in load");
      }
    }
    if (!dst_in_reg) {
      return store_from(fn, &in->dst, SCRATCH_A);
    }
    return 1;
  }

  /* STORE: [base (+ index*scale + disp)] <- a, width bytes. */
  if (in->dst.kind == MIR_OPK_MEM) {
    int ok1, ok2;
    int scalar_w = (in->width == 1 || in->width == 2 || in->width == 4 ||
                    in->width == 8);
    if (in->a.kind == MIR_OPK_IMM && scalar_w) {
      int has_index = in->dst.mem.index != MIR_VREG_NONE;
      MirOperand bop = mir_op_vreg(in->dst.mem.base);
      BinaryGpRegister base_reg = value_reg(fn, &bop, SCRATCH_B, &ok1);
      BinaryGpRegister index_reg = BINARY_GP_RAX;
      if (!ok1) {
        return 0;
      }
      if (has_index) {
        MirOperand iop = mir_op_vreg(in->dst.mem.index);
        index_reg = value_reg(fn, &iop, SCRATCH_A, &ok2);
        if (!ok2) {
          return 0;
        }
      }
      if (binary_emit_mov_mem_imm_width(&ctx->code, base_reg, has_index,
                                        index_reg, in->dst.mem.scale,
                                        in->dst.mem.disp, in->a.imm,
                                        in->width)) {
        return 1;
      }
    }
    if (in->dst.mem.index != MIR_VREG_NONE) {
      /* One direct SIB `mov [base+idx*scale+disp], val` at every width. A
       * byte store whose value register encodes as 4..7 needs a forced REX so
       * the operand reads SPL..DIL rather than AH..BH.
       * Spilled operands stage through SCRATCH_B / RDX / SCRATCH_A. */
      MirOperand bop = mir_op_vreg(in->dst.mem.base);
      MirOperand iop = mir_op_vreg(in->dst.mem.index);
      /* Three operands stage through three scratches here, so every pick has to
       * clear both the scratches already handed out AND the registers the other
       * operands are already resident in -- staging over a resident value
       * destroys it before its read. Seed the set from all three up front,
       * since base is staged before the value is even looked at. */
      BinaryGpRegister taken[6];
      int tn = 0;
      mir_note_fixed_reg(fn, &bop, taken, &tn);
      mir_note_fixed_reg(fn, &iop, taken, &tn);
      mir_note_fixed_reg(fn, &in->a, taken, &tn);

      size_t idx = (size_t)(in - fn->insns);
      BinaryGpRegister base_scratch, index_scratch, val_scratch;
      if (!mir_pick_scratch(fn, idx, SCRATCH_B, taken, tn, NULL, 0, &base_scratch)) {
        return enc_err(fn, "no free scratch register for a scaled store base");
      }
      BinaryGpRegister base_reg = value_reg(fn, &bop, base_scratch, &ok1);
      if (!ok1) {
        return 0;
      }
      taken[tn++] = base_reg;
      if (!mir_pick_scratch(fn, idx, BINARY_GP_RDX, taken, tn, NULL, 0, &index_scratch)) {
        return enc_err(fn, "no free scratch register for a scaled store index");
      }
      BinaryGpRegister index_reg = value_reg(fn, &iop, index_scratch, &ok2);
      if (!ok2) {
        return 0;
      }
      taken[tn++] = index_reg;
      int scalar_width = (in->width == 1 || in->width == 2 ||
                          in->width == 4 || in->width == 8);
      if (!scalar_width) {
        /* The aggregate path below leas the address into SCRATCH_B after
         * reading base and index, so the value may not live there. */
        taken[tn++] = SCRATCH_B;
      }
      BinaryGpRegister val;
      if (mir_env_addr_store() ||
          !mir_pick_scratch(fn, idx, SCRATCH_A, taken, tn, NULL, 0,
                            &val_scratch)) {
        /* Three operands, two encoder scratches: base and index each took one
         * and RDX was unavailable, so nothing is left to stage the value in.
         * Form the address first instead. lea reads base and index and writes
         * afterwards, so once it retires both staging registers are dead and
         * one of them carries the value; the store drops its index and becomes
         * a plain [addr]. Deep self-recursion expansion reaches this. */
        BinaryGpRegister addr = SCRATCH_B;
        BinaryGpRegister val_stage = SCRATCH_A;
        BinaryGpRegister val_fixed;
        if (mir_operand_fixed_reg(fn, &in->a, &val_fixed) &&
            val_fixed == addr) {
          addr = SCRATCH_A;
          val_stage = SCRATCH_B;
        }
        if (!binary_emit_lea_reg_base_index_scale_disp(
                &ctx->code, addr, base_reg, index_reg, in->dst.mem.scale,
                in->dst.mem.disp)) {
          return enc_err(fn, "out of memory in scaled store address");
        }
        val = value_reg(fn, &in->a, val_stage, &ok1);
        if (!ok1) {
          return 0;
        }
        if (!scalar_width) {
          if (!code_generator_binary_emit_store_to_address(g, ctx, addr,
                                                           in->width, val)) {
            return enc_err(fn, "out of memory in store");
          }
          return 1;
        }
        int prefix66 = in->width == 2;
        int rexw = in->width == 8;
        unsigned char op = in->width == 1 ? 0x88 : 0x89;
        int done =
            (in->width == 1 && val >= BINARY_GP_RSP && val <= BINARY_GP_RDI)
                ? binary_emit_memory_access_ex_forced(&ctx->code, prefix66,
                                                      rexw, op, 0, 0, val, addr,
                                                      0)
                : binary_emit_memory_access_ex(&ctx->code, prefix66, rexw, op,
                                               0, 0, val, addr, 0);
        if (!done) {
          return enc_err(fn, "out of memory in scaled store");
        }
        return 1;
      }
      val = value_reg(fn, &in->a, val_scratch, &ok1);
      if (!ok1) {
        return 0;
      }
      if (scalar_width) {
        int prefix66 = in->width == 2;
        int rexw = in->width == 8;
        unsigned char op = in->width == 1 ? 0x88 : 0x89;
        int done =
            (in->width == 1 && val >= BINARY_GP_RSP && val <= BINARY_GP_RDI)
                ? binary_emit_memory_access_sib_forced(
                      &ctx->code, prefix66, rexw, op, 0, 0, val, base_reg,
                      index_reg, in->dst.mem.scale, in->dst.mem.disp)
                : binary_emit_memory_access_sib(
                      &ctx->code, prefix66, rexw, op, 0, 0, val, base_reg,
                      index_reg, in->dst.mem.scale, in->dst.mem.disp);
        if (!done) {
          return enc_err(fn, "out of memory in scaled store");
        }
        return 1;
      }
      if (!binary_emit_lea_reg_base_index_scale_disp(
              &ctx->code, SCRATCH_B, base_reg, index_reg, in->dst.mem.scale,
              in->dst.mem.disp)) {
        return enc_err(fn, "out of memory in scaled store address");
      }
      if (!code_generator_binary_emit_store_to_address(g, ctx, SCRATCH_B,
                                                       in->width, val)) {
        return enc_err(fn, "out of memory in store");
      }
      return 1;
    }
    MirOperand base = mir_op_vreg(in->dst.mem.base);
    BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok1);
    if (!ok1) {
      return 0;
    }
    BinaryGpRegister val = value_reg(fn, &in->a, SCRATCH_A, &ok2);
    if (!ok2) {
      return 0;
    }
    if (in->width == 1 || in->width == 2 || in->width == 4 || in->width == 8) {
      /* Direct `mov [addr+disp], val` at scalar widths; the displacement folds
       * into the store itself, with no lea detour. */
      int prefix66 = in->width == 2;
      int rexw = in->width == 8;
      unsigned char op = in->width == 1 ? 0x88 : 0x89;
      int done =
          (in->width == 1 && val >= BINARY_GP_RSP && val <= BINARY_GP_RDI)
              ? binary_emit_memory_access_ex_forced(&ctx->code, prefix66, rexw,
                                                    op, 0, 0, val, addr,
                                                    in->dst.mem.disp)
              : binary_emit_memory_access_ex(&ctx->code, prefix66, rexw, op, 0,
                                             0, val, addr, in->dst.mem.disp);
      if (!done) {
        return enc_err(fn, "out of memory in store");
      }
      return 1;
    }
    if (in->dst.mem.disp != 0) {
      /* Aggregate-width store: fold the displacement with a lea and hand the
       * copy to the width-general helper. lea into SCRATCH_B so a base held in
       * a live vreg register is preserved. */
      if (!binary_emit_lea_reg_mem(&ctx->code, SCRATCH_B, addr,
                                   in->dst.mem.disp)) {
        return enc_err(fn, "out of memory in store address");
      }
      addr = SCRATCH_B;
    }
    if (!code_generator_binary_emit_store_to_address(g, ctx, addr, in->width,
                                                     val)) {
      return enc_err(fn, "out of memory in store");
    }
    return 1;
  }

  /* Plain register/immediate move.
   *
   * An immediate goes straight to its destination. Routing it through the
   * scratch register the way the general path does costs an extra instruction
   * at every single constant, and constants are everywhere: loop bounds, zero
   * initializers, small call arguments. */
  if (in->a.kind == MIR_OPK_IMM) {
    BinaryGpRegister D;
    if (dst_is_reg(fn, &in->dst, &D)) {
      if (!binary_emit_mov_reg_imm64(&ctx->code, D, (uint64_t)in->a.imm)) {
        return enc_err(fn, "out of memory in immediate move");
      }
      return 1;
    }
    if (in->dst.kind == MIR_OPK_VREG) {
      /* Spilled destination: store the constant into its home directly, when
       * it fits the sign-extended imm32 the C7 /0 form carries. */
      const MirVreg *v = &fn->vregs[in->dst.vreg];
      if (in->a.imm >= INT32_MIN && in->a.imm <= INT32_MAX) {
        if (!binary_emit_mov_mem_imm32(&ctx->code, frame_base(fn),
                                       frame_disp(fn, -spill_off(v)),
                                       (int32_t)in->a.imm)) {
          return enc_err(fn, "out of memory in immediate spill store");
        }
        return 1;
      }
    }
  }

  int ok;
  BinaryGpRegister src = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  return store_from(fn, &in->dst, src);
}

/* ---- prologue / epilogue ------------------------------------------------ */

static int mir_has_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    /* MIR_TRAP also emits calls (puts/exit), so it needs outgoing shadow space
     * reserved at the bottom of the frame just like a MIR_CALL. */
    if (fn->insns[i].op == MIR_CALL ||
        fn->insns[i].op == MIR_CALL_INDIRECT ||
        fn->insns[i].op == MIR_SYSCALL ||
        fn->insns[i].op == MIR_TRAP) {
      return 1;
    }
  }
  return 0;
}

static int mir_layout_frame(MirFunction *fn) {
  /* Spill slots occupy [rbp-8 .. rbp-spill_bytes]; saved nonvolatiles sit
   * below them. If the function makes calls, 32 bytes of Win64 shadow space are
   * reserved at the very bottom of the frame (where rsp points), so an outgoing
   * call has shadow space and a 16-aligned rsp without adjusting rsp in-body.
   * frame_size is 16-aligned. */
  BinaryFunctionContext *ctx = fn->context;
  int spill = fn->spill_bytes;
  for (size_t i = 0; i < ctx->saved_register_count; i++) {
    ctx->saved_register_offsets[i] = spill + (int)((i + 1) * 8);
  }
  int after_gp = spill + (int)(ctx->saved_register_count * 8);
  /* Saved XMM nonvolatiles sit below the GP saves, 16 bytes (full movdqu) each. */
  for (size_t i = 0; i < ctx->saved_xmm_count; i++) {
    ctx->saved_xmm_offsets[i] = after_gp + (int)((i + 1) * 16);
  }
  int raw = after_gp + (int)(ctx->saved_xmm_count * 16);
  if (mir_has_calls(fn)) {
    /* Outgoing call region at the very bottom of the frame: the INDIRECT
     * struct-argument copy region (lowest, rsp-relative), then 32B Win64 shadow
     * space, then any outgoing stack-argument bytes (calls with more GP args
     * than argument registers). Spills/saves sit above and never reach it. */
    raw += fn->outgoing_indirect_bytes + 32 + fn->outgoing_stack_bytes;
  }
  if (!binary_align_up_int(raw, 16, &ctx->frame_size)) {
    return enc_err(fn, "stack frame too large");
  }
  ctx->raw_frame_size = raw;
  if (ir_machine_collecting()) {
    long long spilled = 0;
    for (size_t v = 0; v < fn->vreg_count; v++) {
      if (fn->vregs[v].spill_offset != 0) {
        spilled++;
      }
    }
    mir_encode_last_spills = spilled;
  }
  return 1;
}

/* Home one GP parameter from its incoming argument register into its vreg,
 * extending narrow signed/unsigned values to 64 bits. */
static int mir_home_gp_param(MirFunction *fn, const MirParam *p,
                             BinaryGpRegister arg) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirOperand dst = mir_op_vreg(p->vreg);
  if (p->width == 8) {
    return store_from(fn, &dst, arg);
  }
  BinaryGpRegister D;
  if (dst_is_reg(fn, &dst, &D)) {
    int ok = 1;
    if (p->width == 4) {
      /* movzx_reg_reg32: must emit even when D == arg (the regalloc often
       * coalesces a param into its incoming register), the skip-when-equal
       * mov would silently drop the uint32 canonicalization. */
      ok = p->is_signed ? binary_emit_movsxd_reg_reg32(code, D, arg)
                        : binary_emit_movzx_reg_reg32(code, D, arg);
    } else if (p->width == 2 && p->is_signed) {
      ok = binary_emit_movsx_reg_reg16(code, D, arg);
    } else if (p->width == 1 && p->is_signed) {
      ok = binary_emit_movsx_reg_reg8(code, D, arg);
    } else {
      ok = (p->width == 2) ? binary_emit_movzx_reg_reg16(code, D, arg)
                           : binary_emit_movzx_reg_reg8(code, D, arg);
    }
    return ok ? 1 : enc_err(fn, "out of memory extending parameter");
  }
  /* Spilled destination: extend arg into SCRATCH_A (general reg-reg forms), then
   * store. */
  BinaryGpRegister S = SCRATCH_A;
  int ok = 1;
  if (p->width == 4) {
    ok = p->is_signed ? binary_emit_movsxd_reg_reg32(code, S, arg)
                      : binary_emit_movzx_reg_reg32(code, S, arg);
  } else if (p->width == 2) {
    ok = p->is_signed ? binary_emit_movsx_reg_reg16(code, S, arg)
                      : binary_emit_movzx_reg_reg16(code, S, arg);
  } else if (p->width == 1) {
    ok = p->is_signed ? binary_emit_movsx_reg_reg8(code, S, arg)
                      : binary_emit_movzx_reg_reg8(code, S, arg);
  }
  if (!ok || !store_from(fn, &dst, S)) {
    return enc_err(fn, "out of memory extending parameter");
  }
  return 1;
}

/* Home one GP parameter passed on the caller's stack into its vreg. The slot is
 * a full 8-byte slot above saved-rbp+return-address (16) and the callee's shadow
 * space; the caller stored the (already-extended) value there, so an 8-byte load
 * matches the fallback emitter exactly. */
static int mir_home_gp_stack_param(MirFunction *fn, const MirParam *p,
                                   int rbp_offset) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirOperand dst = mir_op_vreg(p->vreg);
  BinaryGpRegister D;
  if (dst_is_reg(fn, &dst, &D)) {
    return binary_emit_mov_reg_mem(code, D, frame_base(fn),
                                   frame_disp(fn, rbp_offset))
               ? 1
               : enc_err(fn, "out of memory homing stack parameter");
  }
  if (!binary_emit_mov_reg_mem(code, SCRATCH_A, frame_base(fn),
                               frame_disp(fn, rbp_offset))) {
    return enc_err(fn, "out of memory homing stack parameter");
  }
  return store_from(fn, &dst, SCRATCH_A);
}

/* A pending XMM->home move for float-parameter homing. */
typedef struct {
  BinaryXmmRegister src;
  int is_spill;
  int dst; /* xmm register (is_spill==0) or rbp-relative spill offset */
  int width;
  int done;
} MirXmmMove;

/* Home float parameters: incoming XMM arg registers -> param vregs. The arg
 * registers (XMM0..XMM3) are themselves allocatable, so this is a parallel
 * move: spill destinations are emitted first (they only read sources), then the
 * register->register permutation is resolved, breaking any cycle with the XMM
 * scratch register. All copies use movsd (low 64 bits) which preserves a scalar
 * float of either width. */
static int mir_home_float_params(MirFunction *fn, MirXmmMove *mv, int n) {
  BinaryCodeBuffer *code = &fn->context->code;
  /* Spill destinations first, while every source register is still intact. */
  for (int i = 0; i < n; i++) {
    if (!mv[i].is_spill) {
      continue;
    }
    int ok = (mv[i].width == 4)
                 ? (binary_emit_movd_reg_xmm(code, SCRATCH_A, mv[i].src) &&
                    binary_emit_mov_mem_reg32(code, frame_base(fn),
                                              frame_disp(fn, -mv[i].dst),
                                              SCRATCH_A))
                 : (binary_emit_movq_reg_xmm(code, SCRATCH_A, mv[i].src) &&
                    binary_emit_mov_mem_reg(code, frame_base(fn),
                                            frame_disp(fn, -mv[i].dst),
                                            SCRATCH_A));
    if (!ok) {
      return enc_err(fn, "out of memory homing float parameter");
    }
    mv[i].done = 1;
  }
  /* Register->register permutation. */
  int remaining = 0;
  for (int i = 0; i < n; i++) {
    if (!mv[i].done && (BinaryXmmRegister)mv[i].dst == mv[i].src) {
      mv[i].done = 1; /* already in place */
    }
    if (!mv[i].done) {
      remaining++;
    }
  }
  while (remaining > 0) {
    int progressed = 0;
    for (int i = 0; i < n; i++) {
      if (mv[i].done) {
        continue;
      }
      int dst_is_src = 0;
      for (int j = 0; j < n; j++) {
        if (!mv[j].done && j != i && mv[j].src == (BinaryXmmRegister)mv[i].dst) {
          dst_is_src = 1;
          break;
        }
      }
      if (!dst_is_src) {
        if (!binary_emit_sse_reg_reg(code, 0xF2, 0, 0x0F, 0x10,
                                     (BinaryXmmRegister)mv[i].dst, mv[i].src)) {
          return enc_err(fn, "out of memory homing float parameter");
        }
        mv[i].done = 1;
        remaining--;
        progressed = 1;
      }
    }
    if (progressed) {
      continue;
    }
    /* Pure cycle: save one destination's current value into the scratch XMM,
     * then redirect the move that consumes it to read the scratch. */
    int i;
    for (i = 0; i < n; i++) {
      if (!mv[i].done) {
        break;
      }
    }
    if (!binary_emit_sse_reg_reg(code, 0xF2, 0, 0x0F, 0x10, FSCRATCH_A,
                                 (BinaryXmmRegister)mv[i].dst)) {
      return enc_err(fn, "out of memory breaking float-param cycle");
    }
    for (int j = 0; j < n; j++) {
      if (!mv[j].done && mv[j].src == (BinaryXmmRegister)mv[i].dst) {
        mv[j].src = FSCRATCH_A;
      }
    }
    if (!binary_emit_sse_reg_reg(code, 0xF2, 0, 0x0F, 0x10,
                                 (BinaryXmmRegister)mv[i].dst, mv[i].src)) {
      return enc_err(fn, "out of memory homing float parameter");
    }
    mv[i].done = 1;
    remaining--;
  }
  return 1;
}

/* Home all parameters from their ABI incoming locations into their vregs. */
static int mir_home_parameters(MirFunction *fn) {
  size_t pc = fn->param_count;
  const BinaryAbi *abi = code_generator_binary_active_abi();
  /* An INDIRECT struct return prepends a hidden integer out-pointer argument
   * (Win64: RCX, SysV: RDI); home it into the reserved vreg and shift every
   * user parameter up one ABI slot in the layout. */
  size_t hidden = fn->returns_indirect ? 1 : 0;
  if (hidden) {
    if (fn->indirect_return_vreg != MIR_VREG_NONE &&
        fn->vregs[fn->indirect_return_vreg].assigned) {
      MirOperand dst = mir_op_vreg(fn->indirect_return_vreg);
      if (!store_from(fn, &dst, abi->indirect_return_register)) {
        return enc_err(fn, "out of memory homing indirect-return pointer");
      }
    }
  }
  if (pc == 0) {
    return 1;
  }
  int is_float[MIR_MAX_PARAMS + 1];
  BinaryArgLocation locs[MIR_MAX_PARAMS + 1];
  if (hidden) {
    is_float[0] = 0; /* hidden out-pointer is an integer arg */
  }
  for (size_t i = 0; i < pc; i++) {
    is_float[i + hidden] = fn->params[i].is_float;
  }
  if (!code_generator_binary_compute_arg_layout(abi, is_float, pc + hidden, locs,
                                                NULL)) {
    return enc_err(fn, "failed to compute parameter layout");
  }

  MirXmmMove xm[MIR_MAX_PARAMS];
  int nxm = 0;
  const MirParam *fstack[MIR_MAX_PARAMS];
  int fstack_off[MIR_MAX_PARAMS];
  int nfstack = 0;
  for (size_t i = 0; i < pc; i++) {
    const MirParam *p = &fn->params[i];
    if (!fn->vregs[p->vreg].assigned) {
      continue; /* unused parameter */
    }
    const BinaryArgLocation *loc = &locs[i + hidden];
    if (!p->is_float) {
      if (loc->kind == BINARY_ARG_IN_GP_REGISTER) {
        if (!mir_home_gp_param(fn, p, loc->gp_register)) {
          return 0;
        }
      } else if (loc->kind == BINARY_ARG_ON_STACK) {
        int rbp_offset = 16 + abi->shadow_space_size + loc->stack_offset;
        if (!mir_home_gp_stack_param(fn, p, rbp_offset)) {
          return 0;
        }
      } else {
        return enc_err(fn, "unsupported parameter location");
      }
    } else {
      if (loc->kind == BINARY_ARG_ON_STACK) {
        /* 5th+ float param. Deferred below: its destination register could be
         * an XMM0-3 still holding a not-yet-homed incoming arg. */
        fstack[nfstack] = p;
        fstack_off[nfstack] = 16 + abi->shadow_space_size + loc->stack_offset;
        nfstack++;
        continue;
      }
      if (loc->kind != BINARY_ARG_IN_XMM_REGISTER) {
        return enc_err(fn, "unsupported float parameter location");
      }
      MirVreg *vr = &fn->vregs[p->vreg];
      xm[nxm].src = loc->xmm_register;
      xm[nxm].width = p->width;
      xm[nxm].done = 0;
      if (vr->in_register) {
        xm[nxm].is_spill = 0;
        xm[nxm].dst = vr->phys;
      } else {
        xm[nxm].is_spill = 1;
        xm[nxm].dst = vr->spill_offset;
      }
      nxm++;
    }
  }
  if (!mir_home_float_params(fn, xm, nxm)) {
    return 0;
  }
  for (int i = 0; i < nfstack; i++) {
    const MirParam *p = fstack[i];
    MirVreg *vr = &fn->vregs[p->vreg];
    int ok;
    if (vr->in_register) {
      ok = (p->width == 4)
               ? wcs_movss_xmm_mem(&fn->context->code, vr->phys, frame_base(fn),
                                   frame_disp(fn, fstack_off[i]))
               : wcs_movsd_xmm_mem(&fn->context->code, vr->phys, frame_base(fn),
                                   frame_disp(fn, fstack_off[i]));
    } else {
      ok = binary_emit_mov_reg_mem(&fn->context->code, SCRATCH_A,
                                   frame_base(fn),
                                   frame_disp(fn, fstack_off[i])) &&
           binary_emit_mov_mem_reg(&fn->context->code, frame_base(fn),
                                   frame_disp(fn, -vr->spill_offset),
                                   SCRATCH_A);
    }
    if (!ok) {
      return enc_err(fn, "out of memory homing float stack parameter");
    }
  }
  return 1;
}

/* Same operand, and reading it emits nothing: a vreg already in a register,
 * or an immediate. A spilled operand would be staged with instructions of its
 * own before the compare, which is exactly what must not happen between the
 * compare whose flags are being reused and the branch reusing them. */
/* A label no branch names is a fall-through marker, not a join: control
 * cannot arrive there carrying different flags. */
static int mir_label_is_branch_target(const MirFunction *fn,
                                      const char *name) {
  if (!name) {
    return 1;
  }
  for (size_t k = 0; k < fn->insn_count; k++) {
    const MirInst *b = &fn->insns[k];
    /* A switch case is named by the jump table rather than by a branch, and
     * control reaching one carries whatever flags the dispatch left. */
    if (b->op == MIR_JMP_TABLE) {
      const MirJumpTable *tbl = (const MirJumpTable *)b->aux;
      if (tbl) {
        for (size_t t = 0; t < tbl->count; t++) {
          if (tbl->labels[t] && strcmp(tbl->labels[t], name) == 0) {
            return 1;
          }
        }
      }
      continue;
    }
    if (b->op != MIR_JMP && b->op != MIR_JCC && b->op != MIR_CMPBR &&
        b->op != MIR_FCMPBR) {
      continue;
    }
    if (b->dst.kind == MIR_OPK_LABEL && b->dst.sym &&
        strcmp(b->dst.sym, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Can this sit between a compare and a branch reusing its flags? Only if it
 * emits nothing at all: a nop, a fall-through label, or a register copy the
 * allocator already collapsed by giving both ends the same register. */
static int mir_cmp_gap_is_empty(const MirFunction *fn, const MirInst *in) {
  if (in->op == MIR_NOP) {
    return 1;
  }
  if (in->op == MIR_LABEL) {
    return in->dst.kind == MIR_OPK_LABEL &&
           !mir_label_is_branch_target(fn, in->dst.sym);
  }
  if (in->op == MIR_MOV && !in->is_float && in->dst.kind == MIR_OPK_VREG &&
      in->a.kind == MIR_OPK_VREG && in->dst.vreg != MIR_VREG_NONE &&
      in->a.vreg != MIR_VREG_NONE &&
      fn->vregs[in->dst.vreg].in_register &&
      fn->vregs[in->a.vreg].in_register) {
    return fn->vregs[in->dst.vreg].phys == fn->vregs[in->a.vreg].phys &&
           fn->vregs[in->dst.vreg].rclass == fn->vregs[in->a.vreg].rclass;
  }
  return 0;
}

static int mir_cmp_operand_reusable(const MirFunction *fn,
                                    const MirOperand *x,
                                    const MirOperand *y) {
  if (x->kind != y->kind) {
    return 0;
  }
  if (x->kind == MIR_OPK_IMM) {
    return x->imm == y->imm;
  }
  if (x->kind != MIR_OPK_VREG || x->vreg == MIR_VREG_NONE ||
      y->vreg == MIR_VREG_NONE) {
    return 0;
  }
  if (!fn->vregs[x->vreg].in_register || !fn->vregs[y->vreg].in_register) {
    return 0;
  }
  return fn->vregs[x->vreg].phys == fn->vregs[y->vreg].phys &&
         fn->vregs[x->vreg].rclass == fn->vregs[y->vreg].rclass;
}

static int mir_emit_prologue(MirFunction *fn) {
  BinaryFunctionContext *ctx = fn->context;
  BinaryCodeBuffer *code = &ctx->code;
  if (ctx->omit_frame_pointer) {
    /* No rbp frame: fold the 8 bytes the saved-rbp slot used to occupy into the
     * allocation so rsp stays 16-aligned at calls (entry rsp == 8 mod 16, and
     * frame_size is 16-aligned, so +8 realigns to 0). rbp is now an ordinary
     * allocatable callee-saved register; if the allocator used it, the saved-
     * register loop below preserves it (caller's value is still intact here). */
    if (!binary_emit_frame_allocation(code, ctx->frame_size + 8)) {
      return enc_err(fn, "out of memory allocating frame");
    }
  } else {
    if (!binary_emit_push_reg(code, BINARY_GP_RBP) ||
        !binary_emit_mov_reg_reg(code, BINARY_GP_RBP, BINARY_GP_RSP)) {
      return enc_err(fn, "out of memory in prologue");
    }
    if (!binary_emit_frame_allocation(code, ctx->frame_size)) {
      return enc_err(fn, "out of memory allocating frame");
    }
  }
  for (size_t i = 0; i < ctx->saved_register_count; i++) {
    if (!binary_emit_mov_mem_reg(code, frame_base(fn),
                                 frame_disp(fn, -ctx->saved_register_offsets[i]),
                                 ctx->saved_registers[i])) {
      return enc_err(fn, "out of memory saving callee registers");
    }
  }
  for (size_t i = 0; i < ctx->saved_xmm_count; i++) {
    if (!simd_movdqu_mem_xmm_disp(code, frame_base(fn),
                                  frame_disp(fn, -ctx->saved_xmm_offsets[i]),
                                  ctx->saved_xmm_registers[i])) {
      return enc_err(fn, "out of memory saving callee xmm registers");
    }
  }
  if (!mir_home_parameters(fn)) {
    return 0;
  }
  return 1;
}

static int mir_emit_epilogue(MirFunction *fn) {
  BinaryFunctionContext *ctx = fn->context;
  BinaryCodeBuffer *code = &ctx->code;
  /* An inline vector kernel (e.g. MIR_SIMD_SLP_MAC) left the YMM upper halves
   * dirty; clear them once here so a caller running legacy SSE pays no AVX->SSE
   * transition penalty. Emitted per RET, but functions typically have one. */
  if (fn->used_inline_vector && !code_generator_binary_emit_vzeroupper(code)) {
    return enc_err(fn, "out of memory emitting epilogue vzeroupper");
  }
  for (size_t i = ctx->saved_xmm_count; i > 0; i--) {
    size_t j = i - 1;
    if (!simd_movdqu_xmm_mem_disp(code, ctx->saved_xmm_registers[j],
                                  frame_base(fn),
                                  frame_disp(fn, -ctx->saved_xmm_offsets[j]))) {
      return enc_err(fn, "out of memory restoring callee xmm registers");
    }
  }
  for (size_t i = ctx->saved_register_count; i > 0; i--) {
    size_t j = i - 1;
    if (!binary_emit_mov_reg_mem(code, ctx->saved_registers[j], frame_base(fn),
                                 frame_disp(fn,
                                            -ctx->saved_register_offsets[j]))) {
      return enc_err(fn, "out of memory restoring callee registers");
    }
  }
  if (ctx->omit_frame_pointer) {
    /* Slots are addressed off rsp, which is still at the frame bottom here, so
     * tear the frame down (the saved-rbp +8) and return. No pop rbp. */
    if (!binary_emit_add_rsp_imm32(code, (uint32_t)(ctx->frame_size + 8)) ||
        !binary_emit_ret(code)) {
      return enc_err(fn, "out of memory in epilogue");
    }
  } else if (!binary_emit_mov_reg_reg(code, BINARY_GP_RSP, BINARY_GP_RBP) ||
             !binary_emit_pop_reg(code, BINARY_GP_RBP) ||
             !binary_emit_ret(code)) {
    return enc_err(fn, "out of memory in epilogue");
  }
  return 1;
}

/* MIR_LOAD_GLOBAL: dst <- value of the read-only global named by in->a (SYMBOL).
 * Uses the const-table immediate when the global folds to a constant, otherwise
 * a RIP-relative load (which sign/zero-extends to the dst register width). */
static int encode_load_global(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;
  const char *name = in->a.sym;
  if (!name) {
    return enc_err(fn, "MIR_LOAD_GLOBAL without a symbol");
  }

  /* A float global is cached in an XMM vreg: load its raw bits into a GP scratch
   * (the RIP-relative load helper is GP-only) then movd/movq them into the XMM
   * lane. Float globals are never const-folded (see globals.c), so no immediate
   * branch is needed here. */
  if (in->dst.kind == MIR_OPK_VREG &&
      fn->vregs[in->dst.vreg].rclass == MIR_RC_XMM) {
    int width = fn->vregs[in->dst.vreg].width;
    const char *link = code_generator_get_link_symbol_name(g, name);
    const CgSym *s =
        (g && g->ir_program) ? code_generator_lookup_symbol(g, name)
                               : NULL;
    if (!link || !link[0] || !s) {
      return enc_err(fn, "unresolved global in MIR_LOAD_GLOBAL");
    }
    if (s->type && (s->type->kind == MTLC_TYPE_FLOAT16 || s->type->kind == MTLC_TYPE_BFLOAT16)) {
      if (!code_generator_binary_emit_global_symbol_load(g, ctx, link, s->type, s->is_extern, SCRATCH_A)) {
        return enc_err(fn, "out of memory loading float global");
      }
      if (s->type->kind == MTLC_TYPE_FLOAT16) {
        if (!binary_emit_movd_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A)) {
          return enc_err(fn, "out of memory staging float global to xmm");
        }
        if (!wcs_avx_vcvtph2ps_xmm(&ctx->code, (int)FSCRATCH_A, (int)FSCRATCH_A)) {
          return enc_err(fn, "out of memory converting float global");
        }
      } else {
        if (!binary_emit_shift_reg_imm8(&ctx->code, 4, SCRATCH_A, 16)) {
          return enc_err(fn, "out of memory converting float global");
        }
        if (!binary_emit_movd_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A)) {
          return enc_err(fn, "out of memory staging float global to xmm");
        }
      }
      return xmm_store(fn, &in->dst, FSCRATCH_A, width);
    }
    if (!code_generator_binary_emit_global_symbol_load(g, ctx, link, s->type,
                                                       s->is_extern, SCRATCH_A)) {
      return enc_err(fn, "out of memory loading float global");
    }
    int moved = (width == 4)
                    ? binary_emit_movd_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A)
                    : binary_emit_movq_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A);
    if (!moved) {
      return enc_err(fn, "out of memory staging float global to xmm");
    }
    return xmm_store(fn, &in->dst, FSCRATCH_A, width);
  }

  BinaryGpRegister D;
  int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
  BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;

  uint64_t cval = 0;
  if (binary_global_const_table_get(name, &cval)) {
    if (!binary_emit_mov_reg_imm64(&ctx->code, target, cval)) {
      return enc_err(fn, "out of memory loading global constant");
    }
  } else {
    const char *link = code_generator_get_link_symbol_name(g, name);
    const CgSym *s =
        (g && g->ir_program) ? code_generator_lookup_symbol(g, name)
                               : NULL;
    if (!link || !link[0] || !s) {
      return enc_err(fn, "unresolved global in MIR_LOAD_GLOBAL");
    }
    if (!code_generator_binary_emit_global_symbol_load(g, ctx, link, s->type,
                                                       s->is_extern, target)) {
      return enc_err(fn, "out of memory loading global");
    }
  }
  if (!dst_in_reg) {
    return store_from(fn, &in->dst, target);
  }
  return 1;
}

/* MIR_STORE_GLOBAL: global named by in->a (SYMBOL) <- value in in->b (vreg).
 * Writes a register-promoted global back to memory via a RIP-relative store of
 * the low `width` bytes. Symmetric to encode_load_global. */
static int encode_store_global(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;
  const char *name = in->a.sym;
  if (!name) {
    return enc_err(fn, "MIR_STORE_GLOBAL without a symbol");
  }
  BinaryGpRegister src;
  /* A float global is cached in an XMM vreg: pull its bits out of the XMM lane
   * into a GP scratch, then the RIP-relative store writes the low `size` bytes
   * (the GP store helper is GP-only). */
  if (in->b.kind == MIR_OPK_VREG &&
      fn->vregs[in->b.vreg].rclass == MIR_RC_XMM) {
    int width = fn->vregs[in->b.vreg].width;
    int xok = 1;
    BinaryXmmRegister xsrc = xmm_value(fn, &in->b, FSCRATCH_A, width, &xok);
    if (!xok) {
      return 0;
    }
    const char *gname = in->a.sym;
    const CgSym *gs = (g && g->ir_program && gname) ? code_generator_lookup_symbol(g, gname) : NULL;
    if (gs && gs->type && (gs->type->kind == MTLC_TYPE_FLOAT16 || gs->type->kind == MTLC_TYPE_BFLOAT16)) {
      if (gs->type->kind == MTLC_TYPE_FLOAT16) {
        if (!wcs_avx_vcvtps2ph_xmm(&ctx->code, (int)FSCRATCH_A, (int)xsrc, 0)) {
          return enc_err(fn, "out of memory converting float global");
        }
        if (!binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A, FSCRATCH_A)) {
          return enc_err(fn, "out of memory staging float global from xmm");
        }
      } else {
        if (!binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A, xsrc)) {
          return enc_err(fn, "out of memory staging float global from xmm");
        }
        {
          BinaryGpRegister tmp = BINARY_GP_R11;
          if (!binary_emit_mov_reg_reg(&ctx->code, tmp, SCRATCH_A)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_shift_reg_imm8(&ctx->code, 5, tmp, 16)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_and_reg_imm32(&ctx->code, tmp, 1)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_alu_reg_imm32(&ctx->code, 0, tmp, 0x7FFF)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_alu_reg_reg(&ctx->code, 0x03, SCRATCH_A, tmp)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_shift_reg_imm8(&ctx->code, 5, SCRATCH_A, 16)) {
            return enc_err(fn, "out of memory converting float global");
          }
        }
      }
      src = SCRATCH_A;
    } else {
      int moved = (width == 4)
                      ? binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A, xsrc)
                      : binary_emit_movq_reg_xmm(&ctx->code, SCRATCH_A, xsrc);
      if (!moved) {
        return enc_err(fn, "out of memory staging float global from xmm");
      }
      src = SCRATCH_A;
    }
  } else {
    int rok = 1;
    src = value_reg(fn, &in->b, SCRATCH_A, &rok);
    if (!rok) {
      return 0;
    }
  }
  const char *link = code_generator_get_link_symbol_name(g, name);
  const CgSym *s = (g && g->ir_program) ? code_generator_lookup_symbol(g, name)
                                     : NULL;
  if (!link || !link[0] || !s) {
    return enc_err(fn, "unresolved global in MIR_STORE_GLOBAL");
  }
  if (!code_generator_binary_emit_global_symbol_store(g, ctx, link, s->type,
                                                      s->is_extern, src)) {
    return enc_err(fn, "out of memory storing global");
  }
  return 1;
}

/* MIR index of the MIR_LABEL defining `name`, or -1. */
static int mir_encode_label_index(const MirFunction *fn, const char *name) {
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

/* Is the MIR_JMP at `index` a jump to the code that immediately follows it?
 *
 * Only NOPs and labels may sit in between: both emit no bytes, so control
 * reaches the target either way. Alignment padding before a loop label is NOPs
 * too -- skipping it by falling through instead of branching over it is
 * harmless, since NOPs do nothing whichever way they are reached. */
static int mir_vreg_is_byte_load(const MirFunction *fn, size_t before,
                                 MirVregId v) {
  size_t limit = before > 16 ? before - 16 : 0;
  for (size_t k = before; k-- > limit;) {
    const MirInst *in = &fn->insns[k];
    if (in->dst.kind != MIR_OPK_VREG || in->dst.vreg != v) {
      continue;
    }
    return in->op == MIR_MOV && !in->is_float && in->width == 1 &&
           in->is_unsigned && in->a.kind == MIR_OPK_MEM;
  }
  return 0;
}

static int mir_mem_operand_in_registers(const MirFunction *fn,
                                        const MirOperand *op) {
  if (op->kind != MIR_OPK_MEM || op->mem.base == MIR_VREG_NONE ||
      op->mem.phys_base_valid) {
    return 0;
  }
  if (!fn->vregs[op->mem.base].in_register) {
    return 0;
  }
  if (op->mem.index != MIR_VREG_NONE) {
    if (!fn->vregs[op->mem.index].in_register) {
      return 0;
    }
    if (op->mem.scale != 1 && op->mem.scale != 2 && op->mem.scale != 4 &&
        op->mem.scale != 8) {
      return 0;
    }
    if (fn->vregs[op->mem.index].phys == BINARY_GP_RSP) {
      return 0;
    }
  }
  if (fn->vregs[op->mem.base].phys == BINARY_GP_RSP) {
    return 0;
  }
  return 1;
}

static int mir_mask_test_fusable(const MirFunction *fn, size_t i) {
  const MirInst *and_op;
  const MirInst *cmp;
  const MirVreg *masked;
  if (i + 1 >= fn->insn_count) {
    return 0;
  }
  and_op = &fn->insns[i];
  cmp = &fn->insns[i + 1];
  if (and_op->op != MIR_AND || and_op->is_float ||
      and_op->dst.kind != MIR_OPK_VREG || and_op->a.kind != MIR_OPK_VREG ||
      and_op->b.kind != MIR_OPK_IMM || and_op->b.imm < 0 ||
      and_op->b.imm > 2147483647LL) {
    return 0;
  }
  if (cmp->op != MIR_CMPBR || cmp->is_float ||
      (cmp->cc != 0x84 && cmp->cc != 0x85) || cmp->a.kind != MIR_OPK_VREG ||
      cmp->a.vreg != and_op->dst.vreg || cmp->b.kind != MIR_OPK_IMM ||
      cmp->b.imm != 0) {
    return 0;
  }
  masked = &fn->vregs[and_op->dst.vreg];
  if (!masked->in_register || masked->live_end != (int)(i + 1)) {
    return 0;
  }
  return fn->vregs[and_op->a.vreg].in_register;
}

static int mir_byte_compare_fusable(const MirFunction *fn, size_t i) {
  const MirInst *load;
  const MirInst *cmp;
  const MirVreg *loaded;
  if (i + 1 >= fn->insn_count) {
    return 0;
  }
  load = &fn->insns[i];
  cmp = &fn->insns[i + 1];
  if (load->op != MIR_MOV || load->is_float || load->width != 1 ||
      !load->is_unsigned || load->dst.kind != MIR_OPK_VREG ||
      !mir_mem_operand_in_registers(fn, &load->a)) {
    return 0;
  }
  if (cmp->op != MIR_CMPBR || cmp->is_float ||
      (cmp->cc != 0x84 && cmp->cc != 0x85) || cmp->b.kind != MIR_OPK_VREG ||
      cmp->b.vreg != load->dst.vreg || cmp->a.kind != MIR_OPK_VREG) {
    return 0;
  }
  loaded = &fn->vregs[load->dst.vreg];
  if (!loaded->in_register || loaded->live_end != (int)(i + 1)) {
    return 0;
  }
  return mir_vreg_is_byte_load(fn, i, cmp->a.vreg);
}

static int mir_jump_is_fallthrough(const MirFunction *fn, size_t index) {
  const MirInst *jmp = &fn->insns[index];
  if (jmp->dst.kind != MIR_OPK_LABEL || !jmp->dst.sym) {
    return 0;
  }
  for (size_t k = index + 1; k < fn->insn_count; k++) {
    const MirInst *in = &fn->insns[k];
    if (in->op == MIR_NOP) {
      continue;
    }
    if (in->op != MIR_LABEL) {
      return 0;
    }
    if (in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, jmp->dst.sym) == 0) {
      return 1;
    }
    /* A different label: keep looking. Falling through it reaches the same
     * place a branch past it would. */
  }
  return 0;
}

int mir_encode(MirFunction *fn) {
  if (!fn || !fn->context) {
    return 0;
  }
  BinaryFunctionContext *ctx = fn->context;

  /* --annotate-asm: byte offsets are reported relative to this function's start
   * (the context's code buffer may already hold earlier functions). */
  size_t annot_base = ctx->code.size;
  int annot = mir_annotate_enabled();
  struct {
    size_t lea_off;
    const MirJumpTable *table;
  } pending_tables[MIR_MAX_JUMP_TABLES];
  size_t pending_table_count = 0;
  size_t fused_byte_load = (size_t)-1;
  size_t fused_mask_test = (size_t)-1;
  size_t prev_cmpbr = (size_t)-1;

  home_fwd_clear();
  if (!mir_layout_frame(fn) || !mir_emit_prologue(fn)) {
    return 0;
  }
  if (annot && ctx->code.size > annot_base) {
    mir_annotate_record_synthetic("prologue", "frame", 0,
                                  ctx->code.size - annot_base,
                                  ctx->code.data + annot_base);
  }

  /* Loop-header alignment: a label that is the target of a BACKWARD branch is a
   * loop top; pad it to BINARY_LOOP_ALIGN (like gcc -falign-loops) so the hot
   * loop's instruction fetch does not depend on where the function happened to
   * land. The pad NOPs sit BEFORE the label, so a back-edge (which jumps to the
   * label, past the pad) never executes them; only a fall-through into the loop
   * pays them, once. Pure performance -- it cannot change behaviour. */
  char *align_label = (char *)calloc(fn->insn_count ? fn->insn_count : 1, 1);
  if (align_label) {
    for (size_t b = 0; b < fn->insn_count; b++) {
      const MirInst *in = &fn->insns[b];
      if (in->op != MIR_JMP && in->op != MIR_JCC && in->op != MIR_CMPBR &&
          in->op != MIR_FCMPBR) {
        continue;
      }
      if (in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
        continue;
      }
      int d = mir_encode_label_index(fn, in->dst.sym);
      if (d >= 0 && (size_t)d < b) {
        /* 1 = align, 2 = align wider: the body from here to this back-edge is
         * big enough that the extra padding costs nothing next to it. The
         * FURTHEST back-edge decides, so a nested loop sharing a header is
         * measured at its full extent. */
        if (b - (size_t)d >= BINARY_LOOP_BIG_MIR_INSTRUCTIONS ||
            align_label[d] == 2) {
          align_label[d] = 2;
        } else if (b - (size_t)d <= BINARY_LOOP_TIGHT_MIR_INSTRUCTIONS &&
                   align_label[d] != 1) {
          align_label[d] = 3;
        } else {
          align_label[d] = 1;
        }
      }
    }
  }

  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    int ok = 1;
    size_t annot_off = ctx->code.size;
    home_fwd_note_boundary(in->op);
    if (in->op == MIR_LABEL && align_label && align_label[i]) {
      if (align_label[i] == 2) {
        ctx->wants_wide_loop_alignment = 1;
        ok = binary_emit_align_code(&ctx->code, BINARY_LOOP_ALIGN_BIG,
                                    BINARY_LOOP_ALIGN_BIG_MAX_PAD);
      } else if (align_label[i] == 3) {
        ctx->wants_wide_loop_alignment = 1;
        ok = binary_emit_align_code(&ctx->code, BINARY_LOOP_ALIGN_BIG,
                                    BINARY_LOOP_ALIGN_BIG_MAX_PAD);
      } else {
        ok = binary_emit_align_code(&ctx->code, BINARY_LOOP_ALIGN,
                                    BINARY_LOOP_ALIGN_MAX_PAD);
      }
    }
    if (!ok) {
      free(align_label);
      return 0;
    }
    switch (in->op) {
    case MIR_NOP:
      break;
    case MIR_MOV:
      if (mir_byte_compare_fusable(fn, i)) {
        fused_byte_load = i;
        break;
      }
      ok = encode_mov(fn, in);
      break;
    case MIR_ADD:
    case MIR_SUB:
    case MIR_AND:
    case MIR_OR:
    case MIR_XOR:
      if (in->op == MIR_AND && mir_mask_test_fusable(fn, i)) {
        fused_mask_test = i;
        break;
      }
      ok = encode_alu(fn, in);
      break;
    case MIR_IMUL:
      ok = encode_imul(fn, in);
      break;
    case MIR_NEG:
    case MIR_NOT:
      ok = encode_neg_not(fn, in);
      break;
    case MIR_POPCNT: {
      BinaryGpRegister D;
      if (dst_is_reg(fn, &in->dst, &D)) {
        if (!operand_in_phys(fn, &in->a, D) && !materialize_into(fn, &in->a, D)) {
          ok = 0;
          break;
        }
        if (!wcs_popcnt(&ctx->code, D, D)) {
          ok = enc_err(fn, "out of memory in popcnt");
        }
        break;
      }
      if (!materialize_into(fn, &in->a, SCRATCH_A)) {
        ok = 0;
        break;
      }
      if (!wcs_popcnt(&ctx->code, SCRATCH_A, SCRATCH_A)) {
        ok = enc_err(fn, "out of memory in popcnt");
        break;
      }
      ok = store_from(fn, &in->dst, SCRATCH_A);
      break;
    }
    case MIR_IDIV:
      ok = encode_div(fn, in);
      break;
    case MIR_MULHI:
      ok = encode_mulhi(fn, in);
      break;
    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR:
      ok = encode_shift(fn, in);
      break;
    case MIR_SETCC:
      ok = encode_setcc(fn, in);
      break;
    case MIR_MOVZX:
    case MIR_MOVSX:
      ok = encode_extend(fn, in);
      break;
    case MIR_LOAD_GLOBAL:
      ok = encode_load_global(fn, in);
      break;
    case MIR_STORE_GLOBAL:
      ok = encode_store_global(fn, in);
      break;
    case MIR_FDUP:
    case MIR_FEXTHI: {
      /* vmovddup dst,a / vunpckhpd dst,a,a: whole-register writes, so a
       * spilled destination stages through FSCRATCH_A like any float op. */
      int lok;
      BinaryXmmRegister D;
      int dst_in_reg = dst_is_xmm_reg(fn, &in->dst, &D);
      if (!dst_in_reg) {
        D = FSCRATCH_A;
      }
      BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_B, in->width,
                                         &lok);
      if (!lok) {
        ok = enc_err(fn, "out of memory in lane op");
        break;
      }
      int done = (in->op == MIR_FDUP)
                     ? vex_xmm_3op(fn, 3, 0x12, D, 0, aval)
                     : vex_xmm_3op(fn, 1, 0x15, D, aval, aval);
      if (!done) {
        ok = enc_err(fn, "out of memory in lane op");
        break;
      }
      ok = dst_in_reg ? 1 : xmm_store(fn, &in->dst, FSCRATCH_A, in->width);
      break;
    }
    case MIR_FADD:
    case MIR_FSUB:
    case MIR_FMUL:
    case MIR_FDIV:
      ok = encode_fbinop(fn, in);
      break;
    case MIR_CVTSI2F:
      ok = encode_cvtsi2f(fn, in);
      break;
    case MIR_CVTF2SI:
      ok = encode_cvtf2si(fn, in);
      break;
    case MIR_CVTF2F:
      ok = encode_cvtf2f(fn, in);
      break;
    case MIR_CVTPH2PS:
      ok = encode_cvtph2ps(fn, in);
      break;
    case MIR_CVTPS2PH:
      ok = encode_cvtps2ph(fn, in);
      break;
    case MIR_MOVD_TO_XMM:
    case MIR_MOVD_TO_GP: {
      int ok2;
      if (in->op == MIR_MOVD_TO_XMM) {
        BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok2);
        BinaryXmmRegister D;
        int dst_in_reg = dst_is_xmm_reg(fn, &in->dst, &D);
        BinaryXmmRegister target = dst_in_reg ? D : FSCRATCH_B;
        if (!ok2) { ok = 0; break; }
        if (!binary_emit_movd_xmm_reg(&fn->context->code, target, areg)) {
          ok = enc_err(fn, "out of memory in movd2xmm");
          break;
        }
        ok = dst_in_reg ? 1 : xmm_store(fn, &in->dst, FSCRATCH_B, 4);
        break;
      }
      BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, 4, &ok2);
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!ok2) { ok = 0; break; }
      if (!binary_emit_movd_reg_xmm(&fn->context->code, target, aval)) {
        ok = enc_err(fn, "out of memory in movd2gp");
        break;
      }
      ok = dst_in_reg ? 1 : store_from(fn, &in->dst, target);
      break;
    }
    case MIR_FSETCC: {
      int rok;
      BinaryXmmRegister av = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &rok);
      if (!rok) { ok = 0; break; }
      BinaryXmmRegister bv = xmm_value(fn, &in->b, FSCRATCH_B, in->width, &rok);
      if (!rok) { ok = 0; break; }
      int cmp = (in->width == 4)
                    ? binary_emit_ucomiss_xmm_xmm(&ctx->code, av, bv)
                    : binary_emit_ucomisd_xmm_xmm(&ctx->code, av, bv);
      if (!cmp || !binary_emit_setcc_reg8(&ctx->code, in->cc, BINARY_GP_RAX) ||
          !binary_emit_movzx_eax_al(&ctx->code)) {
        ok = enc_err(fn, "out of memory in fsetcc");
        break;
      }
      ok = store_from(fn, &in->dst, BINARY_GP_RAX); /* result in RAX (movzx) */
      break;
    }
    case MIR_FCMPBR: {
      int rok;
      BinaryXmmRegister av = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &rok);
      if (!rok) { ok = 0; break; }
      BinaryXmmRegister bv = xmm_value(fn, &in->b, FSCRATCH_B, in->width, &rok);
      if (!rok) { ok = 0; break; }
      int cmp = (in->width == 4)
                    ? binary_emit_ucomiss_xmm_xmm(&ctx->code, av, bv)
                    : binary_emit_ucomisd_xmm_xmm(&ctx->code, av, bv);
      size_t off = 0;
      if (!cmp || !binary_emit_jcc_placeholder(&ctx->code, in->cc, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in fcmpbr");
      }
      break;
    }
    case MIR_LABEL:
      if (!binary_label_table_define(&ctx->labels, in->dst.sym,
                                     ctx->code.size)) {
        ok = enc_err(fn, "duplicate label");
      }
      break;
    case MIR_JMP: {
      /* A jump whose target is the very next thing emitted is the fall-through
       * it would have taken anyway. The lowering produces these wherever a
       * structured statement ends by branching to its own exit label -- an
       * if/else arm, a loop body, a short-circuit -- and each one costs five
       * bytes of instruction fetch for nothing. */
      if (mir_jump_is_fallthrough(fn, i)) {
        break;
      }
      size_t off = 0;
      if (!binary_emit_jmp_placeholder(&ctx->code, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in jmp");
      }
      break;
    }
    case MIR_JCC: {
      /* test cond; je/jcc label. The test only reads the condition, so use its
       * own register directly (staging into RAX only when spilled/immediate). */
      int rok;
      BinaryGpRegister creg = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok || !binary_emit_test_reg_reg(&ctx->code, creg)) {
        ok = enc_err(fn, "out of memory in branch test");
        break;
      }
      size_t off = 0;
      if (!binary_emit_jcc_placeholder(&ctx->code, in->cc, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in branch");
      }
      break;
    }
    case MIR_CALL: {
      /* rsp already points at the reserved shadow space (set by the prologue),
       * so just emit the relocated call. Arguments were moved into ABI
       * registers by preceding MIR_MOVs; the return value is consumed by the
       * following MIR_MOV from RAX/XMM0. */
      const char *link =
          code_generator_get_link_symbol_name(fn->generator, in->dst.sym);
      size_t off = 0;
      /* A preserving call parks RAX in its frame slot and puts it back, so a
       * value the allocator placed there survives (MirInst::preserves_rax). The
       * arguments are already marshalled and none of them is RAX, and the slot
       * is addressed off the frame rather than pushed, because the call's stack
       * arguments and shadow space are measured from rsp. */
      int keep = in->preserves_rax || in->preserves_xmm;
      if (keep && !mir_emit_preserve_volatiles(fn, in, 1)) {
        ok = enc_err(fn, "out of memory saving registers across a checked call");
        break;
      }
      if (!link || !binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, link, off)) {
        ok = enc_err(fn, "out of memory emitting call");
        break;
      }
      if (keep && !mir_emit_preserve_volatiles(fn, in, 0)) {
        ok = enc_err(fn,
                     "out of memory restoring registers across a checked call");
      }
      break;
    }
    case MIR_HEAP_NEW: {
      /* Zeroed Win64 heap allocation (IR_OP_NEW): the byte size arrived in R8
       * via a preceding MIR_MOV. Self-contained rsp bubble: 48 bytes keeps the
       * statement-point 16-alignment, gives both calls fresh shadow space at
       * [rsp,32), and parks the size at [rsp+40] across GetProcessHeap. The
       * result pointer lands in RAX for the following MIR_MOV to consume. */
      size_t d1 = 0;
      size_t d2 = 0;
      if (!code_generator_binary_declare_external_symbol(fn->generator,
                                                         "GetProcessHeap") ||
          !code_generator_binary_declare_external_symbol(fn->generator,
                                                         "HeapAlloc") ||
          !binary_emit_sub_rsp_imm32(&ctx->code, 48) ||
          !binary_emit_mov_mem_reg(&ctx->code, BINARY_GP_RSP, 40,
                                   BINARY_GP_R8) ||
          !binary_emit_call_placeholder(&ctx->code, &d1) ||
          !binary_call_relocation_table_add(&ctx->call_relocations,
                                            "GetProcessHeap", d1) ||
          !binary_emit_mov_reg_reg(&ctx->code, BINARY_GP_RCX, BINARY_GP_RAX) ||
          !binary_emit_mov_reg_imm64(&ctx->code, BINARY_GP_RDX,
                                     8 /* HEAP_ZERO_MEMORY */) ||
          !binary_emit_mov_reg_mem(&ctx->code, BINARY_GP_R8, BINARY_GP_RSP,
                                   40) ||
          !binary_emit_call_placeholder(&ctx->code, &d2) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, "HeapAlloc",
                                            d2) ||
          !binary_emit_add_rsp_imm32(&ctx->code, 48)) {
        ok = enc_err(fn, "out of memory emitting heap allocation");
      }
      break;
    }
    case MIR_SYSCALL: {      if (!binary_emit_syscall(&ctx->code)) {
        ok = enc_err(fn, "out of memory emitting a system call");
      }
      break;
    }
    case MIR_REP_MOVSB:
    case MIR_REP_STOSB: {
      /* The argument marshalling ran already, so the first three integer
       * argument registers of the ACTIVE convention hold
       * destination/source-or-fill/count exactly as for the call this replaces:
       * RCX/RDX/R8 under Win64, RDI/RSI/RDX under SysV. Naming them literally
       * would copy a SysV memset's fill byte in as its destination.
       *
       * Take the destination into RAX first (it is the return value, and the
       * register holding it is about to become the counter or the fill), then
       * run the string operation with RSI and RDI saved around it -- the
       * allocator may be holding live values in both, since they are
       * nonvolatile under Win64. RDI and RSI are loaded before the counter,
       * which is an argument register under neither convention, so no move
       * overwrites a source another still has to read. */
      BinaryCodeBuffer *code = &ctx->code;
      const BinaryAbi *rep_abi = code_generator_binary_active_abi();
      if (!rep_abi || rep_abi->int_param_count < 3) {
        ok = enc_err(fn, "no argument registers for an inline block copy");
        break;
      }
      BinaryGpRegister rep_dst = rep_abi->int_param_registers[0];
      BinaryGpRegister rep_src = rep_abi->int_param_registers[1];
      BinaryGpRegister rep_count = rep_abi->int_param_registers[2];
      int rep_ok = binary_emit_mov_reg_reg(code, BINARY_GP_RAX, rep_dst) &&
                   binary_emit_push_reg(code, BINARY_GP_RDI);
      if (rep_ok && in->op == MIR_REP_MOVSB) {
        rep_ok = binary_emit_push_reg(code, BINARY_GP_RSI) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RDI, rep_dst) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RSI, rep_src) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RCX, rep_count) &&
                 binary_code_buffer_append_u8(code, 0xFC) &&  /* cld */
                 binary_code_buffer_append_u8(code, 0xF3) &&  /* rep  */
                 binary_code_buffer_append_u8(code, 0xA4) &&  /* movsb */
                 binary_emit_pop_reg(code, BINARY_GP_RSI);
      } else if (rep_ok) {
        /* stosb fills from AL, and RAX currently holds the destination, so the
         * fill byte goes in through RAX only after the destination is safe in
         * RDI -- and RAX has to be put back afterwards to return it. */
        rep_ok = binary_emit_mov_reg_reg(code, BINARY_GP_RDI, rep_dst) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RCX, rep_count) &&
                 binary_emit_push_reg(code, BINARY_GP_RAX) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RAX, rep_src) &&
                 binary_code_buffer_append_u8(code, 0xFC) &&  /* cld */
                 binary_code_buffer_append_u8(code, 0xF3) &&  /* rep  */
                 binary_code_buffer_append_u8(code, 0xAA) &&  /* stosb */
                 binary_emit_pop_reg(code, BINARY_GP_RAX);
      }
      if (!rep_ok || !binary_emit_pop_reg(code, BINARY_GP_RDI)) {
        ok = enc_err(fn, "out of memory emitting inline block copy");
      }
      break;
    }
    case MIR_CALL_INDIRECT: {
      /* Same frame contract as MIR_CALL: the prologue reserved shadow/stack
       * argument space, and regalloc kept the target out of any argument
       * register clobbered by the preceding marshalling moves. */
      int rok;
      BinaryGpRegister target = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok || !binary_emit_call_reg(&ctx->code, target)) {
        ok = enc_err(fn, "out of memory emitting indirect call");
      }
      break;
    }
    case MIR_SIMD_SLP_MAC: {
      /* Inline SLP MAC kernel. The preceding MIR_MOVs marshalled a/b/out element
       * pointers into RCX/RDX/R8, the k count into R9, and the byte row stride
       * into RAX; emit the pure inner loop (no operand loads, so it needs no
       * coherent fallback stack homes). dst.imm = K (4/8); width = b's element
       * size (1 = int8-widening kernel, 4 = int32 kernel). */
      if (in->width == 1
              ? !code_generator_binary_emit_simd_slp_mac_i8_loop(&ctx->code,
                                                                 in->dst.imm)
              : !code_generator_binary_emit_simd_slp_mac_i32_loop(&ctx->code,
                                                                  in->dst.imm)) {
        ok = enc_err(fn, "out of memory emitting inline SLP MAC kernel");
      }
      fn->used_inline_vector = 1;
      break;
    }
    case MIR_SIMD_FILL: {
      /* Inline fill kernel. The preceding MIR_MOVs marshalled the base pointer
       * into RCX, the element count (mode 0) or end pointer (mode 1) into R8, and
       * the fill value into RAX; emit the splat-build + 16-byte-store loop +
       * scalar tail (no operand loads, no live-iv write-back). dst.imm = element
       * size (1/2/4/8); a.imm = mode (0 element-counted, 1 byte-walk). The kernel
       * uses VEX.128 stores (upper YMM lanes zeroed), so no vzeroupper is needed
       * and used_inline_vector stays unset. */
      int fok = code_generator_binary_emit_simd_fill_splat(&ctx->code,
                                                           in->dst.imm);
      if (fok) {
        /* Mode 1 computes the byte length as R8-RCX inside the kernel; mode 2
         * arrives with R8 = byte length precomputed by the lowering. */
        fok = (in->a.imm == 0)
                  ? code_generator_binary_emit_simd_fill_loop_mode0(&ctx->code,
                                                                    in->dst.imm)
                  : code_generator_binary_emit_simd_fill_loop_bytewalk(
                        &ctx->code, in->dst.imm, (int)in->a.imm);
      }
      if (!fok) {
        ok = enc_err(fn, "out of memory emitting inline fill kernel");
      }
      break;
    }
    case MIR_SIMD_AFFINE_MAP_F32: {
      /* Inline float32 affine map. The preceding MIR_MOVs marshalled src->RCX,
       * dst->RDX, count->R8; dst.imm/a.imm/b.imm hold the a/b/c coefficient float
       * bits and cc holds b_is_one|b_is_zero<<1|c_is_zero<<2. The kernel emits
       * its own closing vzeroupper, so used_inline_vector stays unset. */
      if (!code_generator_binary_emit_simd_affine_map_f32_inline(
              &ctx->code, (unsigned)in->dst.imm, (unsigned)in->a.imm,
              (unsigned)in->b.imm, (in->cc & 1) != 0, (in->cc & 2) != 0,
              (in->cc & 4) != 0)) {
        ok = enc_err(fn, "out of memory emitting inline affine-map kernel");
      }
      break;
    }
    case MIR_SIMD_AFFINE_MAP_F64: {
      /* Inline float64 affine map. The preceding MIR_MOVs marshalled src->RCX,
       * dst->RDX, count->R8; dst.imm/a.imm/b.imm hold the a/b/c coefficient
       * double bits and cc holds b_is_one|b_is_zero<<1|c_is_zero<<2. The kernel
       * emits its own closing vzeroupper, so used_inline_vector stays unset. */
      if (!code_generator_binary_emit_simd_affine_map_f64_inline(
              &ctx->code, (unsigned long long)in->dst.imm,
              (unsigned long long)in->a.imm, (unsigned long long)in->b.imm,
              (in->cc & 1) != 0, (in->cc & 2) != 0, (in->cc & 4) != 0,
              (in->cc & 8) != 0)) {
        ok = enc_err(fn, "out of memory emitting inline f64 affine-map kernel");
      }
      break;
    }
    case MIR_SIMD_VLOOP: {
      /* Inline general vloop (float64 map). The preceding MIR_MOVs marshalled the
       * base pointers + count into the ABI arg registers; the DAG comes from the
       * borrowed IRInstruction in `aux`. operands_marshaled=1 makes the kernel
       * read them from registers instead of the operands' stack homes. */
      const IRInstruction *vir = (const IRInstruction *)in->aux;
      if (!vir || !code_generator_binary_emit_simd_vloop_f64(
                      fn->generator, fn->context, vir, 1)) {
        ok = enc_err(fn, "out of memory emitting inline vloop kernel");
      }
      break;
    }
    case MIR_IR_KERNEL: {
      /* Generic inline kernel. The preceding MIR_MOVs staged each by-name
       * operand into its own frame slot; publish those slot addresses on the
       * context so the kernel's own emit_operand_load / emit_destination_store
       * calls resolve to them, run the unmodified fallback emitter, then take
       * the map back down. The MIR_MOVs that follow read the slots back.
       *
       * The slot address is the frame base plus the staging vreg's spill
       * offset, both final by now. Some kernels borrow stack below rsp
       * (a balanced sub/add) or push a register, so the base has to be rbp --
       * mir_regalloc keeps the frame pointer for any function containing one of
       * these. */
      const MirKernelAux *ka = (const MirKernelAux *)in->aux;
      const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
      if (!ka || !kern || ka->operand_count > BINARY_MAX_MARSHALED_OPERANDS) {
        ok = enc_err(fn, "malformed inline kernel");
        break;
      }
      for (int s = 0; s < ka->operand_count; s++) {
        const MirVreg *sv = &fn->vregs[ka->slot_vreg[ka->operand_slot[s]]];
        ctx->marshaled_operands[s].operand = ka->operand[s];
        ctx->marshaled_operands[s].base_register = frame_base(fn);
        ctx->marshaled_operands[s].displacement =
            frame_disp(fn, -sv->spill_offset);
      }
      ctx->marshaled_operand_count = (size_t)ka->operand_count;
      int kok = kern->emit(fn->generator, ctx, ka->ir);
      ctx->marshaled_operand_count = 0;
      if (!kok) {
        ok = enc_err(fn, "failed to emit inline kernel");
        break;
      }
      /* These kernels leave the YMM upper halves dirty (several emit their own
       * closing vzeroupper, but not all); one more in the epilogue costs a
       * single instruction per function and removes the need to track which. */
      fn->used_inline_vector = 1;
      break;
    }
    case MIR_SIMD_SILU_F32: {
      /* Inline SiLU/SwiGLU gate. g/out->RCX, u->RDX, count->R8 marshalled by the
       * preceding MIR_MOVs; dst.imm = has_mul. The kernel emits its own closing
       * vzeroupper, so used_inline_vector stays unset. */
      if (!code_generator_binary_emit_simd_silu_f32_inline(&ctx->code,
                                                           (int)in->dst.imm)) {
        ok = enc_err(fn, "out of memory emitting inline SiLU kernel");
      }
      break;
    }
    case MIR_STORE_OUTARG: {
      /* Store an outgoing stack call argument to [rsp + b.imm]. rsp is fixed
       * after the prologue and the outgoing region is reserved there, so this
       * is a plain rsp-relative store. A float value's bits bounce through the
       * GP scratch (movq/movd from its XMM, or a plain load from its spill). */
      int ok = 1;
      BinaryGpRegister r = SCRATCH_A;
      if (in->is_float && in->a.kind == MIR_OPK_VREG) {
        const MirVreg *v = &fn->vregs[in->a.vreg];
        if (v->in_register) {
          ok = (in->width == 4)
                   ? binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A,
                                              (BinaryXmmRegister)v->phys)
                   : binary_emit_movq_reg_xmm(&ctx->code, SCRATCH_A,
                                              (BinaryXmmRegister)v->phys);
        } else {
          ok = binary_emit_mov_reg_mem(&ctx->code, SCRATCH_A, frame_base(fn),
                                       frame_disp(fn, -v->spill_offset));
        }
        if (!ok) {
          ok = enc_err(fn, "out of memory staging float stack argument");
          break;
        }
      } else {
        r = value_reg(fn, &in->a, SCRATCH_A, &ok);
        if (!ok) {
          break;
        }
      }
      if (in->is_float && in->width == 4
              ? !binary_emit_mov_mem_reg32(&ctx->code, BINARY_GP_RSP,
                                           (int)in->b.imm, r)
              : !binary_emit_mov_mem_reg(&ctx->code, BINARY_GP_RSP,
                                         (int)in->b.imm, r)) {
        ok = enc_err(fn, "out of memory storing outgoing call argument");
      }
      break;
    }
    case MIR_CMOV: {
      /* dst = (a != 0) ? b : dst. dst was pre-loaded with the else value by a
       * preceding MIR_MOV, so `test a; cmovnz dst, b` completes the select.
       * A spilled dst is staged through SCRATCH_A; cond stages through
       * SCRATCH_B, and `then` reuses SCRATCH_B (cond is dead after the test). */
      int rok;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = D;
      if (!dst_in_reg) {
        const MirVreg *v = &fn->vregs[in->dst.vreg];
        target = SCRATCH_A;
        if (!gp_home_load(fn, v, SCRATCH_A)) {
          ok = enc_err(fn, "out of memory loading cmov dst");
          break;
        }
      }
      BinaryGpRegister creg = value_reg(fn, &in->a, SCRATCH_B, &rok);
      if (!rok || !binary_emit_test_reg_reg(&ctx->code, creg)) {
        ok = enc_err(fn, "out of memory in cmov test");
        break;
      }
      BinaryGpRegister treg = value_reg(fn, &in->b, SCRATCH_B, &rok);
      if (!rok ||
          !binary_emit_cmovcc_reg_reg(&ctx->code, 0x45, target, treg)) {
        ok = enc_err(fn, "out of memory in cmov");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, target);
      }
      break;
    }
    case MIR_PREFETCH: {
      /* prefetcht0 [base + disp]: advisory, no destination. The address vreg
       * is a plain read; a spilled address stages through SCRATCH_A. */
      if (in->a.kind != MIR_OPK_MEM || in->a.mem.index != MIR_VREG_NONE) {
        ok = enc_err(fn, "MIR_PREFETCH expects a base-only memory operand");
        break;
      }
      int prok;
      MirOperand pbop = mir_op_vreg(in->a.mem.base);
      BinaryGpRegister pbase = value_reg(fn, &pbop, SCRATCH_A, &prok);
      if (!prok) {
        break;
      }
      if (!binary_emit_prefetcht0_mem(&ctx->code, pbase, in->a.mem.disp)) {
        ok = enc_err(fn, "out of memory in prefetch");
      }
      break;
    }
    case MIR_LEA: {
      /* dst <- address of [base + index*scale + disp]. base/index are vregs
       * (index optional). Mirrors the scaled-LOAD address staging but
       * materializes the address instead of dereferencing it. Emitted by the
       * SLP-kernel lowering to form effective element pointers. */
      if (in->a.kind != MIR_OPK_MEM) {
        ok = enc_err(fn, "MIR_LEA expects a memory operand");
        break;
      }
      int rok;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      MirOperand bop = mir_op_vreg(in->a.mem.base);
      /* Same staging collision as the scaled LOAD above: keep base and index
       * clear of the target and of each other, resident registers included. */
      BinaryGpRegister taken[4];
      int tn = 0;
      mir_note_fixed_reg(fn, &bop, taken, &tn);
      MirOperand iop_probe = mir_op_vreg(in->a.mem.index);
      if (in->a.mem.index != MIR_VREG_NONE) {
        mir_note_fixed_reg(fn, &iop_probe, taken, &tn);
      }
      /* As in the scaled load: the lea writes `target` last. */
      BinaryGpRegister vouch[1];
      int vn = mir_reg_in(target, taken, tn) ? 0 : 1;
      vouch[0] = target;
      size_t idx = (size_t)(in - fn->insns);
      BinaryGpRegister base_scratch, index_scratch;
      if (!mir_pick_scratch(fn, idx, SCRATCH_B, taken, tn, vouch, vn,
                            &base_scratch)) {
        ok = enc_err(fn, "no free scratch register for a scaled lea base");
        break;
      }
      BinaryGpRegister base_reg = value_reg(fn, &bop, base_scratch, &rok);
      if (!rok) {
        break;
      }
      taken[tn++] = base_reg;
      if (in->a.mem.index != MIR_VREG_NONE) {
        MirOperand iop = mir_op_vreg(in->a.mem.index);
        if (!mir_pick_scratch(fn, idx, BINARY_GP_RDX, taken, tn, vouch, vn,
                              &index_scratch)) {
          ok = enc_err(fn, "no free scratch register for a scaled lea index");
          break;
        }
        BinaryGpRegister index_reg = value_reg(fn, &iop, index_scratch, &rok);
        if (!rok) {
          break;
        }
        if (!binary_emit_lea_reg_base_index_scale_disp(
                &ctx->code, target, base_reg, index_reg, in->a.mem.scale,
                in->a.mem.disp)) {
          ok = enc_err(fn, "out of memory in scaled lea");
          break;
        }
      } else if (!binary_emit_lea_reg_mem(&ctx->code, target, base_reg,
                                          in->a.mem.disp)) {
        ok = enc_err(fn, "out of memory in lea");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_OUTARG: {
      /* dst <- lea &slot in the INDIRECT struct-arg copy region. That region
       * sits ABOVE the Win64 shadow space and the outgoing stack args (so a
       * callee writing its shadow at [rsp..rsp+32] cannot clobber the copies),
       * hence the absolute rsp offset is shadow + outgoing_stack_bytes + the
       * per-arg slot offset (in->a.imm). rsp is fixed after the prologue. */
      const BinaryAbi *oa = code_generator_binary_active_abi();
      int off = oa->shadow_space_size + fn->outgoing_stack_bytes + (int)in->a.imm;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!binary_emit_lea_reg_mem(&ctx->code, target, BINARY_GP_RSP, off)) {
        ok = enc_err(fn, "out of memory in lea outarg");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_GLOBAL: {
      /* dst <- RIP-relative address of global symbol a.sym. is_unsigned carries
       * the declare-external flag (set by lowering from the symbol). */
      const char *name = in->a.sym ? in->a.sym : "";
      const char *link = code_generator_get_link_symbol_name(fn->generator, name);
      if (!link || link[0] == '\0') {
        ok = enc_err(fn, "invalid global symbol in address-of");
        break;
      }
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_symbol_address(fn->generator, ctx, link,
                                                     in->is_unsigned, target)) {
        ok = enc_err(fn, "out of memory emitting global address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_FUNC: {
      /* dst <- RIP-relative address of function symbol a.sym. This shares the
       * same relocation path as global addresses; the linker resolves the code
       * symbol and the function pointer receives that address. */
      const char *name = in->a.sym ? in->a.sym : "";
      const char *link = code_generator_get_link_symbol_name(fn->generator, name);
      if (!link || link[0] == '\0') {
        ok = enc_err(fn, "invalid function symbol in address-of");
        break;
      }
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_symbol_address(fn->generator, ctx, link,
                                                     in->is_unsigned, target)) {
        ok = enc_err(fn, "out of memory emitting function address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_LOCAL: {
      /* dst <- address of local vreg a's stack home. The allocator forces an
       * address-taken value to spill, so a is always memory-resident. */
      const MirVreg *lv = &fn->vregs[in->a.vreg];
      if (lv->in_register) {
        ok = enc_err(fn, "address-taken value was not spilled");
        break;
      }
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!binary_emit_lea_reg_mem(&ctx->code, target, frame_base(fn),
                                   frame_disp(fn, -lv->spill_offset))) {
        ok = enc_err(fn, "out of memory emitting local address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_CSTR: {
      /* dst <- address of the string literal a.sym (RIP-relative lea into a
       * .rdata cstring). dst is typically an ABI argument register. */
      const char *s = in->a.sym ? in->a.sym : "";
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_cstring_literal_address(fn->generator, ctx,
                                                              s, target)) {
        ok = enc_err(fn, "out of memory emitting cstring argument");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_STRLIT: {
      /* dst <- address of the string literal a.sym's {chars,length} record in
       * .rdata (the fat `string` value the fallback's
       * emit_string_literal_value_address materializes). */
      const char *s = in->a.sym ? in->a.sym : "";
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_string_literal_value_address(
              fn->generator, ctx, s,
              in->a.imm > 0 ? (size_t)in->a.imm : strlen(s), target)) {
        ok = enc_err(fn, "out of memory emitting string-literal record address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_TRAP: {
      /* Terminal abort for a failed safety check. MIR only runs without
       * stack-trace support, so this is the degraded path: puts(message) +
       * exit(1) (matching code_generator_binary_emit_runtime_trap_call). rsp
       * already sits on the reserved shadow space (mir_has_calls counts
       * MIR_TRAP), so the calls need no rsp adjustment. The sequence never
       * returns; it is reached only on the cold guard-fail branch. */
      const BinaryAbi *abi = code_generator_binary_active_abi();
      BinaryGpRegister arg0 = abi->int_param_registers[0];
      const char *msg = in->a.sym ? in->a.sym : "";
      size_t off = 0;
      if (!code_generator_binary_declare_external_symbol(fn->generator, "puts") ||
          !code_generator_binary_declare_external_symbol(fn->generator, "exit")) {
        ok = enc_err(fn, "out of memory declaring trap externals");
        break;
      }
      if (!code_generator_binary_emit_cstring_literal_address(fn->generator, ctx,
                                                              msg, arg0)) {
        ok = enc_err(fn, "out of memory emitting trap message");
        break;
      }
      if (!binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, "puts",
                                            off)) {
        ok = enc_err(fn, "out of memory emitting trap puts");
        break;
      }
      if (!binary_emit_mov_reg_imm64(&ctx->code, arg0, 1)) {
        ok = enc_err(fn, "out of memory emitting trap exit arg");
        break;
      }
      off = 0;
      if (!binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, "exit",
                                            off)) {
        ok = enc_err(fn, "out of memory emitting trap exit");
        break;
      }
      break;
    }
    case MIR_CMPBR: {
      /* cmp a,b ; j<cc> label  (fused compare-and-branch). */
      int rok;
      BinaryGpRegister cbase;
      int cdisp;
      BinaryGpRegister areg;
      /* `a < b || (a == b && ...)` -- a lexicographic compare, and the shape
       * every comparator and heap sift-down is written in -- emits the same
       * cmp twice with two condition codes. Only the first branch stands
       * between them and a jcc leaves the flags alone, so the second compare
       * is recomputing what the register already holds. */
      {
        size_t p = i;
        while (p > 0 && prev_cmpbr != p - 1 &&
               mir_cmp_gap_is_empty(fn, &fn->insns[p - 1])) {
          p--;
        }
        if (p > 0 && prev_cmpbr == p - 1) {
          const MirInst *pv = &fn->insns[prev_cmpbr];
          if (pv->width == in->width && pv->is_unsigned == in->is_unsigned &&
              !pv->is_float && !in->is_float &&
              mir_cmp_operand_reusable(fn, &pv->a, &in->a) &&
              mir_cmp_operand_reusable(fn, &pv->b, &in->b)) {
            size_t reuse_off = 0;
            if (!binary_emit_jcc_placeholder(&ctx->code, in->cc,
                                             &reuse_off) ||
                !binary_label_fixup_table_add(&ctx->label_fixups,
                                              in->dst.sym, reuse_off)) {
              ok = enc_err(fn, "out of memory in cmpbr");
            }
            prev_cmpbr = i;
            break;
          }
        }
      }
      areg = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok) {
        ok = 0;
        break;
      }
      if (fused_mask_test != (size_t)-1 && fused_mask_test + 1 == i) {
        const MirInst *and_op = &fn->insns[i - 1];
        BinaryGpRegister src =
            (BinaryGpRegister)fn->vregs[and_op->a.vreg].phys;
        if (!binary_emit_test_reg_imm32(&ctx->code, src,
                                        (uint32_t)and_op->b.imm)) {
          ok = enc_err(fn, "out of memory in fused mask test");
          break;
        }
      } else if (fused_byte_load != (size_t)-1 && fused_byte_load + 1 == i) {
        const MirMem *m = &fn->insns[i - 1].a.mem;
        BinaryGpRegister mb = (BinaryGpRegister)fn->vregs[m->base].phys;
        int need_rex = (areg >= 4 && areg <= 7);
        int emitted;
        if (m->index != MIR_VREG_NONE) {
          BinaryGpRegister mi = (BinaryGpRegister)fn->vregs[m->index].phys;
          emitted = need_rex ? binary_emit_memory_access_sib_forced(
                                   &ctx->code, 0, 0, 0x3A, 0, 0, areg, mb, mi,
                                   m->scale, m->disp)
                             : binary_emit_memory_access_sib(
                                   &ctx->code, 0, 0, 0x3A, 0, 0, areg, mb, mi,
                                   m->scale, m->disp);
        } else {
          emitted = need_rex ? binary_emit_memory_access_ex_forced(
                                   &ctx->code, 0, 0, 0x3A, 0, 0, areg, mb,
                                   m->disp)
                             : binary_emit_memory_access_ex(&ctx->code, 0, 0,
                                                            0x3A, 0, 0, areg,
                                                            mb, m->disp);
        }
        if (!emitted) {
          ok = enc_err(fn, "out of memory in fused byte compare");
          break;
        }
      } else if (in->width == 4) {
        /* 4-byte (int32/uint32) compare: 32-bit cmp ignores garbage high bits a
         * 64-bit MIR value may carry (see encode_setcc). An immediate folds into
         * the 32-bit cmp directly (its low 32 bits are the constant); only a
         * register operand needs the reg-reg form. */
        if (in->b.kind == MIR_OPK_IMM) {
          if (!binary_emit_cmp_reg_imm_w32(&ctx->code, areg,
                                           (uint32_t)in->b.imm)) {
            ok = enc_err(fn, "out of memory in cmpbr32 imm");
            break;
          }
        } else if (gp_home_mem(fn, &in->b, &cbase, &cdisp)) {
          if (!binary_emit_alu_reg_mem(&ctx->code, 0x39, areg, cbase, cdisp,
                                       4)) {
            ok = enc_err(fn, "out of memory in cmpbr32 mem");
            break;
          }
        } else {
          BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &rok);
          if (!rok || !binary_emit_cmp_reg_reg32(&ctx->code, areg, breg)) {
            ok = enc_err(fn, "out of memory in cmpbr32");
            break;
          }
        }
      } else if (in->b.kind == MIR_OPK_IMM &&
                 code_generator_binary_immediate_fits_signed_32(in->b.imm)) {
        if (!binary_emit_cmp_reg_imm32(&ctx->code, areg, (uint32_t)in->b.imm)) {
          ok = enc_err(fn, "out of memory in cmpbr");
          break;
        }
      } else if (gp_home_mem(fn, &in->b, &cbase, &cdisp)) {
        if (!binary_emit_alu_reg_mem(&ctx->code, 0x39, areg, cbase, cdisp, 8)) {
          ok = enc_err(fn, "out of memory in cmpbr mem");
          break;
        }
      } else {
        BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &rok);
        if (!rok || !binary_emit_cmp_reg_reg(&ctx->code, areg, breg)) {
          ok = enc_err(fn, "out of memory in cmpbr");
          break;
        }
      }
      size_t off = 0;
      if (!binary_emit_jcc_placeholder(&ctx->code, in->cc, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in cmpbr");
      }
      prev_cmpbr = i;
      break;
    }
    case MIR_JMP_TABLE: {
      const MirJumpTable *tbl = (const MirJumpTable *)in->aux;
      int rok;
      BinaryGpRegister idx;
      size_t lea_off = 0;
      if (!tbl || pending_table_count >= MIR_MAX_JUMP_TABLES) {
        ok = enc_err(fn, "jump table without a target list");
        break;
      }
      idx = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok) {
        ok = 0;
        break;
      }
      if (!binary_emit_lea_reg_rip_placeholder(&ctx->code, SCRATCH_B,
                                               &lea_off) ||
          !emit_ext_load(&ctx->code, SCRATCH_A, SCRATCH_B, 1, idx, 4, 0, 4, 1) ||
          !binary_emit_alu_reg_reg(&ctx->code, 0x01, SCRATCH_A, SCRATCH_B) ||
          !binary_emit_jmp_reg(&ctx->code, SCRATCH_A)) {
        ok = enc_err(fn, "out of memory in jump table");
        break;
      }
      pending_tables[pending_table_count].lea_off = lea_off;
      pending_tables[pending_table_count].table = tbl;
      pending_table_count++;
      break;
    }
    case MIR_RET:
      ok = mir_emit_epilogue(fn);
      break;
    default:
      ok = enc_err(fn, "unsupported MIR opcode in encoder");
      break;
    }
    if (annot && ok && ctx->code.size > annot_off) {
      mir_annotate_record(fn, in, (int)i, annot_off - annot_base,
                          ctx->code.size - annot_off,
                          ctx->code.data + annot_off);
    }
    if (!ok) {
      free(align_label);
      return 0;
    }
  }
  free(align_label);

  for (size_t t = 0; t < pending_table_count; t++) {
    const MirJumpTable *tbl = pending_tables[t].table;
    size_t table_off;
    while ((ctx->code.size & 3u) != 0u) {
      if (!binary_code_buffer_append_u8(&ctx->code, 0xCC)) {
        return enc_err(fn, "out of memory in jump table");
      }
    }
    table_off = ctx->code.size;
    if (!binary_function_context_patch_rel32(ctx, pending_tables[t].lea_off,
                                             table_off)) {
      return enc_err(fn, "jump table out of range");
    }
    for (size_t e = 0; e < tbl->count; e++) {
      BinaryLabelEntry *label = binary_label_table_get(&ctx->labels,
                                                       tbl->labels[e]);
      long long delta;
      if (!label) {
        return enc_err(fn, "undefined jump table target");
      }
      delta = (long long)label->offset - (long long)table_off;
      if (delta < -2147483648LL || delta > 2147483647LL ||
          !binary_code_buffer_append_u32(&ctx->code,
                                         (uint32_t)(int32_t)delta)) {
        return enc_err(fn, "jump table out of range");
      }
    }
  }

  /* Resolve label/jump rel32 fixups against the defined labels. */
  if (!code_generator_binary_resolve_fixups(fn->generator, ctx,
                                            ctx->code.size)) {
    return 0;
  }
  return 1;
}
