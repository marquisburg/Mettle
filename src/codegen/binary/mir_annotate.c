#include "codegen/binary/mir_annotate.h"
#include "common.h"
#include "codegen/binary/mir.h"
#include "ir/ir.h"
#include "ir/ir_optimize.h"
#include "ir/ir_profile.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- microarchitectural model ------------------------------------------- *
 *
 * The annotator carries a small static cost model so a decision can be backed
 * by a number, not just a name. It is a steady-state port-pressure model of a
 * Skylake-class core: six issue resources, each able to retire one micro-op per
 * cycle. Per instruction we record its latency, its reciprocal throughput, and
 * how its micro-ops spread across the resources. Summed over a recovered loop
 * body this yields the throughput floor (cycles/iteration) and the bottleneck
 * port -- the same reasoning a developer does by hand reading -O2 output, made
 * explicit. The numbers are approximate and labelled as such; their value is in
 * relative hot-spotting and showing which resource a hot loop is bound on. */

enum {
  RES_P0 = 0, /* ALU, integer/vector mul, divide, fp */
  RES_P1,     /* ALU, integer mul, fp */
  RES_P5,     /* ALU, vector shuffle, lea */
  RES_P6,     /* ALU, branch */
  RES_LD,     /* load AGU + data (two units, modelled as 0.5c each) */
  RES_ST,     /* store AGU + data */
  RES_COUNT
};

static const char *res_name[RES_COUNT] = {"p0", "p1", "p5", "p6", "load", "store"};

#define M_P0 (1u << RES_P0)
#define M_P1 (1u << RES_P1)
#define M_P5 (1u << RES_P5)
#define M_P6 (1u << RES_P6)
#define M_ALU4 (M_P0 | M_P1 | M_P5 | M_P6)
#define M_P06 (M_P0 | M_P6)
#define M_P15 (M_P1 | M_P5)
#define M_P01 (M_P0 | M_P1)

/* Instruction-mix buckets (also the per-insn `kind` tag). Static strings; never
 * freed, compared by value where needed. */
static const char *const KINDS[] = {
    "mov",  "alu",   "mul",  "div",   "shift", "lea",
    "cmp",  "setcc", "cmov", "branch", "call",  "float",
    "vec",  "load",  "store", "kernel", "frame", "other"};
#define KIND_COUNT ((int)(sizeof(KINDS) / sizeof(KINDS[0])))

static int kind_index(const char *k) {
  if (k)
    for (int i = 0; i < KIND_COUNT; i++)
      if (strcmp(KINDS[i], k) == 0) return i;
  return KIND_COUNT - 1; /* "other" */
}

static int popcnt(unsigned x) {
  int n = 0;
  while (x) {
    n += (int)(x & 1u);
    x >>= 1;
  }
  return n;
}

/* ---- state -------------------------------------------------------------- */

typedef struct {
  int mir_index;        /* index into the MIR function, -1 for synthetic spans */
  size_t off;
  size_t len;
  char *bytes;          /* hex string "55 48 89 e5" */
  char *intel;
  char *att;
  char *mir;            /* MIR opcode mnemonic, e.g. "MOV" */
  size_t line;          /* source line, 0 if synthetic/unknown */
  char *tag;            /* short decision tag */
  char *note;           /* decision detail (may be NULL) */
  /* analysis */
  int lat;              /* latency in cycles (approx) */
  int rthru;            /* reciprocal throughput in centicycles (0.01c units) */
  int press[RES_COUNT]; /* FIXED port-pressure contribution (narrow uops) */
  int flex_alu;         /* centicycles of ALU uops eligible on p0/p1/p5/p6,
                           distributed per-loop by water-filling, not pinned */
  const char *kind;     /* mix bucket; one of KINDS (static) */
  const char *ports;    /* human port string for display (static) */
  unsigned char is_kernel;
  unsigned char is_branch; /* control transfer that can close a loop */
  unsigned char is_label;
  char *target;         /* branch target label name, or NULL */
  char *label;          /* this label's name, or NULL */
  int loop_depth;       /* nesting depth, set at end_function */
  unsigned char cost_estimated; /* 1 = bytes didn't decode; opcode estimate used */
  int block;            /* program-wide basic-block id (--profile-blocks), or -1.
                           Joins the measured .mprof execution counts to this
                           instruction's static cost in the VTune-style view. */
} AnnotInsn;

/* One physical-register live interval, snapshotted from the allocator. */
typedef struct {
  int phys;
  int rclass;       /* MirRegClass */
  int width;
  int vreg;
  int start;        /* MIR index of first def */
  int end;          /* MIR index of last use */
  int crosses_call;
  int loop_carried;
} RegInterval;

/* A recovered natural loop, expressed over AnnotInsn positions. */
typedef struct {
  int start_rec;            /* header position (inclusive) */
  int end_rec;              /* back-edge position (inclusive) */
  size_t head_line;
  size_t tail_line;
  int depth;                /* number of loops enclosing this one */
  int press[RES_COUNT];     /* summed body pressure, centicycles */
  int bottleneck;           /* resource index of the busiest port */
  int cycles_per_iter;      /* press[bottleneck], centicycles */
  int has_kernel;
  int has_estimated;        /* a body span fell back to an opcode estimate */
  char *header;             /* header label name, or NULL */
} Loop;

typedef struct {
  char *name;
  char *file;
  size_t line;
  size_t byte_size;
  char *backend;        /* "register-allocated" or "baseline (fallback)" */
  char *backend_reason; /* MIR-gate bail code for the fallback, or NULL */
  AnnotInsn *insns;
  size_t insn_count;
  size_t insn_cap;
  /* register-allocation snapshot (MIR backend only) */
  RegInterval *regs;
  size_t reg_count;
  size_t reg_cap;
  int spill_count;      /* vregs that the allocator spilled */
  int axis;             /* MIR instruction count = timeline x-axis extent */
  int snapped;          /* regmap captured for this function */
  /* loops */
  Loop *loops;
  size_t loop_count;
  size_t loop_cap;
  /* summary */
  int mix[KIND_COUNT];
  int total_rthru;      /* sum of reciprocal throughput, centicycles */
  long hot_cost;        /* loop-depth-weighted throughput */
  int vec_ops;          /* count of vectorized/kernel ops */
  int cost_estimated;   /* number of spans that fell back to an opcode estimate */
} AnnotFunc;

static MTLC_THREAD_LOCAL struct {
  int enabled;
  int cost_only; /* measure for --explain-json; emit no listing or sidecar */
  MirAnnotSyntax syntax;
  char *output_path;
  char *source_file;
  AnnotFunc *funcs;
  size_t func_count;
  size_t func_cap;
  /* The function currently being encoded. */
  AnnotFunc *cur;
  const IRFunction *cur_ir;
  int cur_block; /* id of the basic block currently being emitted, or -1 */
  /* LLM-facing focused queries (see header). */
  int q_lo, q_hi;
  char *q_fn;
  int q_hot;
} g;

static void analyze_function(AnnotFunc *f);

void mir_annotate_set_enabled(int enabled) { g.enabled = enabled ? 1 : 0; }
int mir_annotate_enabled(void) { return g.enabled; }
void mir_annotate_set_cost_only(int cost_only) {
  g.cost_only = cost_only ? 1 : 0;
}
void mir_annotate_set_syntax(MirAnnotSyntax syntax) { g.syntax = syntax; }

static char *dupstr(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

void mir_annotate_set_output_path(const char *output_path) {
  free(g.output_path);
  g.output_path = dupstr(output_path);
}

void mir_annotate_set_source_file(const char *source_file) {
  free(g.source_file);
  g.source_file = dupstr(source_file);
}

void mir_annotate_set_line_query(int lo, int hi, const char *fn) {
  g.q_lo = lo;
  g.q_hi = hi;
  free(g.q_fn);
  g.q_fn = fn ? dupstr(fn) : NULL;
}

void mir_annotate_set_hot_query(int n) { g.q_hot = n; }

/* ---- register naming ---------------------------------------------------- */

static const char *gp_name(int phys, int width) {
  static const char *n64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                "r12", "r13", "r14", "r15"};
  static const char *n32[16] = {"eax",  "ecx",  "edx",  "ebx",  "esp",  "ebp",
                                "esi",  "edi",  "r8d",  "r9d",  "r10d", "r11d",
                                "r12d", "r13d", "r14d", "r15d"};
  static const char *n16[16] = {"ax",   "cx",   "dx",   "bx",   "sp",   "bp",
                                "si",   "di",   "r8w",  "r9w",  "r10w", "r11w",
                                "r12w", "r13w", "r14w", "r15w"};
  static const char *n8[16] = {"al",   "cl",   "dl",   "bl",   "spl",  "bpl",
                               "sil",  "dil",  "r8b",  "r9b",  "r10b", "r11b",
                               "r12b", "r13b", "r14b", "r15b"};
  if (phys < 0 || phys > 15) return "?reg";
  switch (width) {
  case 1: return n8[phys];
  case 2: return n16[phys];
  case 4: return n32[phys];
  default: return n64[phys];
  }
}

static const char *vec_name(int phys, MirRegClass rclass) {
  static const char *xmm[16] = {"xmm0",  "xmm1",  "xmm2",  "xmm3", "xmm4",
                                "xmm5",  "xmm6",  "xmm7",  "xmm8", "xmm9",
                                "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",
                                "xmm15"};
  static const char *ymm[16] = {"ymm0",  "ymm1",  "ymm2",  "ymm3", "ymm4",
                                "ymm5",  "ymm6",  "ymm7",  "ymm8", "ymm9",
                                "ymm10", "ymm11", "ymm12", "ymm13", "ymm14",
                                "ymm15"};
  if (phys < 0 || phys > 15) return "?vec";
  return rclass == MIR_RC_VEC ? ymm[phys] : xmm[phys];
}

/* x86 condition tttn -> mnemonic suffix (jCC/setCC/cmovCC). */
static const char *cc_suffix(unsigned char cc) {
  static const char *s[16] = {"o",  "no", "b",  "ae", "e",  "ne", "be", "a",
                              "s",  "ns", "p",  "np", "l",  "ge", "le", "g"};
  return s[cc & 0xF];
}

/* ---- small string buffer ------------------------------------------------ */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} Sb;

static void sb_putc(Sb *b, char c) {
  if (b->len + 1 >= b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 64;
    char *g2 = (char *)realloc(b->data, nc);
    if (!g2) return;
    b->data = g2;
    b->cap = nc;
  }
  b->data[b->len++] = c;
  b->data[b->len] = '\0';
}

static void sb_puts(Sb *b, const char *s) {
  if (!s) return;
  while (*s) sb_putc(b, *s++);
}

static void sb_putf(Sb *b, const char *fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  sb_puts(b, tmp);
}

/* ---- operand rendering -------------------------------------------------- */

/* Resolve a vreg to a register name (allocated) or its spill memory. width is
 * the operation width; for XMM/VEC the vreg's own class governs. att controls
 * sigils and memory syntax. Returns a static-ish rendering into `out`. */
static void render_vreg(const MirFunction *fn, MirVregId v, int width, int att,
                        Sb *out) {
  if (!fn || v < 0 || (size_t)v >= fn->vreg_count) {
    sb_putf(out, "v%d", v);
    return;
  }
  const MirVreg *vr = &fn->vregs[v];
  if (vr->in_register && vr->assigned) {
    const char *nm = (vr->rclass == MIR_RC_GP)
                         ? gp_name(vr->phys, width ? width : vr->width)
                         : vec_name(vr->phys, vr->rclass);
    sb_putf(out, "%s%s", att ? "%" : "", nm);
  } else {
    /* Spilled or address-taken: lives at [rbp - spill_offset]. */
    int off = vr->spill_offset;
    if (att)
      sb_putf(out, "-%d(%%rbp)", off);
    else
      sb_putf(out, "[rbp-%d]", off);
  }
}

static void render_mem(const MirFunction *fn, const MirMem *m, int att, Sb *out) {
  const char *base = NULL;
  char basebuf[16];
  if (m->phys_base_valid) {
    base = gp_name(m->phys_base, 8);
  } else if (m->base != MIR_VREG_NONE && fn && (size_t)m->base < fn->vreg_count &&
             fn->vregs[m->base].in_register) {
    snprintf(basebuf, sizeof(basebuf), "%s", gp_name(fn->vregs[m->base].phys, 8));
    base = basebuf;
  }
  char idxbuf[16];
  const char *index = NULL;
  if (m->index != MIR_VREG_NONE && fn && (size_t)m->index < fn->vreg_count &&
      fn->vregs[m->index].in_register) {
    snprintf(idxbuf, sizeof(idxbuf), "%s", gp_name(fn->vregs[m->index].phys, 8));
    index = idxbuf;
  }
  if (att) {
    if (m->disp) sb_putf(out, "%d", m->disp);
    sb_putc(out, '(');
    if (base) sb_putf(out, "%%%s", base);
    if (index) sb_putf(out, ",%%%s,%d", index, m->scale ? m->scale : 1);
    sb_putc(out, ')');
  } else {
    sb_putc(out, '[');
    if (base) sb_puts(out, base);
    if (index) sb_putf(out, "%s%s*%d", base ? "+" : "", index, m->scale ? m->scale : 1);
    if (m->disp) sb_putf(out, "%s%d", m->disp < 0 ? "" : "+", m->disp);
    sb_putc(out, ']');
  }
}

static void render_operand(const MirFunction *fn, const MirOperand *op, int width,
                           int att, Sb *out) {
  switch (op->kind) {
  case MIR_OPK_NONE:
    break;
  case MIR_OPK_VREG:
    render_vreg(fn, op->vreg, width, att, out);
    break;
  case MIR_OPK_PHYS: {
    const char *nm = (op->rclass == MIR_RC_GP) ? gp_name(op->phys, width ? width : 8)
                                               : vec_name(op->phys, op->rclass);
    sb_putf(out, "%s%s", att ? "%" : "", nm);
    break;
  }
  case MIR_OPK_IMM:
    sb_putf(out, "%s%lld", att ? "$" : "", op->imm);
    break;
  case MIR_OPK_FIMM:
    sb_putf(out, "%s0x%llx", att ? "$" : "", (unsigned long long)op->imm);
    break;
  case MIR_OPK_MEM:
    render_mem(fn, &op->mem, att, out);
    break;
  case MIR_OPK_LABEL:
    sb_puts(out, op->sym ? op->sym : "?");
    break;
  case MIR_OPK_SYMBOL:
    sb_puts(out, op->sym ? op->sym : "?");
    break;
  case MIR_OPK_STACKHOME:
    if (att)
      sb_putf(out, "-%d(%%rbp)", op->disp);
    else
      sb_putf(out, "[rbp-%d]", op->disp);
    break;
  }
}

/* ---- instruction rendering ---------------------------------------------- */

static const char *const MIR_SCALAR_MNEMONICS[MIR_OPCODE_COUNT] = {
    [MIR_MOV] = "mov",
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
    [MIR_CMP] = "cmp",
    [MIR_TEST] = "test",
    [MIR_LEA] = "lea",
    [MIR_LEA_LOCAL] = "lea",
    [MIR_LEA_GLOBAL] = "lea",
    [MIR_MOVZX] = "movzx",
    [MIR_MOVSX] = "movsx",
    [MIR_JMP] = "jmp",
    [MIR_CALL] = "call",
    [MIR_CALL_INDIRECT] = "call",
    [MIR_REP_MOVSB] = "rep movsb",
    [MIR_REP_STOSB] = "rep stosb",
    [MIR_RET] = "ret",
    [MIR_CQO] = "cqo",
    [MIR_FXOR] = "xorpd",
    [MIR_LOAD_GLOBAL] = "mov",
    [MIR_STORE_GLOBAL] = "mov",
};

static const char *scalar_mnemonic(const MirInst *in) {
  switch (in->op) {
  case MIR_IDIV: return in->is_unsigned ? "div" : "idiv";
  case MIR_MULHI: return in->is_unsigned ? "mul" : "imul";
  case MIR_FADD: return in->width == 4 ? "addss" : "addsd";
  case MIR_FSUB: return in->width == 4 ? "subss" : "subsd";
  case MIR_FMUL: return in->width == 4 ? "mulss" : "mulsd";
  case MIR_FDIV: return in->width == 4 ? "divss" : "divsd";
  case MIR_UCOMIS: return in->width == 4 ? "ucomiss" : "ucomisd";
  default:
    return (unsigned)in->op < (unsigned)MIR_OPCODE_COUNT
               ? MIR_SCALAR_MNEMONICS[in->op]
               : NULL;
  }
}

static char att_suffix(int width) {
  switch (width) {
  case 1: return 'b';
  case 2: return 'w';
  case 4: return 'l';
  default: return 'q';
  }
}

/* Do two operands name the same physical register? Used to collapse the MIR
 * three-address form (dst = a op b) to the faithful x86 two-address form
 * (dst op= b) when dst and a coalesced to one register. */
static int same_reg(const MirFunction *fn, const MirOperand *x,
                    const MirOperand *y) {
  int xp = -2, yp = -2;
  if (x->kind == MIR_OPK_PHYS) xp = x->phys;
  else if (x->kind == MIR_OPK_VREG && fn && x->vreg >= 0 &&
           (size_t)x->vreg < fn->vreg_count && fn->vregs[x->vreg].in_register)
    xp = fn->vregs[x->vreg].phys;
  if (y->kind == MIR_OPK_PHYS) yp = y->phys;
  else if (y->kind == MIR_OPK_VREG && fn && y->vreg >= 0 &&
           (size_t)y->vreg < fn->vreg_count && fn->vregs[y->vreg].in_register)
    yp = fn->vregs[y->vreg].phys;
  return xp >= 0 && xp == yp;
}

/* Render one MIR instruction into Intel and AT&T text. */
static void render_inst(const MirFunction *fn, const MirInst *in, char **intel,
                        char **att) {
  Sb bi = {0}, ba = {0};
  const char *mn = scalar_mnemonic(in);

  /* Branches and labels read better than the generic dst,a,b form. */
  if (in->op == MIR_LABEL) {
    const char *l = in->dst.sym ? in->dst.sym : "?";
    sb_putf(&bi, "%s:", l);
    sb_putf(&ba, "%s:", l);
    goto done;
  }
  if (in->op == MIR_JCC) {
    sb_putf(&bi, "j%s %s", cc_suffix(in->cc), in->dst.sym ? in->dst.sym : "?");
    sb_putf(&ba, "j%s %s", cc_suffix(in->cc), in->dst.sym ? in->dst.sym : "?");
    goto done;
  }
  if (in->op == MIR_SETCC) {
    sb_putf(&bi, "set%s ", cc_suffix(in->cc));
    render_operand(fn, &in->dst, 1, 0, &bi);
    sb_putf(&ba, "set%s ", cc_suffix(in->cc));
    render_operand(fn, &in->dst, 1, 1, &ba);
    goto done;
  }
  if (in->op == MIR_CMOVCC) {
    sb_putf(&bi, "cmov%s ", cc_suffix(in->cc));
    render_operand(fn, &in->dst, in->width, 0, &bi);
    sb_puts(&bi, ", ");
    render_operand(fn, &in->a, in->width, 0, &bi);
    sb_putf(&ba, "cmov%s ", cc_suffix(in->cc));
    render_operand(fn, &in->a, in->width, 1, &ba);
    sb_puts(&ba, ", ");
    render_operand(fn, &in->dst, in->width, 1, &ba);
    goto done;
  }
  if (in->op == MIR_CMPBR || in->op == MIR_FCMPBR) {
    const char *c = in->op == MIR_FCMPBR
                        ? (in->width == 4 ? "ucomiss" : "ucomisd")
                        : "cmp";
    sb_putf(&bi, "%s ", c);
    render_operand(fn, &in->a, in->width, 0, &bi);
    sb_puts(&bi, ", ");
    render_operand(fn, &in->b, in->width, 0, &bi);
    sb_putf(&bi, " ; j%s %s", cc_suffix(in->cc), in->dst.sym ? in->dst.sym : "?");
    sb_putf(&ba, "%s ", c);
    render_operand(fn, &in->b, in->width, 1, &ba);
    sb_puts(&ba, ", ");
    render_operand(fn, &in->a, in->width, 1, &ba);
    sb_putf(&ba, " ; j%s %s", cc_suffix(in->cc), in->dst.sym ? in->dst.sym : "?");
    goto done;
  }

  if (!mn) {
    /* SIMD kernel or other multi-instruction op: show it as a pseudo-op. The
     * decision note carries the semantic meaning, and the raw bytes are kept. */
    const char *nm = mir_opcode_name(in->op);
    sb_putf(&bi, "<%s>", nm);
    sb_putf(&ba, "<%s>", nm);
    goto done;
  }

  /* Generic dst, a, b. x86 is two-address; when dst==a we collapse to the
   * faithful two-operand form, otherwise we keep three operands (dst = a op b)
   * which mirrors the mov+op the encoder actually emits (see raw bytes). */
  /* Collapse the two-address form: when dst and a are the same register and
   * there is a b source, x86 really computes `dst op= b`. */
  int collapse = in->dst.kind != MIR_OPK_NONE && in->a.kind != MIR_OPK_NONE &&
                 in->b.kind != MIR_OPK_NONE && same_reg(fn, &in->dst, &in->a);
  /* Intel */
  sb_puts(&bi, mn);
  sb_putc(&bi, ' ');
  if (collapse) {
    render_operand(fn, &in->dst, in->width, 0, &bi);
    sb_puts(&bi, ", ");
    render_operand(fn, &in->b, in->width, 0, &bi);
  } else {
    if (in->dst.kind != MIR_OPK_NONE) {
      render_operand(fn, &in->dst, in->width, 0, &bi);
      if (in->a.kind != MIR_OPK_NONE) sb_puts(&bi, ", ");
    }
    if (in->a.kind != MIR_OPK_NONE) render_operand(fn, &in->a, in->width, 0, &bi);
    if (in->b.kind != MIR_OPK_NONE) {
      sb_puts(&bi, ", ");
      render_operand(fn, &in->b, in->width, 0, &bi);
    }
  }
  /* AT&T: append size suffix on plain integer ops; reverse operand order. */
  int wants_suffix = 0;
  switch (in->op) {
  case MIR_MOV:
  case MIR_ADD:
  case MIR_SUB:
  case MIR_AND:
  case MIR_OR:
  case MIR_XOR:
  case MIR_IMUL:
  case MIR_NEG:
  case MIR_NOT:
  case MIR_SHL:
  case MIR_SHR:
  case MIR_SAR:
  case MIR_CMP:
  case MIR_TEST:
  case MIR_IDIV:
  case MIR_MULHI:
    wants_suffix = 1;
    break;
  default:
    break;
  }
  sb_puts(&ba, mn);
  if (wants_suffix) sb_putc(&ba, att_suffix(in->width));
  sb_putc(&ba, ' ');
  if (collapse) {
    render_operand(fn, &in->b, in->width, 1, &ba);
    sb_puts(&ba, ", ");
    render_operand(fn, &in->dst, in->width, 1, &ba);
  } else {
    if (in->b.kind != MIR_OPK_NONE) {
      render_operand(fn, &in->b, in->width, 1, &ba);
      sb_puts(&ba, ", ");
    }
    if (in->a.kind != MIR_OPK_NONE) {
      render_operand(fn, &in->a, in->width, 1, &ba);
      if (in->dst.kind != MIR_OPK_NONE) sb_puts(&ba, ", ");
    }
    if (in->dst.kind != MIR_OPK_NONE)
      render_operand(fn, &in->dst, in->width, 1, &ba);
  }

done:
  *intel = bi.data ? bi.data : dupstr("");
  *att = ba.data ? ba.data : dupstr("");
}

/* ---- cost model --------------------------------------------------------- */

/* Does this operand resolve to memory (a real [mem]/home, or a spilled vreg)? */
static int op_is_mem(const MirFunction *fn, const MirOperand *op) {
  if (op->kind == MIR_OPK_MEM || op->kind == MIR_OPK_STACKHOME) return 1;
  if (op->kind == MIR_OPK_VREG && fn && op->vreg >= 0 &&
      (size_t)op->vreg < fn->vreg_count) {
    const MirVreg *vr = &fn->vregs[op->vreg];
    if (vr->assigned && !vr->in_register) return 1; /* spilled / address-taken */
  }
  return 0;
}

static void press_even(int *press, unsigned mask, int centi) {
  int n = popcnt(mask);
  if (!n || centi <= 0) return;
  int each = centi / n;
  for (int r = 0; r < RES_COUNT; r++)
    if (mask & (1u << r)) press[r] += each;
}

typedef struct {
  const char *kind;
  const char *ports;
  unsigned mask;
  int centi;
  int lat;
  signed char store;
  signed char load;
} MirOpCost;

static const MirOpCost MIR_OP_COSTS[MIR_OPCODE_COUNT] = {
    [MIR_MOV] = {"mov", "p0156", M_ALU4, 25, 1, 0, 0},
    [MIR_LOAD_GLOBAL] = {"mov", "p0156", M_ALU4, 25, 1, 0, 0},
    [MIR_STORE_GLOBAL] = {"mov", "p0156", M_ALU4, 25, 1, 0, 0},
    [MIR_ADD] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_SUB] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_AND] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_OR] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_XOR] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_NEG] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_NOT] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_CQO] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_XOR_RDX] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_CMP] = {"cmp", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_TEST] = {"cmp", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_SHL] = {"shift", "p06", M_P06, 50, 1, 0, 0},
    [MIR_SHR] = {"shift", "p06", M_P06, 50, 1, 0, 0},
    [MIR_SAR] = {"shift", "p06", M_P06, 50, 1, 0, 0},
    [MIR_IMUL] = {"mul", "p1", M_P1, 100, 3, 0, 0},
    [MIR_MULHI] = {"mul", "p1", M_P1, 100, 4, 0, 0},
    [MIR_IDIV] = {"div", "p0", M_P0, 800, 20, 0, 0},
    [MIR_DIV] = {"div", "p0", M_P0, 800, 20, 0, 0},
    [MIR_LEA] = {"lea", "p15", M_P15, 50, 1, 0, -1},
    [MIR_LEA_LOCAL] = {"lea", "p15", M_P15, 50, 1, 0, -1},
    [MIR_LEA_GLOBAL] = {"lea", "p15", M_P15, 50, 1, 0, -1},
    [MIR_LEA_FUNC] = {"lea", "p15", M_P15, 50, 1, 0, -1},
    [MIR_LEA_CSTR] = {"lea", "p15", M_P15, 50, 1, 0, -1},
    [MIR_LEA_STRLIT] = {"lea", "p15", M_P15, 50, 1, 0, -1},
    [MIR_LEA_OUTARG] = {"lea", "p15", M_P15, 50, 1, 0, -1},
    [MIR_MOVZX] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_MOVSX] = {"alu", NULL, M_ALU4, 25, 1, 0, 0},
    [MIR_SETCC] = {"setcc", "p06", M_P06, 50, 1, 0, 0},
    [MIR_CMOVCC] = {"cmov", "p06", M_P06, 50, 1, 0, 0},
    [MIR_JMP] = {"branch", "p6", M_P6, 50, 1, 0, 0},
    [MIR_JCC] = {"branch", "p6", M_P6, 50, 1, 0, 0},
    [MIR_CMPBR] = {"branch", "p06", M_P06, 75, 1, 0, 0},
    [MIR_FCMPBR] = {"branch", "p06", M_P06, 75, 3, 0, 0},
    [MIR_CALL] = {"call", "p6", M_P6, 100, 3, 0, 0},
    [MIR_CALL_INDIRECT] = {"call", "p6", M_P6, 100, 3, 0, 0},
    [MIR_RET] = {"branch", "p6", M_P6, 100, 1, 0, 0},
    [MIR_TRAP] = {"other", NULL, 0, 0, 1, 0, 0},
    [MIR_INLINE_ASM] = {"other", NULL, 0, 0, 1, 0, 0},
    [MIR_STORE_OUTARG] = {"store", "store", 0, 0, 1, 1, 0},
    [MIR_FADD] = {"float", "p01", M_P01, 50, 4, 0, 0},
    [MIR_FSUB] = {"float", "p01", M_P01, 50, 4, 0, 0},
    [MIR_FXOR] = {"float", "p01", M_P01, 50, 4, 0, 0},
    [MIR_FMUL] = {"float", "p01", M_P01, 50, 4, 0, 0},
    [MIR_FDIV] = {"float", "p0", M_P0, 400, 14, 0, 0},
    [MIR_UCOMIS] = {"float", "p0", M_P0, 100, 2, 0, 0},
    [MIR_CVTSI2F] = {"float", "p01", M_P01, 100, 5, 0, 0},
    [MIR_CVTF2SI] = {"float", "p01", M_P01, 100, 5, 0, 0},
    [MIR_CVTF2F] = {"float", "p01", M_P01, 100, 5, 0, 0},
    [MIR_CVTPH2PS] = {"float", "p01", M_P01, 100, 5, 0, 0},
    [MIR_CVTPS2PH] = {"float", "p01", M_P01, 100, 5, 0, 0},
    [MIR_FSETCC] = {"float", "p0", M_P0, 100, 4, 0, 0},
    [MIR_MOVD_TO_XMM] = {"float", "p5", M_P5, 100, 2, 0, 0},
    [MIR_MOVD_TO_GP] = {"float", "p5", M_P5, 100, 2, 0, 0},
    [MIR_VADD] = {"vec", "p01", M_P01, 50, 4, 0, 0},
    [MIR_VSUB] = {"vec", "p01", M_P01, 50, 4, 0, 0},
    [MIR_VMUL] = {"vec", "p01", M_P01, 50, 4, 0, 0},
    [MIR_VDIV] = {"vec", "p0", M_P0, 1600, 21, 0, 0},
    [MIR_VLOAD] = {"load", "load", 0, 0, 7, 0, 1},
    [MIR_VSTORE] = {"store", "store", 0, 0, 1, 1, -1},
    [MIR_VBROADCAST] = {"vec", "p5", M_P5, 100, 3, 0, 0},
    [MIR_VIOTA] = {"vec", "p015", M_P0 | M_P1 | M_P5, 100, 3, 0, 0},
    [MIR_VHREDUCE] = {"vec", "p015", M_P0 | M_P1 | M_P5, 300, 8, 0, 0},
    [MIR_VCVTSI2F] = {"vec", "p01", M_P01, 100, 5, 0, 0},
    [MIR_VCVTF2SI] = {"vec", "p01", M_P01, 100, 5, 0, 0},
    [MIR_NOP] = {"other", NULL, 0, 0, 0, 0, 0},
    [MIR_LABEL] = {"other", NULL, 0, 0, 0, 0, 0},
};

static void cost_of_move(const MirFunction *fn, const MirInst *in,
                         MirOpCost *cost, int load) {
  int to_memory = op_is_mem(fn, &in->dst) || in->op == MIR_STORE_GLOBAL;
  int from_memory = load || in->op == MIR_LOAD_GLOBAL;

  if (!to_memory && !from_memory) {
    return;
  }
  cost->mask = 0;
  cost->centi = 0;
  cost->kind = to_memory ? "store" : "load";
  cost->ports = to_memory ? (from_memory ? "load+store" : "store") : "load";
  cost->store = (signed char)(to_memory ? 1 : 0);
  cost->load = (signed char)(to_memory && !from_memory ? -1 : 0);
  cost->lat = from_memory ? 5 : 1;
}

static void cost_of_kernel(MirOpCost *cost) {
  cost->kind = "kernel";
  cost->ports = "kernel";
  cost->mask = 0;
  cost->centi = 0;
  cost->lat = 0;
  cost->load = -1;
}

static MirOpCost cost_of_instruction(const MirFunction *fn, const MirInst *in,
                                     int load, int *is_kernel) {
  MirOpCost cost = {"other", NULL, M_ALU4, 25, 1, 0, 0};
  int wide = in->width > 4;

  if ((unsigned)in->op < (unsigned)MIR_OPCODE_COUNT &&
      MIR_OP_COSTS[in->op].kind) {
    cost = MIR_OP_COSTS[in->op];
  } else if (mir_op_is_inline_kernel(in->op)) {
    *is_kernel = 1;
    cost_of_kernel(&cost);
    return cost;
  } else {
    return cost;
  }

  switch (in->op) {
  case MIR_MOV:
  case MIR_LOAD_GLOBAL:
  case MIR_STORE_GLOBAL:
    cost_of_move(fn, in, &cost, load);
    break;
  case MIR_LEA:
  case MIR_LEA_LOCAL:
  case MIR_LEA_GLOBAL:
  case MIR_LEA_FUNC:
  case MIR_LEA_CSTR:
  case MIR_LEA_STRLIT:
  case MIR_LEA_OUTARG:
    if (in->a.kind == MIR_OPK_MEM && in->a.mem.index != MIR_VREG_NONE) {
      cost.ports = "p1";
      cost.mask = M_P1;
      cost.centi = 100;
      cost.lat = 3;
    }
    break;
  case MIR_MOVZX:
  case MIR_MOVSX:
    if (load) {
      cost.kind = "load";
      cost.mask = 0;
      cost.centi = 0;
      cost.lat = 5;
    }
    break;
  case MIR_IDIV:
  case MIR_DIV:
    cost.centi = wide ? 2500 : 800;
    cost.lat = wide ? 40 : 20;
    break;
  case MIR_FDIV:
    cost.centi = in->width == 4 ? 300 : 400;
    cost.lat = in->width == 4 ? 11 : 14;
    break;
  default:
    break;
  }
  return cost;
}

static void cost_model(const MirFunction *fn, const MirInst *in, int *lat,
                       int press[RES_COUNT], int *flex, int *rthru,
                       const char **kind, const char **ports, int *is_kernel) {
  int load = op_is_mem(fn, &in->a) || op_is_mem(fn, &in->b);
  MirOpCost cost;

  *flex = 0;
  *is_kernel = 0;
  cost = cost_of_instruction(fn, in, load, is_kernel);
  if (cost.load) {
    load = cost.load > 0;
  }
  *kind = cost.kind;
  *ports = cost.ports ? cost.ports : "p0156";
  *lat = cost.lat;

  if (cost.mask == M_ALU4) {
    *flex = cost.centi;
  } else {
    press_even(press, cost.mask, cost.centi);
  }
  if (load) {
    press[RES_LD] += 50;
    if (*lat < 5) {
      *lat = 5;
    }
  }
  if (cost.store) {
    press[RES_ST] += 100;
  }

  *rthru = *flex;
  for (int r = 0; r < RES_COUNT; r++) {
    *rthru += press[r];
  }
}

/* ---- byte-accurate micro-op model --------------------------------------- *
 *
 * The opcode models above estimate cost from the MIR/IR opcode. That is coarse
 * for the baseline backend, where one IR op becomes many machine instructions.
 * This decoder instead reads the ACTUAL emitted bytes of a span and accounts
 * the real micro-ops -- the genuinely accurate basis. The backend emits a known,
 * limited instruction set, so a targeted x86-64 length decoder covers it; it is
 * self-validating: classify_span_bytes only succeeds if it walks the span and
 * consumes EXACTLY its length on instruction boundaries with every opcode
 * recognized. On any mismatch the caller keeps the opcode estimate, so a decode
 * error can never corrupt the numbers -- worst case it is no worse than before.
 *
 * Modeled effects a static analyzer can know: macro-fusion (cmp/test/add/sub/and
 * + jcc -> one branch uop), reg-reg mov elimination (0 execution uops), memory
 * loads/stores, and the per-uop port assignment. Not modeled (data-dependent):
 * branch prediction and cache behaviour. */

enum {
  PC_NONE = 0, PC_ALU, PC_SHIFT, PC_MUL, PC_DIV, PC_LEA, PC_LEA3, PC_SETCC,
  PC_CMOV, PC_BRANCH, PC_RET, PC_CALL, PC_FPADDMUL, PC_FPDIV, PC_FPMISC
};

typedef struct {
  int ilen;             /* total instruction length in bytes */
  int pc;               /* port class */
  int mem_load;
  int mem_store;
  int is_mov_rr;        /* reg-reg integer mov: eliminated at rename */
  int is_cmp_fusible;   /* cmp/test/add/sub/and -- fuses with a following jcc */
  int is_cond_branch;   /* jcc */
  int wide;             /* REX.W (for divide sizing) */
} Insn;

/* ModRM(+SIB+displacement) length. Sets *is_mem and, for SIB, *has_index. */
static int modrm_len(const unsigned char *p, size_t avail, int *is_mem,
                     int *reg, int *has_index) {
  if (avail < 1) return -1;
  unsigned char m = p[0];
  int mod = m >> 6, rm = m & 7;
  *reg = (m >> 3) & 7;
  *is_mem = (mod != 3);
  *has_index = 0;
  int len = 1;
  int sib_base5 = 0;
  if (mod != 3 && rm == 4) { /* SIB */
    if (avail < 2) return -1;
    unsigned char sib = p[1];
    len++;
    if (((sib >> 3) & 7) != 4) *has_index = 1; /* index != none */
    if ((sib & 7) == 5) sib_base5 = 1;
  }
  if (mod == 1) len += 1;
  else if (mod == 2) len += 4;
  else if (mod == 0) {
    if (rm == 5) len += 4;            /* RIP-relative disp32 */
    else if (sib_base5) len += 4;     /* SIB no-base disp32 */
  }
  return len;
}

/* Decode one instruction. Returns 1 and fills `o` on success; 0 to bail (unknown
 * opcode, VEX/kernel, or overrun). */
static int decode_one(const unsigned char *p, size_t len, Insn *o) {
  memset(o, 0, sizeof *o);
  o->pc = PC_ALU;
  size_t i = 0;
  int opsize16 = 0, mand = 0; /* mandatory SSE prefix: 0x66/0xF2/0xF3 */
  for (;;) {
    if (i >= len) return 0;
    unsigned char b = p[i];
    if (b == 0x66) { opsize16 = 1; mand = 0x66; i++; continue; }
    if (b == 0xF2 || b == 0xF3) { mand = b; i++; continue; }
    if (b == 0x67 || b == 0xF0 || b == 0x2E || b == 0x36 || b == 0x3E ||
        b == 0x26 || b == 0x64 || b == 0x65) { i++; continue; }
    break;
  }
  if (i >= len) return 0;
  if ((p[i] & 0xF0) == 0x40) { o->wide = (p[i] & 8) != 0; i++; if (i >= len) return 0; }
  unsigned char op = p[i++];

  int is_mem = 0, reg = 0, has_index = 0, imm = 0;
  int two = 0;
  if (op == 0xC4 || op == 0xC5) return 0; /* VEX: kernel territory, bail */

  if (op == 0x0F) {
    two = 1;
    if (i >= len) return 0;
    unsigned char o2 = p[i++];
    if (o2 == 0x38 || o2 == 0x3A) return 0; /* 3-byte map: bail */
    if (o2 >= 0x80 && o2 <= 0x8F) { /* jcc rel32 */
      o->pc = PC_BRANCH; o->is_cond_branch = 1;
      imm = opsize16 ? 2 : 4;
      o->ilen = (int)i + imm;
      return o->ilen <= (int)len;
    }
    /* the rest have ModRM, no immediate */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml;
    o->ilen = (int)i;
    if (o->ilen > (int)len) return 0;
    if (o2 >= 0x90 && o2 <= 0x9F) { o->pc = PC_SETCC; if (is_mem) o->mem_store = 1; return 1; }
    if (o2 >= 0x40 && o2 <= 0x4F) { o->pc = PC_CMOV; if (is_mem) o->mem_load = 1; return 1; }
    if (o2 == 0xAF) { o->pc = PC_MUL; if (is_mem) o->mem_load = 1; return 1; }
    if (o2 == 0xB6 || o2 == 0xB7 || o2 == 0xBE || o2 == 0xBF) {
      o->pc = PC_ALU; if (is_mem) o->mem_load = 1; return 1; }
    if (o2 == 0x1F) { o->pc = PC_NONE; return 1; } /* multi-byte nop */
    /* SSE/scalar-float */
    switch (o2) {
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5F: /* add/mul/sub/min/max */
      o->pc = PC_FPADDMUL; if (is_mem) o->mem_load = 1; return 1;
    case 0x5E: o->pc = PC_FPDIV; if (is_mem) o->mem_load = 1; return 1; /* div */
    case 0x51: o->pc = PC_FPDIV; if (is_mem) o->mem_load = 1; return 1; /* sqrt */
    case 0x54: case 0x55: case 0x56: case 0x57: /* and/andn/or/xor ps */
      o->pc = PC_FPMISC; if (is_mem) o->mem_load = 1; return 1;
    case 0x2E: case 0x2F: /* ucomis/comis */
      o->pc = PC_FPMISC; if (is_mem) o->mem_load = 1; return 1;
    case 0x2A: case 0x2C: case 0x2D: case 0x5A: case 0x5B: /* cvt* */
      o->pc = PC_FPMISC; if (is_mem) o->mem_load = 1; return 1;
    case 0x10: case 0x28: /* movups/movaps load */
      o->pc = PC_FPMISC; if (is_mem) o->mem_load = 1; return 1;
    case 0x11: case 0x29: /* movups/movaps store */
      o->pc = PC_FPMISC; if (is_mem) o->mem_store = 1; return 1;
    case 0x6E: /* movd/q to xmm */
      o->pc = PC_FPMISC; if (is_mem) o->mem_load = 1; return 1;
    case 0x7E: /* movq (F3=load) / movd (66=store) */
      o->pc = PC_FPMISC;
      if (is_mem) { if (mand == 0xF3) o->mem_load = 1; else o->mem_store = 1; }
      return 1;
    case 0xD6: /* movq store */
      o->pc = PC_FPMISC; if (is_mem) o->mem_store = 1; return 1;
    case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
      o->pc = PC_FPMISC; if (is_mem) { if (o2 & 1) o->mem_store = 1; else o->mem_load = 1; } return 1;
    default:
      return 0; /* unknown 2-byte op: bail */
    }
  }

  /* one-byte opcode map */
  /* integer ALU group: 0x00..0x3D, in 8-opcode blocks (ADD,OR,ADC,SBB,AND,SUB,XOR,CMP). */
  if (op <= 0x3D && (op & 7) <= 5 && op != 0x0F) {
    int blk = op >> 3;          /* 0..7 -> which arith op */
    int form = op & 7;          /* 0,1=Eb/Ev,Gb/Gv ; 2,3=Gb/Gv,Eb/Ev ; 4,5=AL/eAX,imm */
    int is_cmp = (blk == 7);    /* CMP */
    int is_fusible = (blk == 0 || blk == 4 || blk == 5 || blk == 7); /* ADD/AND/SUB/CMP */
    if (form <= 3) {
      int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
      if (ml < 0) return 0;
      i += ml;
      o->ilen = (int)i;
      if (o->ilen > (int)len) return 0;
      o->pc = PC_ALU;
      o->is_cmp_fusible = is_fusible;
      if (is_mem) {
        if (form <= 1) { o->mem_load = 1; if (!is_cmp) o->mem_store = 1; } /* rm dest: RMW (cmp is read-only) */
        else o->mem_load = 1;                                              /* rm source */
      }
      /* reg-reg mov is handled at 0x88-0x8B; here arith reg-reg is a normal alu */
      return 1;
    }
    /* AL/eAX, immediate */
    imm = (form == 4) ? 1 : (opsize16 ? 2 : 4);
    o->ilen = (int)i + imm;
    o->pc = PC_ALU; o->is_cmp_fusible = is_fusible;
    return o->ilen <= (int)len;
  }

  switch (op) {
  case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56:
  case 0x57: /* push r */
    o->pc = PC_NONE; o->mem_store = 1; o->ilen = (int)i; return 1;
  case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E:
  case 0x5F: /* pop r */
    o->pc = PC_NONE; o->mem_load = 1; o->ilen = (int)i; return 1;
  case 0x63: { /* movsxd */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->pc = PC_ALU; if (is_mem) o->mem_load = 1;
    o->ilen = (int)i; return o->ilen <= (int)len; }
  case 0x68: imm = opsize16 ? 2 : 4; o->pc = PC_NONE; o->mem_store = 1;
    o->ilen = (int)i + imm; return o->ilen <= (int)len; /* push imm */
  case 0x6A: o->pc = PC_NONE; o->mem_store = 1; o->ilen = (int)i + 1;
    return o->ilen <= (int)len;
  case 0x69: case 0x6B: { /* imul r, r/m, imm */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; imm = (op == 0x6B) ? 1 : (opsize16 ? 2 : 4);
    o->pc = PC_MUL; if (is_mem) o->mem_load = 1;
    o->ilen = (int)i + imm; return o->ilen <= (int)len; }
  case 0x84: case 0x85: { /* test */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->pc = PC_ALU; o->is_cmp_fusible = 1; if (is_mem) o->mem_load = 1;
    o->ilen = (int)i; return o->ilen <= (int)len; }
  case 0x88: case 0x89: case 0x8A: case 0x8B: { /* mov */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->ilen = (int)i; if (o->ilen > (int)len) return 0;
    if (is_mem) { o->pc = PC_NONE; if (op == 0x88 || op == 0x89) o->mem_store = 1; else o->mem_load = 1; }
    else { o->pc = PC_NONE; o->is_mov_rr = 1; } /* reg-reg mov: eliminated */
    return 1; }
  case 0x8D: { /* lea */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->pc = has_index ? PC_LEA3 : PC_LEA; /* no memory access */
    o->ilen = (int)i; return o->ilen <= (int)len; }
  case 0x80: case 0x81: case 0x83: { /* grp1 Ev, imm */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; imm = (op == 0x81) ? (opsize16 ? 2 : 4) : 1;
    o->pc = PC_ALU; o->is_cmp_fusible = (reg == 0 || reg == 4 || reg == 5 || reg == 7);
    if (is_mem) { o->mem_load = 1; if (reg != 7) o->mem_store = 1; } /* /7=CMP read-only */
    o->ilen = (int)i + imm; return o->ilen <= (int)len; }
  case 0x90: o->pc = PC_NONE; o->ilen = (int)i; return 1; /* nop */
  case 0x98: case 0x99: o->pc = PC_ALU; o->ilen = (int)i; return 1; /* cwde/cqo */
  case 0xA8: o->pc = PC_ALU; o->is_cmp_fusible = 1; o->ilen = (int)i + 1; return o->ilen <= (int)len;
  case 0xA9: imm = opsize16 ? 2 : 4; o->pc = PC_ALU; o->is_cmp_fusible = 1;
    o->ilen = (int)i + imm; return o->ilen <= (int)len;
  case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6:
  case 0xB7: o->pc = PC_NONE; o->ilen = (int)i + 1; return o->ilen <= (int)len; /* mov r8,imm8 */
  case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE:
  case 0xBF: imm = o->wide ? 8 : (opsize16 ? 2 : 4); o->pc = PC_NONE; /* mov r,imm (movabs) */
    o->ilen = (int)i + imm; return o->ilen <= (int)len;
  case 0xC0: case 0xC1: { /* grp2 shift Ev, imm8 */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->pc = PC_SHIFT; if (is_mem) { o->mem_load = 1; o->mem_store = 1; }
    o->ilen = (int)i + 1; return o->ilen <= (int)len; }
  case 0xD0: case 0xD1: case 0xD2: case 0xD3: { /* grp2 shift by 1/CL */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->pc = PC_SHIFT; if (is_mem) { o->mem_load = 1; o->mem_store = 1; }
    o->ilen = (int)i; return o->ilen <= (int)len; }
  case 0xC2: o->pc = PC_RET; o->ilen = (int)i + 2; return o->ilen <= (int)len; /* ret imm16 */
  case 0xC3: o->pc = PC_RET; o->ilen = (int)i; return 1; /* ret */
  case 0xC6: { /* mov Eb, Ib */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->pc = PC_NONE; if (is_mem) o->mem_store = 1;
    o->ilen = (int)i + 1; return o->ilen <= (int)len; }
  case 0xC7: { /* mov Ev, Iz */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; imm = opsize16 ? 2 : 4; o->pc = PC_NONE; if (is_mem) o->mem_store = 1;
    o->ilen = (int)i + imm; return o->ilen <= (int)len; }
  case 0xCC: o->pc = PC_NONE; o->ilen = (int)i; return 1; /* int3 (trap pad) */
  case 0xE8: o->pc = PC_CALL; o->ilen = (int)i + 4; return o->ilen <= (int)len;
  case 0xE9: o->pc = PC_BRANCH; o->ilen = (int)i + 4; return o->ilen <= (int)len; /* jmp rel32 */
  case 0xEB: o->pc = PC_BRANCH; o->ilen = (int)i + 1; return o->ilen <= (int)len; /* jmp rel8 */
  case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76:
  case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D:
  case 0x7E: case 0x7F: /* jcc rel8 */
    o->pc = PC_BRANCH; o->is_cond_branch = 1; o->ilen = (int)i + 1;
    return o->ilen <= (int)len;
  case 0xF6: case 0xF7: { /* grp3 Ev */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml;
    if (reg == 0 || reg == 1) { /* TEST Ev, imm */
      imm = (op == 0xF7) ? (opsize16 ? 2 : 4) : 1;
      o->pc = PC_ALU; if (is_mem) o->mem_load = 1;
      o->ilen = (int)i + imm; return o->ilen <= (int)len;
    }
    if (reg == 2 || reg == 3) o->pc = PC_ALU;         /* NOT/NEG */
    else if (reg == 4 || reg == 5) o->pc = PC_MUL;    /* MUL/IMUL */
    else o->pc = PC_DIV;                              /* DIV/IDIV */
    if (is_mem) { o->mem_load = 1; if (reg == 2 || reg == 3) o->mem_store = 1; }
    o->ilen = (int)i; return o->ilen <= (int)len; }
  case 0xFE: { /* grp4 inc/dec Eb */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->pc = PC_ALU; if (is_mem) { o->mem_load = 1; o->mem_store = 1; }
    o->ilen = (int)i; return o->ilen <= (int)len; }
  case 0xFF: { /* grp5 */
    int ml = modrm_len(p + i, len - i, &is_mem, &reg, &has_index);
    if (ml < 0) return 0;
    i += ml; o->ilen = (int)i; if (o->ilen > (int)len) return 0;
    if (reg == 0 || reg == 1) { o->pc = PC_ALU; if (is_mem) { o->mem_load = 1; o->mem_store = 1; } } /* inc/dec */
    else if (reg == 2 || reg == 3) { o->pc = PC_CALL; if (is_mem) o->mem_load = 1; } /* call */
    else if (reg == 4 || reg == 5) { o->pc = PC_BRANCH; if (is_mem) o->mem_load = 1; } /* jmp */
    else if (reg == 6) { o->pc = PC_NONE; o->mem_store = 1; if (is_mem) o->mem_load = 1; } /* push */
    else return 0;
    return 1; }
  default:
    (void)two;
    return 0; /* unknown: bail to the opcode estimate */
  }
}

/* Account one decoded instruction into the resource model. */
static void account_insn(const Insn *in, int press[RES_COUNT], int *flex,
                         int *lat) {
  int l = 1;
  switch (in->pc) {
  case PC_NONE: break;
  case PC_ALU: *flex += 25; break;
  case PC_SHIFT: press_even(press, M_P06, 50); break;
  case PC_MUL: press[RES_P1] += 100; l = 3; break;
  case PC_DIV: press[RES_P0] += in->wide ? 2500 : 800; l = in->wide ? 35 : 20; break;
  case PC_LEA: press_even(press, M_P15, 50); break;
  case PC_LEA3: press[RES_P1] += 100; l = 3; break;
  case PC_SETCC: press_even(press, M_P06, 50); break;
  case PC_CMOV: press_even(press, M_P06, 50); break;
  case PC_BRANCH: press[RES_P6] += 50; break;
  case PC_RET: press[RES_P6] += 100; press[RES_LD] += 50; break;
  case PC_CALL: press[RES_P6] += 100; l = 3; break;
  case PC_FPADDMUL: press_even(press, M_P01, 50); l = 4; break;
  case PC_FPDIV: press[RES_P0] += 400; l = 14; break;
  case PC_FPMISC: press_even(press, M_P01, 100); l = 4; break;
  }
  if (in->mem_load) { press[RES_LD] += 50; if (l < 5) l = 5; }
  if (in->mem_store) press[RES_ST] += 100;
  if (l > *lat) *lat = l;
}

/* Walk a span's bytes, accounting real micro-ops with macro-fusion and mov
 * elimination. Returns 1 (and fills press/flex/lat + a dominant `kind`) only if
 * the whole span decodes cleanly on instruction boundaries; 0 to bail. */
#define UOP_CAP 512
static int classify_span_bytes(const unsigned char *p, size_t len,
                               int press[RES_COUNT], int *flex, int *lat,
                               const char **kindname) {
  Insn insns[UOP_CAP];
  int n = 0;
  size_t i = 0;
  while (i < len) {
    if (n >= UOP_CAP) return 0;
    Insn *o = &insns[n];
    if (!decode_one(p + i, len - i, o) || o->ilen <= 0) return 0;
    if ((size_t)o->ilen > len - i) return 0;
    i += (size_t)o->ilen;
    n++;
  }
  if (i != len) return 0; /* must land exactly on the span boundary */

  for (int r = 0; r < RES_COUNT; r++) press[r] = 0;
  *flex = 0;
  *lat = 0;
  int kc[16] = {0}; /* per port-class instruction tally for the dominant kind */
  for (int k = 0; k < n; k++) {
    Insn *o = &insns[k];
    /* Macro-fusion: a fusible cmp/test/add/sub/and immediately followed by a
     * conditional branch issues as ONE branch uop -- drop the ALU uop. */
    if (o->is_cmp_fusible && k + 1 < n && insns[k + 1].is_cond_branch) {
      /* account only the branch (next iteration handles it); the compare's mem
       * load, if any, still happens. */
      if (o->mem_load) press[RES_LD] += 50;
      kc[PC_BRANCH]++;
      continue;
    }
    if (o->is_mov_rr) { kc[PC_NONE]++; continue; } /* eliminated */
    account_insn(o, press, flex, lat);
    kc[o->pc]++;
  }

  /* Dominant kind for the instruction-mix label. */
  static const char *pc_kind[16] = {
      "other", "alu", "shift", "mul", "div", "lea", "lea", "setcc",
      "cmov", "branch", "branch", "call", "float", "float", "float"};
  int best = PC_ALU, bestc = -1;
  for (int c = 1; c < 15; c++) if (kc[c] > bestc) { bestc = kc[c]; best = c; }
  if (press[RES_LD] && bestc <= 0) best = -1;
  *kindname = (best >= 0) ? pc_kind[best] : "load";
  if (*flex == 0) {
    int anyfixed = 0;
    for (int r = 0; r < RES_COUNT; r++) anyfixed |= press[r];
    if (!anyfixed) *kindname = "other";
  }
  return 1;
}

/* Port string for a refined (byte-derived) kind, for display. */
static const char *kind_ports(const char *k) {
  if (!k) return "";
  if (!strcmp(k, "alu")) return "p0156";
  if (!strcmp(k, "shift")) return "p06";
  if (!strcmp(k, "mul")) return "p1";
  if (!strcmp(k, "div")) return "p0";
  if (!strcmp(k, "lea")) return "p15";
  if (!strcmp(k, "setcc") || !strcmp(k, "cmov")) return "p06";
  if (!strcmp(k, "branch") || !strcmp(k, "call")) return "p6";
  if (!strcmp(k, "float")) return "p01";
  if (!strcmp(k, "load")) return "load";
  if (!strcmp(k, "store")) return "store";
  return "";
}

/* Replace a recorded span's opcode-estimated cost with the byte-accurate one
 * when the emitted machine bytes decode cleanly. Kernels keep their estimate
 * (they contain VEX the decoder bails on, and are excluded from loop costs
 * anyway). On a decode bail, the opcode estimate stands and the span is flagged
 * so the UI can disclose that one span was estimated. */
static void refine_cost_from_bytes(AnnotInsn *r, const unsigned char *bytes,
                                   size_t byte_len) {
  if (r->is_kernel || !bytes || byte_len == 0) return;
  int bp[RES_COUNT] = {0}, bflex = 0, blat = 0;
  const char *bkind = NULL;
  if (!classify_span_bytes(bytes, byte_len, bp, &bflex, &blat, &bkind)) {
    r->cost_estimated = 1;
    return;
  }
  for (int i = 0; i < RES_COUNT; i++) r->press[i] = bp[i];
  r->flex_alu = bflex;
  r->lat = blat;
  int total = bflex;
  for (int i = 0; i < RES_COUNT; i++) total += bp[i];
  r->rthru = total;
  if (bkind) {
    r->kind = bkind;
    r->ports = kind_ports(bkind);
  }
}

/* ---- decision classification -------------------------------------------- */

/* Derive a structural decision tag/note from the MIR op and allocation state,
 * before the richer --explain join. note is malloc'd or NULL. */
static void classify(const MirFunction *fn, const MirInst *in, char **tag,
                     char **note) {
  *tag = NULL;
  *note = NULL;
  /* Auto-vectorizer kernels and packed-SIMD ops (the contiguous vector range
   * of the opcode enum, from MIR_VADD onward). */
  if (in->op == MIR_SIMD_SILU_F32) {
    *tag = dupstr("vectorized");
    *note = dupstr("AVX2 SiLU/SwiGLU kernel (exp-poly, 8-wide f32)");
    return;
  }
  if (in->op >= MIR_VADD && in->op < MIR_OPCODE_COUNT) {
    *tag = dupstr("vectorized");
    Sb b = {0};
    sb_putf(&b, "auto-vectorized: %s", mir_opcode_name(in->op));
    *note = b.data;
    return;
  }
  if (in->op == MIR_MULHI) {
    *tag = dupstr("strength-reduce");
    *note = dupstr("constant divide/modulo via magic multiply (no idiv)");
    return;
  }
  if (in->op == MIR_LEA || in->op == MIR_LEA_LOCAL || in->op == MIR_LEA_GLOBAL) {
    *tag = dupstr("address-fold");
    *note = dupstr("address arithmetic folded into a single lea");
    return;
  }
  if (in->op == MIR_LOAD_GLOBAL || in->op == MIR_STORE_GLOBAL) {
    *tag = dupstr("global-cache");
    return;
  }
  if (in->op == MIR_CALL || in->op == MIR_CALL_INDIRECT) {
    *tag = dupstr("call");
    return;
  }
  /* A def into a spilled vreg: register pressure forced a stack slot. */
  if (in->dst.kind == MIR_OPK_VREG && fn && in->dst.vreg >= 0 &&
      (size_t)in->dst.vreg < fn->vreg_count) {
    const MirVreg *vr = &fn->vregs[in->dst.vreg];
    if (vr->assigned && !vr->in_register && !vr->address_taken) {
      *tag = dupstr("spill");
      Sb b = {0};
      sb_putf(&b, "register pressure: value lives at [rbp-%d]", vr->spill_offset);
      *note = b.data;
      return;
    }
  }
}

/* ---- recording ---------------------------------------------------------- */

static AnnotFunc *push_func(void) {
  if (g.func_count >= g.func_cap) {
    size_t nc = g.func_cap ? g.func_cap * 2 : 16;
    AnnotFunc *grown = (AnnotFunc *)realloc(g.funcs, nc * sizeof(AnnotFunc));
    if (!grown) return NULL;
    g.funcs = grown;
    g.func_cap = nc;
  }
  AnnotFunc *f = &g.funcs[g.func_count++];
  memset(f, 0, sizeof(*f));
  return f;
}

void mir_annotate_begin_function(const char *name, const IRFunction *ir_fn,
                                 const char *filename, size_t decl_line) {
  if (!g.enabled) return;
  AnnotFunc *f = push_func();
  if (!f) return;
  f->name = dupstr(name ? name : "?");
  f->file = dupstr(filename ? filename : (g.source_file ? g.source_file : "?"));
  f->line = decl_line;
  g.cur = f;
  g.cur_ir = ir_fn;
  g.cur_block = -1;
}

/* Classify an originating IR instruction as profiling instrumentation:
 *   0 = normal user instruction (record it),
 *   1 = mettle_profile_block(id) marker -- updates g.cur_block, do NOT record,
 *   2 = other mettle_profile_* shim (enter/exit/op) -- do NOT record.
 * Markers/shims are kept out of the listing and the cost model; only the real
 * code they bracket is costed, and the measured block id flows onto it. */
static int annot_instrument_kind(const IRInstruction *in) {
  uint32_t id = 0;
  if (!in || in->op != IR_OP_CALL || !in->text) return 0;
  if (ir_profile_instruction_is_block(in, &id)) {
    g.cur_block = (int)id;
    return 1;
  }
  if (strncmp(in->text, "mettle_profile_", 15) == 0) return 2;
  return 0;
}

void mir_annotate_note_backend(const char *backend, const char *reason) {
  if (!g.enabled || !g.cur) return;
  free(g.cur->backend);
  free(g.cur->backend_reason);
  g.cur->backend = dupstr(backend);
  g.cur->backend_reason = dupstr(reason);
}

/* Snapshot the allocator's result for the open MIR function: each physical
 * register's live interval, and the spill count. Called lazily on the first
 * recorded instruction, when the MirFunction is still in scope. */
static void snapshot_regmap(AnnotFunc *f, const MirFunction *fn) {
  if (!f || !fn || f->snapped) return;
  f->snapped = 1;
  f->axis = (int)fn->insn_count;
  for (size_t v = 0; v < fn->vreg_count; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (!vr->assigned) continue;
    if (!vr->in_register) {
      f->spill_count++;
      continue;
    }
    if (vr->live_start == MIR_LIVE_NONE) continue;
    if (f->reg_count >= f->reg_cap) {
      size_t nc = f->reg_cap ? f->reg_cap * 2 : 16;
      RegInterval *grown =
          (RegInterval *)realloc(f->regs, nc * sizeof(RegInterval));
      if (!grown) return;
      f->regs = grown;
      f->reg_cap = nc;
    }
    RegInterval *ri = &f->regs[f->reg_count++];
    ri->phys = vr->phys;
    ri->rclass = vr->rclass;
    ri->width = vr->width;
    ri->vreg = (int)v;
    ri->start = vr->live_start;
    ri->end = vr->live_end == MIR_LIVE_NONE ? vr->live_start : vr->live_end;
    ri->crosses_call = vr->crosses_call;
    ri->loop_carried = vr->loop_carried;
  }
}

void mir_annotate_end_function(void) {
  if (!g.enabled || !g.cur) return;
  if (g.cur->insn_count) {
    AnnotInsn *last = &g.cur->insns[g.cur->insn_count - 1];
    g.cur->byte_size = last->off + last->len;
  }
  /* If the declaration line was unavailable, fall back to the first instruction
   * that carries a source line. */
  if (g.cur->line == 0) {
    for (size_t i = 0; i < g.cur->insn_count; i++) {
      if (g.cur->insns[i].line) {
        g.cur->line = g.cur->insns[i].line;
        break;
      }
    }
  }
  analyze_function(g.cur);
  g.cur = NULL;
  g.cur_ir = NULL;
}

static char *bytes_to_hex(const unsigned char *bytes, size_t len) {
  if (!bytes || !len) return dupstr("");
  char *out = (char *)malloc(len * 3 + 1);
  if (!out) return NULL;
  size_t p = 0;
  for (size_t i = 0; i < len; i++) {
    static const char *hx = "0123456789abcdef";
    if (i) out[p++] = ' ';
    out[p++] = hx[bytes[i] >> 4];
    out[p++] = hx[bytes[i] & 0xF];
  }
  out[p] = '\0';
  return out;
}

static AnnotInsn *push_insn(AnnotFunc *f) {
  if (f->insn_count >= f->insn_cap) {
    size_t nc = f->insn_cap ? f->insn_cap * 2 : 64;
    AnnotInsn *grown = (AnnotInsn *)realloc(f->insns, nc * sizeof(AnnotInsn));
    if (!grown) return NULL;
    f->insns = grown;
    f->insn_cap = nc;
  }
  AnnotInsn *r = &f->insns[f->insn_count++];
  memset(r, 0, sizeof(*r));
  r->mir_index = -1;
  r->block = -1; /* 0 is a valid block id, so an unstamped insn must be -1 */
  return r;
}

void mir_annotate_record(const MirFunction *fn, const MirInst *in, int mir_index,
                         size_t byte_off, size_t byte_len,
                         const unsigned char *bytes) {
  if (!g.enabled || !g.cur || byte_len == 0) return;
  snapshot_regmap(g.cur, fn);
  /* Profiling instrumentation (block markers + enter/exit/op shims) is bracket
   * code, not user code: skip recording it, but let a block marker advance the
   * current block id so it flows onto the real instructions that follow. */
  const IRInstruction *src_ir = NULL;
  if (g.cur_ir && in->ir_index >= 0 &&
      (size_t)in->ir_index < g.cur_ir->instruction_count) {
    src_ir = &g.cur_ir->instructions[in->ir_index];
  }
  if (annot_instrument_kind(src_ir)) return;
  AnnotInsn *r = push_insn(g.cur);
  if (!r) return;
  r->block = g.cur_block;
  r->mir_index = mir_index;
  r->off = byte_off;
  r->len = byte_len;
  r->bytes = bytes_to_hex(bytes, byte_len);
  r->mir = dupstr(mir_opcode_name(in->op));
  render_inst(fn, in, &r->intel, &r->att);
  classify(fn, in, &r->tag, &r->note);
  int kern = 0;
  cost_model(fn, in, &r->lat, r->press, &r->flex_alu, &r->rthru, &r->kind,
             &r->ports, &kern);
  r->is_kernel = (unsigned char)kern;
  refine_cost_from_bytes(r, bytes, byte_len);
  /* Control-flow structure for loop recovery. */
  if (in->op == MIR_LABEL) {
    r->is_label = 1;
    r->label = dupstr(in->dst.sym);
  } else if (in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
             in->op == MIR_FCMPBR) {
    if (in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      r->is_branch = 1;
      r->target = dupstr(in->dst.sym);
    }
  }
  /* Source line via the IR instruction the MIR op came from. */
  r->line = src_ir ? src_ir->location.line : 0;
}

void mir_annotate_record_synthetic(const char *label, const char *decision,
                                   size_t byte_off, size_t byte_len,
                                   const unsigned char *bytes) {
  if (!g.enabled || !g.cur || byte_len == 0) return;
  AnnotInsn *r = push_insn(g.cur);
  if (!r) return;
  r->mir_index = -1;
  r->off = byte_off;
  r->len = byte_len;
  r->bytes = bytes_to_hex(bytes, byte_len);
  r->mir = dupstr(label ? label : "SYNTH");
  r->intel = dupstr(label ? label : "");
  r->att = dupstr(label ? label : "");
  r->tag = decision ? dupstr(decision) : NULL;
  r->note = NULL;
  r->line = 0;
  r->kind = "frame";
  r->ports = "";
}

/* ---- whole-function analysis (loops, depth, summary) -------------------- */

static int find_label_rec(const AnnotFunc *f, const char *name) {
  if (!name) return -1;
  for (size_t i = 0; i < f->insn_count; i++)
    if (f->insns[i].is_label && f->insns[i].label &&
        strcmp(f->insns[i].label, name) == 0)
      return (int)i;
  return -1;
}

static Loop *push_loop(AnnotFunc *f) {
  if (f->loop_count >= f->loop_cap) {
    size_t nc = f->loop_cap ? f->loop_cap * 2 : 8;
    Loop *grown = (Loop *)realloc(f->loops, nc * sizeof(Loop));
    if (!grown) return NULL;
    f->loops = grown;
    f->loop_cap = nc;
  }
  Loop *l = &f->loops[f->loop_count++];
  memset(l, 0, sizeof(*l));
  return l;
}

/* Distribute `add` centicycles of flexible ALU work across the four ALU ports
 * (whose current fixed loads are in base[]) so as to MINIMIZE the resulting
 * maximum -- the classic water-filling an out-of-order scheduler approximates.
 * Writes the leveled per-port loads into out[]. */
static const int alu_ports[4] = {RES_P0, RES_P1, RES_P5, RES_P6};
static void waterfill(const int base[4], int add, int out[4]) {
  for (int i = 0; i < 4; i++) out[i] = base[i];
  if (add <= 0) return;
  int lo = base[0];
  for (int i = 1; i < 4; i++)
    if (base[i] < lo) lo = base[i];
  int hi = lo + add;
  while (lo < hi) { /* largest level L with sum(max(0,L-base)) <= add */
    int mid = lo + (hi - lo + 1) / 2;
    long need = 0;
    for (int i = 0; i < 4; i++)
      if (base[i] < mid) need += mid - base[i];
    if (need <= add) lo = mid;
    else hi = mid - 1;
  }
  long used = 0;
  for (int i = 0; i < 4; i++)
    if (base[i] < lo) { out[i] = lo; used += lo - base[i]; }
  int leftover = add - (int)used; /* < 4; pour onto the least-loaded ports */
  while (leftover-- > 0) {
    int mi = 0;
    for (int i = 1; i < 4; i++)
      if (out[i] < out[mi]) mi = i;
    out[mi]++;
  }
}

/* Recover natural loops (a backward branch to an earlier label), compute each
 * instruction's nesting depth, summarize each loop's port pressure / bottleneck
 * and the function's instruction mix and hot-weighted cost. */
static void analyze_function(AnnotFunc *f) {
  if (!f) return;

  /* 1. Recover loops from backward branches. */
  for (size_t p = 0; p < f->insn_count; p++) {
    AnnotInsn *br = &f->insns[p];
    if (!br->is_branch || !br->target) continue;
    int q = find_label_rec(f, br->target);
    if (q < 0 || (size_t)q >= p) continue; /* forward edge: not a loop */
    /* Merge with an existing loop sharing this header (the natural body is the
     * widest back-edge to a header). */
    Loop *existing = NULL;
    for (size_t li = 0; li < f->loop_count; li++) {
      if (f->loops[li].start_rec == q) {
        existing = &f->loops[li];
        break;
      }
    }
    if (existing) {
      if ((int)p > existing->end_rec) existing->end_rec = (int)p;
      continue;
    }
    Loop *l = push_loop(f);
    if (!l) break;
    l->start_rec = q;
    l->end_rec = (int)p;
    /* The header label itself often carries no source line (it is a synthetic
     * marker); use the first body instruction that does, falling back to the
     * back-edge's line. */
    l->head_line = f->insns[q].line;
    for (int j = q; j <= (int)p && !l->head_line; j++)
      if (f->insns[j].line) l->head_line = f->insns[j].line;
    if (!l->head_line) l->head_line = br->line;
    l->tail_line = br->line;
    l->header = f->insns[q].label ? dupstr(f->insns[q].label) : NULL;
  }

  /* 2. Per-instruction nesting depth, and per-loop containment depth. */
  for (size_t i = 0; i < f->insn_count; i++) {
    int depth = 0;
    for (size_t li = 0; li < f->loop_count; li++)
      if ((int)i >= f->loops[li].start_rec && (int)i <= f->loops[li].end_rec)
        depth++;
    f->insns[i].loop_depth = depth;
  }
  for (size_t li = 0; li < f->loop_count; li++) {
    Loop *l = &f->loops[li];
    int d = 0;
    for (size_t lj = 0; lj < f->loop_count; lj++) {
      if (lj == li) continue;
      const Loop *o = &f->loops[lj];
      if (o->start_rec <= l->start_rec && o->end_rec >= l->end_rec) d++;
    }
    l->depth = d;
  }

  /* 3. Per-loop port pressure + bottleneck. Fixed (narrow) uop pressure sums
   * directly; flexible ALU work is water-filled across p0/p1/p5/p6 around it so
   * the busiest port reflects what a scheduler would actually produce. */
  for (size_t li = 0; li < f->loop_count; li++) {
    Loop *l = &f->loops[li];
    int flex = 0;
    for (int j = l->start_rec; j <= l->end_rec && j < (int)f->insn_count; j++) {
      AnnotInsn *r = &f->insns[j];
      if (r->is_kernel) {
        l->has_kernel = 1;
        continue;
      }
      if (r->cost_estimated) l->has_estimated = 1;
      for (int res = 0; res < RES_COUNT; res++) l->press[res] += r->press[res];
      flex += r->flex_alu;
    }
    int base[4], filled[4];
    for (int k = 0; k < 4; k++) base[k] = l->press[alu_ports[k]];
    waterfill(base, flex, filled);
    for (int k = 0; k < 4; k++) l->press[alu_ports[k]] = filled[k];
    int best = 0;
    for (int res = 1; res < RES_COUNT; res++)
      if (l->press[res] > l->press[best]) best = res;
    l->bottleneck = best;
    l->cycles_per_iter = l->press[best];
  }

  /* 4. Instruction mix, total throughput, hot-weighted cost, vec coverage. */
  for (size_t i = 0; i < f->insn_count; i++) {
    AnnotInsn *r = &f->insns[i];
    if (!r->kind) r->kind = "other";
    f->mix[kind_index(r->kind)]++;
    f->total_rthru += r->rthru;
    if (r->cost_estimated && !r->is_kernel) f->cost_estimated++;
    if (r->is_kernel || (r->kind && strcmp(r->kind, "vec") == 0)) f->vec_ops++;
    long w = 1;
    int d = r->loop_depth;
    while (d-- > 0 && w < 1000) w *= 10;
    f->hot_cost += (long)r->rthru * w;
  }
}

/* ---- output: JSON sidecar + stdout listing ------------------------------ */

static void json_escape(FILE *o, const char *s) {
  if (!s) {
    fputs("null", o);
    return;
  }
  fputc('"', o);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
    case '"': fputs("\\\"", o); break;
    case '\\': fputs("\\\\", o); break;
    case '\n': fputs("\\n", o); break;
    case '\r': fputs("\\r", o); break;
    case '\t': fputs("\\t", o); break;
    default:
      if (c < 0x20)
        fprintf(o, "\\u%04x", c);
      else
        fputc(c, o);
    }
  }
  fputc('"', o);
}

/* Emit this function's --explain remarks (loops/calls vectorized/inlined) as a
 * JSON array, and return them so the per-line join can reuse the same data. */
static void emit_remarks_json(FILE *o, const char *fnname) {
  size_t n = ir_explain_remark_count();
  int first = 1;
  fputs("\"remarks\":[", o);
  for (size_t i = 0; i < n; i++) {
    const char *rfn, *entity, *headline, *reason, *fix, *verified;
    size_t line, depth;
    int positive;
    if (!ir_explain_remark_at(i, &rfn, &entity, &line, &positive, &headline,
                              &reason, &fix, &verified, &depth))
      continue;
    if (!rfn || !fnname || strcmp(rfn, fnname) != 0) continue;
    if (!first) fputc(',', o);
    first = 0;
    fputs("{\"entity\":", o);
    json_escape(o, entity);
    fprintf(o, ",\"line\":%zu,\"positive\":%d,\"headline\":", line, positive);
    json_escape(o, headline);
    fputs(",\"reason\":", o);
    json_escape(o, reason);
    fputs(",\"fix\":", o);
    json_escape(o, fix);
    fputs(",\"verified\":", o);
    json_escape(o, verified);
    fputc('}', o);
  }
  fputc(']', o);
}

static void emit_summary_json(FILE *o, const AnnotFunc *f) {
  fputs("\"summary\":{", o);
  fprintf(o, "\"totalRthru\":%d,\"hotCost\":%ld,\"spills\":%d,\"loops\":%zu,"
             "\"vecOps\":%d,\"axis\":%d,\"regsUsed\":%zu,\"estimatedSpans\":%d,"
             "\"mix\":{",
          f->total_rthru, f->hot_cost, f->spill_count, f->loop_count, f->vec_ops,
          f->axis, f->reg_count, f->cost_estimated);
  int first = 1;
  for (int k = 0; k < KIND_COUNT; k++) {
    if (!f->mix[k]) continue;
    if (!first) fputc(',', o);
    first = 0;
    fprintf(o, "\"%s\":%d", KINDS[k], f->mix[k]);
  }
  fputs("}}", o);
}

static void emit_loops_json(FILE *o, const AnnotFunc *f) {
  fputs("\"loops\":[", o);
  for (size_t li = 0; li < f->loop_count; li++) {
    const Loop *l = &f->loops[li];
    if (li) fputc(',', o);
    fprintf(o, "{\"depth\":%d,\"headLine\":%zu,\"tailLine\":%zu,"
               "\"startRec\":%d,\"endRec\":%d,\"cyclesPerIter\":%d,"
               "\"bottleneck\":\"%s\",\"hasKernel\":%d,\"estimated\":%d,"
               "\"header\":",
            l->depth, l->head_line, l->tail_line, l->start_rec, l->end_rec,
            l->cycles_per_iter, res_name[l->bottleneck], l->has_kernel,
            l->has_estimated);
    json_escape(o, l->header);
    fputs(",\"press\":[", o);
    for (int r = 0; r < RES_COUNT; r++)
      fprintf(o, "%s%d", r ? "," : "", l->press[r]);
    fputs("]}", o);
  }
  fputc(']', o);
}

static void emit_regmap_json(FILE *o, const AnnotFunc *f) {
  fprintf(o, "\"regmap\":{\"axis\":%d,\"spills\":%d,\"resNames\":[", f->axis,
          f->spill_count);
  for (int r = 0; r < RES_COUNT; r++)
    fprintf(o, "%s\"%s\"", r ? "," : "", res_name[r]);
  fputs("],\"intervals\":[", o);
  for (size_t i = 0; i < f->reg_count; i++) {
    const RegInterval *ri = &f->regs[i];
    if (i) fputc(',', o);
    const char *nm = ri->rclass == MIR_RC_GP ? gp_name(ri->phys, 8)
                                             : vec_name(ri->phys, ri->rclass);
    fprintf(o, "{\"name\":\"%s\",\"cls\":%d,\"start\":%d,\"end\":%d,"
               "\"crossesCall\":%d,\"loopCarried\":%d}",
            nm, ri->rclass, ri->start, ri->end, ri->crosses_call,
            ri->loop_carried);
  }
  fputs("]}", o);
}

static char *annot_json_path(void) {
  const char *src = g.output_path ? g.output_path : g.source_file;
  if (!src) return dupstr("a.annot.json");
  size_t n = strlen(src);
  /* Strip the last extension. */
  size_t dot = n;
  for (size_t i = n; i > 0; i--) {
    char c = src[i - 1];
    if (c == '/' || c == '\\') break;
    if (c == '.') {
      dot = i - 1;
      break;
    }
  }
  const char *suffix = ".annot.json";
  char *out = (char *)malloc(dot + strlen(suffix) + 1);
  if (!out) return NULL;
  memcpy(out, src, dot);
  strcpy(out + dot, suffix);
  return out;
}

static void write_json(void) {
  char *path = annot_json_path();
  if (!path) return;
  FILE *o = fopen(path, "wb");
  if (!o) {
    fprintf(stderr, "--annotate-asm: cannot write %s\n", path);
    free(path);
    return;
  }
  fputs("{\n", o);
  fputs("\"version\":3,\n\"source\":", o);
  json_escape(o, g.source_file);
  fprintf(o, ",\n\"syntax\":\"%s\",\n",
          g.syntax == MIR_ANNOT_SYNTAX_INTEL
              ? "intel"
              : (g.syntax == MIR_ANNOT_SYNTAX_ATT ? "att" : "both"));
  fputs("\"functions\":[\n", o);
  for (size_t fi = 0; fi < g.func_count; fi++) {
    AnnotFunc *f = &g.funcs[fi];
    if (fi) fputs(",\n", o);
    fputs("{\"name\":", o);
    json_escape(o, f->name);
    fputs(",\"file\":", o);
    json_escape(o, f->file);
    fprintf(o, ",\"line\":%zu,\"byte_size\":%zu,", f->line, f->byte_size);
    fputs("\"backend\":", o);
    json_escape(o, f->backend);
    fputs(",\"backendReason\":", o);
    json_escape(o, f->backend_reason);
    fputc(',', o);
    emit_summary_json(o, f);
    fputc(',', o);
    emit_loops_json(o, f);
    fputc(',', o);
    emit_regmap_json(o, f);
    fputc(',', o);
    emit_remarks_json(o, f->name);
    fputs(",\"insns\":[", o);
    for (size_t ii = 0; ii < f->insn_count; ii++) {
      AnnotInsn *r = &f->insns[ii];
      if (ii) fputc(',', o);
      fprintf(o, "{\"idx\":%d,\"off\":%zu,\"len\":%zu,\"line\":%zu,\"block\":%d,",
              r->mir_index, r->off, r->len, r->line, r->block);
      fputs("\"bytes\":", o);
      json_escape(o, r->bytes);
      fputs(",\"intel\":", o);
      json_escape(o, r->intel);
      fputs(",\"att\":", o);
      json_escape(o, r->att);
      fputs(",\"mir\":", o);
      json_escape(o, r->mir);
      fputs(",\"tag\":", o);
      json_escape(o, r->tag);
      fputs(",\"note\":", o);
      json_escape(o, r->note);
      fprintf(o, ",\"lat\":%d,\"rthru\":%d,\"depth\":%d,\"est\":%d,"
                 "\"kind\":\"%s\",\"ports\":",
              r->lat, r->rthru, r->loop_depth, r->cost_estimated,
              r->kind ? r->kind : "other");
      json_escape(o, r->ports);
      /* Per-instruction issue-port pressure: the measured-frequency view in the
       * extension scales these by execution count for a real port-utilization
       * breakdown. press[] are FIXED ports; falu is flexible ALU centicycles. */
      fprintf(o, ",\"press\":[%d,%d,%d,%d,%d,%d],\"falu\":%d",
              r->press[RES_P0], r->press[RES_P1], r->press[RES_P5],
              r->press[RES_P6], r->press[RES_LD], r->press[RES_ST], r->flex_alu);
      fputc('}', o);
    }
    fputs("]}", o);
  }
  fputs("\n]\n}\n", o);
  fclose(o);
  fprintf(stderr, "--annotate-asm: wrote %s (%zu functions)\n", path,
          g.func_count);
  free(path);
}

static void cycles_str(int centi, char *out, size_t n) {
  snprintf(out, n, "%d.%02d", centi / 100, centi % 100);
}

/* A compact ASCII register-lifetime map: one lane per physical register, the
 * MIR index axis scaled to a fixed column width, a bar over each live interval.
 * This is the piano-roll that makes spills legible at a glance. */
static void write_regmap_ascii(const AnnotFunc *f) {
  if (f->reg_count == 0 || f->axis <= 0) return;
  const int COLS = 60;
  printf("  -- register lifetimes (MIR index 0..%d, '#'=live, 'C'=crosses call,"
         " '*'=loop-carried) --\n",
         f->axis);
  for (size_t i = 0; i < f->reg_count; i++) {
    const RegInterval *ri = &f->regs[i];
    const char *nm = ri->rclass == MIR_RC_GP ? gp_name(ri->phys, 8)
                                             : vec_name(ri->phys, ri->rclass);
    char lane[64];
    for (int c = 0; c < COLS; c++) lane[c] = ' ';
    lane[COLS] = '\0';
    int s = ri->start * COLS / (f->axis ? f->axis : 1);
    int e = ri->end * COLS / (f->axis ? f->axis : 1);
    if (s < 0) s = 0;
    if (e >= COLS) e = COLS - 1;
    char fill = ri->loop_carried ? '*' : (ri->crosses_call ? 'C' : '#');
    for (int c = s; c <= e; c++) lane[c] = fill;
    printf("  %-5s |%s|\n", nm, lane);
  }
  if (f->spill_count)
    printf("  %d value%s spilled to the stack.\n", f->spill_count,
           f->spill_count == 1 ? "" : "s");
  printf("\n");
}

static void write_summary_ascii(const AnnotFunc *f) {
  char c[16];
  cycles_str(f->total_rthru, c, sizeof(c));
  printf("  -- summary: ~%s cycles static throughput", c);
  if (f->loop_count)
    printf(", %zu loop%s", f->loop_count, f->loop_count == 1 ? "" : "s");
  if (f->spill_count) printf(", %d spill%s", f->spill_count,
                             f->spill_count == 1 ? "" : "s");
  if (f->vec_ops) printf(", %d vector op%s", f->vec_ops,
                         f->vec_ops == 1 ? "" : "s");
  printf(" --\n");
  /* instruction mix */
  printf("     mix:");
  for (int k = 0; k < KIND_COUNT; k++)
    if (f->mix[k]) printf(" %s=%d", KINDS[k], f->mix[k]);
  printf("\n");
  /* per-loop bottlenecks. Costs are decoded from the emitted instructions; a
   * loop that contains an inline SIMD kernel or a span the decoder could not
   * read is marked, since its figure is then partial/estimated. */
  for (size_t li = 0; li < f->loop_count; li++) {
    const Loop *l = &f->loops[li];
    char cy[16];
    cycles_str(l->cycles_per_iter, cy, sizeof(cy));
    printf("     loop @ line %zu (depth %d): ~%s cyc/iter, bound on %s%s%s\n",
           l->head_line, l->depth, cy, res_name[l->bottleneck],
           l->has_kernel ? " (+ inline SIMD kernel, excluded)" : "",
           l->has_estimated ? " (partly estimated)" : "");
  }
  if (f->cost_estimated)
    printf("     note: %d span%s could not be decoded and use an opcode "
           "estimate\n",
           f->cost_estimated, f->cost_estimated == 1 ? "" : "s");
  printf("\n");
}

static void write_stdout(void) {
  int intel = g.syntax != MIR_ANNOT_SYNTAX_ATT;
  int att = g.syntax != MIR_ANNOT_SYNTAX_INTEL;
  printf("; Mettle codegen annotation");
  if (g.source_file) printf("  (%s)", g.source_file);
  printf("\n; one row = one emitted op; raw bytes shown; lat/rt = latency /"
         " reciprocal throughput (cycles), a static Skylake-class port model\n"
         "; decoded from the emitted instructions: macro-fusion (cmp+jcc) and"
         " reg-reg mov elimination modeled; branch prediction is not\n\n");
  for (size_t fi = 0; fi < g.func_count; fi++) {
    AnnotFunc *f = &g.funcs[fi];
    printf("==== %s  (%s:%zu)  %zu bytes  [%s%s%s] ====\n", f->name,
           f->file ? f->file : "?", f->line, f->byte_size,
           f->backend ? f->backend : "?",
           f->backend_reason ? " " : "",
           f->backend_reason ? f->backend_reason : "");
    write_summary_ascii(f);
    /* Function-level remarks first. */
    size_t n = ir_explain_remark_count();
    for (size_t i = 0; i < n; i++) {
      const char *rfn, *entity, *headline, *reason, *fix, *verified;
      size_t line, depth;
      int positive;
      if (!ir_explain_remark_at(i, &rfn, &entity, &line, &positive, &headline,
                                &reason, &fix, &verified, &depth))
        continue;
      if (!rfn || strcmp(rfn, f->name) != 0) continue;
      printf("  ; %s @ line %zu: %s\n", entity ? entity : "?", line,
             positive ? (headline ? headline : "optimized")
                      : (headline ? headline : "not optimized"));
      if (reason) printf("  ;     reason: %s\n", reason);
      if (fix) printf("  ;     fix: %s\n", fix);
      if (verified) printf("  ;     verified: %s\n", verified);
    }
    for (size_t ii = 0; ii < f->insn_count; ii++) {
      AnnotInsn *r = &f->insns[ii];
      char loc[32] = "";
      if (r->line) snprintf(loc, sizeof(loc), ":%zu", r->line);
      const char *asmtext = intel ? r->intel : r->att;
      /* Brief bytes field: whole bytes up to 21 cols, then '+' if truncated. */
      char brief[24];
      const char *hex = r->bytes ? r->bytes : "";
      size_t cut = strlen(hex);
      if (cut > 21) {
        cut = 21;
        while (cut > 0 && hex[cut] != ' ' && hex[cut - 1] != ' ') cut--;
        if (cut == 0) cut = 21;
      }
      snprintf(brief, sizeof(brief), "%.*s%s", (int)cut, hex,
               strlen(hex) > cut ? "+" : "");
      /* Loop-depth gutter: one '|' per nesting level marks hot instructions. */
      char gutter[8] = "";
      int gd = r->loop_depth > 4 ? 4 : r->loop_depth;
      for (int d = 0; d < gd; d++) gutter[d] = '|';
      gutter[gd] = '\0';
      char cost[16] = "";
      if (r->rthru > 0 || r->lat > 1) {
        char rt[12];
        cycles_str(r->rthru, rt, sizeof(rt));
        snprintf(cost, sizeof(cost), "%2dc/%s", r->lat, rt);
      }
      printf("  %04zx %-4s %-18.18s %-30s %-9s", r->off, gutter, brief,
             asmtext ? asmtext : "", cost);
      if (intel && att && r->att && *r->att) printf("  | %s", r->att);
      if (r->tag) {
        printf("   ; %s%s", r->tag, loc);
        if (r->note) printf(" - %s", r->note);
      } else if (r->line) {
        printf("   ; %s", loc + 1);
      }
      printf("\n");
    }
    printf("\n");
    write_regmap_ascii(f);
  }
}

/* ---- LLM-facing focused queries ----------------------------------------- *
 *
 * These print a compact, structured report instead of the full listing: a
 * developer or an agent asks "what did the compiler do with lines A..B" (or
 * "where is the time") and gets back just the relevant asm, cost, covering
 * loops, live registers, and decisions. The goal is high signal per token. */

static const char *base_name(const char *p) {
  const char *b = p;
  if (p)
    for (const char *s = p; *s; s++)
      if (*s == '/' || *s == '\\') b = s + 1;
  return b;
}

static int icase_eq(const char *a, const char *b) {
  if (!a || !b) return a == b;
  for (; *a && *b; a++, b++) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return 0;
  }
  return *a == *b;
}

/* A focused query targets one named function, or (default) every function from
 * the main source file - so a line range is not confused with the same line
 * numbers in an imported stdlib file. */
static int query_in_focus(const AnnotFunc *f) {
  if (g.q_fn) return f->name && strcmp(f->name, g.q_fn) == 0;
  return icase_eq(base_name(f->file), base_name(g.source_file));
}

/* Print this function's --explain remark for `line` (if any), compactly. */
static void query_remark_for_line(const char *fnname, size_t line) {
  size_t n = ir_explain_remark_count();
  for (size_t i = 0; i < n; i++) {
    const char *rfn, *entity, *headline, *reason, *fix, *verified;
    size_t rline, depth;
    int positive;
    if (!ir_explain_remark_at(i, &rfn, &entity, &rline, &positive, &headline,
                              &reason, &fix, &verified, &depth))
      continue;
    if (!rfn || !fnname || strcmp(rfn, fnname) != 0 || rline != line) continue;
    printf("    decision: %s\n", headline ? headline : (positive ? "optimized" : "not optimized"));
    if (reason) printf("      reason: %s\n", reason);
    if (fix) printf("      fix: %s\n", fix);
    if (verified) printf("      verified: %s\n", verified);
  }
}

static void write_line_query(void) {
  int lo = g.q_lo ? g.q_lo : 1;
  int hi = g.q_hi ? g.q_hi : 1000000000;
  printf("# codegen query: %s", g.source_file ? g.source_file : "?");
  if (g.q_lo) printf(" lines %d-%d", lo, hi);
  if (g.q_fn) printf(" fn=%s", g.q_fn);
  printf("\n");
  int any_fn = 0;
  for (size_t fi = 0; fi < g.func_count; fi++) {
    AnnotFunc *f = &g.funcs[fi];
    if (!query_in_focus(f)) continue;
    /* in-range instruction span + aggregates */
    int n_in = 0, min_idx = -1, max_idx = -1, rt = 0, kern = 0;
    size_t bytes = 0;
    for (size_t ii = 0; ii < f->insn_count; ii++) {
      AnnotInsn *r = &f->insns[ii];
      if (r->line < (size_t)lo || r->line > (size_t)hi) continue;
      n_in++;
      bytes += r->len;
      rt += r->rthru;
      if (r->is_kernel) kern++;
      if (r->mir_index >= 0) {
        if (min_idx < 0 || r->mir_index < min_idx) min_idx = r->mir_index;
        if (r->mir_index > max_idx) max_idx = r->mir_index;
      }
    }
    if (!n_in && !g.q_fn) continue;
    any_fn = 1;
    char c[16];
    cycles_str(rt, c, sizeof(c));
    printf("\n=== %s  (%s:%zu, %s) ===\n", f->name, f->file ? f->file : "?",
           f->line, f->backend ? f->backend : "?");
    if (!n_in) {
      printf("  (no emitted code on those lines)\n");
      continue;
    }
    printf("  %d op%s, %zu bytes, ~%s cyc static throughput", n_in,
           n_in == 1 ? "" : "s", bytes, c);
    if (kern)
      printf(" (+%d SIMD kernel%s, run at vector speed, excluded from the static"
             " estimate)",
             kern, kern == 1 ? "" : "s");
    printf("\n");
    /* asm grouped by source line, in emission order */
    size_t cur_line = 0;
    for (size_t ii = 0; ii < f->insn_count; ii++) {
      AnnotInsn *r = &f->insns[ii];
      if (r->line < (size_t)lo || r->line > (size_t)hi) continue;
      if (r->line != cur_line) {
        cur_line = r->line;
        printf("  line %zu:\n", cur_line);
        query_remark_for_line(f->name, cur_line);
      }
      char cost[16] = "";
      if (r->rthru > 0 || r->lat > 1) {
        char rtb[12];
        cycles_str(r->rthru, rtb, sizeof(rtb));
        snprintf(cost, sizeof(cost), "%dc/%s", r->lat, rtb);
      }
      printf("    %04zx  %-30s %-9s", r->off, r->intel ? r->intel : "", cost);
      if (r->tag) {
        printf("  [%s%s%s]", r->tag, r->note ? ": " : "", r->note ? r->note : "");
      }
      printf("\n");
    }
    /* loops covering the range */
    for (size_t li = 0; li < f->loop_count; li++) {
      Loop *l = &f->loops[li];
      size_t lt = l->tail_line ? l->tail_line : l->head_line;
      if ((int)l->head_line > hi || (int)lt < lo) continue;
      char cy[16];
      cycles_str(l->cycles_per_iter, cy, sizeof(cy));
      printf("  loop @ line %zu (depth %d): ~%s cyc/iter, bound on %s%s%s\n",
             l->head_line, l->depth, cy, res_name[l->bottleneck],
             l->has_kernel ? " (+SIMD kernel)" : "",
             l->has_estimated ? " (partly estimated)" : "");
    }
    /* registers live across the range (register-allocated functions only),
     * deduplicated by physical register with merged crosses-call/loop-carried
     * flags - one physical register may host several vregs across the span. */
    if (f->reg_count && min_idx >= 0) {
      const char *names[32];
      int cc[32], lc[32], rcls[32], nd = 0;
      for (size_t ri = 0; ri < f->reg_count; ri++) {
        RegInterval *iv = &f->regs[ri];
        if (iv->start > max_idx || iv->end < min_idx) continue;
        const char *nm = iv->rclass == MIR_RC_GP ? gp_name(iv->phys, 8)
                                                 : vec_name(iv->phys, iv->rclass);
        int found = -1;
        for (int k = 0; k < nd; k++)
          if (icase_eq(names[k], nm)) { found = k; break; }
        if (found < 0 && nd < 32) {
          found = nd++;
          names[found] = nm; cc[found] = 0; lc[found] = 0;
          rcls[found] = iv->rclass;
        }
        if (found >= 0) {
          if (iv->crosses_call) cc[found] = 1;
          if (iv->loop_carried) lc[found] = 1;
        }
      }
      int gp = 0, vec = 0;
      printf("  registers live across MIR [%d..%d]:", min_idx, max_idx);
      for (int k = 0; k < nd; k++) {
        printf(" %s%s%s", names[k], cc[k] ? "(C)" : "", lc[k] ? "(*)" : "");
        if (rcls[k] == MIR_RC_GP) gp++; else vec++;
      }
      printf("  [%d GP, %d vec", gp, vec);
      if (f->spill_count) printf(", %d spilled", f->spill_count);
      printf("; (C)=crosses call, (*)=loop-carried]\n");
    }
  }
  if (!any_fn)
    printf("\n(no register-allocated or emitted code matched the query)\n");
}

/* Top-N hotspots across the program: hottest loops by cycles/iteration weighted
 * by nesting depth, and a note of spills / fallback functions. */
static void write_hot_query(void) {
  int n = g.q_hot > 0 ? g.q_hot : 8;
  /* gather loops */
  typedef struct { const char *fn; size_t line; int depth, cyc, has_kernel; long w; const char *port; } HL;
  size_t cap = 0;
  for (size_t fi = 0; fi < g.func_count; fi++) cap += g.funcs[fi].loop_count;
  HL *hl = cap ? (HL *)malloc(cap * sizeof(HL)) : NULL;
  size_t hn = 0;
  for (size_t fi = 0; fi < g.func_count; fi++) {
    AnnotFunc *f = &g.funcs[fi];
    if (!query_in_focus(f)) continue;
    for (size_t li = 0; li < f->loop_count; li++) {
      Loop *l = &f->loops[li];
      long w = 1;
      int d = l->depth;
      while (d-- > 0 && w < 1000) w *= 10;
      if (hl) {
        hl[hn].fn = f->name; hl[hn].line = l->head_line; hl[hn].depth = l->depth;
        hl[hn].cyc = l->cycles_per_iter; hl[hn].has_kernel = l->has_kernel;
        hl[hn].w = (long)l->cycles_per_iter * w; hl[hn].port = res_name[l->bottleneck];
        hn++;
      }
    }
  }
  /* selection sort the top n by weight (small lists) */
  for (size_t i = 0; i < hn && (int)i < n; i++) {
    size_t best = i;
    for (size_t j = i + 1; j < hn; j++) if (hl[j].w > hl[best].w) best = j;
    if (best != i) { HL t = hl[i]; hl[i] = hl[best]; hl[best] = t; }
  }
  printf("# codegen hotspots: %s (top %d loops by cycles/iter x nesting)\n",
         g.source_file ? g.source_file : "?", n);
  int shown = (int)hn < n ? (int)hn : n;
  if (!shown) printf("  (no loops recovered)\n");
  for (int i = 0; i < shown; i++) {
    char cy[16];
    cycles_str(hl[i].cyc, cy, sizeof(cy));
    printf("  %-14s line %-5zu ~%s cyc/iter  bound on %-5s depth %d%s\n",
           hl[i].fn ? hl[i].fn : "?", hl[i].line, cy, hl[i].port, hl[i].depth,
           hl[i].has_kernel ? "  (+SIMD kernel)" : "");
  }
  /* spills / fallback notes */
  for (size_t fi = 0; fi < g.func_count; fi++) {
    AnnotFunc *f = &g.funcs[fi];
    if (query_in_focus(f) && f->spill_count)
      printf("  note: %s spills %d value%s to the stack\n", f->name,
             f->spill_count, f->spill_count == 1 ? "" : "s");
  }
  for (size_t fi = 0; fi < g.func_count; fi++) {
    AnnotFunc *f = &g.funcs[fi];
    if (query_in_focus(f) && f->backend && strstr(f->backend, "fallback"))
      printf("  note: %s uses the baseline backend (not register-allocated)\n",
             f->name);
  }
  free(hl);
}

static void free_all(void) {
  for (size_t fi = 0; fi < g.func_count; fi++) {
    AnnotFunc *f = &g.funcs[fi];
    for (size_t ii = 0; ii < f->insn_count; ii++) {
      AnnotInsn *r = &f->insns[ii];
      free(r->bytes);
      free(r->intel);
      free(r->att);
      free(r->mir);
      free(r->tag);
      free(r->note);
      free(r->target);
      free(r->label);
    }
    free(f->insns);
    free(f->regs);
    for (size_t li = 0; li < f->loop_count; li++) free(f->loops[li].header);
    free(f->loops);
    free(f->name);
    free(f->file);
    free(f->backend);
    free(f->backend_reason);
  }
  free(g.funcs);
  g.funcs = NULL;
  g.func_count = g.func_cap = 0;
}

/* Hand the cost model to the --explain report.
 *
 * The optimizer's report knows which loops it refused to vectorize; only
 * codegen knows what those loops then cost. Publishing here joins the two
 * without the IR layer having to know codegen exists: the same direction the
 * backend gate already reports in. */
static void publish_costs_to_explain(void) {
  if (!ir_explain_enabled()) return;
  for (size_t fi = 0; fi < g.func_count; fi++) {
    const AnnotFunc *f = &g.funcs[fi];
    ir_explain_backend_cost(f->name, f->file, f->spill_count, (int)f->reg_count,
                            f->total_rthru, f->hot_cost, f->vec_ops,
                            f->cost_estimated);
    for (size_t li = 0; li < f->loop_count; li++) {
      const Loop *l = &f->loops[li];
      ir_explain_backend_loop(f->name, f->file, l->head_line, l->tail_line,
                              l->depth, l->cycles_per_iter,
                              res_name[l->bottleneck], l->has_kernel,
                              l->has_estimated);
    }
  }
}

void mir_annotate_flush(void) {
  if (!g.enabled) return;
  publish_costs_to_explain();
  if (g.cost_only) {
    /* Armed purely to measure for --explain-json: no listing, no sidecar. */
    free_all();
    free(g.q_fn);
    g.q_fn = NULL;
    return;
  }
  if (g.q_hot || g.q_lo || g.q_fn) {
    /* Focused, tool-facing output: no giant listing or sidecar. */
    if (g.q_hot) write_hot_query();
    if (g.q_lo || g.q_fn) write_line_query();
  } else {
    write_json();
    write_stdout();
  }
  free_all();
  free(g.q_fn);
  g.q_fn = NULL;
}
