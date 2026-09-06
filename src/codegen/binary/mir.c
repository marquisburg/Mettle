#include "codegen/binary/mir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mir_function_init(MirFunction *fn, BinaryFunctionContext *context) {
  if (!fn) {
    return;
  }
  memset(fn, 0, sizeof(*fn));
  fn->context = context;
  fn->indirect_return_vreg = MIR_VREG_NONE;
  fn->cur_ir_index = -1; /* no IR instruction open yet (annotator) */
}

void mir_function_destroy(MirFunction *fn) {
  if (!fn) {
    return;
  }
  free(fn->vregs);
  free(fn->insns);
  free(fn->fconsts);
  free(fn->iconsts);
  for (size_t i = 0; i < fn->owned_sym_count; i++) {
    free(fn->owned_syms[i]);
  }
  free(fn->owned_syms);
  for (size_t i = 0; i < fn->owned_aux_count; i++) {
    free(fn->owned_aux[i]);
  }
  free(fn->owned_aux);
  free(fn->label_slots);
  memset(fn, 0, sizeof(*fn));
}

void *mir_function_own_aux(MirFunction *fn, void *block) {
  if (!fn || !block) {
    if (fn) {
      fn->has_error = 1;
    }
    free(block);
    return NULL;
  }
  if (fn->owned_aux_count >= fn->owned_aux_capacity) {
    size_t nc = fn->owned_aux_capacity ? fn->owned_aux_capacity * 2 : 4;
    void **grown = (void **)realloc(fn->owned_aux, nc * sizeof(void *));
    if (!grown) {
      fn->has_error = 1;
      free(block);
      return NULL;
    }
    fn->owned_aux = grown;
    fn->owned_aux_capacity = nc;
  }
  fn->owned_aux[fn->owned_aux_count++] = block;
  return block;
}

MirVregId mir_new_vreg(MirFunction *fn, MirRegClass rclass, int width) {
  if (!fn) {
    return MIR_VREG_NONE;
  }
  if (fn->vreg_count >= fn->vreg_capacity) {
    size_t new_cap = fn->vreg_capacity ? fn->vreg_capacity * 2 : 16;
    MirVreg *grown = (MirVreg *)realloc(fn->vregs, new_cap * sizeof(MirVreg));
    if (!grown) {
      fn->has_error = 1;
      return MIR_VREG_NONE;
    }
    fn->vregs = grown;
    fn->vreg_capacity = new_cap;
  }
  MirVreg *v = &fn->vregs[fn->vreg_count];
  memset(v, 0, sizeof(*v));
  v->rclass = rclass;
  v->width = width;
  v->phys = -1;
  v->spill_offset = 0;
  v->live_start = MIR_LIVE_NONE;
  v->live_end = MIR_LIVE_NONE;
  v->loop_carried = 0;
  v->coalesce_hint = MIR_VREG_NONE;
  return (MirVregId)(fn->vreg_count++);
}

int mir_emit(MirFunction *fn, const MirInst *inst) {
  if (!fn || !inst) {
    return 0;
  }
  if (fn->insn_count >= fn->insn_capacity) {
    size_t new_cap = fn->insn_capacity ? fn->insn_capacity * 2 : 32;
    MirInst *grown = (MirInst *)realloc(fn->insns, new_cap * sizeof(MirInst));
    if (!grown) {
      fn->has_error = 1;
      return 0;
    }
    fn->insns = grown;
    fn->insn_capacity = new_cap;
  }
  fn->insns[fn->insn_count++] = *inst;
  /* --annotate-asm: trace each emitted op back to the IR instruction being
   * lowered, unless the caller already set a specific ir_index. */
  if (inst->ir_index < 0 && fn->cur_ir_index >= 0) {
    fn->insns[fn->insn_count - 1].ir_index = fn->cur_ir_index;
  }
  return 1;
}

MirOperand mir_op_none(void) {
  MirOperand op;
  memset(&op, 0, sizeof(op));
  op.kind = MIR_OPK_NONE;
  op.vreg = MIR_VREG_NONE;
  op.mem.base = MIR_VREG_NONE;
  op.mem.index = MIR_VREG_NONE;
  return op;
}

MirOperand mir_op_vreg(MirVregId v) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_VREG;
  op.vreg = v;
  return op;
}

MirOperand mir_op_phys(int phys, MirRegClass rclass) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_PHYS;
  op.phys = phys;
  op.rclass = rclass;
  return op;
}

MirOperand mir_op_imm(long long value) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_IMM;
  op.imm = value;
  return op;
}

MirOperand mir_op_fimm(uint64_t ieee_bits) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_FIMM;
  op.imm = (long long)ieee_bits;
  return op;
}

MirOperand mir_op_label(const char *name) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_LABEL;
  op.sym = name;
  return op;
}

MirOperand mir_op_symbol(const char *name) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_SYMBOL;
  op.sym = name;
  return op;
}

MirOperand mir_op_stackhome(const char *name, int rbp_disp) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_STACKHOME;
  op.sym = name;
  op.disp = rbp_disp;
  return op;
}

MirOperand mir_op_mem_vreg(MirVregId base, MirVregId index, int scale,
                           int disp) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_MEM;
  op.mem.base = base;
  op.mem.index = index;
  op.mem.scale = scale;
  op.mem.disp = disp;
  op.mem.phys_base_valid = 0;
  return op;
}

MirOperand mir_op_mem_rbp(int rbp_disp) {
  MirOperand op = mir_op_none();
  op.kind = MIR_OPK_MEM;
  op.mem.base = MIR_VREG_NONE;
  op.mem.index = MIR_VREG_NONE;
  op.mem.scale = 0;
  op.mem.disp = rbp_disp;
  op.mem.phys_base_valid = 1;
  op.mem.phys_base = BINARY_GP_RBP;
  return op;
}

/* ---- dump --------------------------------------------------------------- */

const char *mir_opcode_name(MirOpcode op) {
  switch (op) {
  case MIR_NOP: return "nop";
  case MIR_MOV: return "mov";
  case MIR_LEA: return "lea";
  case MIR_LEA_LOCAL: return "lea_local";
  case MIR_LEA_GLOBAL: return "lea_global";
  case MIR_LEA_FUNC: return "lea_func";
  case MIR_LEA_CSTR: return "lea_cstr";
  case MIR_LEA_STRLIT: return "lea_strlit";
  case MIR_POPCNT: return "popcnt";
  case MIR_HEAP_NEW: return "heap_new";
  case MIR_MOVZX: return "movzx";
  case MIR_MOVSX: return "movsx";
  case MIR_LOAD_GLOBAL: return "ldglobal";
  case MIR_STORE_GLOBAL: return "stglobal";
  case MIR_PREFETCH: return "prefetch";
  case MIR_CMOV: return "cmov";
  case MIR_ADD: return "add";
  case MIR_SUB: return "sub";
  case MIR_AND: return "and";
  case MIR_OR: return "or";
  case MIR_XOR: return "xor";
  case MIR_IMUL: return "imul";
  case MIR_NEG: return "neg";
  case MIR_NOT: return "not";
  case MIR_SHL: return "shl";
  case MIR_SHR: return "shr";
  case MIR_SAR: return "sar";
  case MIR_CQO: return "cqo";
  case MIR_XOR_RDX: return "xor_rdx";
  case MIR_IDIV: return "idiv";
  case MIR_DIV: return "div";
  case MIR_MULHI: return "mulhi";
  case MIR_CMP: return "cmp";
  case MIR_TEST: return "test";
  case MIR_SETCC: return "setcc";
  case MIR_CMOVCC: return "cmovcc";
  case MIR_JMP: return "jmp";
  case MIR_JCC: return "jcc";
  case MIR_CMPBR: return "cmpbr";
  case MIR_JMP_TABLE: return "jmp_table";
  case MIR_LABEL: return "label";
  case MIR_CALL: return "call";
  case MIR_CALL_INDIRECT: return "call_indirect";
  case MIR_REP_MOVSB: return "rep_movsb";
  case MIR_REP_STOSB: return "rep_stosb";
  case MIR_SYSCALL: return "syscall";
  case MIR_STORE_OUTARG: return "store_outarg";
  case MIR_LEA_OUTARG: return "lea_outarg";
  case MIR_TRAP: return "trap";
  case MIR_RET: return "ret";
  case MIR_FADD: return "fadd";
  case MIR_FSUB: return "fsub";
  case MIR_FXOR: return "fxor";
  case MIR_FMUL: return "fmul";
  case MIR_FDIV: return "fdiv";
  case MIR_CVTSI2F: return "cvtsi2f";
  case MIR_CVTF2SI: return "cvtf2si";
  case MIR_CVTF2F: return "cvtf2f";
  case MIR_UCOMIS: return "ucomis";
  case MIR_FSETCC: return "fsetcc";
  case MIR_FCMPBR: return "fcmpbr";
  case MIR_MOVD_TO_XMM: return "movd2xmm";
  case MIR_MOVD_TO_GP: return "movd2gp";
  case MIR_CVTPH2PS: return "cvtph2ps";
  case MIR_CVTPS2PH: return "cvtps2ph";
  case MIR_VADD: return "vadd";
  case MIR_VSUB: return "vsub";
  case MIR_VMUL: return "vmul";
  case MIR_VDIV: return "vdiv";
  case MIR_VCVTSI2F: return "vcvtsi2f";
  case MIR_VCVTF2SI: return "vcvtf2si";
  case MIR_VLOAD: return "vload";
  case MIR_VSTORE: return "vstore";
  case MIR_VBROADCAST: return "vbroadcast";
  case MIR_VIOTA: return "viota";
  case MIR_VHREDUCE: return "vhreduce";
  case MIR_SIMD_SLP_MAC: return "simd_slp_mac";
  case MIR_SIMD_FILL: return "simd_fill";
  case MIR_SIMD_AFFINE_MAP_F32: return "simd_affine_map_f32";
  case MIR_SIMD_AFFINE_MAP_F64: return "simd_affine_map_f64";
  case MIR_SIMD_SILU_F32: return "simd_silu_f32";
  case MIR_SIMD_VLOOP: return "simd_vloop";
  case MIR_IR_KERNEL: return "ir_kernel";
  case MIR_OPCODE_COUNT: break;
  }
  return "?";
}

int mir_op_is_inline_kernel(MirOpcode op) {
  switch (op) {
  case MIR_SIMD_SLP_MAC:
  case MIR_SIMD_FILL:
  case MIR_SIMD_AFFINE_MAP_F32:
  case MIR_SIMD_AFFINE_MAP_F64:
  case MIR_SIMD_SILU_F32:
  case MIR_SIMD_VLOOP:
  case MIR_IR_KERNEL:
    return 1;
  default:
    return 0;
  }
}

static void mir_dump_operand(const MirFunction *fn, const MirOperand *op,
                             FILE *out) {
  (void)fn;
  switch (op->kind) {
  case MIR_OPK_NONE:
    break;
  case MIR_OPK_VREG:
    fprintf(out, "v%d", op->vreg);
    break;
  case MIR_OPK_PHYS:
    fprintf(out, "%s%d", op->rclass == MIR_RC_XMM ? "xmm" : "r", op->phys);
    break;
  case MIR_OPK_IMM:
    fprintf(out, "#%lld", op->imm);
    break;
  case MIR_OPK_FIMM:
    fprintf(out, "f#%016llx", (unsigned long long)op->imm);
    break;
  case MIR_OPK_MEM:
    fputc('[', out);
    if (op->mem.phys_base_valid) {
      fprintf(out, "rbp%+d", op->mem.disp);
    } else {
      if (op->mem.base != MIR_VREG_NONE) {
        fprintf(out, "v%d", op->mem.base);
      }
      if (op->mem.index != MIR_VREG_NONE) {
        fprintf(out, "+v%d*%d", op->mem.index, op->mem.scale);
      }
      if (op->mem.disp) {
        fprintf(out, "%+d", op->mem.disp);
      }
    }
    fputc(']', out);
    break;
  case MIR_OPK_LABEL:
    fprintf(out, ".%s", op->sym ? op->sym : "?");
    break;
  case MIR_OPK_SYMBOL:
    fprintf(out, "@%s", op->sym ? op->sym : "?");
    break;
  case MIR_OPK_STACKHOME:
    fprintf(out, "home(%s)[rbp-%d]", op->sym ? op->sym : "?", op->disp);
    break;
  }
}

void mir_function_dump(const MirFunction *fn, FILE *out) {
  if (!fn || !out) {
    return;
  }
  fprintf(out, "; MIR function: %zu vregs, %zu insns, spill_bytes=%d\n",
          fn->vreg_count, fn->insn_count, fn->spill_bytes);
  for (size_t v = 0; v < fn->vreg_count; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (!vr->assigned) {
      continue;
    }
    fprintf(out, ";   v%-4zu %-6s%-4d live=[%d,%d]%s%s%s\n", v,
            vr->in_register ? (vr->rclass == MIR_RC_XMM ? "xmm" : "r") : "spill",
            vr->in_register ? vr->phys : vr->spill_offset, vr->live_start,
            vr->live_end, vr->crosses_call ? " call" : "",
            vr->loop_carried ? " loop" : "",
            vr->address_taken ? " addr" : "");
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    fprintf(out, "%4zu: %-8s", i, mir_opcode_name(in->op));
    if (in->dst.kind != MIR_OPK_NONE) {
      fputc(' ', out);
      mir_dump_operand(fn, &in->dst, out);
    }
    if (in->a.kind != MIR_OPK_NONE) {
      fputs(", ", out);
      mir_dump_operand(fn, &in->a, out);
    }
    if (in->b.kind != MIR_OPK_NONE) {
      fputs(", ", out);
      mir_dump_operand(fn, &in->b, out);
    }
    fprintf(out, "   ; w%d%s%s\n", in->width, in->is_float ? " f" : "",
            in->is_unsigned ? " u" : "");
  }
}
