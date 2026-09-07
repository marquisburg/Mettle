#include "codegen/binary/mir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mir_param_layout(const MirFunction *fn, const BinaryAbi *abi,
                     BinaryArgLocation *locs, size_t *first_slot,
                     size_t *count_out) {
  int is_float[MIR_PARAM_SLOTS];
  int force_stack[MIR_PARAM_SLOTS];
  size_t stack_slots[MIR_PARAM_SLOTS];
  size_t n = 0;
  if (fn->returns_indirect) {
    is_float[n] = 0;
    force_stack[n] = 0;
    stack_slots[n] = 0;
    n++;
  }
  for (size_t i = 0; i < fn->param_count; i++) {
    const MirParam *p = &fn->params[i];
    first_slot[i] = n;
    if (p->sysv_in_memory) {
      if (n >= MIR_PARAM_SLOTS) {
        return 0;
      }
      is_float[n] = 0;
      force_stack[n] = 1;
      stack_slots[n] = ((size_t)p->sysv_size + 7u) / 8u;
      n++;
      continue;
    }
    if (p->sysv_eightbytes > 0) {
      for (int e = 0; e < p->sysv_eightbytes; e++) {
        if (n >= MIR_PARAM_SLOTS) {
          return 0;
        }
        is_float[n] = p->sysv_sse[e];
        force_stack[n] = 0;
        stack_slots[n] = 0;
        n++;
      }
      continue;
    }
    if (n >= MIR_PARAM_SLOTS) {
      return 0;
    }
    is_float[n] = p->is_float || p->sysv_direct_sse;
    force_stack[n] = 0;
    stack_slots[n] = 0;
    n++;
  }
  *count_out = n;
  if (n == 0) {
    return 1;
  }
  return code_generator_binary_compute_arg_layout_ex(
      abi, is_float, force_stack, stack_slots, n, locs, NULL);
}

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

/* ---- dump --------------------------------------------------------------- */

static const char *const MIR_OPCODE_NAMES[MIR_OPCODE_COUNT] = {
    [MIR_NOP] = "nop",
    [MIR_MOV] = "mov",
    [MIR_LEA] = "lea",
    [MIR_LEA_LOCAL] = "lea_local",
    [MIR_LEA_GLOBAL] = "lea_global",
    [MIR_LEA_FUNC] = "lea_func",
    [MIR_LEA_CSTR] = "lea_cstr",
    [MIR_LEA_STRLIT] = "lea_strlit",
    [MIR_POPCNT] = "popcnt",
    [MIR_HEAP_NEW] = "heap_new",
    [MIR_MOVZX] = "movzx",
    [MIR_MOVSX] = "movsx",
    [MIR_LOAD_GLOBAL] = "ldglobal",
    [MIR_STORE_GLOBAL] = "stglobal",
    [MIR_PREFETCH] = "prefetch",
    [MIR_CMOV] = "cmov",
    [MIR_ADD] = "add",
    [MIR_SUB] = "sub",
    [MIR_AND] = "and",
    [MIR_OR] = "or",
    [MIR_XOR] = "xor",
    [MIR_IMUL] = "imul",
    [MIR_NEG] = "neg",
    [MIR_NOT] = "not",
    [MIR_SHL] = "shl",
    [MIR_SHR] = "shr",
    [MIR_SAR] = "sar",
    [MIR_CQO] = "cqo",
    [MIR_XOR_RDX] = "xor_rdx",
    [MIR_IDIV] = "idiv",
    [MIR_DIV] = "div",
    [MIR_MULHI] = "mulhi",
    [MIR_CMP] = "cmp",
    [MIR_TEST] = "test",
    [MIR_SETCC] = "setcc",
    [MIR_CMOVCC] = "cmovcc",
    [MIR_JMP] = "jmp",
    [MIR_JCC] = "jcc",
    [MIR_CMPBR] = "cmpbr",
    [MIR_JMP_TABLE] = "jmp_table",
    [MIR_LABEL] = "label",
    [MIR_CALL] = "call",
    [MIR_CALL_INDIRECT] = "call_indirect",
    [MIR_REP_MOVSB] = "rep_movsb",
    [MIR_REP_STOSB] = "rep_stosb",
    [MIR_SYSCALL] = "syscall",
    [MIR_STORE_OUTARG] = "store_outarg",
    [MIR_LEA_OUTARG] = "lea_outarg",
    [MIR_TRAP] = "trap",
    [MIR_INLINE_ASM] = "inline_asm",
    [MIR_RET] = "ret",
    [MIR_FADD] = "fadd",
    [MIR_FSUB] = "fsub",
    [MIR_FXOR] = "fxor",
    [MIR_FMUL] = "fmul",
    [MIR_FDIV] = "fdiv",
    [MIR_FDUP] = "fdup",
    [MIR_FEXTHI] = "fexthi",
    [MIR_CVTSI2F] = "cvtsi2f",
    [MIR_CVTF2SI] = "cvtf2si",
    [MIR_CVTF2F] = "cvtf2f",
    [MIR_UCOMIS] = "ucomis",
    [MIR_FSETCC] = "fsetcc",
    [MIR_FCMPBR] = "fcmpbr",
    [MIR_MOVD_TO_XMM] = "movd2xmm",
    [MIR_MOVD_TO_GP] = "movd2gp",
    [MIR_CVTPH2PS] = "cvtph2ps",
    [MIR_CVTPS2PH] = "cvtps2ph",
    [MIR_VADD] = "vadd",
    [MIR_VSUB] = "vsub",
    [MIR_VMUL] = "vmul",
    [MIR_VDIV] = "vdiv",
    [MIR_VCVTSI2F] = "vcvtsi2f",
    [MIR_VCVTF2SI] = "vcvtf2si",
    [MIR_VLOAD] = "vload",
    [MIR_VSTORE] = "vstore",
    [MIR_VBROADCAST] = "vbroadcast",
    [MIR_VIOTA] = "viota",
    [MIR_VHREDUCE] = "vhreduce",
    [MIR_SIMD_SLP_MAC] = "simd_slp_mac",
    [MIR_SIMD_FILL] = "simd_fill",
    [MIR_SIMD_AFFINE_MAP_F32] = "simd_affine_map_f32",
    [MIR_SIMD_AFFINE_MAP_F64] = "simd_affine_map_f64",
    [MIR_SIMD_SILU_F32] = "simd_silu_f32",
    [MIR_SIMD_VLOOP] = "simd_vloop",
    [MIR_IR_KERNEL] = "ir_kernel",
};

const char *mir_opcode_name(MirOpcode op) {
  const char *name = (unsigned)op < (unsigned)MIR_OPCODE_COUNT
                         ? MIR_OPCODE_NAMES[op]
                         : NULL;
  return name ? name : "?";
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
