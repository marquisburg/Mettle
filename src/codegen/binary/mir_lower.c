#include "codegen/binary/mir.h"
#include "ir/ir_machine.h"

extern long long mir_encode_last_spills;
#include "codegen/binary/strength_rules.h"

#include "codegen/binary/mir_annotate.h"
#include "common.h"
#include "ir/ir_optimize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Source file a function was declared in (for the --explain focus filter). */
static const char *mir_function_filename(const IRFunction *fn) {
  return fn ? fn->location.filename : NULL;
}

/* Non-nop IR size of the function currently in the eligibility gate, captured
 * at gate entry so the dozens of bail sites can report it to --explain
 * without each threading it through (the report ranks bailed functions by
 * size -- that is where baseline codegen actually costs). */
static size_t g_mir_gate_fn_size = 0;

/* TEMPORARY instrumentation: with METTLE_MIR_TRACE set, log why a function is
 * rejected by the MIR eligibility gate, so the spill-everything-fallback work
 * list can be prioritized by real frequency. Returns 0 (ineligible). Also
 * feeds the --explain backend report (a no-op when --explain is off). */
/* getenv is slow on Windows and these are consulted per function (or per
 * bail); snapshot each knob once per process. */
static int mir_env_trace(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_MIR_TRACE") ? 1 : 0;
  }
  return cached;
}

static const char *mir_env_mir(void) {
  static const char *cached = NULL;
  static int resolved = 0;
  if (!resolved) {
    cached = getenv("METTLE_MIR");
    resolved = 1;
  }
  return cached;
}

static const char *mir_env_skipfn(void) {
  static const char *cached = NULL;
  static int resolved = 0;
  if (!resolved) {
    cached = getenv("METTLE_MIR_SKIPFN");
    resolved = 1;
  }
  return cached;
}

static int g_mir_gate_reported = 0;

static const char *mir_operand_kind_name(int kind);

static const char *mir_bail_kind(const char *reason, int kind) {
  static char joined[64];
  snprintf(joined, sizeof(joined), "%s:%s", reason, mir_operand_kind_name(kind));
  return joined;
}

static int mir_trace_bail(const IRFunction *fn, const char *reason) {
  g_mir_gate_reported = 1;
  if (mir_env_trace()) {
    fprintf(stderr, "MIR-BAIL\t%s\t%s\n", reason,
            (fn && fn->name) ? fn->name : "?");
  }
  if (ir_explain_enabled() && fn && fn->name) {
    ir_explain_backend_function(fn->name, mir_function_filename(fn), 0, reason,
                                g_mir_gate_fn_size);
    if (ir_machine_collecting()) {
      ir_machine_note_backend(fn->name, mir_function_filename(fn),
                              (long long)g_mir_gate_fn_size, 0);
    }
  }
  return 0;
}

extern const char *g_mir_ra_trace_name;

/* ---- inline kernel operand walk ----------------------------------------- */

/* Every operand slot of an instruction in one sequence: dest, lhs, rhs, then
 * the arguments. Returns NULL past the end, so callers just index upward. Both
 * the eligibility gate and the lowering walk kernels this way, which is what
 * keeps them agreeing on exactly which operands get staged. */
static const IROperand *mir_instruction_operand_at(const IRInstruction *in,
                                                   int index) {
  if (index == 0) {
    return &in->dest;
  }
  if (index == 1) {
    return &in->lhs;
  }
  if (index == 2) {
    return &in->rhs;
  }
  size_t a = (size_t)(index - 3);
  if (in->arguments && a < in->argument_count) {
    return &in->arguments[a];
  }
  return NULL;
}

/* Upper bound on the staging slots an inline kernel instruction needs: one per
 * by-name (TEMP/SYMBOL) operand. Immediates, floats, and string literals are
 * materialized by the kernel itself and need no slot. Returns -1 if an operand
 * is of a kind the bridge cannot stage (a LABEL, which no kernel takes). The
 * real slot count can only be lower, since operands naming one value share a
 * slot; over-estimating here just makes the gate marginally strict. */
static int mir_kernel_slot_estimate(const IRInstruction *in) {
  int slots = 0;
  for (int k = 0;; k++) {
    const IROperand *op = mir_instruction_operand_at(in, k);
    if (!op) {
      break;
    }
    switch (op->kind) {
    case IR_OPERAND_TEMP:
    case IR_OPERAND_SYMBOL:
      slots++;
      break;
    case IR_OPERAND_NONE:
    case IR_OPERAND_INT:
    case IR_OPERAND_FLOAT:
    case IR_OPERAND_STRING:
      break;
    default:
      return -1;
    }
  }
  return slots;
}

/* True if `name` resolves to a global variable of any type. Its storage has a
 * link-time address, so `&name` is always one RIP-relative LEA. */
static int mir_name_is_global_variable(CodeGenerator *g, const char *name) {
  if (!g || !g->ir_program || !name) {
    return 0;
  }
  const CgSym *s = code_generator_lookup_symbol(g, name);
  return s && s->kind == CG_SYM_VARIABLE && s->scope &&
         s->scope->type == CG_SCOPE_GLOBAL;
}

/* True if `name`'s address escapes through a module-level initializer: another
 * global holds &name (init_symbol_ref), or an aggregate initializer embeds it
 * (init_relocs). Such a global is aliasable by a pointer the function body
 * never visibly creates, so its cache vreg must ride the address-taken
 * flush/reload discipline even though no IR_OP_ADDRESS_OF names it. */
static int mir_global_address_escapes_via_initializer(CodeGenerator *g,
                                                      const char *name) {
  if (!g || !g->ir_program || !name) {
    return 0;
  }
  const IRProgram *p = g->ir_program;
  for (size_t i = 0; i < p->module_symbol_count; i++) {
    const IRModuleSymbol *s = &p->module_symbols[i];
    if (s->init_symbol_ref && strcmp(s->init_symbol_ref, name) == 0) {
      return 1;
    }
    for (size_t r = 0; r < s->init_reloc_count; r++) {
      if (s->init_relocs[r].symbol &&
          strcmp(s->init_relocs[r].symbol, name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static int mir_global_address_taken_in_module(CodeGenerator *g,
                                              const char *name) {
  if (!g || !g->ir_program) {
    return 0;
  }
  return ir_program_global_address_taken(g->ir_program, name);
}

/* True if the source declared `name` volatile. Reading or writing such a global
 * is observable in itself, so its value may not live in a register: a spin on
 * one would read its cache vreg forever and never see another thread's write. */
static int mir_global_is_volatile(CodeGenerator *g, const char *name) {
  const IRModuleSymbol *s = NULL;
  if (!g || !g->ir_program || !name) {
    return 0;
  }
  s = ir_program_lookup_symbol(g->ir_program, name);
  return s && s->is_volatile;
}

/* True if `op` names a local or parameter of pointer-to-bytes type, the one
 * destination a bare string literal may be assigned to by address: the value
 * IS the address of the literal's NUL-terminated .rdata copy. A `string`
 * destination is a 16-byte record and takes the struct-home path instead, so
 * this never truncates one to its data pointer. */
static MtlcType *mir_local_or_param_type(CodeGenerator *g,
                                         const IRFunction *ir_function,
                                         const char *name, int *is_param_out);

static int mir_operand_is_cstring_home(CodeGenerator *g,
                                       const IRFunction *ir_function,
                                       const IROperand *op) {
  if (!op || !op->name ||
      (op->kind != IR_OPERAND_SYMBOL && op->kind != IR_OPERAND_TEMP)) {
    return 0;
  }
  return code_generator_binary_type_is_cstring(
      mir_local_or_param_type(g, ir_function, op->name, NULL));
}

/* True if `name` resolves to a read-accessible global scalar, a value we can
 * cache in a register at function entry (used by both the eligibility gate and
 * the entry-load emitter, so they agree exactly on what counts as cacheable). */
static int mir_name_is_global_scalar(CodeGenerator *g, const char *name) {
  if (!mir_name_is_global_variable(g, name)) {
    return 0;
  }
  if (mir_global_is_volatile(g, name)) {
    return 0;
  }
  return code_generator_binary_symbol_is_scalar_accessible(g, name);
}

/* IR -> MIR lowering for the Stage 2 scalar-integer subset, plus the
 * per-function eligibility gate and the MIR emit entry point.
 *
 * Eligible functions (see mir_function_is_eligible) are pure leaf integer code:
 * no calls, no address-of, no floats, no aggregates, <=4 GP params, and only
 * the opcodes handled below. Anything else falls back to the legacy emitter.
 * All values are computed as 64-bit; loads/stores carry their own width and
 * casts re-extend, so holding everything in 64-bit registers is exact. */

/* ---- name -> vreg map --------------------------------------------------- */

typedef struct {
  const char *name; /* borrowed (interned IR string) */
  int is_temp;      /* TEMP and SYMBOL operands are distinct namespaces: a
                       compiler temp may share its bare name with a user
                       local, and conflating them merges their storage */
  MirVregId vreg;
} MirNameEntry;

typedef struct {
  MirNameEntry *items;
  size_t count;
  size_t capacity;
  /* Open-addressing index over items (slot+1; 0 = empty). Linear name scans
   * here were a measured hotspot on large inlined functions. */
  size_t *buckets;
  size_t bucket_count;
} MirNameMap;

static void mir_name_map_destroy(MirNameMap *m) {
  free(m->items);
  free(m->buckets);
  m->items = NULL;
  m->buckets = NULL;
  m->count = m->capacity = m->bucket_count = 0;
}

static size_t mir_name_map_hash(const char *name, int is_temp) {
  size_t h = mettle_fnv1a_hash(name);
  return h ^ (is_temp ? 0x9e3779b97f4a7c15ull : 0);
}

static int mir_name_map_reindex(MirNameMap *m, size_t min_buckets) {
  size_t nb = 64;
  while (nb < min_buckets) {
    nb *= 2;
  }
  size_t *fresh = (size_t *)calloc(nb, sizeof(size_t));
  if (!fresh) {
    return 0;
  }
  for (size_t i = 0; i < m->count; i++) {
    size_t b = mir_name_map_hash(m->items[i].name, m->items[i].is_temp) &
               (nb - 1);
    while (fresh[b]) {
      b = (b + 1) & (nb - 1);
    }
    fresh[b] = i + 1;
  }
  free(m->buckets);
  m->buckets = fresh;
  m->bucket_count = nb;
  return 1;
}

static MirVregId mir_name_map_get_or_add(MirNameMap *m, MirFunction *fn,
                                         const char *name, int is_temp,
                                         MirRegClass rclass, int width) {
  if (m->bucket_count && m->count * 4 < m->bucket_count * 3) {
    size_t b = mir_name_map_hash(name, is_temp) & (m->bucket_count - 1);
    while (m->buckets[b]) {
      const MirNameEntry *e = &m->items[m->buckets[b] - 1];
      if (e->is_temp == is_temp && strcmp(e->name, name) == 0) {
        return e->vreg;
      }
      b = (b + 1) & (m->bucket_count - 1);
    }
  } else {
    for (size_t i = 0; i < m->count; i++) {
      if (m->items[i].is_temp == is_temp &&
          strcmp(m->items[i].name, name) == 0) {
        return m->items[i].vreg;
      }
    }
  }
  if (m->count >= m->capacity) {
    size_t nc = m->capacity ? m->capacity * 2 : 16;
    MirNameEntry *grown =
        (MirNameEntry *)realloc(m->items, nc * sizeof(MirNameEntry));
    if (!grown) {
      fn->has_error = 1;
      return MIR_VREG_NONE;
    }
    m->items = grown;
    m->capacity = nc;
  }
  MirVregId v = mir_new_vreg(fn, rclass, width);
  if (v == MIR_VREG_NONE) {
    return MIR_VREG_NONE;
  }
  m->items[m->count].name = name;
  m->items[m->count].is_temp = is_temp;
  m->items[m->count].vreg = v;
  m->count++;
  if ((m->count + 1) * 4 >= m->bucket_count * 3) {
    if (!mir_name_map_reindex(m, (m->count + 1) * 2)) {
      fn->has_error = 1;
      return MIR_VREG_NONE;
    }
  } else {
    size_t b = mir_name_map_hash(name, is_temp) & (m->bucket_count - 1);
    while (m->buckets[b]) {
      b = (b + 1) & (m->bucket_count - 1);
    }
    m->buckets[b] = m->count; /* slot index (count-1) + 1 */
  }
  return v;
}

/* True if symbol `name` already has a vreg binding (param/local/cached
 * global). Symbols only, temps live in a separate namespace. */
static int mir_name_map_has(const MirNameMap *m, const char *name) {
  if (m->bucket_count) {
    size_t b = mir_name_map_hash(name, 0) & (m->bucket_count - 1);
    while (m->buckets[b]) {
      const MirNameEntry *e = &m->items[m->buckets[b] - 1];
      if (!e->is_temp && strcmp(e->name, name) == 0) {
        return 1;
      }
      b = (b + 1) & (m->bucket_count - 1);
    }
    return 0;
  }
  for (size_t i = 0; i < m->count; i++) {
    if (!m->items[i].is_temp && strcmp(m->items[i].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Register-promoted globals. Each referenced global scalar is loaded once at
 * entry (MIR_LOAD_GLOBAL) into a cache vreg; `all` lists every cached global and
 * `names` the subset that is written (stored back before every return). In a
 * function that makes calls, memory, not the cache vreg, is authoritative
 * across a call boundary: the written set is flushed before each call (so the
 * callee sees current values) and the full cached set is reloaded after (so we
 * observe any value the callee changed). Names are borrowed interned IR
 * strings. */
typedef struct {
  const char **names; /* written globals (write-back / flush-before-call) */
  size_t count;
  const char **all; /* every cached global (reload-after-call) */
  size_t all_count;
  const char **at;  /* address-taken globals (aliasable via &g): flush before a
                       pointer LOAD/STORE, reload after a pointer STORE, so a
                       store through the alias and a by-name access stay coherent */
  size_t at_count;
  /* Per-IR-instruction dirty masks over names[] (bit j = names[j] possibly
   * written since the last cleaning point on some path reaching that
   * instruction). NULL = no analysis, flush the whole written set. */
  const unsigned long long *dirty;
} MirGlobalWriteback;

/* ---- operand mapping ---------------------------------------------------- */

/* Map an IR operand that must resolve to a value: a float TEMP/SYMBOL -> an XMM
 * vreg, an int TEMP/SYMBOL -> a GP vreg, INT -> immediate, FLOAT -> float
 * immediate (raw IEEE bits). Sets has_error for anything outside the subset. */
static MirOperand mir_value_operand(MirFunction *fn, CodeGenerator *g,
                                    BinaryFunctionContext *ctx, MirNameMap *map,
                                    const IROperand *op) {
  switch (op->kind) {
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    int fb = code_generator_binary_operand_float_bits(g, ctx, op);
    MirRegClass rc = fb ? MIR_RC_XMM : MIR_RC_GP;
    int w = fb ? fb / 8 : 8;
    MirVregId v = mir_name_map_get_or_add(map, fn, op->name,
                                          op->kind == IR_OPERAND_TEMP, rc, w);
    return mir_op_vreg(v);
  }
  case IR_OPERAND_INT:
    return mir_op_imm(op->int_value);
  case IR_OPERAND_FLOAT: {
    int fb = op->float_bits == 32 ? 32 : 64;
    uint64_t bits;
    if (fb == 32) {
      float fv = (float)op->float_value;
      uint32_t u;
      memcpy(&u, &fv, sizeof(u));
      bits = u;
    } else {
      double dv = op->float_value;
      uint64_t u;
      memcpy(&u, &dv, sizeof(u));
      bits = u;
    }
    return mir_op_fimm(bits);
  }
  default:
    fn->has_error = 1;
    return mir_op_none();
  }
}

static MirOperand mir_gp_value_operand(MirFunction *fn, CodeGenerator *g,
                                       BinaryFunctionContext *ctx,
                                       MirNameMap *map, const IROperand *op) {
  if (op->kind == IR_OPERAND_FLOAT) {
    if (op->float_bits == 32) {
      float single = (float)op->float_value;
      uint32_t single_bits;
      memcpy(&single_bits, &single, sizeof(single_bits));
      return mir_op_imm((long long)(unsigned long long)single_bits);
    }
    double wide = op->float_value;
    uint64_t wide_bits;
    memcpy(&wide_bits, &wide, sizeof(wide_bits));
    return mir_op_imm((long long)wide_bits);
  }
  return mir_value_operand(fn, g, ctx, map, op);
}

/* ---- compare/shift helpers ---------------------------------------------- */

/* setcc opcode (second byte) for an IR comparison operator, signed or not. */
static int mir_setcc_opcode(const char *op, int is_unsigned, unsigned char *out) {
  return binary_semantics_condition_code(op, is_unsigned, out);
}

static int mir_is_comparison(const char *op) {
  return binary_semantics_is_comparison(op);
}

/* jcc opcode (second byte) to take when an IR comparison is FALSE, i.e. the
 * branch a `branch_zero` of the comparison result should take. */
static int mir_false_jcc(const char *op, int is_unsigned, unsigned char *out) {
  if (strcmp(op, "==") == 0) { *out = 0x85; return 1; } /* jne */
  if (strcmp(op, "!=") == 0) { *out = 0x84; return 1; } /* je */
  if (strcmp(op, "<") == 0)  { *out = is_unsigned ? 0x83 : 0x8D; return 1; } /* jae/jge */
  if (strcmp(op, "<=") == 0) { *out = is_unsigned ? 0x87 : 0x8F; return 1; } /* ja/jg */
  if (strcmp(op, ">") == 0)  { *out = is_unsigned ? 0x86 : 0x8E; return 1; } /* jbe/jle */
  if (strcmp(op, ">=") == 0) { *out = is_unsigned ? 0x82 : 0x8C; return 1; } /* jb/jl */
  return 0;
}

/* Ordered float comparison via ucomis. Because ucomis sets CF on "unordered"
 * (NaN), we pick the operand order so the single condition code is NaN-correct
 * (a comparison involving NaN must be false). `swap` requests ucomis(rhs,lhs).
 * For fused branches `cc` is the jcc taken when the comparison is FALSE
 * (branch_zero semantics); otherwise it is the setcc taken when TRUE.
 * Only the ordering operators are handled here; == / != need extra PF handling
 * and are left to the legacy path. */
static int mir_float_cmp_info(const char *op, int fused, int *swap,
                              unsigned char *cc) {
  if (strcmp(op, ">") == 0)  { *swap = 0; *cc = fused ? 0x86 : 0x97; return 1; }
  if (strcmp(op, ">=") == 0) { *swap = 0; *cc = fused ? 0x82 : 0x93; return 1; }
  if (strcmp(op, "<") == 0)  { *swap = 1; *cc = fused ? 0x86 : 0x97; return 1; }
  if (strcmp(op, "<=") == 0) { *swap = 1; *cc = fused ? 0x82 : 0x93; return 1; }
  if (!fused && strcmp(op, "==") == 0) { *swap = 0; *cc = 0x94; return 1; }
  if (!fused && strcmp(op, "!=") == 0) { *swap = 0; *cc = 0x95; return 1; }
  return 0;
}

/* Float arithmetic operator -> MIR opcode (divide is supported for floats). */
static int mir_float_arith_opcode(const char *op, MirOpcode *out) {
  if (strcmp(op, "+") == 0) { *out = MIR_FADD; return 1; }
  if (strcmp(op, "-") == 0) { *out = MIR_FSUB; return 1; }
  if (strcmp(op, "*") == 0) { *out = MIR_FMUL; return 1; }
  if (strcmp(op, "/") == 0) { *out = MIR_FDIV; return 1; }
  return 0;
}

/* Arithmetic operator -> MIR opcode. Returns 0 if not an arithmetic op we
 * handle (integer divide/modulo are intentionally excluded). */
static int mir_arith_opcode(const char *op, MirOpcode *out) {
  if (strcmp(op, "+") == 0)  { *out = MIR_ADD; return 1; }
  if (strcmp(op, "-") == 0)  { *out = MIR_SUB; return 1; }
  if (strcmp(op, "*") == 0)  { *out = MIR_IMUL; return 1; }
  if (strcmp(op, "&") == 0)  { *out = MIR_AND; return 1; }
  if (strcmp(op, "|") == 0)  { *out = MIR_OR; return 1; }
  if (strcmp(op, "^") == 0)  { *out = MIR_XOR; return 1; }
  if (strcmp(op, "<<") == 0) { *out = MIR_SHL; return 1; }
  if (strcmp(op, ">>") == 0) { *out = MIR_SHR; return 1; } /* SAR chosen by sign */
  return 0;
}

/* Structural equality of two IR operands (for divmod-pair matching). Only the
 * operand kinds that can be a div dividend/divisor are compared; anything else
 * (float/string/none) is treated as unequal. */
static int mir_ir_operand_equal(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return a->name && b->name && strcmp(a->name, b->name) == 0;
  case IR_OPERAND_INT:
    return a->int_value == b->int_value;
  default:
    return 0;
  }
}

static int mir_operand_is_unsigned(CodeGenerator *g, BinaryFunctionContext *ctx,
                                   const IROperand *op) {
  MtlcType *t = code_generator_binary_get_operand_type_in_context(g, ctx, op);
  if (!t) {
    return 0; /* default signed */
  }
  return !code_generator_binary_resolved_type_is_signed_integer(t);
}

/* Byte width that constrains an integer comparison operand: its scalar size for
 * a known <=4-byte integer, 8 for a 64-bit integer / pointer / unknown type, and
 * 0 for an INT literal (it constrains nothing, it follows the other operand). */
static int mir_cmp_operand_width(CodeGenerator *g, BinaryFunctionContext *ctx,
                                 const IROperand *op) {
  if (op->kind == IR_OPERAND_INT) {
    return 0;
  }
  MtlcType *t = code_generator_binary_get_operand_type_in_context(g, ctx, op);
  if (!t || code_generator_type_is_aggregate(t) ||
      code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 8;
  }
  int s = code_generator_binary_resolved_type_scalar_size(t);
  return (s == 1 || s == 2 || s == 4) ? s : 8;
}

/* Width at which to compare two integer operands. MIR computes in 64-bit, so a
 * narrow value (e.g. a uint32 product) can carry garbage in its high bits; a
 * full 64-bit compare would then see that garbage and give the wrong answer.
 *
 * C compares at the promoted operand width, and so must MIR. We narrow to a
 * 32-bit cmp when BOTH typed operands are exactly 4-byte (int32/uint32)
 * integers: the 32-bit cmp looks only at the low 32 bits, which are always the
 * true value (the carried garbage lives above bit 31), and the signed/unsigned
 * setcc/jcc the caller picks reads the 32-bit flags, correct for equality AND
 * ordering. 1/2-byte operands and any 8-byte/pointer operand (or missing type
 * info) keep the conservative 64-bit compare. `op` is currently unused but kept
 * so the policy can be refined per operator if ever needed. */
static int mir_int_compare_width(CodeGenerator *g, BinaryFunctionContext *ctx,
                                 const char *op, const IROperand *lhs,
                                 const IROperand *rhs) {
  (void)op;
  int wl = mir_cmp_operand_width(g, ctx, lhs);
  int wr = mir_cmp_operand_width(g, ctx, rhs);
  int m = wl > wr ? wl : wr;
  return m == 4 ? 4 : 8;
}

/* ---- eligibility -------------------------------------------------------- */

static int mir_type_is_gp_scalar(CodeGenerator *g, const char *type_name) {
  MtlcType *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  if (!t) {
    return 0;
  }
  if (code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 0;
  }
  if (code_generator_type_is_aggregate(t)) {
    return 0;
  }
  int sz = code_generator_binary_resolved_type_scalar_size(t);
  return sz == 1 || sz == 2 || sz == 4 || sz == 8;
}

/* A GP scalar OR a float32/float64 (the types MIR can now keep in a register). */
static int mir_type_is_numeric_scalar(CodeGenerator *g, const char *type_name) {
  if (mir_type_is_gp_scalar(g, type_name)) {
    return 1;
  }
  MtlcType *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  return t && code_generator_binary_resolved_type_float_bits(t) != 0;
}

/* A DIRECT small aggregate (struct/array by value, size 1/2/4/8): the Win64 ABI
 * passes and returns it in a single GP register exactly like an integer, so MIR
 * can carry it as an 8-byte value. Its memory home (when its address is taken
 * for field access) is 8 bytes, which covers the whole struct. Larger or
 * non-power-of-2 aggregates are INDIRECT (hidden pointer) and still bail. */
static int mir_type_is_direct_small_aggregate(CodeGenerator *g,
                                              const char *type_name) {
  MtlcType *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  if (!t || !code_generator_type_is_aggregate(t)) {
    return 0;
  }
  if (code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 0;
  }
  if (code_generator_abi_classify(t) != ABI_PASS_DIRECT) {
    return 0;
  }
  size_t sz = code_generator_abi_type_size(t);
  return sz == 1 || sz == 2 || sz == 4 || sz == 8;
}

/* A type MIR can carry as a register-or-home value at a signature/local
 * boundary: a numeric scalar, or a DIRECT small aggregate (treated as 8 bytes).
 */
static int mir_type_is_mir_value(CodeGenerator *g, const char *type_name) {
  return mir_type_is_numeric_scalar(g, type_name) ||
         mir_type_is_direct_small_aggregate(g, type_name);
}

/* An INDIRECT aggregate (struct/array by value, size>8 or non-power-of-2): the
 * Win64 ABI passes it BY REFERENCE, the caller copies it to a temp and passes
 * the address in a GP register. A parameter of this type therefore arrives as a
 * pointer, which MIR can hold as an 8-byte value; the body accesses fields
 * through that pointer (&@p yields the pointer, not a stack home). */
static int mir_type_is_indirect_aggregate(CodeGenerator *g,
                                          const char *type_name) {
  MtlcType *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  return t && code_generator_type_is_aggregate(t) &&
         code_generator_abi_classify(t) == ABI_PASS_INDIRECT;
}

/* A type acceptable as a PARAMETER: a MIR value (scalar / DIRECT small agg) or
 * an INDIRECT aggregate (received by reference as a pointer). */
static int mir_type_is_param_value(CodeGenerator *g, const char *type_name) {
  return mir_type_is_mir_value(g, type_name) ||
         mir_type_is_indirect_aggregate(g, type_name);
}

/* Resolve the type of a NAME that is a parameter or a declared local of this IR
 * function. The symbol table has popped function scope by codegen time, so a
 * direct symbol_table_lookup returns NULL for locals/params; instead read the
 * function signature and DECLARE_LOCAL instructions, exactly as
 * code_generator_binary_get_operand_type_in_context does. *is_param_out (if
 * given) is set when the name is a parameter. Returns NULL for globals/unknown
 * names (which the caller resolves through the global symbol table). */
static MtlcType *mir_local_or_param_type(CodeGenerator *g,
                                     const IRFunction *ir_function,
                                     const char *name, int *is_param_out) {
  if (is_param_out) {
    *is_param_out = 0;
  }
  if (!g || !ir_function || !name) {
    return NULL;
  }
  for (size_t i = 0; i < ir_function->parameter_count; i++) {
    if (ir_function->parameter_names && ir_function->parameter_names[i] &&
        ir_function->parameter_names[i][0] == name[0] &&
        strcmp(ir_function->parameter_names[i], name) == 0) {
      if (is_param_out) {
        *is_param_out = 1;
      }
      return code_generator_binary_get_resolved_type(
          g, ir_function->parameter_types ? ir_function->parameter_types[i]
                                          : NULL,
          0);
    }
  }
  {
    const IRInstruction *declaration =
        ir_function_find_declaration(ir_function, name, 0);

    if (declaration) {
      return code_generator_binary_get_resolved_type(g, declaration->text, 0);
    }
  }
  return NULL;
}

/* If `dest` names a signed/unsigned sub-64-bit integer variable (local, param, or
 * global scalar), return its byte width (1/2/4), else 0. Used to keep narrow
 * homes canonical: MIR computes in 64 bits, so an arithmetic result written to
 * a typed int32/uint32/int16/etc. home can carry bits above the type's width.
 * Narrow integer homes wrap to their width, so each such write is followed by
 * sign- or zero-extension of the destination vreg.
 */
static int mir_dest_integer_narrow_width(CodeGenerator *g,
                                         BinaryFunctionContext *ctx,
                                         const IROperand *dest,
                                         int *is_signed_out) {
  if (is_signed_out) {
    *is_signed_out = 0;
  }
  if (!g || !ctx || !ctx->function_name || !dest || !dest->name ||
      (dest->kind != IR_OPERAND_SYMBOL && dest->kind != IR_OPERAND_TEMP)) {
    return 0;
  }
  MtlcType *t = NULL;
  if (dest->kind == IR_OPERAND_TEMP) {
    /* A temporary has no local/param/global home; its defining instruction
     * bakes the result type into value_type (builder API). Resolving it lets a
     * narrow temp carry the same canonicalization as a narrow named home, so
     * `(x << 28)` computed into a temp is sign-extended before a following
     * arithmetic shift reads it -- the frontend no longer needs to force the
     * operand into a local. */
    t = code_generator_binary_get_operand_type_in_context(g, ctx, dest);
  } else {
    IRFunction *irf =
        code_generator_find_ir_function_binary(g, ctx->function_name);
    if (!irf) {
      return 0;
    }
    t = mir_local_or_param_type(g, irf, dest->name, NULL);
    if (!t && g->ir_program) {
      /* Not a local/param: a global scalar (its symbol never goes out of
       * scope). The cached-global vreg carries the value across the function
       * body, so it needs the same canonicalization as a local's vreg. */
      const CgSym *s = code_generator_lookup_symbol(g, dest->name);
      t = s ? s->type : NULL;
    }
  }
  if (!t || code_generator_type_is_aggregate(t) ||
      code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 0;
  }
  int w = code_generator_binary_resolved_type_scalar_size(t);
  if (is_signed_out) {
    *is_signed_out = code_generator_binary_resolved_type_is_signed_integer(t);
  }
  return (w == 1 || w == 2 || w == 4) ? w : 0;
}

/* True if NAME is an INDIRECT aggregate local or by-reference parameter of this
 * function. MIR only touches such a value through its ADDRESS (field LOAD/STORE
 * off &@sym); a by-NAME use of the whole aggregate (assign, return, call
 * argument) would be miscompiled as an 8-byte MOV, so the eligibility gate
 * forbids it (except `return @local`, handled by Link 2). */
static int mir_name_is_indirect_aggregate(CodeGenerator *g,
                                          const IRFunction *ir_function,
                                          const char *name) {
  MtlcType *t = mir_local_or_param_type(g, ir_function, name, NULL);
  return t && code_generator_type_is_aggregate(t) &&
         code_generator_abi_classify(t) == ABI_PASS_INDIRECT;
}

/* True if NAME is a struct LOCAL (not a by-reference parameter): one that owns a
 * stack home holding the struct itself. `return @local` for an INDIRECT return
 * copies from that home; a by-ref PARAMETER's home holds a pointer, not the
 * struct, so it is excluded (deferred to the fallback). */
static int mir_name_is_indirect_struct_local(CodeGenerator *g,
                                             const IRFunction *ir_function,
                                             const char *name) {
  int is_param = 0;
  MtlcType *t = mir_local_or_param_type(g, ir_function, name, &is_param);
  return t && !is_param && code_generator_type_is_aggregate(t) &&
         code_generator_abi_classify(t) == ABI_PASS_INDIRECT;
}

/* roundup8 byte size of an INDIRECT aggregate type, or 0 if `t` isn't one. */
static int mir_indirect_type_home_bytes(CodeGenerator *g, MtlcType *t) {
  if (!t || !code_generator_type_is_aggregate(t) ||
      code_generator_abi_classify(t) != ABI_PASS_INDIRECT) {
    return 0;
  }
  (void)g;
  return (int)((code_generator_abi_type_size(t) + 7) & ~(size_t)7);
}

/* If TEMP `name` holds an INDIRECT struct VALUE, return its home byte size
 * (roundup8), else 0. The IR routes struct call results and intermediates
 * through temps; a temp's struct size is recovered from its context: the
 * INDIRECT return type of the call that defines it, the INDIRECT param type of
 * a call that consumes it, or the type of a struct SYMBOL it is whole-struct
 * assigned to/from. (Resolution is via calls/symbols only, never transitively
 * through another temp, so it cannot recurse.) */
static int mir_struct_temp_size(CodeGenerator *g, const IRFunction *irf,
                                const char *name) {
  if (!g || !irf || !name || !g->ir_program) {
    return 0;
  }
  for (size_t i = 0; i < irf->instruction_count; i++) {
    const IRInstruction *in = &irf->instructions[i];
    if (in->op == IR_OP_CALL && in->text) {
      const CgSym *cal = code_generator_lookup_symbol(g, in->text);
      if (cal && cal->kind == CG_SYM_FUNCTION) {
        /* defined by a struct-returning call */
        if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
            strcmp(in->dest.name, name) == 0) {
          const MtlcType *r = cal->data.function.return_type ? cal->data.function.return_type
                                                   : cal->type;
          int hb = mir_indirect_type_home_bytes(g, r);
          if (hb) {
            return hb;
          }
        }
        /* consumed as a struct-by-value argument */
        if (cal->data.function.parameter_types) {
          for (size_t a = 0; a < in->argument_count &&
                             a < cal->data.function.parameter_count;
               a++) {
            if (in->arguments[a].kind == IR_OPERAND_TEMP &&
                in->arguments[a].name &&
                strcmp(in->arguments[a].name, name) == 0) {
              int hb = mir_indirect_type_home_bytes(
                  g, cal->data.function.parameter_types[a]);
              if (hb) {
                return hb;
              }
            }
          }
        }
      }
    }
    if (in->op == IR_OP_ASSIGN) {
      const IROperand *other = NULL;
      if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
          strcmp(in->dest.name, name) == 0) {
        other = &in->lhs;
      } else if (in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name &&
                 strcmp(in->lhs.name, name) == 0) {
        other = &in->dest;
      }
      if (other && other->kind == IR_OPERAND_SYMBOL && other->name) {
        MtlcType *t = mir_local_or_param_type(g, irf, other->name, NULL);
        int hb = mir_indirect_type_home_bytes(g, t);
        if (hb) {
          return hb;
        }
      }
    }
  }
  return 0;
}

/* Home byte size of an operand that holds an INDIRECT struct VALUE in a stack
 * home we can LEA (a struct LOCAL symbol or a struct TEMP), else 0. A by-ref
 * struct PARAMETER is excluded (its home holds a pointer, not the struct). */
static int mir_operand_struct_home_size(CodeGenerator *g,
                                        const IRFunction *irf,
                                        const IROperand *op) {
  if (op->kind == IR_OPERAND_SYMBOL && op->name) {
    if (!mir_name_is_indirect_struct_local(g, irf, op->name)) {
      return 0;
    }
    MtlcType *t = mir_local_or_param_type(g, irf, op->name, NULL);
    return mir_indirect_type_home_bytes(g, t);
  }
  if (op->kind == IR_OPERAND_TEMP && op->name) {
    return mir_struct_temp_size(g, irf, op->name);
  }
  return 0;
}

/* True if NAME is a by-reference (INDIRECT aggregate) PARAMETER: its value IS
 * the struct's address, so it can source an indirect copy without a home. */
static int mir_name_is_indirect_param(CodeGenerator *g,
                                      const IRFunction *ir_function,
                                      const char *name) {
  int is_param = 0;
  MtlcType *t = mir_local_or_param_type(g, ir_function, name, &is_param);
  return t && is_param && code_generator_type_is_aggregate(t) &&
         code_generator_abi_classify(t) == ABI_PASS_INDIRECT;
}

/* True if NAME is a module-level AGGREGATE variable (struct/array/string
 * global, or an extern one): not a local/param, not scalar-accessible (small
 * DIRECT global aggregates are cached like scalars and stay off this path).
 * Such a global is never register-cached; memory is authoritative and its
 * address is a RIP-relative LEA. */
static int mir_name_is_global_aggregate(CodeGenerator *g,
                                        const IRFunction *irf,
                                        const char *name) {
  if (!g || !name || mir_local_or_param_type(g, irf, name, NULL)) {
    return 0;
  }
  if (!mir_name_is_global_variable(g, name)) {
    return 0;
  }
  const CgSym *s = code_generator_lookup_symbol(g, name);
  return s && s->type && code_generator_type_is_aggregate(s->type) &&
         !code_generator_binary_symbol_is_scalar_accessible(g, name);
}

/* True if NAME is a string-typed LOCAL (not a by-ref param). Under the
 * backend's string convention an 8-byte string VALUE is a pointer to the
 * {chars,length} record, so a string local used by value yields the ADDRESS
 * of its 16-byte home (the fallback's emit_operand_load does the same LEA). */
static int mir_name_is_string_local(CodeGenerator *g, const IRFunction *irf,
                                    const char *name) {
  int is_param = 0;
  MtlcType *t = mir_local_or_param_type(g, irf, name, &is_param);
  return t && !is_param && t->kind == MTLC_TYPE_STRING;
}

/* An operand that can SOURCE an INDIRECT (by-value) aggregate copy: a struct
 * LOCAL or struct TEMP (LEA-able home), a by-ref aggregate PARAM (its value is
 * the address), a global aggregate (RIP-relative LEA; memory authoritative),
 * or a string LITERAL (its {chars,length} record is in .rdata). This is the
 * eligibility-side mirror of the fallback's emit_indirect_source_address. */
static int mir_indirect_source_is_supported(CodeGenerator *g,
                                            const IRFunction *irf,
                                            const IROperand *op) {
  if (op->kind == IR_OPERAND_STRING) {
    return 1;
  }
  if (mir_operand_struct_home_size(g, irf, op) > 0) {
    return 1;
  }
  return op->kind == IR_OPERAND_SYMBOL && op->name &&
         (mir_name_is_indirect_param(g, irf, op->name) ||
          mir_name_is_global_aggregate(g, irf, op->name));
}

/* True if temp `name` holds a float value, judged from the producing
 * instruction's is_float flag (transitively through assign chains and call
 * return types). Uses IR structure only, so it is safe in eligibility (no
 * function context). Conservative: returns 0 when it cannot tell. */
static int mir_temp_is_float(CodeGenerator *g, IRFunction *function,
                             const char *name, int depth) {
  if (!name || depth > 16) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name ||
        strcmp(in->dest.name, name) != 0) {
      continue;
    }
    if (in->is_float) {
      /* A comparison's is_float flag describes its OPERANDS; the RESULT is an
       * integer 0/1 (ucomis + setcc / fused jcc), so it is a GP value and is
       * fine as a branch condition. */
      if (in->op == IR_OP_BINARY && in->text && mir_is_comparison(in->text)) {
        return 0;
      }
      return 1;
    }
    if (in->op == IR_OP_ASSIGN && in->lhs.kind == IR_OPERAND_TEMP) {
      return mir_temp_is_float(g, function, in->lhs.name, depth + 1);
    }
    if (in->op == IR_OP_CALL && in->text && g->ir_program) {
      const CgSym *callee = code_generator_lookup_symbol(g, in->text);
      if (callee && callee->kind == CG_SYM_FUNCTION) {
        return code_generator_binary_resolved_type_float_bits(
                   callee->data.function.return_type) != 0;
      }
    }
    if (in->op == IR_OP_CALL_INDIRECT && g->ir_program &&
        in->lhs.kind == IR_OPERAND_SYMBOL && in->lhs.name) {
      MtlcType *ft = mir_local_or_param_type(g, function, in->lhs.name, NULL);
      const CgSym *callee = ft ? NULL : code_generator_lookup_symbol(g,
                                                       in->lhs.name);
      if (ft && ft->kind != MTLC_TYPE_FUNCTION_POINTER) {
        ft = NULL;
      }
      if (!ft) {
        ft = (callee && callee->type &&
              callee->type->kind == MTLC_TYPE_FUNCTION_POINTER)
                 ? callee->type
                 : NULL;
      }
      return code_generator_binary_resolved_type_float_bits(
                 ft ? ft->fn_return_type : NULL) != 0;
    }
    return 0;
  }
  return 0;
}

/* A direct call MIR can lower: a known function, <=4 register arguments all of
 * GP-scalar type (float args are deferred), a non-INDIRECT (register) return,
 * and simple argument/destination operands. */
static void mir_call_trace(const char *sub) {
  if (mir_env_trace()) {
    fprintf(stderr, "MIR-CALLBAIL\t%s\n", sub);
  }
}

static void mir_call_trace_named(const char *sub, const char *name) {
  if (mir_env_trace()) {
    fprintf(stderr, "MIR-CALLBAIL\t%s\t%s\n", sub, name ? name : "?");
  }
}

/* The runtime abort traps the compiler injects for failed safety checks
 * (bounds, overflow, null, ...). They never return (puts+exit / handler abort),
 * so MIR can lower them as a self-contained terminal sequence. */
static int mir_call_is_runtime_trap(const IRInstruction *in) {
  return in->text && (strcmp(in->text, "mettle_crash_trap_ex") == 0 ||
                      strcmp(in->text, "mettle_crash_trap") == 0);
}

static int mir_call_is_runtime_hook(const IRInstruction *in);
static int mir_runtime_hook_is_supported(const IRInstruction *in);

static int mir_asm_next_binding(const char **cursor, char *name,
                                size_t capacity) {
  const char *at = *cursor ? strchr(*cursor, '{') : NULL;
  while (at) {
    const char *start = at + 1;
    const char *end;
    size_t length;
    while (*start == ' ' || *start == '\t') {
      start++;
    }
    end = start;
    while (*end && *end != '}' && *end != ' ' && *end != '\t') {
      end++;
    }
    length = (size_t)(end - start);
    while (*end == ' ' || *end == '\t') {
      end++;
    }
    if (*end == '}' && length > 0 && length < capacity) {
      memcpy(name, start, length);
      name[length] = '\0';
      *cursor = end + 1;
      return 1;
    }
    at = strchr(at + 1, '{');
  }
  *cursor = NULL;
  return 0;
}

static int mir_name_is_volatile_global_scalar(CodeGenerator *g,
                                              const char *name) {
  return mir_name_is_global_variable(g, name) &&
         mir_global_is_volatile(g, name) &&
         code_generator_binary_symbol_is_scalar_accessible(g, name);
}

static const BinaryGpRegister MIR_SYSCALL_SYSV_REGISTERS[] = {
    BINARY_GP_RDI, BINARY_GP_RSI, BINARY_GP_RDX,
    BINARY_GP_R10, BINARY_GP_R8,  BINARY_GP_R9};
static const BinaryGpRegister MIR_SYSCALL_NT_REGISTERS[] = {
    BINARY_GP_R10, BINARY_GP_RDX, BINARY_GP_R8, BINARY_GP_R9};
#define MIR_SYSCALL_NT_STACK_OFFSET 0x28

static int mir_call_is_syscall(const IRInstruction *in) {
  return in->text && strcmp(in->text, IR_SYSCALL_CALL_NAME) == 0;
}

static int mir_syscall_operand_split(const IRInstruction *in,
                                     const BinaryGpRegister **registers_out,
                                     size_t *register_count_out,
                                     size_t *stacked_out) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  int nt = abi->shadow_space_size > 0;
  size_t register_count =
      nt ? sizeof(MIR_SYSCALL_NT_REGISTERS) / sizeof(*MIR_SYSCALL_NT_REGISTERS)
         : sizeof(MIR_SYSCALL_SYSV_REGISTERS) /
               sizeof(*MIR_SYSCALL_SYSV_REGISTERS);
  size_t arguments = 0;

  if (in->argument_count == 0) {
    return 0;
  }
  arguments = in->argument_count - 1;
  if (arguments > register_count && !nt) {
    return 0;
  }
  *registers_out = nt ? MIR_SYSCALL_NT_REGISTERS : MIR_SYSCALL_SYSV_REGISTERS;
  *register_count_out = register_count;
  *stacked_out = arguments > register_count ? arguments - register_count : 0;
  return 1;
}

/* The zero-fill lowering emits for an aggregate local declared without an
 * initializer. It names memset, which the call lowering turns into an inline
 * rep stos rather than a call, so nothing about it needs a declared callee --
 * and requiring one would drop every function holding an uninitialized struct
 * off the register-allocating backend. A user's own memset comes with an
 * `extern fn` declaration and takes the ordinary known-callee path. */
static int mir_call_is_inline_zero_fill(const IRInstruction *in) {
  size_t a = 0;

  if (!in->text || strcmp(in->text, "memset") != 0 ||
      in->argument_count != 3 || !in->arguments ||
      in->dest.kind != IR_OPERAND_NONE) {
    return 0;
  }
  for (a = 0; a < 3; a++) {
    if (in->arguments[a].kind != IR_OPERAND_TEMP &&
        in->arguments[a].kind != IR_OPERAND_SYMBOL &&
        in->arguments[a].kind != IR_OPERAND_INT) {
      return 0;
    }
  }
  return 1;
}

static int mir_arg_float_bits(CodeGenerator *g, const IRFunction *ir_function,
                              const IROperand *op);

static MtlcType *mir_indirect_call_type(CodeGenerator *g,
                                    const IRFunction *ir_function,
                                    const IRInstruction *in) {
  if (!g || !in || in->lhs.kind != IR_OPERAND_SYMBOL || !in->lhs.name) {
    return NULL;
  }
  MtlcType *local = mir_local_or_param_type(g, ir_function, in->lhs.name, NULL);
  if (local && local->kind == MTLC_TYPE_FUNCTION_POINTER) {
    return local;
  }
  const CgSym *sym = g->ir_program ? code_generator_lookup_symbol(g,
                                                      in->lhs.name)
                                : NULL;
  return (sym && sym->type && sym->type->kind == MTLC_TYPE_FUNCTION_POINTER)
             ? sym->type
             : NULL;
}

static IRFunction *mir_find_ir_function_named(CodeGenerator *g,
                                              const char *name) {
  if (!g || !name || !name[0]) {
    return NULL;
  }
  IRFunction *f = code_generator_find_ir_function_binary(g, name);
  if (!f && name[0] == '@') {
    f = code_generator_find_ir_function_binary(g, name + 1);
  }
  return f;
}

static int mir_call_indirect_is_supported(CodeGenerator *g,
                                          const IRFunction *ir_function,
                                          const IRInstruction *in) {
  if (!in ||
      (in->lhs.kind != IR_OPERAND_SYMBOL && in->lhs.kind != IR_OPERAND_TEMP) ||
      !in->lhs.name) {
    mir_call_trace("indirect_no_symbol");
    return 0;
  }
  if (in->argument_count > MIR_MAX_PARAMS) {
    mir_call_trace("indirect_args>max");
    return 0;
  }
  MtlcType *ft = mir_indirect_call_type(g, ir_function, in);
  if (!ft) {
    /* Callee through a TEMP (closure thunks): the fn-ptr type is unknown, so
     * classify every argument as GP -- exactly what the fallback does with a
     * NULL callee type. Anything float-valued or aggregate-valued must defer,
     * since the GP marshalling can't carry it. */
    if (in->lhs.kind != IR_OPERAND_TEMP ||
        mir_temp_is_float(g, (IRFunction *)ir_function, in->lhs.name, 0)) {
      mir_call_trace("indirect_no_type");
      return 0;
    }
    for (size_t a = 0; a < in->argument_count; a++) {
      const IROperand *arg = &in->arguments[a];
      if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
          arg->kind != IR_OPERAND_INT) {
        mir_call_trace("indirect_untyped_arg_kind");
        return 0;
      }
      if (mir_arg_float_bits(g, ir_function, arg) != 0 ||
          (arg->kind == IR_OPERAND_TEMP &&
           mir_temp_is_float(g, (IRFunction *)ir_function, arg->name, 0))) {
        mir_call_trace("indirect_untyped_arg_float");
        return 0;
      }
      if (arg->kind == IR_OPERAND_SYMBOL && arg->name &&
          (mir_name_is_global_aggregate(g, ir_function, arg->name) ||
           mir_name_is_indirect_aggregate(g, ir_function, arg->name))) {
        mir_call_trace("indirect_untyped_arg_aggregate");
        return 0;
      }
    }
    if (in->dest.kind != IR_OPERAND_NONE && in->dest.kind != IR_OPERAND_TEMP &&
        in->dest.kind != IR_OPERAND_SYMBOL) {
      mir_call_trace("indirect_dest_kind");
      return 0;
    }
    return 1;
  }
  if (in->argument_count != ft->fn_param_count) {
    mir_call_trace("indirect_arity_mismatch");
    return 0;
  }
  MtlcType *ret = ft->fn_return_type;
  if (!code_generator_binary_resolved_type_is_abi_supported(ret, 1)) {
    mir_call_trace("indirect_ret_unsupported");
    return 0;
  }
  if (ret && (code_generator_type_is_aggregate(ret) ||
              code_generator_binary_type_is_string(ret))) {
    mir_call_trace("indirect_ret_aggregate");
    return 0;
  }
  if (in->dest.kind != IR_OPERAND_NONE && in->dest.kind != IR_OPERAND_TEMP &&
      in->dest.kind != IR_OPERAND_SYMBOL) {
    mir_call_trace("indirect_dest_kind");
    return 0;
  }

  const BinaryAbi *call_abi = code_generator_binary_active_abi();
  size_t indirect_float_slot = 0;
  for (size_t a = 0; a < in->argument_count; a++) {
    MtlcType *pt = ft->fn_param_types ? ft->fn_param_types[a] : NULL;
    const IROperand *arg = &in->arguments[a];
    if (!pt) {
      mir_call_trace("indirect_arg_no_type");
      return 0;
    }
    if (!code_generator_binary_resolved_type_is_abi_supported(pt, 0)) {
      mir_call_trace("indirect_arg_unsupported");
      return 0;
    }
    if (code_generator_type_is_aggregate(pt) ||
        code_generator_binary_type_is_string(pt) ||
        code_generator_abi_classify(pt) == ABI_PASS_INDIRECT) {
      mir_call_trace("indirect_arg_aggregate");
      return 0;
    }
    if (code_generator_binary_resolved_type_float_bits(pt) != 0) {
      size_t slot = call_abi && call_abi->counts_classes_separately
                        ? indirect_float_slot++
                        : a;
      if (call_abi && call_abi->float_param_registers &&
          slot < call_abi->float_param_count &&
          mir_xmm_is_encoder_scratch(call_abi->float_param_registers[slot])) {
        mir_call_trace("indirect_arg_float_scratch_register");
        return 0;
      }
      if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
          arg->kind != IR_OPERAND_INT && arg->kind != IR_OPERAND_FLOAT) {
        mir_call_trace("indirect_arg_float_operand_kind");
        return 0;
      }
      if (arg->kind == IR_OPERAND_SYMBOL && arg->name &&
          mir_name_is_global_aggregate(g, ir_function, arg->name)) {
        mir_call_trace("indirect_arg_aggregate_value");
        return 0;
      }
      continue;
    }
    if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
        arg->kind != IR_OPERAND_INT && arg->kind != IR_OPERAND_STRING) {
      mir_call_trace("indirect_arg_operand_kind");
      return 0;
    }
    if (arg->kind == IR_OPERAND_SYMBOL && arg->name &&
        mir_name_is_global_aggregate(g, ir_function, arg->name)) {
      mir_call_trace("indirect_arg_aggregate_value");
      return 0;
    }
    if (arg->kind == IR_OPERAND_STRING &&
        !code_generator_binary_type_is_cstring(pt)) {
      mir_call_trace("indirect_arg_string_non_cstring");
      return 0;
    }
  }
  return 1;
}

/* Classify an IR_OP_ADDRESS_OF target. */
typedef enum {
  MIR_ADDROF_UNSUPPORTED = 0, /* function/string/other: deferred */
  MIR_ADDROF_LOCAL,           /* scalar/DIRECT-agg local or parameter (lea home) */
  MIR_ADDROF_GLOBAL,          /* scalar global (lea RIP-relative) */
  MIR_ADDROF_FUNCTION,        /* function symbol (lea code address) */
  MIR_ADDROF_INDIRECT_PARAM   /* INDIRECT-aggregate param: &@p IS the by-ref
                                 pointer, so copy the param value (no home) */
} MirAddrofKind;

static MirAddrofKind mir_addressof_kind(CodeGenerator *g,
                                        const IRFunction *ir_function,
                                        const IRInstruction *in) {
  if (in->lhs.kind != IR_OPERAND_SYMBOL || !in->lhs.name) {
    return MIR_ADDROF_UNSUPPORTED;
  }
  const CgSym *sym = g && g->ir_program
                    ? code_generator_lookup_symbol(g, in->lhs.name)
                    : NULL;
  if ((sym && sym->kind == CG_SYM_FUNCTION) ||
      mir_find_ir_function_named(g, in->lhs.name)) {
    return MIR_ADDROF_FUNCTION;
  }
  /* Resolve the target as a local/parameter of this function from the IR (the
   * symbol table has popped function scope by codegen time, so a direct lookup
   * fails for locals/params). A NULL type means the name is a global/external. */
  int is_param = 0;
  MtlcType *t = mir_local_or_param_type(g, ir_function, in->lhs.name, &is_param);
  if (!t) {
    /* Not a local/param: a global (or extern). Any global's address is a
     * RIP-relative LEA. A scalar one is additionally register-cached, so the
     * flush/reload around pointer ops keeps cache and memory coherent; an
     * aggregate is never cached, leaving memory authoritative on its own. */
    return mir_name_is_global_variable(g, in->lhs.name) ? MIR_ADDROF_GLOBAL
                                                        : MIR_ADDROF_UNSUPPORTED;
  }
  /* An INDIRECT-aggregate parameter is passed by reference: the parameter value
   * already IS the struct's address, so &@p just yields that pointer. `string`
   * is an aggregate on exactly a struct's terms ({chars,length}, 16 bytes,
   * INDIRECT), so string params and locals take these same two paths. */
  if (is_param && code_generator_type_is_aggregate(t) &&
      code_generator_abi_classify(t) == ABI_PASS_INDIRECT) {
    return MIR_ADDROF_INDIRECT_PARAM;
  }
  return MIR_ADDROF_LOCAL; /* scalar/DIRECT/INDIRECT-agg local or param: lea home */
}

/* Float bit-width (32/64) of a call-argument value operand for the eligibility
 * gate, which (unlike lowering) has no BinaryFunctionContext. Uses the operand's
 * own float_bits tag and the symbol table only, the same signals lowering's
 * code_generator_binary_operand_float_bits treats as authoritative (it returns
 * the operand's float_bits first), so the gate and lowering agree on which args
 * are float. Returns 0 for a non-float or undeterminable operand (gate defers). */
static int mir_arg_float_bits(CodeGenerator *g, const IRFunction *ir_function,
                              const IROperand *op) {
  if (!op) {
    return 0;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    return op->float_bits == 32 ? 32 : 64;
  }
  if (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) {
    if (op->float_bits == 32 || op->float_bits == 64) {
      return op->float_bits;
    }
    if (op->kind == IR_OPERAND_SYMBOL && op->name) {
      /* A float LOCAL or PARAMETER: resolve its declared type from the IR (the
       * symbol table holds only globals at codegen time, scope having been
       * popped). This is exactly the type lowering's mir_value_operand will see,
       * so the gate and the homing agree on which args are float. */
      MtlcType *lt = mir_local_or_param_type(g, ir_function, op->name, NULL);
      if (lt) {
        return code_generator_binary_resolved_type_float_bits(lt);
      }
      if (g && g->ir_program) {
        const CgSym *s = code_generator_lookup_symbol(g, op->name);
        if (s) {
          return code_generator_binary_resolved_type_float_bits(s->type);
        }
      }
    }
  }
  return 0;
}

/* SysV hands an aggregate of 16 bytes or less back in registers rather than
 * through a hidden out-pointer. MIR spills the eightbytes into the destination
 * struct's home itself, which needs both of them to be INTEGER class -- an SSE
 * eightbyte would have to cross banks on the way to memory, and no call the
 * standard library makes returns one. Mirrors the fallback emitter's
 * return_in_sysv_registers so the two agree on which calls take a hidden
 * pointer. */
static int mir_call_sysv_returns_in_gp_registers(CodeGenerator *g,
                                                 const char *callee_name,
                                                 MtlcType *ret,
                                                 BinarySysvAggregate *out) {
  BinarySysvAggregate agg;
  size_t e = 0;
  if (!out) {
    out = &agg;
  }
  if (!callee_name ||
      !code_generator_binary_active_abi()->counts_classes_separately ||
      !code_generator_binary_function_is_abi_public(g, callee_name)) {
    return 0;
  }
  if (!code_generator_binary_classify_sysv_aggregate(ret, out) ||
      out->in_memory || out->eightbyte_count == 0 ||
      out->eightbyte_count > 2) {
    return 0;
  }
  for (e = 0; e < out->eightbyte_count; e++) {
    if (out->classes[e] != BINARY_EIGHTBYTE_INTEGER) {
      return 0;
    }
  }
  return 1;
}

static const char *mir_operand_kind_name(int kind) {
  switch (kind) {
  case IR_OPERAND_NONE: return "none";
  case IR_OPERAND_TEMP: return "temp";
  case IR_OPERAND_SYMBOL: return "symbol";
  case IR_OPERAND_INT: return "int";
  case IR_OPERAND_FLOAT: return "float";
  case IR_OPERAND_STRING: return "string";
  case IR_OPERAND_LABEL: return "label";
  default: return "?";
  }
}

static int mir_call_is_supported(CodeGenerator *g,
                                 const IRFunction *ir_function,
                                 const IRInstruction *in) {
  if (!in->text || in->text[0] == '\0') {
    mir_call_trace("no_name");
    return 0;
  }
  /* Runtime safety-check traps are terminal and lowered specially (MIR_TRAP),
   * so they bypass the normal known-function / argument-shape requirements. */
  if (mir_call_is_runtime_trap(in)) {
    if (g->generate_stack_trace_support) {
      int is_ex = strcmp(in->text, "mettle_crash_trap_ex") == 0;
      if (is_ex) {
        for (size_t a = 2; a < in->argument_count && a < 4; a++) {
          int kind = in->arguments[a].kind;
          if (kind != IR_OPERAND_INT && kind != IR_OPERAND_TEMP &&
              kind != IR_OPERAND_SYMBOL) {
            mir_call_trace("trap_detail_kind");
            return 0;
          }
        }
      } else if (in->argument_count < 1 ||
                 in->arguments[0].kind != IR_OPERAND_STRING) {
        mir_call_trace("trap_message_kind");
        return 0;
      }
    }
    return 1;
  }
  if (mir_call_is_runtime_hook(in)) {
    return mir_runtime_hook_is_supported(in);
  }
  if (mir_call_is_inline_zero_fill(in)) {
    return 1;
  }
  if (mir_call_is_syscall(in)) {
    const BinaryGpRegister *registers = NULL;
    size_t register_count = 0;
    size_t stacked = 0;
    if (!mir_syscall_operand_split(in, &registers, &register_count, &stacked)) {
      mir_call_trace("syscall_args>max");
      return 0;
    }
    for (size_t a = 0; a < in->argument_count; a++) {
      if (in->arguments[a].kind == IR_OPERAND_STRING) {
        mir_call_trace("syscall_string_operand");
        return 0;
      }
    }
    return 1;
  }
  if (in->argument_count > MIR_MAX_PARAMS) {
    mir_call_trace("args>max");
    return 0;
  }
  const CgSym *callee =
      g->ir_program ? code_generator_lookup_symbol(g, in->text) : NULL;
  if (!callee || callee->kind != CG_SYM_FUNCTION) {
    mir_call_trace_named("not_known_function", in->text);
    return 0;
  }
  const MtlcType *ret = callee->data.function.return_type
                  ? callee->data.function.return_type
                  : callee->type;
  /* An aggregate crossing to a foreign function on SysV has to be classified
   * into eightbytes: 16 bytes or less travels in registers, and a float-only
   * eightbyte travels in an XMM. MIR knows only the Microsoft rule, so those
   * calls go to the baseline emitter, which does classify them. Calls between
   * Mettle functions keep the fast path: both sides agree on the convention
   * whatever it is, and `string` is a 16-byte aggregate that would otherwise
   * drag most of the standard library off MIR. */
  int sysv_gp_return = 0;
  {
    const BinaryAbi *sysv_probe = code_generator_binary_active_abi();
    if (sysv_probe->counts_classes_separately &&
        code_generator_binary_function_is_abi_public(g, in->text)) {
      size_t a = 0;
      if (ret && code_generator_type_is_aggregate(ret)) {
        /* A 16-byte-or-less aggregate comes back in RAX/RDX; MIR spills it
         * into the destination's home after the call. Anything larger, or
         * carrying a float eightbyte, still goes to the baseline emitter. */
        if (!mir_call_sysv_returns_in_gp_registers(g, in->text, ret, NULL) ||
            mir_operand_struct_home_size(g, ir_function, &in->dest) == 0) {
          mir_call_trace("sysv_extern_aggregate_ret");
          return 0;
        }
        sysv_gp_return = 1;
      }
      for (a = 0; a < in->argument_count; a++) {
        MtlcType *pt = callee->data.function.parameter_types
                           ? callee->data.function.parameter_types[a]
                           : NULL;
        if (pt && code_generator_type_is_aggregate(pt)) {
          mir_call_trace("sysv_extern_aggregate_arg");
          return 0;
        }
      }
    }
  }

  int hidden = 0;
  if (!sysv_gp_return && ret &&
      code_generator_abi_classify(ret) == ABI_PASS_INDIRECT) {
    /* struct-by-value return: the caller passes a hidden out-pointer as the
     * first integer arg, pointed at the destination struct's home (a struct
     * LOCAL or a struct TEMP), so the callee writes the result directly there. */
    if (mir_operand_struct_home_size(g, ir_function, &in->dest) == 0) {
      mir_call_trace("ret_indirect");
      return 0;
    }
    hidden = 1; /* hidden out-pointer occupies the first positional ABI slot */
  }
  if (callee->data.function.parameter_count != in->argument_count) {
    mir_call_trace("arity_mismatch");
    return 0; /* variadic / arity mismatch: not yet */
  }
  /* The positional ABI slot count that lands in a register (Win64: 4 shared
   * int/float; SysV draws int and float from separate larger pools). Float args
   * are homed only when they fall in an XMM register; a float STACK arg (5th+
   * positional under Win64) is still deferred to the fallback. */
  const BinaryAbi *call_abi = code_generator_binary_active_abi();
  size_t float_slot = 0;
  for (size_t a = 0; a < in->argument_count; a++) {
    MtlcType *pt = callee->data.function.parameter_types
                   ? callee->data.function.parameter_types[a]
                   : NULL;
    const IROperand *arg = &in->arguments[a];
    if (!pt) {
      mir_call_trace("arg_no_type");
      return 0;
    }
    if (code_generator_binary_resolved_type_float_bits(pt) != 0) {
      /* XMM4/XMM5 are the encoder's float scratch. Where the convention also
       * makes them argument registers -- SysV's fifth and sixth float -- the
       * value staged for a later argument lands in the register an earlier one
       * was already marshalled into, so such a call stays on the baseline. */
      size_t slot = call_abi && call_abi->counts_classes_separately
                        ? float_slot++
                        : a + (size_t)hidden;
      if (call_abi && call_abi->float_param_registers &&
          slot < call_abi->float_param_count &&
          mir_xmm_is_encoder_scratch(call_abi->float_param_registers[slot])) {
        mir_call_trace("arg_float_scratch_register");
        return 0;
      }
      /* Float parameter: any scalar source works (literals fold, float values
       * width-convert, int values cvtsi2sd -- coerce_float_operand). */
      if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
          arg->kind != IR_OPERAND_INT && arg->kind != IR_OPERAND_FLOAT) {
        mir_call_trace("arg_float_operand_kind");
        return 0;
      }
      if (arg->kind == IR_OPERAND_SYMBOL && arg->name &&
          mir_name_is_global_aggregate(g, ir_function, arg->name)) {
        mir_call_trace("arg_aggregate_scalar_param");
        return 0;
      }
      continue;
    }
    if (code_generator_abi_classify(pt) == ABI_PASS_INDIRECT) {
      /* struct passed BY VALUE: the caller copies it to an outgoing temp and
       * passes the temp's address. The copy can source from a struct LOCAL or
       * TEMP's home, a by-ref param's pointer, or a string literal's .rdata
       * record. */
      if (!mir_indirect_source_is_supported(g, ir_function, arg)) {
        mir_call_trace("arg_struct_nonlocal");
        return 0;
      }
      continue;
    }
    if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
        arg->kind != IR_OPERAND_INT && arg->kind != IR_OPERAND_STRING &&
        arg->kind != IR_OPERAND_FLOAT) {
      mir_call_trace_named("arg_operand_kind",
                           mir_operand_kind_name((int)arg->kind));
      return 0;
    }
    /* A global AGGREGATE has no cached value vreg; it may only feed an
     * INDIRECT param (handled above, by address). Reaching here with one
     * means a scalar param, which the value path cannot serve. */
    if (arg->kind == IR_OPERAND_SYMBOL && arg->name &&
        mir_name_is_global_aggregate(g, ir_function, arg->name)) {
      mir_call_trace("arg_aggregate_scalar_param");
      return 0;
    }
    if (arg->kind == IR_OPERAND_STRING &&
        !code_generator_binary_type_is_cstring(pt)) {
      /* A string literal is only lowered to a bare cstring (char* in one GP
       * register) when the parameter is itself a cstring, matching the fallback
       * emitter (emit_call_argument_load). A `string` fat-pointer parameter
       * ({chars,length}) needs the struct ABI, which MIR does not build yet. */
      mir_call_trace("arg_string_non_cstring");
      return 0;
    }
  }
  if (in->dest.kind != IR_OPERAND_NONE && in->dest.kind != IR_OPERAND_TEMP &&
      in->dest.kind != IR_OPERAND_SYMBOL) {
    mir_call_trace("dest_kind");
    return 0;
  }
  return 1;
}

int mir_rewrite_string_concat_calls(IRFunction *ir_function) {
  if (!ir_function) {
    return 1;
  }
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    IRInstruction *in = &ir_function->instructions[i];
    if (in->op != IR_OP_BINARY || !in->text || strcmp(in->text, "+") != 0 ||
        !in->value_type || in->value_type->kind != MTLC_TYPE_STRING ||
        in->arguments) {
      continue;
    }
    IROperand *args = calloc(2, sizeof(*args));
    MtlcType **types = calloc(2, sizeof(*types));
    char *name = mettle_strdup("mettle_string_concat");
    if (!args || !types || !name) {
      free(args);
      free(types);
      mettle_free_string(name);
      return 0;
    }
    args[0] = in->lhs;
    args[1] = in->rhs;
    types[0] = in->value_type;
    types[1] = in->value_type;
    in->lhs = ir_operand_none();
    in->rhs = ir_operand_none();
    mettle_free_string(in->text);
    in->text = name;
    in->arguments = args;
    in->argument_types = types;
    in->argument_count = 2;
    in->op = IR_OP_CALL;
  }
  return 1;
}

/* Pure-ish scan: returns 1 if every instruction is in the supported set and the
 * signature is GP-only. Uses generator for type queries; no MIR built yet. */
static int mir_gate_control(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_JUMP:
    break;
  case IR_OP_DECLARE_LOCAL:
    /* A DIRECT small-aggregate local is allowed: field access lowers to
     * &local + offset + LOAD/STORE (all supported), and when its address is
     * taken it becomes memory-resident with an 8-byte home covering it. An
     * INDIRECT struct local is also allowed: it gets a multi-slot home sized
     * to the whole struct (home_bytes), and the same &local + offset + memory
     * machinery reaches every field. Whole-struct by-name uses of it are
     * rejected by the guard below, so only field access ever touches it. */
    if (in->text && !mir_type_is_mir_value(generator, in->text) &&
        !mir_type_is_indirect_aggregate(generator, in->text)) {
      return mir_trace_bail(ir_function, "declare_local:nonscalar");
    }
    break;
  case IR_OP_BRANCH_ZERO:
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, "branch_zero:operand_kind");
    }
    /* branch_zero on a float value (e.g. errdefer on a float return) needs a
     * float-zero compare; float branches are deferred -> fall back. */
    break;
    break;
  case IR_OP_BRANCH_EQ: {
    /* if (lhs == rhs) goto label: integer equality (switch/match dispatch).
     * Both operands must be register-resident or an int literal; float
     * equality would need ucomis, so defer it. */
    const IROperand *eq[2] = {&in->lhs, &in->rhs};
    for (int k = 0; k < 2; k++) {
      if (eq[k]->kind != IR_OPERAND_TEMP && eq[k]->kind != IR_OPERAND_SYMBOL &&
          eq[k]->kind != IR_OPERAND_INT) {
        return mir_trace_bail(ir_function, "branch_eq:operand_kind");
      }
      if (eq[k]->kind == IR_OPERAND_TEMP &&
          mir_temp_is_float(generator, ir_function, eq[k]->name, 0)) {
        return mir_trace_bail(ir_function, "branch_eq:float");
      }
    }
    if (in->is_float) {
      return mir_trace_bail(ir_function, "branch_eq:float");
    }
    break;
  }
  case IR_OP_INLINE_ASM: {
    const char *cursor = in->text;
    char name[128];
    int count = 0;
    while (mir_asm_next_binding(&cursor, name, sizeof(name))) {
      if (mir_local_or_param_type(generator, ir_function, name, NULL)) {
        if (++count > MIR_ASM_MAX_BINDS) {
          return mir_trace_bail(ir_function, "asm:bindings>max");
        }
        continue;
      }
      if (!mir_name_is_global_variable(generator, name)) {
        return mir_trace_bail(ir_function, "asm:binding");
      }
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_arith(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_BINARY: {
    MirOpcode tmp;
    if (!in->text) {
      return mir_trace_bail(ir_function, "binary:no_text");
    }
    /* String '+' is the concat kernel; only the fallback emitter has it. */
    if (in->value_type && in->value_type->kind == MTLC_TYPE_STRING) {
      return mir_trace_bail(ir_function, "binary:string");
    }
    if (in->is_float) {
      /* Float arithmetic, or an ordered float comparison (<,<=,>,>=). */
      int sw;
      unsigned char fcc;
      if (!mir_float_arith_opcode(in->text, &tmp) &&
          !mir_float_cmp_info(in->text, 0, &sw, &fcc)) {
        return mir_trace_bail(ir_function, "binary:float_op");
      }
    } else if (!mir_arith_opcode(in->text, &tmp) &&
               !mir_is_comparison(in->text) &&
               strcmp(in->text, "/") != 0 && strcmp(in->text, "%") != 0) {
      return mir_trace_bail(ir_function, "binary:other");
    }
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "binary:dest");
    }
    for (int k = 0; k < 2; k++) {
      const IROperand *o = k == 0 ? &in->lhs : &in->rhs;
      if (o->kind != IR_OPERAND_TEMP && o->kind != IR_OPERAND_SYMBOL &&
          o->kind != IR_OPERAND_INT && o->kind != IR_OPERAND_FLOAT) {
        return mir_trace_bail(ir_function, "binary:operand_kind");
      }
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_convert(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_CAST:
    /* Any scalar numeric cast: int<->int, int<->float, float<->float. The
     * direction is resolved from operand types during lowering, which is
     * exhaustive for these, so it cannot fail mid-function. */
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "cast:dest");
    }
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
      return mir_trace_bail(ir_function, "cast:operand_kind");
    }
    break;
  case IR_OP_UNARY:
    /* Integer unary `-`, `~`, `+`, `!`; float unary `-` (negate as 0-x) and
     * `+` (copy). Float `~`/`!` are not valid and popcnt is deferred. */
    if (!in->text) {
      return mir_trace_bail(ir_function, "unary:float_or_unsupported");
    }
    if (in->is_float) {
      if (strcmp(in->text, "-") != 0 && strcmp(in->text, "+") != 0) {
        return mir_trace_bail(ir_function, "unary:float_or_unsupported");
      }
    } else if (strcmp(in->text, "-") != 0 && strcmp(in->text, "~") != 0 &&
               strcmp(in->text, "+") != 0 && strcmp(in->text, "!") != 0 &&
               strcmp(in->text, "popcnt") != 0) {
      return mir_trace_bail(ir_function, "unary:float_or_unsupported");
    }
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return 0;
    }
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
      return mir_trace_bail(ir_function, "unary:operand_kind");
    }
    break;
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_value(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_ASSIGN:
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "assign:dest");
    }
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
      /* `@s <- "lit"`: a string literal into a string local/temp is a
       * 16-byte copy from the literal's .rdata record (MIR_LEA_STRLIT). */
      if (in->lhs.kind == IR_OPERAND_STRING &&
          mir_operand_struct_home_size(generator, ir_function, &in->dest) >
              0) {
        break;
      }
      if (in->lhs.kind == IR_OPERAND_STRING &&
          mir_operand_is_cstring_home(generator, ir_function, &in->dest)) {
        break;
      }
      return mir_trace_bail(ir_function, mir_bail_kind("assign:operand_kind", in->lhs.kind));
    }
    break;
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_memory(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_LOAD:
    /* `%t <- *"literal" [8]` reads the data-pointer field of a string
     * literal's fat struct: the value IS the address of a NUL-terminated
     * .rdata cstring, so it lowers to MIR_LEA_CSTR (the same materialization
     * used for string-literal call arguments). Any other width/shape on a
     * STRING operand is deferred. */
    if (in->lhs.kind == IR_OPERAND_STRING) {
      if (in->is_float || in->rhs.kind != IR_OPERAND_INT ||
          in->rhs.int_value != 8) {
        return mir_trace_bail(ir_function, "load:string_shape");
      }
      if (in->dest.kind != IR_OPERAND_TEMP &&
          in->dest.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(ir_function, "load:dest");
      }
      break;
    }
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, mir_bail_kind("load:address_kind", in->lhs.kind));
    }
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "load:dest");
    }
    break;
  case IR_OP_STORE:
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL &&
        in->dest.kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, mir_bail_kind("store:address_kind", in->dest.kind));
    }
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
      return mir_trace_bail(ir_function, "store:value_kind");
    }
    break;
  case IR_OP_PREFETCH:
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "prefetch:addr");
    }
    break;
  case IR_OP_ROTATE_ADD:
    /* next = a + b; a = b; b = next. Writes lhs and rhs too, which the
     * written-global tracking only covers for dest, so a and b must be
     * locals/params. */
    if (in->dest.kind != IR_OPERAND_SYMBOL ||
        in->lhs.kind != IR_OPERAND_SYMBOL ||
        in->rhs.kind != IR_OPERAND_SYMBOL || in->is_float ||
        !mir_local_or_param_type(generator, ir_function, in->lhs.name,
                                 NULL) ||
        !mir_local_or_param_type(generator, ir_function, in->rhs.name,
                                 NULL)) {
      return mir_trace_bail(ir_function, "rotate_add:operand");
    }
    break;
  case IR_OP_NEW:
    /* Zeroed heap allocation: size is a compile-time INT, absent (defaults
     * to 8), or a runtime GP value; the result pointer lands in a
     * TEMP/SYMBOL. Win64 lowers to the inline GetProcessHeap+HeapAlloc
     * sequence (MIR_HEAP_NEW), SysV to a plain calloc call. */
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "new:dest");
    }
    if (in->rhs.kind != IR_OPERAND_NONE && in->rhs.kind != IR_OPERAND_INT &&
        in->rhs.kind != IR_OPERAND_TEMP && in->rhs.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "new:size");
    }
    break;
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_select(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SELECT: {
    /* dest = (cond != 0) ? then : else. Each of cond/then/else may be a
     * temp/symbol/int; the dest is a temp/symbol. */
    const IROperand *sops[3] = {&in->lhs, &in->rhs,
                                in->argument_count > 0 ? &in->arguments[0]
                                                       : NULL};
    if (!sops[2]) {
      return mir_trace_bail(ir_function, "select:no_else");
    }
    for (int s = 0; s < 3; s++) {
      if (sops[s]->kind != IR_OPERAND_TEMP &&
          sops[s]->kind != IR_OPERAND_SYMBOL &&
          sops[s]->kind != IR_OPERAND_INT) {
        return mir_trace_bail(ir_function, "select:operand_kind");
      }
    }
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "select:dest_kind");
    }
    break;
  }
  case IR_OP_RETURN:
    if (in->lhs.kind == IR_OPERAND_STRING) {
      int literal_ok =
          (mir_type_is_indirect_aggregate(generator,
                                          ir_function->return_type_name) &&
           mir_indirect_source_is_supported(generator, ir_function,
                                            &in->lhs)) ||
          code_generator_binary_type_is_cstring(
              code_generator_binary_get_resolved_type(
                  generator, ir_function->return_type_name, 1));
      if (!literal_ok) {
        return mir_trace_bail(ir_function, "return:string_literal");
      }
      break;
    }
    if (in->lhs.kind != IR_OPERAND_NONE && in->lhs.kind != IR_OPERAND_TEMP &&
        in->lhs.kind != IR_OPERAND_SYMBOL && in->lhs.kind != IR_OPERAND_INT &&
        in->lhs.kind != IR_OPERAND_FLOAT) {
      return mir_trace_bail(ir_function, "return:operand_kind");
    }
    /* An INDIRECT-returning function returns anything the indirect-copy
     * machinery can source: a struct LOCAL or TEMP home (a call result
     * lands in the temp's home via the hidden pointer), a by-ref param's
     * pointee, a global aggregate, or a string literal. */
    if (mir_type_is_indirect_aggregate(generator,
                                       ir_function->return_type_name) &&
        !mir_indirect_source_is_supported(generator, ir_function, &in->lhs)) {
      return mir_trace_bail(ir_function, "return:indirect_nonlocal");
    }
    break;
  case IR_OP_CALL:
    if (!mir_call_is_supported(generator, ir_function, in)) {
      return mir_trace_bail(ir_function, "call_unsupported");
    }
    break;
  case IR_OP_CALL_INDIRECT:
    if (!mir_call_indirect_is_supported(generator, ir_function, in)) {
      return mir_trace_bail(ir_function, "call_indirect_unsupported");
    }
    break;
  case IR_OP_ADDRESS_OF:
    /* &local/&param (made memory-resident via forced spill) or &global (kept
     * cached but coherent via flush/reload around pointer memory ops).
     * Functions/strings have their own address forms and are deferred. */
    if (mir_addressof_kind(generator, ir_function, in) ==
        MIR_ADDROF_UNSUPPORTED) {
      return mir_trace_bail(ir_function, "addressof:unsupported");
    }
    if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "addressof:dest");
    }
    break;
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_mac(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SLP_MAC_I32: {
    /* SLP MAC kernel run INLINE inside the MIR function (so the surrounding
     * outer loops keep register-allocated codegen). The lowering marshals the
     * three base pointers + count + byte stride into RCX/RDX/R8/R9/RAX, so the
     * only compile-time-constant requirement is the lane count K (it selects
     * the xmm-vs-ymm kernel width); the bases, offsets, count, and stride may
     * each be a runtime temp/symbol resolved via mir_value_operand. The I8
     * variant (int8 a/b, int32 c) uses the same shape with different element
     * scaling, handled in lowering. */
    if (in->argument_count < 6 || !in->arguments ||
        in->arguments[0].kind != IR_OPERAND_INT ||
        (in->arguments[0].int_value != 4 &&
         in->arguments[0].int_value != 8)) {
      return mir_trace_bail(ir_function, "slp_mac:nonconst_K");
    }
    const IROperand *bases[3] = {&in->dest, &in->lhs, &in->rhs};
    for (int k = 0; k < 3; k++) {
      if (bases[k]->kind != IR_OPERAND_TEMP &&
          bases[k]->kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(ir_function, "slp_mac:base_kind");
      }
    }
    /* count, a_off, b_off, b_stride, out_off */
    const int run_args[5] = {1, 2, 3, 4, 5};
    for (int k = 0; k < 5; k++) {
      const IROperand *o = &in->arguments[run_args[k]];
      if (o->kind != IR_OPERAND_TEMP && o->kind != IR_OPERAND_SYMBOL &&
          o->kind != IR_OPERAND_INT) {
        return mir_trace_bail(ir_function, "slp_mac:arg_kind");
      }
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_fill_counter(const IRFunction *ir_function,
                                 const IRInstruction *in,
                                 long long fill_mode) {
  /* Mode 0 must start the induction variable at 0 (a nonzero start adjusts
   * both the base and the count; deferred). A nonzero/runtime OFFSET (the
   * invariant part of `base[offset + i]`) is supported by folding
   * `base + offset*size` in MIR before the kernel, but only for an int64
   * (wide) index so the pointer math is plain 64-bit -- an int32 offset would
   * need the fallback's sign-extension to match exactly. */
  if (fill_mode == 0) {
    int start_zero = (in->arguments[3].kind == IR_OPERAND_INT &&
                      in->arguments[3].int_value == 0);
    int offset_zero = (in->arguments[4].kind == IR_OPERAND_INT &&
                       in->arguments[4].int_value == 0);
    int wide = in->argument_count > 5 &&
               in->arguments[5].kind == IR_OPERAND_INT &&
               in->arguments[5].int_value == 64;
    /* A nonzero start folds into the base and the count; a nonzero offset
     * folds into the base. An int32 start subtracts at 32 bits and
     * sign-extends (matching the fallback's movsxd); combining a narrow
     * start with a runtime offset still defers. */
    if (!start_zero) {
      if (!wide && !offset_zero) {
        return mir_trace_bail(ir_function, "simd_fill:start");
      }
      if (in->arguments[3].kind != IR_OPERAND_TEMP &&
          in->arguments[3].kind != IR_OPERAND_SYMBOL &&
          in->arguments[3].kind != IR_OPERAND_INT) {
        return mir_trace_bail(ir_function, "simd_fill:start");
      }
    }
    if (!offset_zero) {
      if (!wide) {
        return mir_trace_bail(ir_function, "simd_fill:offset_width");
      }
      if (in->arguments[4].kind != IR_OPERAND_TEMP &&
          in->arguments[4].kind != IR_OPERAND_SYMBOL &&
          in->arguments[4].kind != IR_OPERAND_INT) {
        return mir_trace_bail(ir_function, "simd_fill:offset_kind");
      }
    }
  }
  return 1;
}

static int mir_gate_fill_value(CodeGenerator *generator,
                               const IRFunction *ir_function,
                               const IRInstruction *in) {
  /* The fill value: a compile-time INT, or a runtime invariant GP value
   * (mem_fill's splatted word). A float-valued symbol would resolve to an
   * XMM vreg the RAX marshalling cannot take, so it stays deferred. */
  (void)generator;
  if (in->arguments[2].kind != IR_OPERAND_INT &&
      in->arguments[2].kind != IR_OPERAND_FLOAT &&
      in->arguments[2].kind != IR_OPERAND_TEMP &&
      in->arguments[2].kind != IR_OPERAND_SYMBOL) {
    return mir_trace_bail(ir_function,
                          mir_bail_kind("simd_fill:value",
                                        in->arguments[2].kind));
  }
  return 1;
}

static int mir_gate_fill(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_FILL: {
    /* Inline fill passthrough: element-counted (mode 0), begin->end byte
     * walk (mode 1), and byte-offset walk (mode 2, the mem_zero/mem_fill
     * word loop), with a compile-time or runtime-invariant GP value. What
     * still defers: float-valued fills, mode-1 pointer-iv write-backs, and
     * mode-0 nonzero starts. */
    if (in->argument_count < 5 ||
        in->arguments[0].kind != IR_OPERAND_INT ||
        (in->arguments[0].int_value != 1 && in->arguments[0].int_value != 2 &&
         in->arguments[0].int_value != 4 &&
         in->arguments[0].int_value != 8) ||
        in->arguments[1].kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, "simd_fill:shape");
    }
    /* Mode 0 (element-counted), mode 1 (begin->end byte walk), and mode 2
     * (byte-offset walk: the lowering folds base+start and bound-start in
     * 64-bit MIR, and writes the live iv back as start + bytes walked). */
    long long fill_mode = in->arguments[1].int_value;
    if (fill_mode != 0 && fill_mode != 1 && fill_mode != 2) {
      return mir_trace_bail(ir_function, "simd_fill:mode");
    }
    if (fill_mode == 2 && in->arguments[3].kind != IR_OPERAND_TEMP &&
        in->arguments[3].kind != IR_OPERAND_SYMBOL &&
        in->arguments[3].kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, "simd_fill:start");
    }
    if (!mir_gate_fill_counter(ir_function, in, fill_mode)) {
      return 0;
    }
    /* A live induction variable (dest = the iv symbol) needs a final
     * write-back, folded in MIR after the kernel: mode 0 (start 0) leaves
     * iv = max(count, 0); mode 2 leaves iv = start + bytes walked. Either
     * needs the iv to be a LOCAL/PARAM (resolvable to a vreg); mode 1's
     * pointer iv and a global iv stay in the fallback. */
    if (in->dest.kind == IR_OPERAND_SYMBOL) {
      if ((fill_mode != 0 && fill_mode != 2) ||
          !mir_local_or_param_type(generator, ir_function, in->dest.name,
                                   NULL)) {
        return mir_trace_bail(ir_function, "simd_fill:writeback");
      }
    }
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "simd_fill:base");
    }
    if (in->rhs.kind != IR_OPERAND_TEMP && in->rhs.kind != IR_OPERAND_SYMBOL &&
        in->rhs.kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, "simd_fill:count");
    }
    if (!mir_gate_fill_value(generator, ir_function, in)) {
      return 0;
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_affine(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32: {
    /* Inline float affine-map passthrough (`dst[i]=a*src[i]+b*dst[i]+c`, the
     * float-copy/saxpy class). src (lhs) and dst (rhs) must be LEA-able
     * pointers (TEMP/SYMBOL), the count GP-resolvable, and the a/b/c
     * coefficients compile-time FLOAT immediates (so the kernel can bake their
     * broadcasts); a runtime coefficient stays in the fallback. F32 and F64
     * share this validation; the lowering below picks the width. */
    if (in->argument_count < 4 || !in->arguments) {
      return mir_trace_bail(ir_function, "affine_map:shape");
    }
    if ((in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) ||
        (in->rhs.kind != IR_OPERAND_TEMP && in->rhs.kind != IR_OPERAND_SYMBOL)) {
      return mir_trace_bail(ir_function, "affine_map:ptr");
    }
    if (in->arguments[0].kind != IR_OPERAND_TEMP &&
        in->arguments[0].kind != IR_OPERAND_SYMBOL &&
        in->arguments[0].kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, "affine_map:count");
    }
    for (int k = 1; k <= 3; k++) {
      if (in->arguments[k].kind == IR_OPERAND_FLOAT) continue;
      /* F64 additionally accepts a RUNTIME `a` scale (arguments[1]) -- the
       * saxpy `y=a*x+y` shape where a varies per pass; it is marshalled into
       * an xmm and broadcast at runtime. b and c (args 2,3) must stay
       * compile-time so their broadcasts are baked. F32 stays const-only. */
      if (k == 1 && (in->arguments[k].kind == IR_OPERAND_TEMP ||
                     in->arguments[k].kind == IR_OPERAND_SYMBOL)) {
        continue;
      }
      return mir_trace_bail(ir_function, "affine_map:coeff");
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_vloop(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_VLOOP_F64: {
    /* Inline general-vectorized-loop passthrough. Maps marshal <=3 distinct
     * base pointers + count through RCX/RDX/R8/R9; reductions go through
     * the generic kernel bridge (staged frame slots), which also carries
     * their accumulator and any invariant scalars. */
    const char *vnames[4];
    const IROperand *vsrcs[4];
    const int vi32 = (in->op == IR_OP_SIMD_VLOOP_I32);
    int vn = 0;
    if (in->argument_count < 7 || !in->arguments) {
      return mir_trace_bail(ir_function, "vloop:shape");
    }
    if (vi32 ? (in->float_bits != 32 && in->float_bits != 8)
             : (in->float_bits != 64 && in->float_bits != 32)) {
      return mir_trace_bail(ir_function, "vloop:width");
    }
    /* Reductions, scalar-reading DAGs, and 4-base maps run through the
     * generic kernel bridge (staged slots); plain maps with <=3 bases take
     * the marshalled fast path. */
    {
      const int vreduce = in->arguments[0].int_value != 0;
      int bridge = vreduce || in->arguments[5].int_value != 0;
      if (!vreduce) {
        if (code_generator_vloop_collect_dist(in, 0, vnames, vsrcs, &vn) <
            0) {
          return mir_trace_bail(ir_function, "vloop:bases");
        }
        if (vn > 3) {
          bridge = 1;
        }
      }
      if (bridge) {
        int slots = mir_kernel_slot_estimate(in);
        if (slots < 0 || slots > MIR_KERNEL_MAX_SLOTS) {
          return mir_trace_bail(ir_function,
                                vreduce ? "vloop:reduce" : "vloop:scalars");
        }
        break;
      }
    }
    for (int vk = 0; vk < vn; vk++) {
      if (!vsrcs[vk] || (vsrcs[vk]->kind != IR_OPERAND_TEMP &&
                         vsrcs[vk]->kind != IR_OPERAND_SYMBOL)) {
        return mir_trace_bail(ir_function, "vloop:ptr");
      }
    }
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, "vloop:count");
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return 1;
}

static int mir_gate_silu(CodeGenerator *generator,
                       const IRFunction *ir_function,
                       const IRInstruction *in, size_t i,
                       int *handled) {
  (void)generator;
  (void)i;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_SILU_F32: {
    /* Inline SiLU/SwiGLU passthrough. g (lhs) must be a LEA-able pointer, the
     * count GP-resolvable, and (SwiGLU) u (rhs) a pointer too; plain SiLU
     * leaves rhs NONE/"" (no multiply). */
    if (in->argument_count < 1 || !in->arguments ||
        (in->lhs.kind != IR_OPERAND_TEMP &&
         in->lhs.kind != IR_OPERAND_SYMBOL)) {
      return mir_trace_bail(ir_function, "silu:g");
    }
    if (in->arguments[0].kind != IR_OPERAND_TEMP &&
        in->arguments[0].kind != IR_OPERAND_SYMBOL &&
        in->arguments[0].kind != IR_OPERAND_INT) {
      return mir_trace_bail(ir_function, "silu:count");
    }
    if (in->rhs.kind != IR_OPERAND_NONE && in->rhs.kind != IR_OPERAND_STRING &&
        in->rhs.kind != IR_OPERAND_TEMP && in->rhs.kind != IR_OPERAND_SYMBOL) {
      return mir_trace_bail(ir_function, "silu:u");
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return 1;
}


static int mir_gate_inline_kernel(const IRFunction *ir_function,
                                  const IRInstruction *in) {
  /* A kernel in the inline-kernel table runs in place (MIR_IR_KERNEL): it
   * needs no per-opcode gate, only room to stage its by-name operands. */
  if (mir_ir_kernel_for_op(in->op)) {
    int slots = mir_kernel_slot_estimate(in);
    if (slots < 0) {
      return mir_trace_bail(ir_function, "kernel:operand_kind");
    }
    if (slots > MIR_KERNEL_MAX_SLOTS) {
      return mir_trace_bail(ir_function, "kernel:slots");
    }
    return 1;
  }
  /* NEW, ROTATE_ADD, the tensor ops: not yet. */
  char buf[40];
  snprintf(buf, sizeof(buf), "op:%d", (int)in->op);
  return mir_trace_bail(ir_function, buf);
}

static int mir_gate_function_shape(CodeGenerator *generator,
                                   const IRFunction *ir_function) {
  /* Kill switch for bisecting MIR vs legacy regressions. */
  {
    const char *off = mir_env_mir();
    if (off && off[0] == '0') {
      return 0;
    }
    /* Bisect: comma-separated list of function names forced to fallback. */
    const char *skip = mir_env_skipfn();
    if (skip && ir_function->name) {
      const char *nm = ir_function->name;
      size_t nl = strlen(nm);
      const char *p = skip;
      while (*p) {
        const char *c = strchr(p, ',');
        size_t seg = c ? (size_t)(c - p) : strlen(p);
        if (seg == nl && strncmp(p, nm, nl) == 0) {
          return 0;
        }
        if (!c) break;
        p = c + 1;
      }
    }
  }
  /* An `asm` block is written against named stack homes and clobbers whatever
   * registers it likes. The allocated frame has neither, so the whole function
   * goes to the baseline emitter, which keeps every value in its own slot. */
  if (ir_function->is_naked) {
    return mir_trace_bail(ir_function, "naked");
  }
  /* A function reached from outside under SysV takes and returns aggregates by
   * eightbyte. That prologue and that return live in the baseline emitter, so
   * the whole function goes there. */
  if (code_generator_binary_active_abi()->counts_classes_separately &&
      code_generator_binary_function_is_abi_public(generator,
                                                   ir_function->name)) {
    BinarySysvAggregate agg;
    if (code_generator_binary_classify_sysv_aggregate(
            code_generator_binary_get_resolved_type(
                generator, ir_function->return_type_name, 1),
            &agg)) {
      mir_trace_bail(ir_function, "sysv_public_aggregate_ret");
      return 0;
    }
    for (size_t i = 0; i < ir_function->parameter_count; i++) {
      if (code_generator_binary_classify_sysv_aggregate(
              code_generator_binary_get_resolved_type(
                  generator,
                  ir_function->parameter_types
                      ? ir_function->parameter_types[i]
                      : NULL,
                  0),
              &agg)) {
        mir_trace_bail(ir_function, "sysv_public_aggregate_param");
        return 0;
      }
    }
  }
  return 1;
}

static int mir_gate_signature(CodeGenerator *generator,
                              const IRFunction *ir_function) {
  /* Signature: <=4 GP params, GP-or-void return, no indirect return. */
  if (ir_function->parameter_count > MIR_MAX_PARAMS) {
    return mir_trace_bail(ir_function, "sig:params>max");
  }
  {
    int pis_float[MIR_MAX_PARAMS];
    for (size_t i = 0; i < ir_function->parameter_count; i++) {
      const char *pt = ir_function->parameter_types
                           ? ir_function->parameter_types[i]
                           : NULL;
      if (!mir_type_is_param_value(generator, pt)) {
        return mir_trace_bail(ir_function, "sig:param_nonscalar");
      }
      MtlcType *rt = code_generator_binary_get_resolved_type(generator, pt, 0);
      pis_float[i] =
          (rt && code_generator_binary_resolved_type_float_bits(rt) != 0) ? 1 : 0;
    }
    /* GP params beyond the ABI's argument registers are homed from the caller's
     * stack frame (handled below). A FLOAT param landing on the stack is not
     * homed yet, so defer those functions to the fallback. An INDIRECT struct
     * return consumes the first integer argument slot as a hidden out-pointer,
     * shifting every user parameter up by one, model that here so the on-stack
     * detection matches the prologue's homing exactly. */
    int hidden = mir_type_is_indirect_aggregate(generator,
                                                ir_function->return_type_name)
                     ? 1
                     : 0;
    if (ir_function->parameter_count > 0) {
      const BinaryAbi *abi = code_generator_binary_active_abi();
      int aug_float[MIR_MAX_PARAMS + 1];
      BinaryArgLocation locs[MIR_MAX_PARAMS + 1];
      size_t n = ir_function->parameter_count + (size_t)hidden;
      if (n > MIR_MAX_PARAMS) {
        return mir_trace_bail(ir_function, "sig:params>max");
      }
      if (hidden) {
        aug_float[0] = 0; /* hidden out-pointer is an integer arg */
      }
      for (size_t i = 0; i < ir_function->parameter_count; i++) {
        aug_float[i + (size_t)hidden] = pis_float[i];
      }
      if (!code_generator_binary_compute_arg_layout(abi, aug_float, n, locs,
                                                    NULL)) {
        return mir_trace_bail(ir_function, "sig:arg_layout");
      }
    }
  }
  /* A non-void return must be a register value (scalar / DIRECT small agg) OR an
   * INDIRECT aggregate returned via the hidden out-pointer (handled at RETURN). */
  if (ir_function->return_type_name && ir_function->return_type_name[0] &&
      strcmp(ir_function->return_type_name, "void") != 0 &&
      !mir_type_is_mir_value(generator, ir_function->return_type_name) &&
      !mir_type_is_indirect_aggregate(generator, ir_function->return_type_name)) {
    return mir_trace_bail(ir_function, "sig:return_nonscalar");
  }
  return 1;
}

static void mir_scan_global_write(CodeGenerator *generator,
                                  const IRFunction *ir_function,
                                  const IRInstruction *in,
                                  MirNameMap *defined, int *globals_ok,
                                  int *has_global_write, int *gw_overflow,
                                  const char **gw_names,
                                  size_t *gw_count) {
  /* An undefined SYMBOL written here is a global STORE. */
  if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name) {
    int found = 0;
    for (size_t j = 0; j < defined->count; j++) {
      if (strcmp(defined->items[j].name, in->dest.name) == 0) {
        found = 1;
        break;
      }
    }
    /* STORE's dest is an ADDRESS operand: `*@g <- v` stores through a global
     * aggregate's memory (never cached, so it is not a cache write). */
    int agg_addr_dest =
        !found && in->op == IR_OP_STORE &&
        mir_name_is_global_aggregate(generator, ir_function, in->dest.name);
    if (!found && !agg_addr_dest &&
        mir_name_is_volatile_global_scalar(generator, in->dest.name)) {
      return;
    }
    if (!found && !agg_addr_dest &&
        !mir_name_is_global_scalar(generator, in->dest.name)) {
      mir_call_trace_named("global_write", in->dest.name);
      *globals_ok = 0;
      return;
    }
    if (!found && !agg_addr_dest) {
      *has_global_write = 1;
      int seen = 0;
      for (size_t j = 0; j < (*gw_count); j++) {
        if (strcmp(gw_names[j], in->dest.name) == 0) {
          seen = 1;
          break;
        }
      }
      if (!seen) {
        if ((*gw_count) < 64) {
          gw_names[(*gw_count)++] = in->dest.name;
        } else {
          *gw_overflow = 1;
        }
      }
    }
  }
}

static void mir_scan_global_operands(CodeGenerator *generator,
                                     const IRFunction *ir_function,
                                     MirNameMap *defined, int *globals_ok,
                                     int *has_global_write, int *has_call,
                                     int *gw_overflow) {
  const char *gw_names[64];
  size_t gw_count = 0;
  for (size_t i = 0; i < ir_function->instruction_count && *globals_ok; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT) {
      *has_call = 1;
    }
    mir_scan_global_write(generator, ir_function, in, defined, globals_ok,
                          has_global_write, gw_overflow, gw_names,
                          &gw_count);
    /* An undefined SYMBOL read must be a global scalar. */
    const IROperand *reads[2] = {&in->lhs, &in->rhs};
    for (int k = 0; k < 2; k++) {
      if (in->op == IR_OP_ADDRESS_OF && reads[k] == &in->lhs) {
        continue; /* &symbol names an address target, not a by-value read. */
      }
      if (reads[k]->kind == IR_OPERAND_SYMBOL && reads[k]->name) {
        int found = 0;
        for (size_t j = 0; j < defined->count; j++) {
          if (strcmp(defined->items[j].name, reads[k]->name) == 0) {
            found = 1;
            break;
          }
        }
        if (!found && !mir_name_is_global_scalar(generator, reads[k]->name) &&
            !mir_name_is_volatile_global_scalar(generator, reads[k]->name)) {
          /* A global AGGREGATE name is usable in the positions where the
           * lowering materializes its RIP-relative address instead of a
           * cached value: a LOAD/PREFETCH address (`*@g [w]`), or the source
           * of a whole-struct ASSIGN into a LEA-able home. */
          int agg_ok =
              mir_name_is_global_aggregate(generator, ir_function,
                                           reads[k]->name) &&
              (((in->op == IR_OP_LOAD || in->op == IR_OP_PREFETCH) &&
                reads[k] == &in->lhs) ||
               (in->op == IR_OP_ASSIGN && reads[k] == &in->lhs &&
                mir_operand_struct_home_size(generator, ir_function,
                                             &in->dest) > 0));
          if (!agg_ok) {
            mir_call_trace_named("global_read", reads[k]->name);
            *globals_ok = 0;
            break;
          }
        }
      }
    }
    /* Undefined SYMBOL call arguments are global reads too (e.g. f(g)). They
     * must be scalar globals so the entry-load pass can cache them, or global
     * aggregates a CALL copies from by address (mir_call_is_supported vets the
     * parameter match; CALL_INDIRECT rejects aggregate args in its own gate). */
    for (size_t a = 0; a < in->argument_count && *globals_ok; a++) {
      const IROperand *arg = &in->arguments[a];
      if (arg->kind != IR_OPERAND_SYMBOL || !arg->name) {
        continue;
      }
      int found = 0;
      for (size_t j = 0; j < defined->count; j++) {
        if (strcmp(defined->items[j].name, arg->name) == 0) {
          found = 1;
          break;
        }
      }
      if (!found && !mir_name_is_global_scalar(generator, arg->name) &&
          !mir_name_is_volatile_global_scalar(generator, arg->name) &&
          !((in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT) &&
            mir_name_is_global_aggregate(generator, ir_function, arg->name))) {
        mir_call_trace_named("global_arg", arg->name);
        *globals_ok = 0;
        break;
      }
    }
  }
}

static int mir_gate_globals(CodeGenerator *generator,
                            IRFunction *ir_function) {
  /* Collect the names that are defined inside the function: parameters and
   * declared locals. Any SYMBOL operand naming something outside this set is a
   * global (or otherwise externally-defined) value. Those become vregs that no
   * prologue/def ever initializes, so the function is not yet MIR-eligible. */
  MirNameMap defined = {0};
  MirFunction scratch_fn;
  memset(&scratch_fn, 0, sizeof(scratch_fn));
  int globals_ok = 1;
  int has_global_write = 0;
  int has_call = 0;
  int gw_overflow = 0;
  for (size_t i = 0; i < ir_function->parameter_count; i++) {
    if (ir_function->parameter_names[i]) {
      mir_name_map_get_or_add(&defined, &scratch_fn,
                              ir_function->parameter_names[i], 0, MIR_RC_GP,
                              8);
    }
  }
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      mir_name_map_get_or_add(&defined, &scratch_fn, in->dest.name, 0,
                              MIR_RC_GP, 8);
    }
  }
  if (scratch_fn.has_error) {
    mir_name_map_destroy(&defined);
    mir_function_destroy(&scratch_fn);
    return mir_trace_bail(ir_function, "globals:lowering_error");
  }
  /* Any SYMBOL operand not defined in this function is a global access. It is
   * eligible iff it is a plain scalar global (no address-of in scope, an
   * IR_OP_ADDRESS_OF would be rejected below, so no aliasing pointer can reach
   * it). Calls are fine: the lowering flushes written globals before each call
   * and reloads cached globals after, keeping memory authoritative across the
   * call boundary. */
  mir_scan_global_operands(generator, ir_function, &defined, &globals_ok,
                           &has_global_write, &has_call, &gw_overflow);
  mir_name_map_destroy(&defined);
  mir_function_destroy(&scratch_fn);
  if (!globals_ok) {
    return mir_trace_bail(ir_function, "global_access");
  }
  /* Mixing global writes with calls is fine now that the flush before each
   * call/return is flow-sensitive: only globals actually dirtied since the
   * last cleaning point are stored back, so a clean cached value can never
   * stomp another thread's write between a reload and the next call (the
   * lock()/unlock() hazard that used to force these functions to the
   * fallback). The dirty analysis tracks at most 64 written globals; past
   * that it degrades to flush-everything, so such functions stay deferred. */
  if (has_global_write && has_call && gw_overflow) {
    return mir_trace_bail(ir_function, "global_write_with_call");
  }
  return 1;
}

static int mir_gate_indirect_operands(CodeGenerator *generator,
                                      const IRFunction *ir_function,
                                      const IRInstruction *in) {
  const IROperand *whole[3] = {&in->dest, &in->lhs, &in->rhs};
  for (int k = 0; k < 3; k++) {
    const IROperand *o = whole[k];
    if (o->kind != IR_OPERAND_SYMBOL || !o->name ||
        !mir_name_is_indirect_aggregate(generator, ir_function, o->name)) {
      continue;
    }
    int allowed =
        (in->op == IR_OP_DECLARE_LOCAL && o == &in->dest) ||
        (in->op == IR_OP_ADDRESS_OF && o == &in->lhs) ||
        /* RETURN copies from any supported indirect source: a local or
         * temp home, a by-ref param's pointee, or a global aggregate. */
        (in->op == IR_OP_RETURN && o == &in->lhs &&
         mir_indirect_source_is_supported(generator, ir_function,
                                          &in->lhs)) ||
        /* `@local = f()` for a struct-returning callee: the call writes the
         * struct directly into the dest local's home via the hidden return
         * pointer (mir_call_is_supported validates the callee returns
         * INDIRECT). */
        (in->op == IR_OP_CALL && o == &in->dest &&
         mir_name_is_indirect_struct_local(generator, ir_function, o->name)) ||
        /* Whole-struct ASSIGN into a LEA-able struct home (rep-movsb): the
         * source may be another home, a by-ref param (copy through its
         * pointer), or a string literal (copy from its .rdata record). */
        (in->op == IR_OP_ASSIGN && (o == &in->dest || o == &in->lhs) &&
         mir_operand_struct_home_size(generator, ir_function, &in->dest) >
             0 &&
         mir_indirect_source_is_supported(generator, ir_function,
                                          &in->lhs)) ||
        /* An 8-byte string VALUE is a record pointer: storing a string
         * local stores its home's address, storing a by-ref param stores
         * the pointer it holds (both mirror emit_operand_load). */
        (in->op == IR_OP_STORE && o == &in->lhs &&
         in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 8 &&
         (mir_name_is_string_local(generator, ir_function, o->name) ||
          mir_name_is_indirect_param(generator, ir_function, o->name))) ||
        /* `@s <- *addr [8]` with a string dest: the loaded pointer is
         * deref-copied into the local's home (a by-ref param dest just
         * takes the pointer as its new value). */
        (in->op == IR_OP_LOAD && o == &in->dest &&
         in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 8 &&
         (mir_name_is_string_local(generator, ir_function, o->name) ||
          mir_name_is_indirect_param(generator, ir_function, o->name))) ||
        /* `*@s [w]` / `*@s <- v [w]`: the name used as a memory ADDRESS.
         * A string local's address is its home (mir_address_operand leas
         * it); a by-ref param's value already is the address. */
        (((in->op == IR_OP_LOAD && o == &in->lhs) ||
          (in->op == IR_OP_STORE && o == &in->dest)) &&
         (mir_name_is_string_local(generator, ir_function, o->name) ||
          mir_name_is_indirect_param(generator, ir_function, o->name)));
    if (!allowed) {
      return mir_trace_bail(ir_function, "indirect_agg_byname");
    }
  }
  return 1;
}

static int mir_gate_indirect_aggregate(CodeGenerator *generator,
                                       const IRFunction *ir_function,
                                       const IRInstruction *in) {
  /* Whole-struct by-name guard: an INDIRECT aggregate (struct local or
   * by-reference param) may only be DECLARED or have its ADDRESS taken; MIR
   * reaches its fields exclusively through &@sym + offset memory ops. Any
   * other by-name appearance (assign/return/call-arg/store value) would copy
   * just the low 8 bytes, so defer such a function to the fallback. */
  {
    if (!mir_gate_indirect_operands(generator, ir_function, in)) {
      return 0;
    }
    for (size_t a = 0; a < in->argument_count; a++) {
      if (in->arguments[a].kind == IR_OPERAND_SYMBOL &&
          in->arguments[a].name &&
          mir_name_is_indirect_aggregate(generator, ir_function,
                                         in->arguments[a].name) &&
          /* A struct passed by value is allowed when Link 4 can source the
           * outgoing copy: a struct LOCAL's home, or a by-ref param's
           * pointer (mir_call_is_supported validates the callee param). */
          !(in->op == IR_OP_CALL &&
            mir_indirect_source_is_supported(generator, ir_function,
                                             &in->arguments[a]))) {
        return mir_trace_bail(ir_function, "indirect_agg_byname");
      }
    }
  }
  return 1;
}

static int mir_function_is_eligible_inner(CodeGenerator *generator,
                                          IRFunction *ir_function);

int mir_function_is_eligible(CodeGenerator *generator,
                             IRFunction *ir_function) {
  int eligible;
  g_mir_gate_reported = 0;
  eligible = mir_function_is_eligible_inner(generator, ir_function);
  if (!eligible && !g_mir_gate_reported) {
    mir_trace_bail(ir_function, "unreported");
  }
  return eligible;
}

static int mir_function_is_eligible_inner(CodeGenerator *generator,
                                          IRFunction *ir_function) {
  if (!generator || !ir_function) {
    return 0;
  }
  g_mir_gate_fn_size = 0;
  if (ir_explain_enabled()) {
    for (size_t i = 0; i < ir_function->instruction_count; i++) {
      if (ir_function->instructions[i].op != IR_OP_NOP) {
        g_mir_gate_fn_size++;
      }
    }
  }
  if (!mir_gate_function_shape(generator, ir_function) ||
      !mir_gate_signature(generator, ir_function) ||
      !mir_gate_globals(generator, ir_function)) {
    return 0;
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (!mir_gate_indirect_aggregate(generator, ir_function, in)) {
      return 0;
    }
    {
      static int (*const GATES[])(CodeGenerator *, const IRFunction *,
                                  const IRInstruction *, size_t, int *) = {
          mir_gate_control, mir_gate_value,  mir_gate_arith,
          mir_gate_convert, mir_gate_memory, mir_gate_select,
          mir_gate_mac,     mir_gate_fill,   mir_gate_affine,
          mir_gate_vloop,   mir_gate_silu};
      size_t gate;
      int claimed = 0;
      for (gate = 0; gate < sizeof(GATES) / sizeof(GATES[0]); gate++) {
        int handled = 0;
        if (!GATES[gate](generator, ir_function, in, i, &handled)) {
          if (!g_mir_gate_reported) {
            static char reason[48];
            snprintf(reason, sizeof(reason), "gate%u:op%d", (unsigned)gate,
                     (int)in->op);
            mir_trace_bail(ir_function, reason);
          }
          return 0;
        }
        if (handled) {
          claimed = 1;
          break;
        }
      }
      if (!claimed && !mir_gate_inline_kernel(ir_function, in)) {
        return 0;
      }
    }
  }
  if (mir_env_trace()) {
    fprintf(stderr, "MIR-OK\t%s\n",
            ir_function->name ? ir_function->name : "?");
  }
  if (ir_explain_enabled() && ir_function->name) {
    ir_explain_backend_function(ir_function->name,
                                mir_function_filename(ir_function), 1, NULL,
                                g_mir_gate_fn_size);
  }
  if (ir_machine_collecting() && ir_function->name) {
    ir_machine_note_backend(ir_function->name,
                            mir_function_filename(ir_function),
                            (long long)g_mir_gate_fn_size, 1);
  }
  return 1;
}

/* ---- lowering ----------------------------------------------------------- */

static int mir_emit1(MirFunction *fn, MirOpcode op, MirOperand dst,
                     MirOperand a, MirOperand b, int width, int is_unsigned,
                     unsigned char cc) {
  MirInst in;
  memset(&in, 0, sizeof(in));
  in.op = op;
  in.dst = dst;
  in.a = a;
  in.b = b;
  in.width = width;
  in.is_unsigned = is_unsigned;
  in.cc = cc;
  in.ir_index = -1;
  return mir_emit(fn, &in);
}

static int mir_emit_bf16_narrow(MirFunction *fn, MirVregId gbits,
                                MirVregId gdst) {
  MirVregId glsb = mir_new_vreg(fn, MIR_RC_GP, 8);
  MirVregId gbias = mir_new_vreg(fn, MIR_RC_GP, 8);
  MirVregId gtmp = mir_new_vreg(fn, MIR_RC_GP, 8);
  MirVregId gnan = mir_new_vreg(fn, MIR_RC_GP, 8);
  MirVregId gabs = mir_new_vreg(fn, MIR_RC_GP, 8);
  MirVregId gcond = mir_new_vreg(fn, MIR_RC_GP, 8);
  unsigned char cc = 0;
  if (glsb == MIR_VREG_NONE || gbias == MIR_VREG_NONE ||
      gtmp == MIR_VREG_NONE || gnan == MIR_VREG_NONE ||
      gabs == MIR_VREG_NONE || gcond == MIR_VREG_NONE ||
      !mir_setcc_opcode(">", 1, &cc)) {
    fn->has_error = 1;
    return 0;
  }
  return mir_emit1(fn, MIR_SHR, mir_op_vreg(glsb), mir_op_vreg(gbits),
                   mir_op_imm(16), 8, 1, 0) &&
         mir_emit1(fn, MIR_AND, mir_op_vreg(glsb), mir_op_vreg(glsb),
                   mir_op_imm(1), 8, 0, 0) &&
         mir_emit1(fn, MIR_MOV, mir_op_vreg(gbias), mir_op_imm(0x7FFF),
                   mir_op_none(), 8, 0, 0) &&
         mir_emit1(fn, MIR_ADD, mir_op_vreg(gbias), mir_op_vreg(gbias),
                   mir_op_vreg(glsb), 8, 0, 0) &&
         mir_emit1(fn, MIR_ADD, mir_op_vreg(gtmp), mir_op_vreg(gbits),
                   mir_op_vreg(gbias), 8, 0, 0) &&
         mir_emit1(fn, MIR_SHR, mir_op_vreg(gtmp), mir_op_vreg(gtmp),
                   mir_op_imm(16), 8, 1, 0) &&
         mir_emit1(fn, MIR_SHR, mir_op_vreg(gnan), mir_op_vreg(gbits),
                   mir_op_imm(16), 8, 1, 0) &&
         mir_emit1(fn, MIR_AND, mir_op_vreg(gnan), mir_op_vreg(gnan),
                   mir_op_imm(0xFF80), 8, 0, 0) &&
         mir_emit1(fn, MIR_OR, mir_op_vreg(gnan), mir_op_vreg(gnan),
                   mir_op_imm(0x40), 8, 0, 0) &&
         mir_emit1(fn, MIR_AND, mir_op_vreg(gabs), mir_op_vreg(gbits),
                   mir_op_imm(0x7FFFFFFF), 8, 0, 0) &&
         mir_emit1(fn, MIR_SETCC, mir_op_vreg(gcond), mir_op_vreg(gabs),
                   mir_op_imm(0x7F800000), 8, 1, cc) &&
         mir_emit1(fn, MIR_MOV, mir_op_vreg(gdst), mir_op_vreg(gtmp),
                   mir_op_none(), 8, 0, 0) &&
         mir_emit1(fn, MIR_CMOV, mir_op_vreg(gdst), mir_op_vreg(gcond),
                   mir_op_vreg(gnan), 8, 0, 0);
}

/* ---- constant-divisor strength reduction (magic multiply) --------------- */

/* The pooled GP vreg for a loop-invariant 64-bit integer constant, or NONE. */
static MirVregId mir_iconst_lookup(MirFunction *fn, int64_t value) {
  for (size_t i = 0; i < fn->iconst_count; i++) {
    if (fn->iconsts[i].value == value) {
      return fn->iconsts[i].vreg;
    }
  }
  return MIR_VREG_NONE;
}

/* Add `value` to the integer-constant pool and emit its initial materialization.
 * A later MIR layout pass moves the movabs to a hot-loop preheader. No-op if
 * already pooled. */
static int mir_iconst_add(MirFunction *fn, int64_t value) {
  if (mir_iconst_lookup(fn, value) != MIR_VREG_NONE) {
    return 1;
  }
  if (fn->iconst_count >= fn->iconst_capacity) {
    size_t nc = fn->iconst_capacity ? fn->iconst_capacity * 2 : 8;
    MirIConst *grown =
        (MirIConst *)realloc(fn->iconsts, nc * sizeof(MirIConst));
    if (!grown) {
      fn->has_error = 1;
      return 0;
    }
    fn->iconsts = grown;
    fn->iconst_capacity = nc;
  }
  MirVregId v = mir_new_vreg(fn, MIR_RC_GP, 8);
  if (v == MIR_VREG_NONE) {
    return 0;
  }
  fn->iconsts[fn->iconst_count].value = value;
  fn->iconsts[fn->iconst_count].vreg = v;
  fn->iconst_count++;
  return mir_emit1(fn, MIR_MOV, mir_op_vreg(v), mir_op_imm(value),
                   mir_op_none(), 8, 0, 0);
}

/* An integer-constant operand: the hoisted pool vreg if `value` was pooled,
 * otherwise an inline immediate. */
static MirOperand mir_iconst_operand(MirFunction *fn, int64_t value) {
  MirVregId v = mir_iconst_lookup(fn, value);
  return (v != MIR_VREG_NONE) ? mir_op_vreg(v) : mir_op_imm(value);
}

/* If `a / C` or `a % C` (compile-time constant C, dividend signedness `uns`)
 * lowers via a magic-multiply MULHI, return 1 and set *Mout to the 64-bit magic
 * constant the MULHI multiplies by; return 0 for the forms that emit no MULHI
 * (C in {0, 1, -1} or |C| a power of two). Mirrors the magic selection inside
 * mir_emit_const_divmod so the magic can be pre-pooled and hoisted out of a
 * loop. */
static int mir_divmod_magic(int64_t C, int uns, int64_t *Mout) {
  CgStrengthRewrite rw;
  if (!cg_strength_classify('/', C, uns, &rw) || rw.kind != CG_SR_DIV_MAGIC) {
    return 0; /* trivial or power-of-two divisors lower without a MULHI */
  }
  *Mout = rw.magic;
  return 1;
}

/* Strength-reduce `dst = a / C` or `dst = a % C` for a compile-time constant C
 * into a magic-number multiply (+ shifts), avoiding the long-latency divide.
 * Returns 1 if it emitted the reduced form, 0 to fall back to a real divide
 * (C == 0 keeps the divide so the /0 runtime trap fires). `uns` is the
 * dividend's signedness; `mod` selects remainder. All math is 64-bit. */
static int mir_emit_const_divmod(MirFunction *fn, MirOperand dst, MirOperand a,
                                 int64_t C, int uns, int mod) {
  if (C == 0) {
    return 0;
  }
  /* The dividend is read repeatedly (MULHI, then the remainder/sign-correction
   * ops). A vreg is safe to re-read directly: it never lives in RAX/RDX (those
   * are non-allocatable encoder scratch), so MULHI's RAX:RDX clobber cannot
   * corrupt it, and this function never writes `a` before its last read. Only an
   * immediate (or other non-vreg) needs a fresh-vreg snapshot, copying it once
   * avoids re-emitting a 10-byte movabs at each use. Skipping the copy for the
   * common register dividend removes one mov per div/mod in hot loops. */
  MirOperand A;
  if (a.kind == MIR_OPK_VREG) {
    A = a;
  } else {
    MirVregId av = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (av == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(av), a, mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    A = mir_op_vreg(av);
  }

  if (C == 1) {
    return mir_emit1(fn, MIR_MOV, dst, mod ? mir_op_imm(0) : A, mir_op_none(), 8,
                     0, 0);
  }
  if (!uns && C == -1) {
    if (mod) {
      return mir_emit1(fn, MIR_MOV, dst, mir_op_imm(0), mir_op_none(), 8, 0, 0);
    }
    return mir_emit1(fn, MIR_NEG, dst, A, mir_op_none(), 8, 0, 0);
  }

  uint64_t ad = uns ? (uint64_t)C : (uint64_t)(C < 0 ? -C : C);
  int is_pow2 = (ad & (ad - 1)) == 0;
  int k = 0;
  for (uint64_t tt = ad; tt > 1; tt >>= 1) {
    k++;
  }

  int q_in_dst = !mod && dst.kind == MIR_OPK_VREG &&
                 !(A.kind == MIR_OPK_VREG && A.vreg == dst.vreg);
  MirVregId qv = q_in_dst ? dst.vreg : mir_new_vreg(fn, MIR_RC_GP, 8);
  if (qv == MIR_VREG_NONE) {
    return 0;
  }
  MirOperand Q = q_in_dst ? dst : mir_op_vreg(qv);

  if (is_pow2) {
    if (uns) {
      if (!mir_emit1(fn, MIR_SHR, Q, A, mir_op_imm(k), 8, 1, 0)) {
        return 0;
      }
    } else {
      /* bias = (a < 0) ? (2^k - 1) : 0 ; q = (a + bias) >> k (arithmetic). */
      MirVregId t1 = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId t2 = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (t1 == MIR_VREG_NONE || t2 == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_SAR, mir_op_vreg(t1), A, mir_op_imm(63), 8, 0, 0) ||
          !mir_emit1(fn, MIR_SHR, mir_op_vreg(t2), mir_op_vreg(t1),
                     mir_op_imm(64 - k), 8, 1, 0) ||
          !mir_emit1(fn, MIR_ADD, Q, A, mir_op_vreg(t2), 8, 0, 0) ||
          !mir_emit1(fn, MIR_SAR, Q, Q, mir_op_imm(k), 8, 0, 0)) {
        return 0;
      }
      if (C < 0 && !mir_emit1(fn, MIR_NEG, Q, Q, mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
  } else if (uns) {
    uint64_t M;
    int s, add;
    cg_magic_u64(ad, &M, &s, &add);
    MirVregId tv = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (tv == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MULHI, mir_op_vreg(tv), A,
                   mir_iconst_operand(fn, (int64_t)M), 8, 1, 0)) {
      return 0;
    }
    if (!add) {
      if (!mir_emit1(fn, MIR_SHR, Q, mir_op_vreg(tv), mir_op_imm(s), 8, 1, 0)) {
        return 0;
      }
    } else {
      /* q = (((a - t) >> 1) + t) >> (s - 1)  (overflow-safe average). */
      MirVregId d1 = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (d1 == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_SUB, mir_op_vreg(d1), A, mir_op_vreg(tv), 8, 0,
                     0) ||
          !mir_emit1(fn, MIR_SHR, mir_op_vreg(d1), mir_op_vreg(d1),
                     mir_op_imm(1), 8, 1, 0) ||
          !mir_emit1(fn, MIR_ADD, mir_op_vreg(d1), mir_op_vreg(d1),
                     mir_op_vreg(tv), 8, 0, 0) ||
          !mir_emit1(fn, MIR_SHR, Q, mir_op_vreg(d1), mir_op_imm(s - 1), 8, 1,
                     0)) {
        return 0;
      }
    }
  } else {
    int64_t M;
    int s;
    cg_magic_s64(C, &M, &s);
    if (!mir_emit1(fn, MIR_MULHI, Q, A, mir_iconst_operand(fn, M), 8, 0, 0)) {
      return 0;
    }
    if (C > 0 && M < 0) {
      if (!mir_emit1(fn, MIR_ADD, Q, Q, A, 8, 0, 0)) {
        return 0;
      }
    } else if (C < 0 && M > 0) {
      if (!mir_emit1(fn, MIR_SUB, Q, Q, A, 8, 0, 0)) {
        return 0;
      }
    }
    if (s > 0 && !mir_emit1(fn, MIR_SAR, Q, Q, mir_op_imm(s), 8, 0, 0)) {
      return 0;
    }
    /* q += sign bit of q (round toward zero). */
    MirVregId sb = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (sb == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_SHR, mir_op_vreg(sb), Q, mir_op_imm(63), 8, 1, 0) ||
        !mir_emit1(fn, MIR_ADD, Q, Q, mir_op_vreg(sb), 8, 0, 0)) {
      return 0;
    }
  }

  if (!mod) {
    return q_in_dst ? 1
                    : mir_emit1(fn, MIR_MOV, dst, Q, mir_op_none(), 8, 0, 0);
  }
  /* remainder = a - q * C */
  MirVregId mv = mir_new_vreg(fn, MIR_RC_GP, 8);
  if (mv == MIR_VREG_NONE) {
    return 0;
  }
  if (C >= INT32_MIN && C <= INT32_MAX) {
    if (!mir_emit1(fn, MIR_IMUL, mir_op_vreg(mv), Q, mir_op_imm(C), 8, 0, 0)) {
      return 0;
    }
  } else {
    MirVregId cv = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (cv == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(cv), mir_op_imm(C), mir_op_none(), 8,
                   0, 0) ||
        !mir_emit1(fn, MIR_IMUL, mir_op_vreg(mv), Q, mir_op_vreg(cv), 8, 0, 0)) {
      return 0;
    }
  }
  return mir_emit1(fn, MIR_SUB, dst, A, mir_op_vreg(mv), 8, 0, 0);
}

/* Emit a MIR_STORE_GLOBAL for each named global, writing its cached vreg back to
 * memory (Vg -> [g]). */
static int mir_emit_global_flush_names(MirFunction *fn, CodeGenerator *g,
                                       MirNameMap *map, const char **names,
                                       size_t count) {
  for (size_t i = 0; i < count; i++) {
    const char *name = names[i];
    const CgSym *s = code_generator_lookup_symbol(g, name);
    int size = s ? code_generator_binary_resolved_type_scalar_size(s->type) : 0;
    if (size != 1 && size != 2 && size != 4 && size != 8) {
      fn->has_error = 1;
      return 0;
    }
    MirVregId v = mir_name_map_get_or_add(map, fn, name, 0, MIR_RC_GP, 8);
    if (v == MIR_VREG_NONE) {
      return 0;
    }
    if (!mir_emit1(fn, MIR_STORE_GLOBAL, mir_op_none(), mir_op_symbol(name),
                   mir_op_vreg(v), size, 0, 0)) {
      return 0;
    }
  }
  return 1;
}

/* Emit a MIR_LOAD_GLOBAL for each named global, refreshing its cache vreg from
 * memory ([g] -> Vg). */
static int mir_emit_global_reload_names(MirFunction *fn, CodeGenerator *g,
                                        MirNameMap *map, const char **names,
                                        size_t count) {
  for (size_t i = 0; i < count; i++) {
    const char *name = names[i];
    const CgSym *s = code_generator_lookup_symbol(g, name);
    int size = s ? code_generator_binary_resolved_type_scalar_size(s->type) : 0;
    if (size != 1 && size != 2 && size != 4 && size != 8) {
      fn->has_error = 1;
      return 0;
    }
    int is_signed =
        code_generator_binary_resolved_type_is_signed_integer(s->type);
    int fbits = code_generator_binary_resolved_type_float_bits(s->type);
    MirVregId v = mir_name_map_get_or_add(map, fn, name, 0,
                                          fbits ? MIR_RC_XMM : MIR_RC_GP,
                                          fbits ? fbits / 8 : 8);
    if (v == MIR_VREG_NONE) {
      return 0;
    }
    if (!mir_emit1(fn, MIR_LOAD_GLOBAL, mir_op_vreg(v), mir_op_symbol(name),
                   mir_op_none(), size, is_signed ? 0 : 1, 0)) {
      return 0;
    }
  }
  return 1;
}

/* Flush the DIRTY cached globals back to memory. Called before each MIR_RET
 * (so memory is consistent on every exit) and before a call (so the callee sees
 * current values). With the dirty analysis available, only globals actually
 * written since the last cleaning point are stored: writing a merely-cached
 * (clean) value back would race a concurrent writer that updated the global
 * between our reload and this flush -- the lock()/unlock() idiom, where the
 * synchronizing calls are exactly the boundaries a stale store-back must not
 * cross. */
static int mir_emit_global_writebacks(MirFunction *fn, CodeGenerator *g,
                                      MirNameMap *map,
                                      const MirGlobalWriteback *wb) {
  if (!wb) {
    return 1;
  }
  if (wb->dirty && fn->cur_ir_index >= 0) {
    unsigned long long m = wb->dirty[fn->cur_ir_index];
    for (size_t j = 0; j < wb->count && j < 64; j++) {
      if ((m >> j) & 1ull) {
        if (!mir_emit_global_flush_names(fn, g, map, &wb->names[j], 1)) {
          return 0;
        }
      }
    }
    return 1;
  }
  return mir_emit_global_flush_names(fn, g, map, wb->names, wb->count);
}

/* Flush the address-taken globals a pointer access could read, before that
 * access. Only the ones whose cache can differ from memory: a global the
 * function never writes by name was loaded once at entry and refreshed after
 * every aliasing store, so storing it back writes the value already there.
 *
 * The unconditional form put that dead store in front of every load and every
 * store in the function, which inside a loop means several per iteration. A
 * kernel inlined into a caller that happens to cache one address-taken global
 * paid for it on every element it touched. */
static int mir_emit_global_alias_flush(MirFunction *fn, CodeGenerator *g,
                                       MirNameMap *map,
                                       const MirGlobalWriteback *wb) {
  if (!wb) {
    return 1;
  }
  for (size_t i = 0; i < wb->at_count; i++) {
    size_t written = wb->count;
    for (size_t j = 0; j < wb->count; j++) {
      if (strcmp(wb->names[j], wb->at[i]) == 0) {
        written = j;
        break;
      }
    }
    if (written == wb->count) {
      continue; /* never written by name: the cache holds what memory holds */
    }
    if (wb->dirty && fn->cur_ir_index >= 0 && written < 64 &&
        !((wb->dirty[fn->cur_ir_index] >> written) & 1ull)) {
      continue; /* clean on every path reaching here */
    }
    if (!mir_emit_global_flush_names(fn, g, map, &wb->at[i], 1)) {
      return 0;
    }
  }
  return 1;
}

/* Flow-sensitive dirty-global analysis over the IR CFG. Returns a malloc'd
 * array of instruction_count masks: mask[i] bit j set = names[j] was possibly
 * written (cache newer than memory) on some path reaching instruction i, with
 * function entry clean and every call a cleaning point (its flush-before /
 * reload-after leaves cache == memory). Forward may-analysis to fixpoint;
 * unreachable code keeps an empty mask. Returns NULL when the analysis does
 * not apply (no instructions, no written globals, more than 64 of them, or a
 * malformed branch target); the caller then flushes the whole set. */
static unsigned long long *mir_compute_global_dirty_masks(
    const IRFunction *irf, const char **names, size_t count) {
  size_t n = irf->instruction_count;
  if (n == 0 || count == 0 || count > 64) {
    return NULL;
  }
  unsigned long long *mask =
      (unsigned long long *)calloc(n, sizeof(*mask));
  int *target = (int *)malloc(n * sizeof(*target));
  if (!mask || !target) {
    free(mask);
    free(target);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    const IRInstruction *in = &irf->instructions[i];
    target[i] = -1;
    if ((in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO ||
         in->op == IR_OP_BRANCH_EQ) &&
        in->text) {
      for (size_t k = 0; k < n; k++) {
        const IRInstruction *lk = &irf->instructions[k];
        if (lk->op == IR_OP_LABEL && lk->text &&
            strcmp(lk->text, in->text) == 0) {
          target[i] = (int)k;
          break;
        }
      }
      if (target[i] < 0) {
        free(mask);
        free(target);
        return NULL;
      }
    }
  }
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < n; i++) {
      const IRInstruction *in = &irf->instructions[i];
      unsigned long long s = mask[i];
      if (in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT ||
          in->op == IR_OP_INLINE_ASM) {
        s = 0; /* kill first: the flush+reload cleans, THEN a @g=f() dest
                  capture re-dirties below */
      }
      if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
          in->op != IR_OP_DECLARE_LOCAL) {
        for (size_t j = 0; j < count; j++) {
          if (strcmp(names[j], in->dest.name) == 0) {
            s |= 1ull << j;
            break;
          }
        }
      }
      if (in->op == IR_OP_RETURN) {
        continue;
      }
      if (target[i] >= 0) {
        size_t t = (size_t)target[i];
        if ((mask[t] | s) != mask[t]) {
          mask[t] |= s;
          changed = 1;
        }
        if (in->op == IR_OP_JUMP) {
          continue;
        }
      }
      if (i + 1 < n && (mask[i + 1] | s) != mask[i + 1]) {
        mask[i + 1] |= s;
        changed = 1;
      }
    }
  }
  free(target);
  return mask;
}

/* Reload every cached global EXCEPT `except` (borrowed name). Used after a call
 * whose result is assigned straight to a global (`@g = f()`, which the optimizer
 * fuses into one CALL with dest=@g): the call lowering has already captured the
 * return value into @g's cache vreg, and C semantics discard any write the
 * callee made to @g's memory, so reloading @g from (still-stale) memory would
 * wrongly clobber the fresh result with the old value. */
static int mir_emit_global_reloads_except(MirFunction *fn, CodeGenerator *g,
                                          MirNameMap *map,
                                          const MirGlobalWriteback *wb,
                                          const char *except) {
  if (!wb) {
    return 1;
  }
  for (size_t i = 0; i < wb->all_count; i++) {
    if (except && wb->all[i] && strcmp(wb->all[i], except) == 0) {
      continue;
    }
    if (!mir_emit_global_reload_names(fn, g, map, &wb->all[i], 1)) {
      return 0;
    }
  }
  return 1;
}

static int mir_instruction_writes_symbol(const IRInstruction *in,
                                         const char *name) {
  if (!in || in->op == IR_OP_DECLARE_LOCAL || in->op == IR_OP_NOP) {
    return 0;
  }
  return name && in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
         strcmp(in->dest.name, name) == 0;
}

static int mir_address_of_function_target(CodeGenerator *g,
                                          const IROperand *op) {
  if (!g || !op || op->kind != IR_OPERAND_SYMBOL || !op->name) {
    return 0;
  }
  const CgSym *s = g->ir_program ? code_generator_lookup_symbol(g, op->name)
                              : NULL;
  return (s && s->kind == CG_SYM_FUNCTION) ||
         mir_find_ir_function_named(g, op->name) != NULL;
}

static const char *mir_known_function_pointer_target(CodeGenerator *g,
                                                     const IRFunction *irf,
                                                     size_t before,
                                                     const char *name) {
  if (!g || !irf || !name) {
    return NULL;
  }
  int is_param = 0;
  MtlcType *ft = mir_local_or_param_type(g, irf, name, &is_param);
  if (!ft || is_param || ft->kind != MTLC_TYPE_FUNCTION_POINTER) {
    return NULL;
  }

  const char *target = NULL;
  int writes = 0;
  for (size_t i = 0; i < before && i < irf->instruction_count; i++) {
    const IRInstruction *in = &irf->instructions[i];
    if (!mir_instruction_writes_symbol(in, name)) {
      continue;
    }
    writes++;
    if (writes > 1 || in->op != IR_OP_ADDRESS_OF ||
        !mir_address_of_function_target(g, &in->lhs)) {
      return NULL;
    }
    target = in->lhs.name;
  }
  return writes == 1 ? target : NULL;
}

static int mir_ir_function_may_write_global(CodeGenerator *g,
                                            const IRFunction *irf) {
  if (!g || !irf) {
    return 1;
  }
  for (size_t i = 0; i < irf->instruction_count; i++) {
    const IRInstruction *in = &irf->instructions[i];
    switch (in->op) {
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_INLINE_ASM:
    case IR_OP_NEW:
    case IR_OP_STORE:
      return 1;
    default:
      break;
    }
    if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        !mir_local_or_param_type(g, irf, in->dest.name, NULL) &&
        mir_name_is_global_scalar(g, in->dest.name)) {
      return 1;
    }
  }
  return 0;
}

static int mir_operand_names_temp(const IROperand *op, const char *name) {
  return op->kind == IR_OPERAND_TEMP && op->name && strcmp(op->name, name) == 0;
}

static int mir_address_only_feeds_calls(const IRFunction *irf,
                                        const IRInstruction *take) {
  const char *temp;
  if (take->dest.kind != IR_OPERAND_TEMP || !take->dest.name) {
    return 0;
  }
  temp = take->dest.name;
  for (size_t j = 0; j < irf->instruction_count; j++) {
    const IRInstruction *u = &irf->instructions[j];
    int is_call = u->op == IR_OP_CALL || u->op == IR_OP_CALL_INDIRECT;
    if (u == take) {
      continue;
    }
    if (mir_operand_names_temp(&u->dest, temp) ||
        mir_operand_names_temp(&u->lhs, temp) ||
        mir_operand_names_temp(&u->rhs, temp)) {
      return 0;
    }
    for (size_t a = 0; a < u->argument_count; a++) {
      if (mir_operand_names_temp(&u->arguments[a], temp) && !is_call) {
        return 0;
      }
    }
  }
  return 1;
}

static int mir_call_may_write_globals(CodeGenerator *g, const IRFunction *irf,
                                      size_t index,
                                      const IRInstruction *in) {
  if (!g || !irf || !in) {
    return 1;
  }
  const char *target = NULL;
  if (in->op == IR_OP_CALL) {
    target = in->text;
  } else if (in->op == IR_OP_CALL_INDIRECT &&
             in->lhs.kind == IR_OPERAND_SYMBOL && in->lhs.name) {
    target = mir_known_function_pointer_target(g, irf, index, in->lhs.name);
  }
  if (!target || !target[0]) {
    return 1;
  }
  IRFunction *target_ir = mir_find_ir_function_named(g, target);
  return !target_ir || mir_ir_function_may_write_global(g, target_ir);
}

/* Emit a fixed-size byte copy of `size` bytes from [src_base] to [dst_base],
 * where both bases are pointer vregs. Lowered as a straight-line sequence of
 * load/store pairs through a fresh GP temp (8 bytes at a time, then a 4/2/1
 * tail), exactly the [base + disp] memory MOVs the field-access path already
 * uses, so it needs no new encoder support and the allocator schedules the
 * pointers and temps normally. Used to copy an INDIRECT struct into a caller's
 * hidden return slot (and, later, for whole-struct assignment and arguments). */
/* Above this many bytes a block copy stops being unrolled. Each unrolled word
 * costs a load and a store, about sixteen bytes of code, so a 608-byte struct
 * -- a rule table entry, an engine's config record -- was expanding to more
 * than a kilobyte of moves at every copy site. `rep movsb` is a dozen
 * instructions whatever the count, and the microcoded copy beats a long
 * straight-line run once the count is this large anyway. Below the threshold
 * the unrolled form still wins: it needs no register marshalling and leaves
 * the function a leaf. */
#define MIR_STRUCT_COPY_UNROLL_MAX 128

static int mir_emit_struct_copy(MirFunction *fn, MirVregId dst_base,
                                MirVregId src_base, int size) {
  if (size > MIR_STRUCT_COPY_UNROLL_MAX) {
    /* Same shape the memcpy call lowering uses: put destination, source and
     * count in the active convention's first three integer argument registers
     * and let MIR_REP_MOVSB be the copy. */
    const BinaryAbi *abi = code_generator_binary_active_abi();
    if (abi && abi->int_param_count >= 3) {
      return mir_emit1(fn, MIR_MOV,
                       mir_op_phys(abi->int_param_registers[0], MIR_RC_GP),
                       mir_op_vreg(dst_base), mir_op_none(), 8, 0, 0) &&
             mir_emit1(fn, MIR_MOV,
                       mir_op_phys(abi->int_param_registers[1], MIR_RC_GP),
                       mir_op_vreg(src_base), mir_op_none(), 8, 0, 0) &&
             mir_emit1(fn, MIR_MOV,
                       mir_op_phys(abi->int_param_registers[2], MIR_RC_GP),
                       mir_op_imm(size), mir_op_none(), 8, 0, 0) &&
             mir_emit1(fn, MIR_REP_MOVSB, mir_op_symbol("memcpy"),
                       mir_op_none(), mir_op_none(), 8, 0, 0);
    }
  }
  for (int k = 0; k < size;) {
    int rem = size - k;
    int w = rem >= 8 ? 8 : (rem >= 4 ? 4 : (rem >= 2 ? 2 : 1));
    MirVregId tmp = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (tmp == MIR_VREG_NONE) {
      return 0;
    }
    MirOperand src_mem = mir_op_mem_vreg(src_base, MIR_VREG_NONE, 1, k);
    MirOperand dst_mem = mir_op_mem_vreg(dst_base, MIR_VREG_NONE, 1, k);
    if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(tmp), src_mem, mir_op_none(), w, 1,
                   0) ||
        !mir_emit1(fn, MIR_MOV, dst_mem, mir_op_vreg(tmp), mir_op_none(), w, 1,
                   0)) {
      return 0;
    }
    k += w;
  }
  return 1;
}

/* Materialize the ADDRESS of an INDIRECT-aggregate copy SOURCE into a fresh
 * vreg: a struct LOCAL/TEMP leas its home (marking it memory-resident at least
 * `sz` bytes), a by-ref aggregate PARAM's value is already the address (plain
 * MOV), and a string LITERAL leas its {chars,length} record. The MIR mirror of
 * the fallback's emit_indirect_source_address; eligibility has vetted the
 * operand via mir_indirect_source_is_supported. Returns MIR_VREG_NONE on
 * failure. */
static MirVregId mir_emit_indirect_source_addr(MirFunction *fn,
                                               CodeGenerator *g,
                                               BinaryFunctionContext *ctx,
                                               MirNameMap *map,
                                               const IRFunction *irf,
                                               const IROperand *op, int sz) {
  MirVregId base = mir_new_vreg(fn, MIR_RC_GP, 8);
  if (base == MIR_VREG_NONE) {
    return MIR_VREG_NONE;
  }
  if (op->kind == IR_OPERAND_STRING) {
    const char *s = op->name ? op->name : "";
    /* imm carries the literal's byte length, which strlen cannot recover once
     * the bytes hold an interior NUL. */
    MirOperand lit = mir_op_symbol(s);
    lit.imm = (long long)ir_operand_string_length(op);
    if (!mir_emit1(fn, MIR_LEA_STRLIT, mir_op_vreg(base), lit,
                   mir_op_none(), 8, 0, 0)) {
      return MIR_VREG_NONE;
    }
    return base;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name &&
      mir_name_is_global_aggregate(g, irf, op->name)) {
    const CgSym *s =
        g->ir_program ? code_generator_lookup_symbol(g, op->name) : NULL;
    int is_extern = (s && s->is_extern) ? 1 : 0;
    if (!mir_emit1(fn, MIR_LEA_GLOBAL, mir_op_vreg(base),
                   mir_op_symbol(op->name), mir_op_none(), 8, is_extern, 0)) {
      return MIR_VREG_NONE;
    }
    return base;
  }
  MirOperand v = mir_value_operand(fn, g, ctx, map, op);
  if (v.kind != MIR_OPK_VREG) {
    fn->has_error = 1;
    return MIR_VREG_NONE;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name &&
      mir_name_is_indirect_param(g, irf, op->name)) {
    if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(base), v, mir_op_none(), 8, 0, 0)) {
      return MIR_VREG_NONE;
    }
    return base;
  }
  fn->vregs[v.vreg].address_taken = 1;
  if (fn->vregs[v.vreg].home_bytes < ((sz + 7) & ~7)) {
    fn->vregs[v.vreg].home_bytes = (sz + 7) & ~7;
  }
  if (!mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(base), v, mir_op_none(), 8, 0,
                 0)) {
    return MIR_VREG_NONE;
  }
  return base;
}

/* Resolve an operand used as a memory ADDRESS: a global aggregate's name
 * materializes its RIP-relative address into a fresh vreg (it has no cached
 * value vreg; memory is authoritative), anything else is the plain value
 * operand (a pointer temp/local, or a cached scalar's vreg). */
static MirOperand mir_address_operand(MirFunction *fn, CodeGenerator *g,
                                      BinaryFunctionContext *ctx,
                                      MirNameMap *map, const IROperand *op) {
  if (op->kind == IR_OPERAND_SYMBOL && op->name) {
    const IRFunction *irf =
        ctx && ctx->function_name
            ? code_generator_find_ir_function_binary(g, ctx->function_name)
            : NULL;
    if (mir_name_is_string_local(g, irf, op->name)) {
      MirVregId a = mir_emit_indirect_source_addr(fn, g, ctx, map, irf, op, 16);
      if (a == MIR_VREG_NONE) {
        fn->has_error = 1;
        return mir_op_none();
      }
      return mir_op_vreg(a);
    }
    if (mir_name_is_global_aggregate(g, irf, op->name)) {
      MirVregId a = mir_new_vreg(fn, MIR_RC_GP, 8);
      const CgSym *s =
          g->ir_program ? code_generator_lookup_symbol(g, op->name) : NULL;
      int is_extern = (s && s->is_extern) ? 1 : 0;
      if (a == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_LEA_GLOBAL, mir_op_vreg(a),
                     mir_op_symbol(op->name), mir_op_none(), 8, is_extern, 0)) {
        fn->has_error = 1;
        return mir_op_none();
      }
      return mir_op_vreg(a);
    }
  }
  {
    MirOperand addr = mir_value_operand(fn, g, ctx, map, op);
    if (addr.kind == MIR_OPK_IMM) {
      MirVregId t = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (t == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_MOV, mir_op_vreg(t), addr, mir_op_none(), 8, 0,
                     0)) {
        fn->has_error = 1;
        return mir_op_none();
      }
      return mir_op_vreg(t);
    }
    return addr;
  }
}

/* A width-tagged float register move (xmm copy). */
static int mir_emit_fmov(MirFunction *fn, MirOperand dst, MirOperand src,
                         int width) {
  MirInst in;
  memset(&in, 0, sizeof(in));
  in.op = MIR_MOV;
  in.is_float = 1;
  in.dst = dst;
  in.a = src;
  in.width = width;
  in.ir_index = -1;
  return mir_emit(fn, &in);
}

static MirOperand mir_float_const_operand(MirFunction *fn, double value,
                                          int width_bytes);

/* Store a float call argument into its outgoing stack slot, converted to the
 * parameter's width. */
static MirOperand coerce_float_operand(MirFunction *fn, CodeGenerator *g,
                                       BinaryFunctionContext *ctx,
                                       MirNameMap *map, const IROperand *op,
                                       int target_bytes);

static int mir_emit_float_stack_arg(MirFunction *fn, CodeGenerator *g,
                                    BinaryFunctionContext *ctx, MirNameMap *map,
                                    const IROperand *arg_op, int pfb,
                                    int slot) {
  MirOperand fv = coerce_float_operand(fn, g, ctx, map, arg_op, pfb / 8);
  if (fv.kind == MIR_OPK_FIMM) {
    MirVregId t = mir_new_vreg(fn, MIR_RC_XMM, pfb / 8);
    if (t == MIR_VREG_NONE || !mir_emit_fmov(fn, mir_op_vreg(t), fv, pfb / 8)) {
      return 0;
    }
    fv = mir_op_vreg(t);
  }
  MirInst st;
  memset(&st, 0, sizeof(st));
  st.op = MIR_STORE_OUTARG;
  st.is_float = 1;
  st.a = fv;
  st.b = mir_op_imm(slot);
  st.width = pfb / 8;
  st.ir_index = -1;
  return mir_emit(fn, &st);
}

/* Raw IEEE-754 bits of a double value at the given float width (4 or 8). */
static uint64_t mir_float_bits_at(double value, int width_bytes) {
  if (width_bytes == 4) {
    float f = (float)value;
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
  }
  uint64_t u;
  memcpy(&u, &value, sizeof(u));
  return u;
}

/* The pooled vreg for a loop-invariant constant (bits,width), or MIR_VREG_NONE. */
static MirVregId mir_pool_lookup(MirFunction *fn, uint64_t bits, int width) {
  for (size_t i = 0; i < fn->fconst_count; i++) {
    if (fn->fconsts[i].bits == bits && fn->fconsts[i].width == width) {
      return fn->fconsts[i].vreg;
    }
  }
  return MIR_VREG_NONE;
}

/* Add (bits,width) to the float-constant pool and emit its initial
 * materialization.
 * A later MIR layout pass moves it to a hot-loop preheader. No-op if already
 * pooled. */
static int mir_pool_add(MirFunction *fn, uint64_t bits, int width) {
  if (mir_pool_lookup(fn, bits, width) != MIR_VREG_NONE) {
    return 1;
  }
  if (fn->fconst_count >= fn->fconst_capacity) {
    size_t nc = fn->fconst_capacity ? fn->fconst_capacity * 2 : 8;
    MirFConst *grown =
        (MirFConst *)realloc(fn->fconsts, nc * sizeof(MirFConst));
    if (!grown) {
      fn->has_error = 1;
      return 0;
    }
    fn->fconsts = grown;
    fn->fconst_capacity = nc;
  }
  MirVregId v = mir_new_vreg(fn, MIR_RC_XMM, width);
  if (v == MIR_VREG_NONE) {
    return 0;
  }
  fn->fconsts[fn->fconst_count].bits = bits;
  fn->fconsts[fn->fconst_count].width = width;
  fn->fconsts[fn->fconst_count].vreg = v;
  fn->fconst_count++;
  return mir_emit_fmov(fn, mir_op_vreg(v), mir_op_fimm(bits), width);
}

/* A float-constant operand: the hoisted pool vreg if this (value,width) was
 * pooled, otherwise an inline float immediate. */
static MirOperand mir_float_const_operand(MirFunction *fn, double value,
                                          int width) {
  uint64_t bits = mir_float_bits_at(value, width);
  MirVregId v = mir_pool_lookup(fn, bits, width);
  return (v != MIR_VREG_NONE) ? mir_op_vreg(v) : mir_op_fimm(bits);
}

/* Resolve a float operand to the operation's width `target_bytes`, inserting a
 * cvtss2sd/cvtsd2ss when the operand's natural float width differs. A float
 * literal is materialized directly at the target width. This is the implicit
 * promotion/narrowing the IR leaves to the backend (e.g. float32 * 1.5 computes
 * at float64). */
static MirOperand coerce_float_operand(MirFunction *fn, CodeGenerator *g,
                                       BinaryFunctionContext *ctx,
                                       MirNameMap *map, const IROperand *op,
                                       int target_bytes) {
  if (op->kind == IR_OPERAND_FLOAT) {
    return mir_float_const_operand(fn, op->float_value, target_bytes);
  }
  if (op->kind == IR_OPERAND_INT) {
    /* Integer literal used in a float op -> a float constant of that value. */
    return mir_float_const_operand(fn, (double)op->int_value, target_bytes);
  }
  MirOperand v = mir_value_operand(fn, g, ctx, map, op);
  int fb = code_generator_binary_operand_float_bits(g, ctx, op);
  if (fb == 0) {
    /* Integer operand promoted into a float op (the IR leaves the cvtsi2sd to
     * the backend, e.g. `f + y` with y an int). */
    MirVregId tmp = mir_new_vreg(fn, MIR_RC_XMM, target_bytes);
    if (tmp == MIR_VREG_NONE) {
      return v;
    }
    mir_emit1(fn, MIR_CVTSI2F, mir_op_vreg(tmp), v, mir_op_none(), target_bytes,
              0, 0);
    return mir_op_vreg(tmp);
  }
  if (fb / 8 != target_bytes) {
    MirVregId tmp = mir_new_vreg(fn, MIR_RC_XMM, target_bytes);
    if (tmp == MIR_VREG_NONE) {
      return v;
    }
    mir_emit1(fn, MIR_CVTF2F, mir_op_vreg(tmp), v, mir_op_none(), target_bytes,
              0, 0);
    return mir_op_vreg(tmp);
  }
  return v;
}

/* Operand (compute) width in bytes of a float comparison's operands. */
static int mir_float_cmp_width(CodeGenerator *g, BinaryFunctionContext *ctx,
                               const IRInstruction *in) {
  int fb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
  if (!fb) {
    fb = code_generator_binary_operand_float_bits(g, ctx, &in->rhs);
  }
  return fb ? fb / 8 : 8;
}

/* IR index of a label definition by name, or SIZE_MAX. */
static size_t mir_ir_label_index(IRFunction *function, const char *name) {
  if (!name) {
    return SIZE_MAX;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LABEL && in->text && strcmp(in->text, name) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

/* Build the constant pools: every distinct float literal used INSIDE a loop (a
 * backward jump/branch range), plus the 64-bit magic-multiply constant of every
 * in-loop `x / C` / `x % C` with a constant divisor, is hoisted to a vreg.
 * Constants outside loops are left inline (no register-pressure benefit). Must
 * run before the body is lowered so uses can resolve to pooled vregs; the
 * materializations are relocated after MIR layout. */
static int mir_build_const_pool(MirFunction *fn, CodeGenerator *g,
                                BinaryFunctionContext *ctx,
                                IRFunction *function) {
  size_t n = function->instruction_count;
  if (n == 0) {
    return 1;
  }
  char *in_loop = (char *)calloc(n, 1);
  if (!in_loop) {
    fn->has_error = 1;
    return 0;
  }
  for (size_t j = 0; j < n; j++) {
    const IRInstruction *in = &function->instructions[j];
    const char *target = (in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO)
                             ? in->text
                             : NULL;
    if (!target) {
      continue;
    }
    size_t l = mir_ir_label_index(function, target);
    if (l != SIZE_MAX && l < j) {
      for (size_t k = l; k <= j; k++) {
        in_loop[k] = 1;
      }
    }
  }

  int ok = 1;
  for (size_t j = 0; j < n && ok; j++) {
    if (!in_loop[j]) {
      continue;
    }
    const IRInstruction *in = &function->instructions[j];
    /* Pool the div/mod magic-multiply constant for `x / C` / `x % C` (a
     * compile-time-constant divisor) so the 64-bit magic is materialized once at
     * preheader instead of with a 10-byte movabs every loop iteration. */
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        (in->text[0] == '/' || in->text[0] == '%') && in->text[1] == '\0' &&
        in->rhs.kind == IR_OPERAND_INT) {
      int uns = in->is_unsigned || mir_operand_is_unsigned(g, ctx, &in->lhs);
      int64_t M;
      if (mir_divmod_magic(in->rhs.int_value, uns, &M) && !mir_iconst_add(fn, M)) {
        ok = 0;
        break;
      }
    }
    const IROperand *ops[2] = {NULL, NULL};
    int w = 0;
    if (in->op == IR_OP_BINARY && in->is_float) {
      int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
      w = fb ? fb / 8 : 8;
      ops[0] = &in->lhs;
      ops[1] = &in->rhs;
    } else if (in->op == IR_OP_ASSIGN) {
      int fb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
      if (fb) {
        w = fb / 8;
        ops[0] = &in->lhs;
      }
    }
    if (!w) {
      continue;
    }
    for (int k = 0; k < 2; k++) {
      if (ops[k] && ops[k]->kind == IR_OPERAND_FLOAT) {
        if (!mir_pool_add(fn, mir_float_bits_at(ops[k]->float_value, w), w)) {
          ok = 0;
          break;
        }
      }
    }
  }
  free(in_loop);
  return ok;
}

/* If `op` is an integer constant usable as a 32-bit compare immediate, return 1
 * and set *out to its sign-extended value. Recognizes a literal INT directly, or
 * a temp whose single definition is a CAST of an integer literal to an integer
 * type (the shape a loop bound like `i < (int64)N` takes). The cast value is
 * recomputed at the destination width/signedness so a narrowing cast cannot fold
 * to the wrong number, and only values fitting signed-32 are accepted. This lets
 * a counted-loop bound become `cmp reg, imm32` instead of being rematerialized
 * into a register every iteration. */
static int mir_fused_cmp_imm(CodeGenerator *g, BinaryFunctionContext *ctx,
                             const IRFunction *f, const IROperand *op,
                             long long *out) {
  long long v;
  if (op->kind == IR_OPERAND_INT) {
    v = op->int_value;
  } else if (op->kind == IR_OPERAND_TEMP && op->name) {
    const IRInstruction *def = NULL;
    int defs = 0;
    for (size_t i = 0; i < f->instruction_count; i++) {
      const IRInstruction *in = &f->instructions[i];
      if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
          strcmp(in->dest.name, op->name) == 0) {
        def = in;
        defs++;
      }
    }
    if (defs != 1 || !def || def->op != IR_OP_CAST ||
        def->lhs.kind != IR_OPERAND_INT || !def->text) {
      return 0;
    }
    /* The cast target type is named by def->text (e.g. "int64"); the temp's dest
     * type is not registered in this context, so resolve from the name. */
    MtlcType *dt = code_generator_binary_get_resolved_type(g, def->text, 0);
    if (!dt || code_generator_binary_resolved_type_float_bits(dt)) {
      return 0;
    }
    (void)ctx;
    int sz = code_generator_binary_resolved_type_scalar_size(dt);
    int sgn = code_generator_binary_resolved_type_is_signed_integer(dt);
    v = def->lhs.int_value;
    if (sz == 1) {
      v = sgn ? (long long)(signed char)v : (long long)(unsigned char)v;
    } else if (sz == 2) {
      v = sgn ? (long long)(short)v : (long long)(unsigned short)v;
    } else if (sz == 4) {
      v = sgn ? (long long)(int)v : (long long)(unsigned int)v;
    } else if (sz != 8) {
      return 0;
    }
  } else {
    return 0;
  }
  if (v < -2147483648LL || v > 2147483647LL) {
    return 0;
  }
  *out = v;
  return 1;
}

/* Fuse `%t = a CMP b; branch_zero %t -> L` into a compare-and-branch: integer
 * `cmp a,b; j<!CMP> L`, or float `ucomis a,b; j<!CMP> L`. */
static int mir_lower_compare_branch(MirFunction *fn, CodeGenerator *g,
                                    BinaryFunctionContext *ctx, MirNameMap *map,
                                    const IRFunction *ir_function,
                                    const IRInstruction *cmp,
                                    const IRInstruction *br) {
  if (cmp->is_float) {
    int swap;
    unsigned char cc = 0;
    if (!mir_float_cmp_info(cmp->text, 1, &swap, &cc)) {
      fn->has_error = 1;
      return 0;
    }
    int w = mir_float_cmp_width(g, ctx, cmp);
    const IROperand *lo = swap ? &cmp->rhs : &cmp->lhs;
    const IROperand *ro = swap ? &cmp->lhs : &cmp->rhs;
    MirOperand a = coerce_float_operand(fn, g, ctx, map, lo, w);
    MirOperand b = coerce_float_operand(fn, g, ctx, map, ro, w);
    return mir_emit1(fn, MIR_FCMPBR, mir_op_label(br->text), a, b, w, 0, cc);
  }
  MirOperand a = mir_value_operand(fn, g, ctx, map, &cmp->lhs);
  int uns = cmp->is_unsigned || mir_operand_is_unsigned(g, ctx, &cmp->lhs) ||
            mir_operand_is_unsigned(g, ctx, &cmp->rhs);
  /* Fold a constant right-hand bound into the compare as an imm32 so the loop
   * does not rematerialize it into a register every iteration. The producer is
   * dropped separately (mir_compute_const_compare_skips). */
  long long imm;
  MirOperand b;
  if (mir_fused_cmp_imm(g, ctx, ir_function, &cmp->rhs, &imm)) {
    b = mir_op_imm(imm);
  } else {
    b = mir_value_operand(fn, g, ctx, map, &cmp->rhs);
  }
  unsigned char cc = 0;
  if (!mir_false_jcc(cmp->text, uns, &cc)) {
    fn->has_error = 1;
    return 0;
  }
  int w = mir_int_compare_width(g, ctx, cmp->text, &cmp->lhs, &cmp->rhs);
  return mir_emit1(fn, MIR_CMPBR, mir_op_label(br->text), a, b, w, uns, cc);
}

/* True when instruction i is a single-use comparison (integer or ordered float)
 * whose result is consumed only by an immediately-following branch_zero. */
static int mir_fuses_compare_branch(CodeGenerator *g, IRFunction *function,
                                    size_t i) {
  if (i + 1 >= function->instruction_count) {
    return 0;
  }
  const IRInstruction *cmp = &function->instructions[i];
  const IRInstruction *br = &function->instructions[i + 1];
  if (cmp->op != IR_OP_BINARY || !cmp->text ||
      cmp->dest.kind != IR_OPERAND_TEMP || !cmp->dest.name) {
    return 0;
  }
  int sw;
  unsigned char fcc;
  int ok_cmp = cmp->is_float ? mir_float_cmp_info(cmp->text, 1, &sw, &fcc)
                             : mir_is_comparison(cmp->text);
  if (!ok_cmp) {
    return 0;
  }
  if (br->op != IR_OP_BRANCH_ZERO || br->lhs.kind != IR_OPERAND_TEMP ||
      !br->lhs.name || strcmp(br->lhs.name, cmp->dest.name) != 0) {
    return 0;
  }
  (void)g;
  return code_generator_binary_function_temp_use_count(function,
                                                       cmp->dest.name) == 1;
}

/* ---- generic inline kernel (MIR_IR_KERNEL) ------------------------------- */

/* Run one table kernel in place. Every by-name operand of `in` is staged into
 * an address-taken vreg (a plain frame slot), the kernel runs against those
 * slots, and each slot is read back into the value it came from.
 *
 * Staging through memory rather than fixed registers is what makes this one
 * routine cover the whole table: the bridge never has to know which register a
 * kernel wants an operand in, how many times it loads it, or which of its
 * operands it writes. The cost is a store and a load per operand around a loop
 * that runs over an entire array, which is not measurable.
 *
 * Operands naming the SAME value share one slot (see MirKernelAux): a kernel
 * that accumulates into its own destination reads and writes one variable
 * through two operand positions, and two slots would race on the read-back. */
static int mir_lower_ir_kernel(MirFunction *fn, CodeGenerator *g,
                               BinaryFunctionContext *ctx, MirNameMap *map,
                               const IRInstruction *in) {
  int kernel_index = mir_ir_kernel_index_for_op(in->op);
  if (kernel_index < 0) {
    fn->has_error = 1;
    return 0;
  }

  MirKernelAux *aux = (MirKernelAux *)calloc(1, sizeof(MirKernelAux));
  if (!mir_function_own_aux(fn, aux)) {
    return 0;
  }
  aux->ir = in; /* borrowed: the IR outlives this function's codegen */
  aux->kernel_index = kernel_index;

  /* Source vreg per slot, and the staging vreg it is copied through. Kept
   * alongside the aux (which the encoder reads) rather than in it, since the
   * encoder needs only the staging side. */
  MirVregId source[MIR_KERNEL_MAX_SLOTS];
  int is_float[MIR_KERNEL_MAX_SLOTS];
  int width[MIR_KERNEL_MAX_SLOTS];

  for (int k = 0;; k++) {
    const IROperand *op = mir_instruction_operand_at(in, k);
    if (!op) {
      break;
    }
    if (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) {
      continue; /* the kernel materializes immediates and literals itself */
    }
    MirOperand v = mir_value_operand(fn, g, ctx, map, op);
    if (v.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    int slot = -1;
    for (int s = 0; s < aux->slot_count; s++) {
      if (source[s] == v.vreg) {
        slot = s;
        break;
      }
    }
    if (slot < 0) {
      if (aux->slot_count >= MIR_KERNEL_MAX_SLOTS) {
        fn->has_error = 1; /* the gate sized this; a mismatch is a bug */
        return 0;
      }
      int fb = code_generator_binary_operand_float_bits(g, ctx, op);
      slot = aux->slot_count++;
      source[slot] = v.vreg;
      is_float[slot] = fb ? 1 : 0;
      width[slot] = fb ? fb / 8 : 8;
      MirVregId stage = mir_new_vreg(fn, fb ? MIR_RC_XMM : MIR_RC_GP,
                                     width[slot]);
      if (stage == MIR_VREG_NONE) {
        return 0;
      }
      /* address_taken keeps the staging value out of the register file: it
       * lives only in its frame slot, which is exactly the storage the kernel
       * addresses. */
      fn->vregs[stage].address_taken = 1;
      aux->slot_vreg[slot] = stage;
    }
    if (aux->operand_count >= MIR_KERNEL_MAX_SLOTS) {
      fn->has_error = 1;
      return 0;
    }
    aux->operand[aux->operand_count] = op;
    aux->operand_slot[aux->operand_count] = slot;
    aux->operand_count++;
  }

  for (int s = 0; s < aux->slot_count; s++) {
    MirOperand stage = mir_op_vreg(aux->slot_vreg[s]);
    MirOperand src = mir_op_vreg(source[s]);
    if (is_float[s] ? !mir_emit_fmov(fn, stage, src, width[s])
                    : !mir_emit1(fn, MIR_MOV, stage, src, mir_op_none(), 8, 0,
                                 0)) {
      return 0;
    }
  }

  {
    MirInst kin;
    memset(&kin, 0, sizeof(kin));
    kin.op = MIR_IR_KERNEL;
    kin.dst = mir_op_imm(kernel_index);
    kin.ir_index = -1;
    kin.aux = aux;
    if (!mir_emit(fn, &kin)) {
      return 0;
    }
  }

  /* Read every slot back, not just the ones the kernel writes: which operands
   * are outputs is kernel-specific knowledge the bridge deliberately does not
   * carry, and reloading an input costs one move and restores its own value. */
  for (int s = 0; s < aux->slot_count; s++) {
    MirOperand stage = mir_op_vreg(aux->slot_vreg[s]);
    MirOperand dst = mir_op_vreg(source[s]);
    if (is_float[s] ? !mir_emit_fmov(fn, dst, stage, width[s])
                    : !mir_emit1(fn, MIR_MOV, dst, stage, mir_op_none(), 8, 0,
                                 0)) {
      return 0;
    }
  }
  return 1;
}

static int mir_return_literal_is_canonical(const MirFunction *fn,
                                          const IROperand *value) {
  long long v;
  int bits;
  if (!value || value->kind != IR_OPERAND_INT) {
    return 0;
  }
  v = value->int_value;
  bits = fn->scalar_return_width * 8;
  if (fn->scalar_return_signed) {
    return v >= -(1ll << (bits - 1)) && v < (1ll << (bits - 1));
  }
  return v >= 0 && v < (1ll << bits);
}

static int mir_lower_control(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
    return 1;

  case IR_OP_LABEL:
    return mir_emit1(fn, MIR_LABEL, mir_op_label(in->text), mir_op_none(),
                     mir_op_none(), 8, 0, 0);

  case IR_OP_JUMP:
    return mir_emit1(fn, MIR_JMP, mir_op_label(in->text), mir_op_none(),
                     mir_op_none(), 8, 0, 0);

  case IR_OP_INLINE_ASM: {
    MirAsmAux *aux = (MirAsmAux *)calloc(1, sizeof(MirAsmAux));
    const char *cursor = in->text;
    char name[128];
    MirInst asm_inst;
    if (!aux) {
      fn->has_error = 1;
      return 0;
    }
    aux->ir = in;
    while (mir_asm_next_binding(&cursor, name, sizeof(name))) {
      MtlcType *type = mir_local_or_param_type(g, fn->ir_function, name, NULL);
      int seen = 0;
      MirVregId v;
      int bytes;
      if (!type) {
        continue;
      }
      for (int k = 0; k < aux->count; k++) {
        if (strcmp(aux->names[k], name) == 0) {
          seen = 1;
          break;
        }
      }
      if (seen) {
        continue;
      }
      if (aux->count >= MIR_ASM_MAX_BINDS) {
        free(aux);
        fn->has_error = 1;
        return 0;
      }
      {
        IROperand probe;
        char *owned = mir_function_own_aux(fn, mettle_strdup(name));
        if (!owned) {
          free(aux);
          return 0;
        }
        memset(&probe, 0, sizeof(probe));
        probe.kind = IR_OPERAND_SYMBOL;
        probe.name = owned;
        MirOperand value = mir_value_operand(fn, g, ctx, map, &probe);
        if (value.kind != MIR_OPK_VREG) {
          free(aux);
          fn->has_error = 1;
          return 0;
        }
        v = value.vreg;
        aux->names[aux->count] = owned;
      }
      bytes = (int)((code_generator_abi_type_size(type) + 7) & ~(size_t)7);
      fn->vregs[v].address_taken = 1;
      if (fn->vregs[v].home_bytes < bytes) {
        fn->vregs[v].home_bytes = bytes;
      }
      aux->vregs[aux->count] = v;
      aux->count++;
    }
    if (!mir_function_own_aux(fn, aux)) {
      return 0;
    }
    memset(&asm_inst, 0, sizeof(asm_inst));
    asm_inst.op = MIR_INLINE_ASM;
    asm_inst.dst = mir_op_none();
    asm_inst.a = mir_op_none();
    asm_inst.b = mir_op_none();
    asm_inst.width = 8;
    asm_inst.ir_index = -1;
    asm_inst.aux = aux;
    return mir_emit(fn, &asm_inst);
  }

  case IR_OP_BRANCH_ZERO: {
    /* if (cond == 0) goto label  ->  test cond; je label */
    int cfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
    if (cfb) {
      /* A float condition is zero when it compares equal to 0.0, which is the
       * composite ordered-equality FSETCC, then a branch on that 0/1. */
      int cw = cfb / 8;
      MirOperand fv = coerce_float_operand(fn, g, ctx, map, &in->lhs, cw);
      MirOperand fz = mir_float_const_operand(fn, 0.0, cw);
      MirVregId eq = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (eq == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_FSETCC, mir_op_vreg(eq), fv, fz, cw, 0, 0x94)) {
        return 0;
      }
      return mir_emit1(fn, MIR_JCC, mir_op_label(in->text), mir_op_vreg(eq),
                       mir_op_none(), 8, 0, 0x85);
    }
    if (in->lhs.kind == IR_OPERAND_INT) {
      if (in->lhs.int_value != 0) {
        return 1;
      }
      return mir_emit1(fn, MIR_JMP, mir_op_label(in->text), mir_op_none(),
                       mir_op_none(), 8, 0, 0);
    }
    MirOperand cond = mir_value_operand(fn, g, ctx, map, &in->lhs);
    return mir_emit1(fn, MIR_JCC, mir_op_label(in->text), cond, mir_op_none(), 8,
                     0, 0x84 /* je */);
  }

  case IR_OP_BRANCH_EQ: {
    /* if (lhs == rhs) goto label  ->  cmp lhs,rhs; je label. Equality, so
     * signedness is irrelevant and a constant rhs (the common switch/match
     * case value) folds into the cmp's imm32 (or a scratch reg if it doesn't
     * fit) inside the MIR_CMPBR encoder. */
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand b = mir_value_operand(fn, g, ctx, map, &in->rhs);
    return mir_emit1(fn, MIR_CMPBR, mir_op_label(in->text), a, b, 8, 0,
                     0x84 /* je */);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_assign(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_ASSIGN: {
    /* Whole-struct copy `@a <- @b` / `@a <- %t` / `%t <- @a`: both operands hold
     * an INDIRECT struct in a LEA-able home, so copy the bytes (rep movsb via the
     * struct-copy helper) instead of an 8-byte MOV that would truncate. */
    {
      const IRFunction *airf =
          ctx && ctx->function_name
              ? code_generator_find_ir_function_binary(g, ctx->function_name)
              : NULL;
      int ssz = mir_operand_struct_home_size(g, airf, &in->dest);
      if (ssz > 0) {
        MirOperand dsym = mir_value_operand(fn, g, ctx, map, &in->dest);
        if (dsym.kind != MIR_OPK_VREG) {
          fn->has_error = 1;
          return 0;
        }
        fn->vregs[dsym.vreg].address_taken = 1;
        if (fn->vregs[dsym.vreg].home_bytes < ssz) {
          fn->vregs[dsym.vreg].home_bytes = ssz;
        }
        MirVregId sb = mir_emit_indirect_source_addr(fn, g, ctx, map, airf,
                                                     &in->lhs, ssz);
        MirVregId db = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (sb == MIR_VREG_NONE || db == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(db), dsym, mir_op_none(), 8,
                       0, 0) ||
            !mir_emit_struct_copy(fn, db, sb, ssz)) {
          return 0;
        }
        return 1;
      }
    }
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    int dfb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
    if (dfb) {
      int sfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
      if (in->lhs.kind == IR_OPERAND_FLOAT) {
        /* Literal at the destination width (pooled if loop-invariant). */
        MirOperand lit = mir_float_const_operand(fn, in->lhs.float_value, dfb / 8);
        return mir_emit_fmov(fn, dst, lit, dfb / 8);
      }
      if (in->lhs.kind == IR_OPERAND_INT) {
        /* Integer literal into a float home (`float v;` zero-init): a float
         * constant of that value, matching coerce_float_operand. A raw fmov
         * of the integer immediate is unencodable as an XMM operand. */
        MirOperand lit =
            mir_float_const_operand(fn, (double)in->lhs.int_value, dfb / 8);
        return mir_emit_fmov(fn, dst, lit, dfb / 8);
      }
      MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
      if (sfb && sfb != dfb) {
        /* Float store of a differently-sized value narrows/widens. */
        return mir_emit1(fn, MIR_CVTF2F, dst, src, mir_op_none(), dfb / 8, 0, 0);
      }
      return mir_emit_fmov(fn, dst, src, dfb / 8);
    }
    if (in->lhs.kind == IR_OPERAND_STRING) {
      const char *lit = in->lhs.name ? in->lhs.name : "";
      return mir_emit1(fn, MIR_LEA_CSTR, dst, mir_op_symbol(lit),
                       mir_op_none(), 8, 0, 0);
    }
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    return mir_emit1(fn, MIR_MOV, dst, src, mir_op_none(), 8, 0, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_float_binary(MirFunction *fn, CodeGenerator *g,
                                  BinaryFunctionContext *ctx,
                                  MirNameMap *map,
                                  const IRInstruction *in,
                                  MirOperand dst) {
    MirOpcode fop = MIR_FADD;
    if (mir_float_arith_opcode(in->text, &fop)) {
      int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
      int w = fb ? fb / 8 : 8;
      /* Coerce each operand to the operation width (implicit promotion). */
      MirOperand fa = coerce_float_operand(fn, g, ctx, map, &in->lhs, w);
      MirOperand fbop = coerce_float_operand(fn, g, ctx, map, &in->rhs, w);
      return mir_emit1(fn, fop, dst, fa, fbop, w, 0, 0);
    }
    /* Non-fused ordered float comparison -> 0/1 via ucomis + setcc. */
    int swap;
    unsigned char cc = 0;
    if (!mir_float_cmp_info(in->text, 0, &swap, &cc)) {
      fn->has_error = 1;
      return 0;
    }
    int w = mir_float_cmp_width(g, ctx, in);
    const IROperand *lo = swap ? &in->rhs : &in->lhs;
    const IROperand *ro = swap ? &in->lhs : &in->rhs;
    MirOperand fa = coerce_float_operand(fn, g, ctx, map, lo, w);
    MirOperand fbop = coerce_float_operand(fn, g, ctx, map, ro, w);
    return mir_emit1(fn, MIR_FSETCC, dst, fa, fbop, w, 0, cc);
}

static const IRInstruction *mir_find_divmod_sibling(
    MirFunction *fn, CodeGenerator *g, BinaryFunctionContext *ctx,
    const IRInstruction *in, unsigned char mod) {
  const IRFunction *irf =
      ctx && ctx->function_name
          ? code_generator_find_ir_function_binary(g, ctx->function_name)
          : NULL;
  const IRInstruction *sibling = NULL;
  if (irf && in >= irf->instructions &&
      in < irf->instructions + irf->instruction_count &&
      fn->divmod_precomp_count < 16 && in->dest.kind == IR_OPERAND_TEMP) {
    size_t idx = (size_t)(in - irf->instructions);
    for (size_t j = idx + 1; j < irf->instruction_count; j++) {
      const IRInstruction *nx = &irf->instructions[j];
      if (nx->op == IR_OP_LABEL || nx->op == IR_OP_JUMP ||
          nx->op == IR_OP_BRANCH_ZERO || nx->op == IR_OP_BRANCH_EQ ||
          nx->op == IR_OP_CALL || nx->op == IR_OP_RETURN) {
        break;
      }
      if (mir_ir_operand_equal(&nx->dest, &in->lhs) ||
          mir_ir_operand_equal(&nx->dest, &in->rhs)) {
        break;
      }
      if (nx->op == IR_OP_BINARY && nx->text && !nx->is_float &&
          nx->dest.kind == IR_OPERAND_TEMP && nx->dest.name &&
          ((mod && strcmp(nx->text, "/") == 0) ||
           (!mod && strcmp(nx->text, "%") == 0)) &&
          mir_ir_operand_equal(&nx->lhs, &in->lhs) &&
          mir_ir_operand_equal(&nx->rhs, &in->rhs)) {
        sibling = nx;
        break;
      }
    }
  }
  return sibling;
}

static int mir_lower_divide(MirFunction *fn, CodeGenerator *g,
                            BinaryFunctionContext *ctx,
                            MirNameMap *map,
                            const IRInstruction *in, MirOperand dst,
                            MirOperand a, MirOperand b) {
  (void)map;
    /* idiv/div: signedness is the dividend's (lhs) type; cc carries the
     * quotient-vs-remainder choice (1 == remainder, the `%` case). */
    int uns = in->is_unsigned || mir_operand_is_unsigned(g, ctx, &in->lhs);
    unsigned char mod = (in->text[0] == '%') ? 1 : 0;

    /* Constant-divisor strength reduction: replace the long-latency divide
     * with a magic-number multiply + shifts. Falls through to a real divide
     * for C == 0 (preserves the /0 trap) or unhandled forms. */
    if (in->rhs.kind == IR_OPERAND_INT &&
        mir_emit_const_divmod(fn, dst, a, in->rhs.int_value, uns, mod)) {
      return 1;
    }

    /* Divmod fusion. If a sibling `x op d` already did the divide and captured
     * BOTH results, this op is just a move of the value it needs. */
    if (in->dest.name) {
      for (size_t k = 0; k < fn->divmod_precomp_count; k++) {
        if (fn->divmod_precomp[k].name &&
            strcmp(fn->divmod_precomp[k].name, in->dest.name) == 0) {
          return mir_emit1(fn, MIR_MOV, dst,
                           mir_op_vreg(fn->divmod_precomp[k].vreg),
                           mir_op_none(), 8, 0, 0);
        }
      }
    }

    /* Otherwise look ahead in this basic block for the complementary op (`/`
     * paired with `%`, same operands) so a single divide serves both. The
     * scan stops at a block boundary / call (clobbers RAX:RDX) or any
     * redefinition of the dividend or divisor (would make the cached results
     * stale). */
    const IRInstruction *sibling =
        mir_find_divmod_sibling(fn, g, ctx, in, mod);

    if (sibling) {
      /* One divide; capture quotient (RAX) into qv and remainder (RDX) into
       * rv. The MOV reading RDX must immediately follow the divide (nothing
       * between can clobber RDX, which is non-allocatable scratch). */
      MirVregId qv = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId rv = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (qv == MIR_VREG_NONE || rv == MIR_VREG_NONE) {
        return 0;
      }
      if (!mir_emit1(fn, MIR_IDIV, mir_op_vreg(qv), a, b, 8, uns, 0) ||
          !mir_emit1(fn, MIR_MOV, mir_op_vreg(rv),
                     mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
      MirVregId mine = mod ? rv : qv;     /* this op's result */
      MirVregId theirs = mod ? qv : rv;   /* the sibling's result */
      fn->divmod_precomp[fn->divmod_precomp_count].name = sibling->dest.name;
      fn->divmod_precomp[fn->divmod_precomp_count].vreg = theirs;
      fn->divmod_precomp_count++;
      return mir_emit1(fn, MIR_MOV, dst, mir_op_vreg(mine), mir_op_none(), 8, 0,
                       0);
    }

    return mir_emit1(fn, MIR_IDIV, dst, a, b, 8, uns, mod);
}

static int mir_lower_binary(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_BINARY: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand b = mir_value_operand(fn, g, ctx, map, &in->rhs);
    if (in->is_float) {
      return mir_lower_float_binary(fn, g, ctx, map, in, dst);
    }
    if (mir_is_comparison(in->text)) {
      int uns = in->is_unsigned || mir_operand_is_unsigned(g, ctx, &in->lhs) ||
                mir_operand_is_unsigned(g, ctx, &in->rhs);
      unsigned char cc = 0;
      mir_setcc_opcode(in->text, uns, &cc);
      int w = mir_int_compare_width(g, ctx, in->text, &in->lhs, &in->rhs);
      return mir_emit1(fn, MIR_SETCC, dst, a, b, w, uns, cc);
    }
    if (strcmp(in->text, "/") == 0 || strcmp(in->text, "%") == 0) {
      return mir_lower_divide(fn, g, ctx, map, in, dst, a, b);
    }
    MirOpcode op = MIR_ADD;
    mir_arith_opcode(in->text, &op);
    /* `0 - x` is a negate. Written literally it is rare, but it is the standard
     * way to build an all-ones mask from a bit (`0 - (crc & 1)`), and SUB is
     * two-address: without this the encoder must first materialize the zero
     * into the destination register, so the idiom costs two instructions per
     * use instead of one. */
    if (op == MIR_SUB && in->lhs.kind == IR_OPERAND_INT &&
        in->lhs.int_value == 0) {
      return mir_emit1(fn, MIR_NEG, dst, b, mir_op_none(), 8, 0, 0);
    }
    int uns = 0;
    if (op == MIR_SHR) {
      /* arithmetic vs logical right shift depends on the LHS signedness. */
      if (!in->is_unsigned && !mir_operand_is_unsigned(g, ctx, &in->lhs)) {
        op = MIR_SAR;
      } else {
        uns = 1;
      }
    }
    return mir_emit1(fn, op, dst, a, b, 8, uns, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_unary(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_UNARY: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    const char *op = in->text ? in->text : "";
    if (in->is_float) {
      /* Float negate `-x` as a sign-bit flip, matching the fallback's
       * emit_unary exactly so 0 and NaN signs agree; `+x` is a copy. The
       * operand is coerced to the result precision first.
       *
       * `0 - x` is right for every float except zero, where IEEE 754 asks for
       * -0.0 and the subtract yields +0.0, and it cannot flip the sign of a
       * NaN at all. The mask is the bit pattern of negative zero at this
       * width, which is the sign bit and nothing else. */
      int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
      int w = fb ? fb / 8 : 8;
      MirOperand x = coerce_float_operand(fn, g, ctx, map, &in->lhs, w);
      if (strcmp(op, "+") == 0) {
        return mir_emit_fmov(fn, dst, x, w);
      }
      MirOperand mask = mir_op_fimm(binary_semantics_float_sign_mask(w * 8));
      return mir_emit1(fn, MIR_FXOR, dst, mask, x, w, 0, 0);
    }
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    if (strcmp(op, "-") == 0) {
      return mir_emit1(fn, MIR_NEG, dst, a, mir_op_none(), 8, 0, 0);
    }
    if (strcmp(op, "~") == 0) {
      return mir_emit1(fn, MIR_NOT, dst, a, mir_op_none(), 8, 0, 0);
    }
    if (strcmp(op, "+") == 0) {
      return mir_emit1(fn, MIR_MOV, dst, a, mir_op_none(), 8, 0, 0);
    }
    if (strcmp(op, "!") == 0) {
      /* !x == (x == 0) as 0/1: SETCC does cmp a,0; sete; movzx. */
      unsigned char cc = 0;
      mir_setcc_opcode("==", 0, &cc);
      return mir_emit1(fn, MIR_SETCC, dst, a, mir_op_imm(0), 8, 0, cc);
    }
    if (strcmp(op, "popcnt") == 0) {
      return mir_emit1(fn, MIR_POPCNT, dst, a, mir_op_none(), 8, 0, 0);
    }
    fn->has_error = 1;
    return 0;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_cast_across_banks(MirFunction *fn, CodeGenerator *g,
                                       BinaryFunctionContext *ctx,
                                       const IRInstruction *in,
                                       MirOperand dst, MirOperand a,
                                       int *crossed) {
  *crossed = 1;
  int dfb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
  int sfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
  if (dfb && !sfb) {
    /* int -> float. is_unsigned carries the SOURCE's signedness (set by IR
     * lowering); an unsigned source needs the halve-convert-double sequence
     * because the machine's conversion is signed. */
    return mir_emit1(fn, MIR_CVTSI2F, dst, a, mir_op_none(), dfb / 8,
                     in->is_unsigned ? 1 : 0, 0);
  }
  if (!dfb && sfb) {
    /* float -> int (truncating); width selects cvttsd2si vs cvttss2si.
     * A uint64 target needs the bias sequence: the machine's truncation is
     * signed and answers its sentinel for anything at or above 2^63. */
    const MtlcType *tt = (in->text && g->ir_program)
                   ? code_generator_named_type(g, in->text)
                   : NULL;
    int to_u64 = tt && tt->kind == MTLC_TYPE_UINT64;
    return mir_emit1(fn, MIR_CVTF2SI, dst, a, mir_op_none(), sfb / 8,
                     to_u64 ? 1 : 0, 0);
  }
  if (dfb && sfb) {
    const MtlcType *dt = NULL;
    if (in->text && g && g->ir_program) {
      dt = code_generator_named_type(g, in->text);
    }
    int is_f16 = (dt && dt->kind == MTLC_TYPE_FLOAT16) || (in->text && strcmp(in->text, "float16") == 0);
    int is_bf16 = (dt && dt->kind == MTLC_TYPE_BFLOAT16) || (in->text && strcmp(in->text, "bfloat16") == 0);
    if (is_f16 || is_bf16) {
      MirVregId xsrc = mir_new_vreg(fn, MIR_RC_XMM, 4);
      if (xsrc == MIR_VREG_NONE) {
        fn->has_error = 1;
        return 0;
      }
      if (sfb == 64) {
        if (!mir_emit1(fn, MIR_CVTF2F, mir_op_vreg(xsrc), a, mir_op_none(), 4, 0, 0)) {
          return 0;
        }
      } else {
        if (!mir_emit_fmov(fn, mir_op_vreg(xsrc), a, 4)) {
          return 0;
        }
      }
      if (is_f16) {
        MirVregId xh = mir_new_vreg(fn, MIR_RC_XMM, 4);
        if (xh == MIR_VREG_NONE) {
          fn->has_error = 1;
          return 0;
        }
        if (!mir_emit1(fn, MIR_CVTPS2PH, mir_op_vreg(xh), mir_op_vreg(xsrc), mir_op_none(), 4, 0, 0)) {
          return 0;
        }
        return mir_emit1(fn, MIR_CVTPH2PS, dst, mir_op_vreg(xh), mir_op_none(), 4, 0, 0);
      }
      MirVregId gbits = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId gdst = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId xdst = mir_new_vreg(fn, MIR_RC_XMM, 4);
      if (gbits == MIR_VREG_NONE || gdst == MIR_VREG_NONE || xdst == MIR_VREG_NONE) {
        fn->has_error = 1;
        return 0;
      }
      if (!mir_emit1(fn, MIR_MOVD_TO_GP, mir_op_vreg(gbits), mir_op_vreg(xsrc), mir_op_none(), 4, 0, 0) ||
          !mir_emit_bf16_narrow(fn, gbits, gdst) ||
          !mir_emit1(fn, MIR_SHL, mir_op_vreg(gdst), mir_op_vreg(gdst), mir_op_imm(16), 8, 0, 0) ||
          !mir_emit1(fn, MIR_MOVD_TO_XMM, mir_op_vreg(xdst), mir_op_vreg(gdst), mir_op_none(), 4, 0, 0)) {
        return 0;
      }
      return mir_emit_fmov(fn, dst, mir_op_vreg(xdst), 4);
    }
    if (dfb == sfb) {
      return mir_emit_fmov(fn, dst, a, dfb / 8);
    }
    return mir_emit1(fn, MIR_CVTF2F, dst, a, mir_op_none(), dfb / 8, 0, 0);
  }
  *crossed = 0;
  return 0;
}

static int mir_lower_cast(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_CAST: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    int crossed = 0;
    int converted =
        mir_lower_cast_across_banks(fn, g, ctx, in, dst, a, &crossed);
    if (crossed) {
      return converted;
    }
    /* The cast's target type is named on the instruction (in->text) and is
     * always resolvable; the dest operand's type is not (a temp has no
     * resolved type at -O0, which would silently drop a narrowing cast). Prefer
     * in->text, matching the fallback emitter, and fall back to the operand. */
    const MtlcType *dt = (in->text && g->ir_program)
                   ? code_generator_named_type(g, in->text)
                   : NULL;
    if (!dt) {
      dt = code_generator_binary_get_operand_type_in_context(g, ctx, &in->dest);
    }
    int dw = dt ? code_generator_binary_resolved_type_scalar_size(dt) : 8;
    int dsigned = dt ? code_generator_binary_resolved_type_is_signed_integer(dt)
                     : 1;
    if (dw != 1 && dw != 2 && dw != 4 && dw != 8) {
      dw = 8;
    }
    /* Re-express a's 64-bit value as the dst integer type. A NARROWING cast
     * (dw < source width) truncates to dw bytes then extends per dst signedness.
     * A WIDENING cast (dw >= source width) must extend from the SOURCE width per
     * the SOURCE signedness, because MIR computes in 64-bit and a narrow source
     * value (e.g. a uint32 product) can carry garbage above its width, a plain
     * 64-bit move would carry that garbage into the wider value (e.g.
     * `(int64)(uint32_a * uint32_b)`). */
    MtlcType *st = code_generator_binary_get_operand_type_in_context(g, ctx, &in->lhs);
    int sw = st ? code_generator_binary_resolved_type_scalar_size(st) : 0;
    int ssigned = st ? code_generator_binary_resolved_type_is_signed_integer(st)
                     : 1;
    int swf = st ? code_generator_binary_resolved_type_float_bits(st) : 0;
    if ((sw == 1 || sw == 2 || sw == 4) && swf == 0 && dw >= sw) {
      /* Widening from a known narrow integer source extends by the SOURCE
       * signedness, which is what gives those bits their value. Same width is
       * not a widening but a reinterpretation, and there the destination
       * decides: a uint32 lives in its register zero-extended, so
       * `(uint32)int32_value` has to clear the high half. Extending by the
       * source there left `(uint32)x >> 1` shifting a sign-extended value, and
       * a right shift is exactly where the high half stops being invisible. */
      int extend_signed = (dw == sw) ? dsigned : ssigned;
      /* A signed narrow source widened into an unsigned destination narrower
       * than 64 bits needs both halves of the story: the source's sign is what
       * gives the bits their value, and the destination's width is where that
       * value has to live zero-extended. Extending by the source alone left
       * `(uint32)int16_minus_one` reading 0xFFFFFFFFFFFFFFFF instead of
       * 4294967295. */
      if (extend_signed && !dsigned && dw < 8 && dw > sw) {
        MirVregId widened = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (widened == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_MOVSX, mir_op_vreg(widened), a, mir_op_none(),
                       sw, 0, 0)) {
          fn->has_error = 1;
          return 0;
        }
        return mir_emit1(fn, MIR_MOVZX, dst, mir_op_vreg(widened),
                         mir_op_none(), dw, 1, 0);
      }
      return mir_emit1(fn, extend_signed ? MIR_MOVSX : MIR_MOVZX, dst, a,
                       mir_op_none(), sw, !extend_signed, 0);
    }
    if (dw == 8) {
      /* Widening to 64 bits from an 8-byte or unknown source: a plain move. */
      return mir_emit1(fn, MIR_MOV, dst, a, mir_op_none(), 8, 0, 0);
    }
    /* Narrowing to a < source-width dst: truncate+extend per dst signedness. */
    return mir_emit1(fn, dsigned ? MIR_MOVSX : MIR_MOVZX, dst, a, mir_op_none(),
                     dw, !dsigned, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_load(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_LOAD: {
    if (in->lhs.kind == IR_OPERAND_STRING) {
      /* Data-pointer field of a string literal: materialize the .rdata
       * cstring address directly (validated to be the 8-byte pointer load by
       * the eligibility gate). */
      MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
      const char *s = in->lhs.name ? in->lhs.name : "";
      return mir_emit1(fn, MIR_LEA_CSTR, dst, mir_op_symbol(s), mir_op_none(),
                       8, 0, 0);
    }
    MirOperand addr = mir_address_operand(fn, g, ctx, map, &in->lhs);
    int size = code_generator_binary_get_access_size(g, ctx, &in->rhs);
    if (size <= 0 || addr.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    /* `@s <- *addr [8]` with a string-local dest: the loaded 8 bytes are a
     * record POINTER (the backend's string value convention); deref-copy the
     * 16-byte record into the local's home, mirroring the fallback's
     * emit_local_string_store. */
    if (size == 8 && in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name) {
      const IRFunction *lirf =
          ctx && ctx->function_name
              ? code_generator_find_ir_function_binary(g, ctx->function_name)
              : NULL;
      if (mir_name_is_string_local(g, lirf, in->dest.name)) {
        MirVregId ptr = mir_new_vreg(fn, MIR_RC_GP, 8);
        MirOperand dsym = mir_value_operand(fn, g, ctx, map, &in->dest);
        if (ptr == MIR_VREG_NONE || dsym.kind != MIR_OPK_VREG) {
          fn->has_error = 1;
          return 0;
        }
        fn->vregs[dsym.vreg].address_taken = 1;
        if (fn->vregs[dsym.vreg].home_bytes < 16) {
          fn->vregs[dsym.vreg].home_bytes = 16;
        }
        MirVregId db = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (db == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_MOV, mir_op_vreg(ptr),
                       mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0),
                       mir_op_none(), 8, 1, 0) ||
            !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(db), dsym, mir_op_none(),
                       8, 0, 0) ||
            !mir_emit_struct_copy(fn, db, ptr, 16)) {
          return 0;
        }
        return 1;
      }
    }
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand mem = mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0);
    if (in->is_float && size == 2 &&
        (in->alias_class == IR_ALIAS_CLASS_F16 ||
         in->alias_class == IR_ALIAS_CLASS_BF16)) {
      MirVregId gtmp = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId xtmp = mir_new_vreg(fn, MIR_RC_XMM, 4);
      if (gtmp == MIR_VREG_NONE || xtmp == MIR_VREG_NONE) {
        fn->has_error = 1;
        return 0;
      }
      if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(gtmp), mem, mir_op_none(), 2, 1, 0)) {
        return 0;
      }
      if (in->alias_class == IR_ALIAS_CLASS_F16) {
        if (!mir_emit1(fn, MIR_MOVD_TO_XMM, mir_op_vreg(xtmp), mir_op_vreg(gtmp), mir_op_none(), 4, 0, 0)) {
          return 0;
        }
        return mir_emit1(fn, MIR_CVTPH2PS, dst, mir_op_vreg(xtmp), mir_op_none(), 4, 0, 0);
      }
      if (!mir_emit1(fn, MIR_SHL, mir_op_vreg(gtmp), mir_op_vreg(gtmp), mir_op_imm(16), 8, 0, 0)) {
        return 0;
      }
      return mir_emit1(fn, MIR_MOVD_TO_XMM, dst, mir_op_vreg(gtmp), mir_op_none(), 4, 0, 0);
    }
    if (in->is_float) {
      int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
      return mir_emit_fmov(fn, dst, mem, fb ? fb / 8 : size);
    }
    int sign_ext = !in->is_unsigned &&
                   code_generator_binary_load_needs_sign_extend(g, ctx,
                                                               &in->dest, size);
    return mir_emit1(fn, MIR_MOV, dst, mem, mir_op_none(), size,
                     sign_ext ? 0 : 1, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_store(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_STORE: {
    MirOperand addr = mir_address_operand(fn, g, ctx, map, &in->dest);
    int size = code_generator_binary_get_access_size(g, ctx, &in->rhs);
    if (size <= 0 || addr.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
      MirOperand src_base = mir_value_operand(fn, g, ctx, map, &in->lhs);
      if (addr.kind != MIR_OPK_VREG || src_base.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      return mir_emit_struct_copy(fn, addr.vreg, src_base.vreg, size);
    }
    MirOperand mem = mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0);
    if (in->is_float && size == 2 &&
        (in->alias_class == IR_ALIAS_CLASS_F16 ||
         in->alias_class == IR_ALIAS_CLASS_BF16)) {
      MirOperand fval = coerce_float_operand(fn, g, ctx, map, &in->lhs, 4);
      MirVregId xsrc = mir_new_vreg(fn, MIR_RC_XMM, 4);
      if (xsrc == MIR_VREG_NONE) {
        fn->has_error = 1;
        return 0;
      }
      if (!mir_emit_fmov(fn, mir_op_vreg(xsrc), fval, 4)) {
        return 0;
      }
      if (in->alias_class == IR_ALIAS_CLASS_F16) {
        MirVregId xdst = mir_new_vreg(fn, MIR_RC_XMM, 4);
        MirVregId gdst = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (xdst == MIR_VREG_NONE || gdst == MIR_VREG_NONE) {
          fn->has_error = 1;
          return 0;
        }
        if (!mir_emit1(fn, MIR_CVTPS2PH, mir_op_vreg(xdst), mir_op_vreg(xsrc), mir_op_none(), 4, 0, 0)) {
          return 0;
        }
        if (!mir_emit1(fn, MIR_MOVD_TO_GP, mir_op_vreg(gdst), mir_op_vreg(xdst), mir_op_none(), 4, 0, 0)) {
          return 0;
        }
        return mir_emit1(fn, MIR_MOV, mem, mir_op_vreg(gdst), mir_op_none(), 2, 0, 0);
      }
      MirVregId gbits = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId gdst = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (gbits == MIR_VREG_NONE || gdst == MIR_VREG_NONE) {
        fn->has_error = 1;
        return 0;
      }
      if (!mir_emit1(fn, MIR_MOVD_TO_GP, mir_op_vreg(gbits), mir_op_vreg(xsrc), mir_op_none(), 4, 0, 0) ||
          !mir_emit_bf16_narrow(fn, gbits, gdst)) {
        return 0;
      }
      return mir_emit1(fn, MIR_MOV, mem, mir_op_vreg(gdst), mir_op_none(), 2, 0, 0);
    }
    if (in->is_float) {
      /* Coerce the value to the store width: a literal is materialized at
       * that width, and a float64-tracked arithmetic result narrows via
       * cvtsd2ss before a 4-byte store (a raw movss of a double's low dword
       * silently stores garbage, 0 for round values). */
      MirOperand fval =
          coerce_float_operand(fn, g, ctx, map, &in->lhs, size);
      return mir_emit_fmov(fn, mem, fval, size);
    }
    /* `*addr <- @s [8]` with a string-local value: the stored 8 bytes are the
     * local's home ADDRESS (a record pointer, the string value convention). A
     * by-ref param's plain vreg already holds its pointer, so it takes the
     * generic path below. */
    if (size == 8 &&
        (in->lhs.kind == IR_OPERAND_SYMBOL || in->lhs.kind == IR_OPERAND_TEMP) &&
        in->lhs.name) {
      const IRFunction *sirf =
          ctx && ctx->function_name
              ? code_generator_find_ir_function_binary(g, ctx->function_name)
              : NULL;
      int hsz = (in->lhs.kind == IR_OPERAND_SYMBOL)
                    ? (mir_name_is_string_local(g, sirf, in->lhs.name) ? 16 : 0)
                    : mir_struct_temp_size(g, sirf, in->lhs.name);
      if (hsz > 0) {
        MirVregId sb =
            mir_emit_indirect_source_addr(fn, g, ctx, map, sirf, &in->lhs, hsz);
        if (sb == MIR_VREG_NONE) {
          return 0;
        }
        return mir_emit1(fn, MIR_MOV, mem, mir_op_vreg(sb), mir_op_none(), 8, 0,
                         0);
      }
    }
    MirOperand val = mir_value_operand(fn, g, ctx, map, &in->lhs);
    return mir_emit1(fn, MIR_MOV, mem, val, mir_op_none(), size, 0, 0);
  }

  case IR_OP_PREFETCH: {
    MirOperand addr = mir_address_operand(fn, g, ctx, map, &in->lhs);
    if (addr.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    MirOperand mem = mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0);
    return mir_emit1(fn, MIR_PREFETCH, mir_op_none(), mem, mir_op_none(), 8, 0,
                     0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_select(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SELECT: {
    /* dst = (cond != 0) ? then : else. Stage cond and then in vregs, pre-load
     * a result vreg with else, then MIR_CMOV res, cond, then. Pre-loading res
     * makes its live range start before the cmov so it interferes with
     * cond/then and gets a distinct register (cmov needs res != then). Finally
     * move res into the IR dest (which may be a memory-resident local). */
    MirOperand cond = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand then_v = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand else_v = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    MirOperand dest = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirVregId cond_r = mir_new_vreg(fn, MIR_RC_GP, 8);
    MirVregId then_r = mir_new_vreg(fn, MIR_RC_GP, 8);
    MirVregId res_r = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (cond_r == MIR_VREG_NONE || then_r == MIR_VREG_NONE ||
        res_r == MIR_VREG_NONE) {
      return 0;
    }
    if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(cond_r), cond, mir_op_none(), 8, 0,
                   0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(then_r), then_v, mir_op_none(), 8,
                   0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(res_r), else_v, mir_op_none(), 8, 0,
                   0) ||
        !mir_emit1(fn, MIR_CMOV, mir_op_vreg(res_r), mir_op_vreg(cond_r),
                   mir_op_vreg(then_r), 8, 0, 0)) {
      return 0;
    }
    return mir_emit1(fn, MIR_MOV, dest, mir_op_vreg(res_r), mir_op_none(), 8, 0,
                     0);
  }

  case IR_OP_ROTATE_ADD: {
    MirOperand next = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand b = mir_value_operand(fn, g, ctx, map, &in->rhs);
    if (!mir_emit1(fn, MIR_ADD, next, a, b, 8, 0, 0)) {
      return 0;
    }
    int signed_home = 0;
    int cw = mir_dest_integer_narrow_width(g, ctx, &in->dest, &signed_home);
    if (cw && !mir_emit1(fn, signed_home ? MIR_MOVSX : MIR_MOVZX, next, next,
                         mir_op_none(), cw, !signed_home, 0)) {
      return 0;
    }
    return mir_emit1(fn, MIR_MOV, a, b, mir_op_none(), 8, 0, 0) &&
           mir_emit1(fn, MIR_MOV, b, next, mir_op_none(), 8, 0, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_alloc(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_NEW: {
    /* Zeroed heap allocation. Size: compile-time INT (>0), defaulted 8 (NONE
     * or <=0), or a runtime GP value. Win64: marshal size->R8 and emit the
     * inline GetProcessHeap+HeapAlloc(HEAP_ZERO_MEMORY) sequence; SysV:
     * calloc(1, size). Result moves out of RAX into the dest. */
    const BinaryAbi *nabi = code_generator_binary_active_abi();
    MirOperand sz;
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 0) {
      sz = mir_op_imm(in->rhs.int_value);
    } else if (in->rhs.kind == IR_OPERAND_NONE ||
               in->rhs.kind == IR_OPERAND_INT) {
      sz = mir_op_imm(8);
    } else {
      sz = mir_value_operand(fn, g, ctx, map, &in->rhs);
    }
    if (nabi->shadow_space_size > 0) {
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), sz,
                     mir_op_none(), 8, 0, 0) ||
          !mir_emit1(fn, MIR_HEAP_NEW, mir_op_none(), mir_op_none(),
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    } else {
      if (!code_generator_binary_declare_external_symbol(g, "calloc")) {
        fn->has_error = 1;
        return 0;
      }
      if (!mir_emit1(fn, MIR_MOV,
                     mir_op_phys(nabi->int_param_registers[0], MIR_RC_GP),
                     mir_op_imm(1), mir_op_none(), 8, 0, 0) ||
          !mir_emit1(fn, MIR_MOV,
                     mir_op_phys(nabi->int_param_registers[1], MIR_RC_GP), sz,
                     mir_op_none(), 8, 0, 0) ||
          !mir_emit1(fn, MIR_CALL, mir_op_symbol("calloc"), mir_op_none(),
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    MirOperand ndst = mir_value_operand(fn, g, ctx, map, &in->dest);
    return mir_emit1(fn, MIR_MOV, ndst, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                     mir_op_none(), 8, 0, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_return(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_RETURN: {
    if (!fn->returns_indirect && in->lhs.kind == IR_OPERAND_STRING) {
      if (!mir_emit_global_writebacks(fn, g, map, wb) ||
          !mir_emit1(fn, MIR_LEA_CSTR, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                     mir_op_symbol(in->lhs.name ? in->lhs.name : ""),
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
      return mir_emit1(fn, MIR_RET, mir_op_none(), mir_op_none(), mir_op_none(),
                       8, 0, 0);
    }
    if (fn->returns_indirect && (in->lhs.kind == IR_OPERAND_SYMBOL ||
                                 in->lhs.kind == IR_OPERAND_TEMP ||
                                 in->lhs.kind == IR_OPERAND_STRING)) {
      /* INDIRECT struct return: copy the struct into the caller's hidden slot
       * (whose pointer the prologue homed into indirect_return_vreg), then
       * leave that pointer in RAX as the Win64/SysV ABI requires. The source
       * is a struct local's stack home or a by-ref param's pointer. */
      const IRFunction *rirf =
          ctx && ctx->function_name
              ? code_generator_find_ir_function_binary(g, ctx->function_name)
              : NULL;
      if (fn->indirect_return_vreg == MIR_VREG_NONE) {
        fn->has_error = 1;
        return 0;
      }
      MirVregId src_base = mir_emit_indirect_source_addr(
          fn, g, ctx, map, rirf, &in->lhs, fn->indirect_return_size);
      if (src_base == MIR_VREG_NONE ||
          !mir_emit_struct_copy(fn, fn->indirect_return_vreg, src_base,
                                fn->indirect_return_size)) {
        return 0;
      }
      /* Writeback before the RAX move (the flush uses RAX as scratch). */
      if (!mir_emit_global_writebacks(fn, g, map, wb)) {
        return 0;
      }
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                     mir_op_vreg(fn->indirect_return_vreg), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
      return mir_emit1(fn, MIR_RET, mir_op_none(), mir_op_none(), mir_op_none(),
                       8, 0, 0);
    }
    /* Flush register-promoted globals to memory BEFORE materializing the return
     * value. The writeback uses RAX as scratch to store spilled (memory-homed)
     * global caches, so doing it after the return value is placed in RAX would
     * clobber that value (e.g. `return some_global` corrupted by a later cache
     * flush). The writeback only reads cache vregs and stores to memory, so the
     * return source vreg is still valid afterwards. */
    if (!mir_emit_global_writebacks(fn, g, map, wb)) {
      return 0;
    }
    if (in->lhs.kind != IR_OPERAND_NONE) {
      MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
      int rfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
      if (rfb) {
        /* Float return value goes in XMM0, converted to the DECLARED return
         * width: a float64-tracked temp returned from a float32 function
         * narrows via cvtsd2ss (a raw movss would hand the caller the low
         * dword of a double). */
        int want = fn->float_return_bits ? fn->float_return_bits : rfb;
        if (want != rfb) {
          MirVregId tmp = mir_new_vreg(fn, MIR_RC_XMM, want / 8);
          if (tmp == MIR_VREG_NONE ||
              !mir_emit1(fn, MIR_CVTF2F, mir_op_vreg(tmp), src, mir_op_none(),
                         want / 8, 0, 0)) {
            return 0;
          }
          src = mir_op_vreg(tmp);
        }
        if (!mir_emit_fmov(fn, mir_op_phys(BINARY_XMM0, MIR_RC_XMM), src,
                           want / 8)) {
          return 0;
        }
      } else if ((fn->scalar_return_width == 1 || fn->scalar_return_width == 2 ||
                  fn->scalar_return_width == 4) &&
                 !mir_return_literal_is_canonical(fn, &in->lhs)) {
        /* Canonicalize a narrow integer return to 64 bits (the high RAX bits
         * are ABI-undefined for a sub-64-bit return, and MIR may have left
         * garbage there) so a caller using the full register is correct. */
        if (!mir_emit1(fn,
                       fn->scalar_return_signed ? MIR_MOVSX : MIR_MOVZX,
                       mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), src, mir_op_none(),
                       fn->scalar_return_width, !fn->scalar_return_signed, 0)) {
          return 0;
        }
      } else if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                            src, mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    return mir_emit1(fn, MIR_RET, mir_op_none(), mir_op_none(), mir_op_none(), 8,
                     0, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

typedef struct {
  MirFunction *fn;
  CodeGenerator *g;
  BinaryFunctionContext *ctx;
  MirNameMap *map;
  const IRInstruction *in;
  const BinaryAbi *abi;
  const CgSym *call_callee;
  const BinaryArgLocation *locs;
  const int *indirect_off;
  const int *arg_is_float;
  int hidden;
} MirCallArgs;

static int mir_lower_runtime_trap(MirFunction *fn, CodeGenerator *g,
                                  BinaryFunctionContext *ctx, MirNameMap *map,
                                  const IRInstruction *in) {
  int is_ex = strcmp(in->text, "mettle_crash_trap_ex") == 0;
  int msg_idx = is_ex ? 1 : 0;
  const char *msg = "";
  MirOperand detail = mir_op_none();
  if ((size_t)msg_idx < in->argument_count &&
      in->arguments[msg_idx].kind == IR_OPERAND_STRING &&
      in->arguments[msg_idx].name) {
    msg = in->arguments[msg_idx].name;
  }
  if (is_ex && g->generate_stack_trace_support && in->argument_count >= 4) {
    MirOperand second = mir_value_operand(fn, g, ctx, map, &in->arguments[3]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R10, MIR_RC_GP), second,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    detail = mir_value_operand(fn, g, ctx, map, &in->arguments[2]);
  }
  return mir_emit1(fn, MIR_TRAP, mir_op_none(), mir_op_symbol(msg), detail, 8,
                   0, 0);
}

static int mir_call_is_runtime_hook(const IRInstruction *in) {
  return in->text && (strncmp(in->text, "mettle_profile_", 15) == 0 ||
                      strncmp(in->text, "mettle_dbg_", 11) == 0);
}

static int mir_runtime_hook_is_supported(const IRInstruction *in) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  if (in->dest.kind != IR_OPERAND_NONE) {
    mir_call_trace("hook_dest");
    return 0;
  }
  if (in->argument_count > abi->int_param_count) {
    mir_call_trace("hook_args>registers");
    return 0;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    int kind = in->arguments[a].kind;
    if (kind != IR_OPERAND_INT && kind != IR_OPERAND_TEMP &&
        kind != IR_OPERAND_SYMBOL) {
      mir_call_trace("hook_arg_kind");
      return 0;
    }
  }
  return 1;
}

static int mir_lower_runtime_hook(MirFunction *fn, CodeGenerator *g,
                                  BinaryFunctionContext *ctx, MirNameMap *map,
                                  const IRInstruction *in) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  if (!code_generator_binary_declare_external_symbol(g, in->text)) {
    fn->has_error = 1;
    return 0;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    MirOperand value = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
    if (!mir_emit1(fn, MIR_MOV,
                   mir_op_phys(abi->int_param_registers[a], MIR_RC_GP), value,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
  }
  return mir_emit1(fn, MIR_CALL, mir_op_symbol(in->text), mir_op_none(),
                   mir_op_none(), 8, 0, 0);
}

static int mir_lower_syscall(MirFunction *fn, CodeGenerator *g,
                             BinaryFunctionContext *ctx, MirNameMap *map,
                             const IRInstruction *in) {
  const BinaryGpRegister *registers = NULL;
  size_t register_count = 0;
  size_t stacked = 0;
  size_t arguments = 0;

  if (!mir_syscall_operand_split(in, &registers, &register_count, &stacked)) {
    fn->has_error = 1;
    return 0;
  }
  arguments = in->argument_count - 1;
  if (stacked > 0) {
    const BinaryAbi *abi = code_generator_binary_active_abi();
    int needed = MIR_SYSCALL_NT_STACK_OFFSET - abi->shadow_space_size +
                 (int)(stacked * 8u);
    if (needed > fn->outgoing_stack_bytes) {
      fn->outgoing_stack_bytes = needed;
    }
    for (size_t k = 0; k < stacked; k++) {
      MirOperand value = mir_value_operand(
          fn, g, ctx, map, &in->arguments[register_count + 1 + k]);
      if (!mir_emit1(fn, MIR_STORE_OUTARG, mir_op_none(), value,
                     mir_op_imm(MIR_SYSCALL_NT_STACK_OFFSET + (int)(k * 8u)), 8,
                     0, 0)) {
        return 0;
      }
    }
  }
  for (size_t k = 0; k < arguments && k < register_count; k++) {
    if (registers[k] == BINARY_GP_R10) {
      continue;
    }
    MirOperand value = mir_value_operand(fn, g, ctx, map, &in->arguments[k + 1]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(registers[k], MIR_RC_GP), value,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
  }
  {
    MirOperand number = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), number,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
  }
  for (size_t k = 0; k < arguments && k < register_count; k++) {
    if (registers[k] != BINARY_GP_R10) {
      continue;
    }
    MirOperand value = mir_value_operand(fn, g, ctx, map, &in->arguments[k + 1]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R10, MIR_RC_GP), value,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
  }
  if (!mir_emit1(fn, MIR_SYSCALL, mir_op_none(), mir_op_none(), mir_op_none(),
                 8, 0, 0)) {
    return 0;
  }
  if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
    return 1;
  }
  {
    MirOperand destination = mir_value_operand(fn, g, ctx, map, &in->dest);
    return mir_emit1(fn, MIR_MOV, destination,
                     mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), mir_op_none(), 8, 0,
                     0);
  }
}

static int mir_marshal_stack_args(const MirCallArgs *c) {
  MirFunction *fn = c->fn;
  CodeGenerator *g = c->g;
  BinaryFunctionContext *ctx = c->ctx;
  MirNameMap *map = c->map;
  const IRInstruction *in = c->in;
  const BinaryAbi *abi = c->abi;
  const CgSym *call_callee = c->call_callee;
  const BinaryArgLocation *locs = c->locs;
  const int *indirect_off = c->indirect_off;
  const int *arg_is_float = c->arg_is_float;
  int hidden = c->hidden;
  (void)g;
  (void)ctx;
  (void)map;
  (void)abi;
  (void)call_callee;
  (void)indirect_off;
  (void)arg_is_float;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (locs[a + hidden].kind != BINARY_ARG_ON_STACK) {
      continue;
    }
    int slot = abi->shadow_space_size + locs[a + hidden].stack_offset;
    if (indirect_off[a] < 0 && arg_is_float[a + (size_t)hidden]) {
      MtlcType *fpt = (call_callee && call_callee->kind == CG_SYM_FUNCTION &&
                       call_callee->data.function.parameter_types)
                          ? call_callee->data.function.parameter_types[a]
                          : NULL;
      int pfb = fpt ? code_generator_binary_resolved_type_float_bits(fpt) : 0;
      if (pfb != 32 && pfb != 64) {
        pfb = 64;
      }
      if (!mir_emit_float_stack_arg(fn, g, ctx, map, &in->arguments[a], pfb,
                                    slot)) {
        return 0;
      }
      continue;
    }
    MirOperand val;
    if (indirect_off[a] >= 0) {
      /* INDIRECT struct arg: pass &copy_slot. */
      MirVregId t = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (t == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_LEA_OUTARG, mir_op_vreg(t),
                     mir_op_imm(indirect_off[a]), mir_op_none(), 8, 0, 0)) {
        return 0;
      }
      val = mir_op_vreg(t);
    } else if (in->arguments[a].kind == IR_OPERAND_STRING) {
      /* Stage the cstring address in a temp, then store it to the slot. */
      const char *s = in->arguments[a].name ? in->arguments[a].name : "";
      MirVregId t = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (t == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_LEA_CSTR, mir_op_vreg(t), mir_op_symbol(s),
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
      val = mir_op_vreg(t);
    } else {
      val = mir_gp_value_operand(fn, g, ctx, map, &in->arguments[a]);
    }
    if (!mir_emit1(fn, MIR_STORE_OUTARG, mir_op_none(), val,
                   mir_op_imm(slot), 8, 0, 0)) {
      return 0;
    }
  }
  return 1;
}

static int mir_marshal_gp_args(const MirCallArgs *c) {
  MirFunction *fn = c->fn;
  CodeGenerator *g = c->g;
  BinaryFunctionContext *ctx = c->ctx;
  MirNameMap *map = c->map;
  const IRInstruction *in = c->in;
  const BinaryAbi *abi = c->abi;
  const CgSym *call_callee = c->call_callee;
  const BinaryArgLocation *locs = c->locs;
  const int *indirect_off = c->indirect_off;
  const int *arg_is_float = c->arg_is_float;
  int hidden = c->hidden;
  (void)g;
  (void)ctx;
  (void)map;
  (void)abi;
  (void)call_callee;
  (void)indirect_off;
  (void)arg_is_float;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (locs[a + hidden].kind != BINARY_ARG_IN_GP_REGISTER) {
      continue;
    }
    BinaryGpRegister reg = locs[a + hidden].gp_register;
    if (indirect_off[a] >= 0) {
      /* INDIRECT struct arg: lea &copy_slot directly into the ABI arg reg. */
      if (!mir_emit1(fn, MIR_LEA_OUTARG, mir_op_phys(reg, MIR_RC_GP),
                     mir_op_imm(indirect_off[a]), mir_op_none(), 8, 0, 0)) {
        return 0;
      }
      continue;
    }
    if (in->arguments[a].kind == IR_OPERAND_STRING) {
      /* A string-literal argument is passed as the address of its .rdata
       * cstring (lea directly into the ABI argument register). */
      const char *s = in->arguments[a].name ? in->arguments[a].name : "";
      if (!mir_emit1(fn, MIR_LEA_CSTR, mir_op_phys(reg, MIR_RC_GP),
                     mir_op_symbol(s), mir_op_none(), 8, 0, 0)) {
        return 0;
      }
      continue;
    }
    MirOperand arg = mir_gp_value_operand(fn, g, ctx, map, &in->arguments[a]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(reg, MIR_RC_GP), arg,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
  }
  return 1;
}

static int mir_marshal_xmm_args(const MirCallArgs *c) {
  MirFunction *fn = c->fn;
  CodeGenerator *g = c->g;
  BinaryFunctionContext *ctx = c->ctx;
  MirNameMap *map = c->map;
  const IRInstruction *in = c->in;
  const BinaryAbi *abi = c->abi;
  const CgSym *call_callee = c->call_callee;
  const BinaryArgLocation *locs = c->locs;
  const int *indirect_off = c->indirect_off;
  const int *arg_is_float = c->arg_is_float;
  int hidden = c->hidden;
  (void)g;
  (void)ctx;
  (void)map;
  (void)abi;
  (void)call_callee;
  (void)indirect_off;
  (void)arg_is_float;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (locs[a + hidden].kind != BINARY_ARG_IN_XMM_REGISTER) {
      continue;
    }
    fn->has_xmm_arg_call = 1;
    BinaryXmmRegister xreg = locs[a + hidden].xmm_register;
    MtlcType *pt = (call_callee && call_callee->kind == CG_SYM_FUNCTION &&
                call_callee->data.function.parameter_types)
                   ? call_callee->data.function.parameter_types[a]
                   : NULL;
    int pfb = pt ? code_generator_binary_resolved_type_float_bits(pt) : 0;
    if (pfb != 32 && pfb != 64) {
      pfb = 64;
    }
    /* coerce handles every source: literals at the param width, float
     * values width-converted, int values via cvtsi2sd. */
    MirOperand val =
        coerce_float_operand(fn, g, ctx, map, &in->arguments[a], pfb / 8);
    if (val.kind == MIR_OPK_FIMM) {
      /* A float immediate cannot move straight into a physical register;
       * stage it in a vreg first. */
      MirVregId t = mir_new_vreg(fn, MIR_RC_XMM, pfb / 8);
      if (t == MIR_VREG_NONE ||
          !mir_emit_fmov(fn, mir_op_vreg(t), val, pfb / 8)) {
        return 0;
      }
      val = mir_op_vreg(t);
    }
    if (!mir_emit_fmov(fn, mir_op_phys(xreg, MIR_RC_XMM), val, pfb / 8)) {
      return 0;
    }
  }
  return 1;
}

static int mir_copy_indirect_args(MirFunction *fn, CodeGenerator *g,
                                  BinaryFunctionContext *ctx,
                                  MirNameMap *map,
                                  const IRInstruction *in,
                                  int *indirect_off) {
  const CgSym *call_callee =
      g->ir_program ? code_generator_lookup_symbol(g, in->text) : NULL;
  const IRFunction *cirf =
      ctx && ctx->function_name
          ? code_generator_find_ir_function_binary(g, ctx->function_name)
          : NULL;
  int indirect_region = 0;
  for (size_t a = 0; a < in->argument_count; a++) {
    indirect_off[a] = -1;
    MtlcType *pt = (call_callee && call_callee->kind == CG_SYM_FUNCTION &&
                call_callee->data.function.parameter_types)
                   ? call_callee->data.function.parameter_types[a]
                   : NULL;
    if (!pt || code_generator_abi_classify(pt) != ABI_PASS_INDIRECT) {
      continue;
    }
    int sz = (int)code_generator_abi_type_size(pt);
    indirect_off[a] = indirect_region;
    indirect_region += (sz + 7) & ~7;
    /* Copy the struct into the slot: from a local/temp home, through a
     * by-ref param's pointer, or from a string literal's .rdata record. */
    MirVregId src_base = mir_emit_indirect_source_addr(
        fn, g, ctx, map, cirf, &in->arguments[a], sz);
    MirVregId dst_base = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (src_base == MIR_VREG_NONE || dst_base == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_LEA_OUTARG, mir_op_vreg(dst_base),
                   mir_op_imm(indirect_off[a]), mir_op_none(), 8, 0, 0) ||
        !mir_emit_struct_copy(fn, dst_base, src_base, sz)) {
      return 0;
    }
  }
  if (indirect_region > 0) {
    indirect_region = (indirect_region + 15) & ~15;
    if (indirect_region > fn->outgoing_indirect_bytes) {
      fn->outgoing_indirect_bytes = indirect_region;
    }
  }
  return 1;
}

static int mir_emit_call_result(MirFunction *fn, CodeGenerator *g,
                                BinaryFunctionContext *ctx,
                                MirNameMap *map,
                                const IRInstruction *in,
                                int ret_indirect, int sysv_gp_return,
                                const BinarySysvAggregate *sysv_ret) {
  if (ret_indirect) {
    /* The struct result was written into the dest local's home by the callee;
     * nothing to move out of RAX. */
    return 1;
  }
  if (sysv_gp_return) {
    /* Take the eightbytes out of RAX and RDX first: the address of the
     * destination's home is computed after they are in vregs, so the
     * allocator cannot hand the address register one that still holds a
     * piece of the result. */
    MirVregId parts[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
    BinaryGpRegister srcs[2] = {BINARY_GP_RAX, BINARY_GP_RDX};
    size_t e = 0;
    for (e = 0; e < sysv_ret->eightbyte_count; e++) {
      parts[e] = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (parts[e] == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_MOV, mir_op_vreg(parts[e]),
                     mir_op_phys(srcs[e], MIR_RC_GP), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
    }
    MirOperand dstsym = mir_value_operand(fn, g, ctx, map, &in->dest);
    if (dstsym.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    fn->vregs[dstsym.vreg].address_taken = 1;
    {
      const IRFunction *dirf =
          ctx && ctx->function_name
              ? code_generator_find_ir_function_binary(g, ctx->function_name)
              : NULL;
      int hb = mir_operand_struct_home_size(g, dirf, &in->dest);
      if (hb > 0 && fn->vregs[dstsym.vreg].home_bytes < hb) {
        fn->vregs[dstsym.vreg].home_bytes = hb;
      }
    }
    MirVregId addr = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (addr == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(addr), dstsym,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    for (e = 0; e < sysv_ret->eightbyte_count; e++) {
      if (!mir_emit1(fn, MIR_MOV,
                     mir_op_mem_vreg(addr, MIR_VREG_NONE, 1, (int)(e * 8u)),
                     mir_op_vreg(parts[e]), mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    return 1;
  }
  /* Move the return value out of RAX / XMM0 before anything clobbers it. */
  if (in->dest.kind == IR_OPERAND_TEMP || in->dest.kind == IR_OPERAND_SYMBOL) {
    int rfb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    if (rfb) {
      return mir_emit_fmov(fn, dst, mir_op_phys(BINARY_XMM0, MIR_RC_XMM),
                           rfb / 8);
    }
    return mir_emit1(fn, MIR_MOV, dst,
                     mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), mir_op_none(), 8,
                     0, 0);
  }
  return 1;
}

static int mir_call_return_classification(CodeGenerator *g,
                                          const IRInstruction *in,
                                          int *ret_indirect,
                                          BinarySysvAggregate *sysv_ret) {
  int sysv_gp_return = 0;
  {
    const CgSym *rc =
        g->ir_program ? code_generator_lookup_symbol(g, in->text) : NULL;
    const MtlcType *rret = (rc && rc->kind == CG_SYM_FUNCTION)
                     ? (rc->data.function.return_type ? rc->data.function.return_type
                                                      : rc->type)
                     : NULL;
    if (rret &&
        mir_call_sysv_returns_in_gp_registers(g, in->text, rret, sysv_ret) &&
        (in->dest.kind == IR_OPERAND_SYMBOL ||
         in->dest.kind == IR_OPERAND_TEMP)) {
      sysv_gp_return = 1;
    } else if (rret && code_generator_abi_classify(rret) == ABI_PASS_INDIRECT &&
        (in->dest.kind == IR_OPERAND_SYMBOL ||
         in->dest.kind == IR_OPERAND_TEMP)) {
      *ret_indirect = 1;
    }
  }
  return sysv_gp_return;
}

static void mir_tag_float_arg_slots(CodeGenerator *g,
                                    const IRInstruction *in,
                                    int hidden, int *arg_is_float) {
  /* Tag each positional slot's float class from the callee's parameter types so
   * the ABI layout routes float args to XMM registers. Without this every arg
   * defaults to integer and a float arg is homed into a GP register (and a
   * float immediate then reaches the GP value path, an encoder error). */
  {
    const CgSym *fc = g->ir_program
                     ? code_generator_lookup_symbol(g, in->text)
                     : NULL;
    if (fc && fc->kind == CG_SYM_FUNCTION &&
        fc->data.function.parameter_types) {
      for (size_t a = 0; a < in->argument_count &&
                         a < fc->data.function.parameter_count;
           a++) {
        MtlcType *pt = fc->data.function.parameter_types[a];
        if (pt && code_generator_binary_resolved_type_float_bits(pt) != 0) {
          arg_is_float[a + (size_t)hidden] = 1;
        }
      }
    }
  }
}

static int mir_lower_call(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_CALL: {
    /* A failed-safety-check trap: lower to a terminal MIR_TRAP carrying the
     * abort message (the STRING argument). MIR only runs without stack-trace
     * support, so the trap degrades to puts(message)+exit(1); the remaining
     * trap arguments (kind, pc, rbp) are unused on that path. */
    if (mir_call_is_runtime_trap(in)) {
      return mir_lower_runtime_trap(fn, g, ctx, map, in);
    }
    if (mir_call_is_runtime_hook(in)) {
      return mir_lower_runtime_hook(fn, g, ctx, map, in);
    }
    if (mir_call_is_syscall(in)) {
      return mir_lower_syscall(fn, g, ctx, map, in);
    }
    /* Declare external callees so the linker resolves the relocation. */
    IRFunction *target = code_generator_find_ir_function_binary(g, in->text);
    if (!target) {
      const char *link = code_generator_get_link_symbol_name(g, in->text);
      if (link && !code_generator_binary_declare_external_symbol(g, link)) {
        fn->has_error = 1;
        return 0;
      }
    }
    /* Marshal arguments per the ABI layout. Arguments up to the ABI's
     * argument-register count go into registers (GP, or XMM for float args); the
     * rest are stored into the outgoing stack-argument region (reserved once in
     * the prologue). Eligibility guarantees every float arg lands in an XMM
     * register (float stack args still bail). */
    const BinaryAbi *abi = code_generator_binary_active_abi();
    /* Caller-side INDIRECT return: the callee returns a struct by value, so the
     * ABI passes a hidden out-pointer as the first integer arg, shifting every
     * user arg up one slot. We point the hidden arg at the destination struct
     * LOCAL's home so the callee writes the result there directly. */
    int ret_indirect = 0;
    /* SysV brings a small aggregate back in RAX/RDX instead, so the call takes
     * no hidden out-pointer and the result is spilled into the destination's
     * home after it returns. */
    BinarySysvAggregate sysv_ret = {0};
    int sysv_gp_return = mir_call_return_classification(
        g, in, &ret_indirect, &sysv_ret);
    int hidden = ret_indirect ? 1 : 0;
    int arg_is_float[MIR_MAX_PARAMS + 1] = {0};
    mir_tag_float_arg_slots(g, in, hidden, arg_is_float);
    BinaryArgLocation locs[MIR_MAX_PARAMS + 1];
    int stack_bytes = 0;
    size_t nlocs = in->argument_count + (size_t)hidden;
    if (nlocs > (size_t)(MIR_MAX_PARAMS + 1)) {
      fn->has_error = 1;
      return 0;
    }
    if (nlocs > 0 &&
        !code_generator_binary_compute_arg_layout(abi, arg_is_float, nlocs, locs,
                                                  &stack_bytes)) {
      fn->has_error = 1;
      return 0;
    }
    if (stack_bytes > fn->outgoing_stack_bytes) {
      fn->outgoing_stack_bytes = stack_bytes;
    }
    /* INDIRECT (by-value) struct arguments: the ABI passes a pointer to a
     * caller-made copy. Lay out a copy slot per such arg in the outgoing
     * indirect region (at the bottom of the frame), copy each struct there, and
     * pass &slot as the (integer) argument value. Eligibility has proven every
     * INDIRECT arg is a struct LOCAL, so its source is its stack home. */
    int indirect_off[MIR_MAX_PARAMS] = {0};
    const CgSym *call_callee =
        g->ir_program ? code_generator_lookup_symbol(g, in->text) : NULL;
    if (!mir_copy_indirect_args(fn, g, ctx, map, in, indirect_off)) {
      return 0;
    }
    {
      MirCallArgs args;
      args.fn = fn;
      args.g = g;
      args.ctx = ctx;
      args.map = map;
      args.in = in;
      args.abi = abi;
      args.call_callee = call_callee;
      args.locs = locs;
      args.indirect_off = indirect_off;
      args.arg_is_float = arg_is_float;
      args.hidden = hidden;
      if (!mir_marshal_stack_args(&args) || !mir_marshal_gp_args(&args) ||
          !mir_marshal_xmm_args(&args)) {
        return 0;
      }
    }
    /* Hidden INDIRECT-return pointer: lea the destination struct local's home
     * into the ABI's out-pointer register (slot 0). The callee writes the
     * returned struct directly there, so no post-call copy is needed. */
    if (ret_indirect) {
      MirOperand dstsym = mir_value_operand(fn, g, ctx, map, &in->dest);
      if (dstsym.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      fn->vregs[dstsym.vreg].address_taken = 1;
      /* Size the dest's home to the whole struct (a struct LOCAL or struct TEMP
       *: mir_operand_struct_home_size resolves a temp's size from the IR). */
      {
        const IRFunction *dirf =
            ctx && ctx->function_name
                ? code_generator_find_ir_function_binary(g, ctx->function_name)
                : NULL;
        int hb = mir_operand_struct_home_size(g, dirf, &in->dest);
        if (hb > 0 && fn->vregs[dstsym.vreg].home_bytes < hb) {
          fn->vregs[dstsym.vreg].home_bytes = hb;
        }
      }
      if (!mir_emit1(fn, MIR_LEA_LOCAL,
                     mir_op_phys(abi->indirect_return_register, MIR_RC_GP),
                     dstsym, mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    /* memcpy/memset with the ordinary three arguments: the marshalling above
     * already put them where the inline string operation wants them, so the
     * call becomes the operation itself. Matches what the fallback emitter
     * does; see MIR_REP_MOVSB. memmove keeps its call -- the overlap test is
     * worth a real callee, and being slower than the fallback there is not a
     * correctness matter. */
    MirOpcode call_op = MIR_CALL;
    if (in->argument_count == 3 && in->text && !ret_indirect) {
      if (strcmp(in->text, "memcpy") == 0) {
        call_op = MIR_REP_MOVSB;
      } else if (strcmp(in->text, "memset") == 0) {
        call_op = MIR_REP_STOSB;
      }
    }
    if (!mir_emit1(fn, call_op, mir_op_symbol(in->text), mir_op_none(),
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    /* The two `--safe` runtime calls that sit in or around hot loops, marked by
     * name because they are the ones the checking machinery puts there.
     *
     * The check is reached only when the comparison in front of it fails, which
     * on a correct program is never, so saving anything around it is free. The
     * span resolution does run, but once in front of the loop rather than per
     * element, so it is worth eight instructions to leave the loop body's float
     * registers alone. Span returns its answer in RAX and so cannot promise
     * it. See MirVreg::crosses_preserving_only. */
    if (call_op == MIR_CALL && in->text) {
      MirInst *call = &fn->insns[fn->insn_count - 1];
      if (strcmp(in->text, "mettle_safety_check") == 0) {
        call->preserves_rax = 1;
        call->preserves_xmm = 1;
      } else if (strcmp(in->text, "mettle_safety_span") == 0) {
        call->preserves_xmm = 1;
      }
    }
    if (!mir_emit_call_result(fn, g, ctx, map, in, ret_indirect,
                              sysv_gp_return, &sysv_ret)) {
      return 0;
    }
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_marshal_indirect_stack(MirFunction *fn, CodeGenerator *g,
                             BinaryFunctionContext *ctx, MirNameMap *map,
                             const IRInstruction *in, MtlcType *ft,
                             const BinaryAbi *abi,
                             const BinaryArgLocation *locs,
                             const int *arg_is_float) {
  (void)ft;
  (void)arg_is_float;
  (void)g;
  (void)ctx;
  (void)map;
  (void)abi;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (locs[a].kind != BINARY_ARG_ON_STACK) {
      continue;
    }
    int slot = abi->shadow_space_size + locs[a].stack_offset;
    if (arg_is_float[a]) {
      MtlcType *fpt =
          (ft && ft->fn_param_types) ? ft->fn_param_types[a] : NULL;
      int pfb = fpt ? code_generator_binary_resolved_type_float_bits(fpt) : 0;
      if (pfb != 32 && pfb != 64) {
        pfb = 64;
      }
      if (!mir_emit_float_stack_arg(fn, g, ctx, map, &in->arguments[a], pfb,
                                    slot)) {
        return 0;
      }
      continue;
    }
    MirOperand val;
    if (in->arguments[a].kind == IR_OPERAND_STRING) {
      const char *s = in->arguments[a].name ? in->arguments[a].name : "";
      MirVregId t = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (t == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_LEA_CSTR, mir_op_vreg(t), mir_op_symbol(s),
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
      val = mir_op_vreg(t);
    } else {
      val = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
    }
    if (!mir_emit1(fn, MIR_STORE_OUTARG, mir_op_none(), val,
                   mir_op_imm(slot), 8, 0, 0)) {
      return 0;
    }
  }
  return 1;
}

static int mir_marshal_indirect_gp(MirFunction *fn, CodeGenerator *g,
                             BinaryFunctionContext *ctx, MirNameMap *map,
                             const IRInstruction *in, MtlcType *ft,
                             const BinaryAbi *abi,
                             const BinaryArgLocation *locs,
                             const int *arg_is_float) {
  (void)ft;
  (void)arg_is_float;
  (void)g;
  (void)ctx;
  (void)map;
  (void)abi;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (locs[a].kind != BINARY_ARG_IN_GP_REGISTER) {
      continue;
    }
    BinaryGpRegister reg = locs[a].gp_register;
    if (in->arguments[a].kind == IR_OPERAND_STRING) {
      const char *s = in->arguments[a].name ? in->arguments[a].name : "";
      if (!mir_emit1(fn, MIR_LEA_CSTR, mir_op_phys(reg, MIR_RC_GP),
                     mir_op_symbol(s), mir_op_none(), 8, 0, 0)) {
        return 0;
      }
      continue;
    }
    MirOperand arg = mir_gp_value_operand(fn, g, ctx, map, &in->arguments[a]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(reg, MIR_RC_GP), arg,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
  }
  return 1;
}

static int mir_marshal_indirect_xmm(MirFunction *fn, CodeGenerator *g,
                             BinaryFunctionContext *ctx, MirNameMap *map,
                             const IRInstruction *in, MtlcType *ft,
                             const BinaryAbi *abi,
                             const BinaryArgLocation *locs,
                             const int *arg_is_float) {
  (void)ft;
  (void)arg_is_float;
  (void)g;
  (void)ctx;
  (void)map;
  (void)abi;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (locs[a].kind != BINARY_ARG_IN_XMM_REGISTER) {
      continue;
    }
    fn->has_xmm_arg_call = 1;
    BinaryXmmRegister xreg = locs[a].xmm_register;
    MtlcType *pt =
        (ft && ft->fn_param_types) ? ft->fn_param_types[a] : NULL;
    int pfb = code_generator_binary_resolved_type_float_bits(pt);
    if (pfb != 32 && pfb != 64) {
      pfb = 64;
    }
    MirOperand val =
        coerce_float_operand(fn, g, ctx, map, &in->arguments[a], pfb / 8);
    if (val.kind == MIR_OPK_FIMM) {
      MirVregId t = mir_new_vreg(fn, MIR_RC_XMM, pfb / 8);
      if (t == MIR_VREG_NONE ||
          !mir_emit_fmov(fn, mir_op_vreg(t), val, pfb / 8)) {
        return 0;
      }
      val = mir_op_vreg(t);
    }
    if (!mir_emit_fmov(fn, mir_op_phys(xreg, MIR_RC_XMM), val, pfb / 8)) {
      return 0;
    }
  }
  return 1;
}

static int mir_lower_call_indirect(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_CALL_INDIRECT: {
    const IRFunction *irf =
        ctx && ctx->function_name
            ? code_generator_find_ir_function_binary(g, ctx->function_name)
            : NULL;
    MtlcType *ft = mir_indirect_call_type(g, irf, in);
    if (!ft && in->lhs.kind != IR_OPERAND_TEMP) {
      fn->has_error = 1;
      return 0;
    }
    const BinaryAbi *abi = code_generator_binary_active_abi();
    int arg_is_float[MIR_MAX_PARAMS] = {0};
    BinaryArgLocation locs[MIR_MAX_PARAMS];
    int stack_bytes = 0;
    for (size_t a = 0; a < in->argument_count; a++) {
      MtlcType *pt =
          (ft && ft->fn_param_types) ? ft->fn_param_types[a] : NULL;
      arg_is_float[a] =
          code_generator_binary_resolved_type_float_bits(pt) != 0;
    }
    if (in->argument_count > 0 &&
        !code_generator_binary_compute_arg_layout(abi, arg_is_float,
                                                  in->argument_count, locs,
                                                  &stack_bytes)) {
      fn->has_error = 1;
      return 0;
    }
    if (stack_bytes > fn->outgoing_stack_bytes) {
      fn->outgoing_stack_bytes = stack_bytes;
    }

    MirOperand callee = mir_value_operand(fn, g, ctx, map, &in->lhs);

    if (!mir_marshal_indirect_stack(fn, g, ctx, map, in, ft, abi, locs,
                                    arg_is_float) ||
        !mir_marshal_indirect_gp(fn, g, ctx, map, in, ft, abi, locs,
                                 arg_is_float) ||
        !mir_marshal_indirect_xmm(fn, g, ctx, map, in, ft, abi, locs,
                                  arg_is_float)) {
      return 0;
    }

    if (!mir_emit1(fn, MIR_CALL_INDIRECT, mir_op_none(),
                   callee, mir_op_none(), 8, 0, 0)) {
      return 0;
    }

    if (in->dest.kind == IR_OPERAND_TEMP || in->dest.kind == IR_OPERAND_SYMBOL) {
      int rfb = ft ? code_generator_binary_resolved_type_float_bits(
                         ft->fn_return_type)
                   : 0;
      MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
      if (rfb) {
        return mir_emit_fmov(fn, dst, mir_op_phys(BINARY_XMM0, MIR_RC_XMM),
                             rfb / 8);
      }
      return mir_emit1(fn, MIR_MOV, dst,
                       mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), mir_op_none(), 8,
                       0, 0);
    }
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_slp_mac(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SLP_MAC_I32: {
    /* Inline SLP MAC kernel. Marshal the three effective element pointers
     * (base + offset*4), the k count, and the byte row stride into
     * RCX/RDX/R8/R9/RAX: like call-argument setup, then emit the pure-loop MIR
     * op. The lane count K is a compile-time constant (validated in
     * eligibility); the kernel advances b by the RAX stride each iteration. The
     * op is treated like a call by the allocator, so no live value occupies a
     * volatile across it.
     *
     * Compute every value into a vreg FIRST, then do all the fixed-register MOVs
     * LAST: the MIR_LEA encoder stages spilled base/index through RDX/R11, which
     * would otherwise clobber a kernel argument already parked in RDX. */
    long long K = in->arguments[0].int_value;
    /* Element size per pointer. int32 SLP: a/b/out all 4-byte. int8 SLP: a and
     * b are int8 arrays (1-byte), out (c) is int32 (4-byte). The stride (b's
     * per-k row advance) is in the same units as b, so it scales by b's element
     * size. The MIR op's `width` carries b's element size so the encoder picks
     * the int8-widening kernel. */
    int is_i8 = (in->op == IR_OP_SIMD_SLP_MAC_I8);
    const int elem[3] = {is_i8 ? 1 : 4, is_i8 ? 1 : 4, 4}; /* a, b, out */
    const IROperand *bases[3] = {&in->lhs, &in->rhs, &in->dest}; /* a, b, out */
    const int off_arg[3] = {2, 3, 5};
    MirVregId ptr_vreg[3];
    for (int p = 0; p < 3; p++) {
      MirOperand base = mir_value_operand(fn, g, ctx, map, bases[p]);
      MirOperand off = mir_value_operand(fn, g, ctx, map, &in->arguments[off_arg[p]]);
      if (base.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      ptr_vreg[p] = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (ptr_vreg[p] == MIR_VREG_NONE) {
        return 0;
      }
      MirOperand mem;
      if (off.kind == MIR_OPK_IMM) {
        mem = mir_op_mem_vreg(base.vreg, MIR_VREG_NONE, 0,
                              (int)(off.imm * elem[p]));
      } else if (off.kind == MIR_OPK_VREG) {
        mem = mir_op_mem_vreg(base.vreg, off.vreg, elem[p], 0);
      } else {
        fn->has_error = 1;
        return 0;
      }
      if (!mir_emit1(fn, MIR_LEA, mir_op_vreg(ptr_vreg[p]), mem, mir_op_none(),
                     8, 0, 0)) {
        return 0;
      }
    }
    /* byte row stride into a vreg (stride_elems * b's element size). */
    int stride_elem = elem[1];
    MirOperand stride = mir_value_operand(fn, g, ctx, map, &in->arguments[4]);
    MirVregId stride_vreg = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (stride_vreg == MIR_VREG_NONE) {
      return 0;
    }
    if (stride.kind == MIR_OPK_IMM) {
      if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(stride_vreg),
                     mir_op_imm(stride.imm * stride_elem), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
    } else if (stride.kind == MIR_OPK_VREG && stride_elem == 4) {
      if (!mir_emit1(fn, MIR_SHL, mir_op_vreg(stride_vreg), stride,
                     mir_op_imm(2), 8, 0, 0)) {
        return 0;
      }
    } else if (stride.kind == MIR_OPK_VREG) { /* stride_elem == 1: no scaling */
      if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(stride_vreg), stride,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    } else {
      fn->has_error = 1;
      return 0;
    }
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[1]);
    MirVregId cnt_vreg = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (cnt_vreg == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(cnt_vreg), cnt, mir_op_none(), 8, 0,
                   0)) {
      return 0;
    }
    /* Now park each computed value in its kernel register (no LEAs left to
     * clobber them). RCX=a, RDX=b, R8=out, R9=count, RAX=byte stride. */
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP),
                   mir_op_vreg(ptr_vreg[0]), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP),
                   mir_op_vreg(ptr_vreg[1]), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP),
                   mir_op_vreg(ptr_vreg[2]), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R9, MIR_RC_GP),
                   mir_op_vreg(cnt_vreg), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                   mir_op_vreg(stride_vreg), mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    /* width = b's element size (1 = int8-widening kernel, 4 = int32 kernel). */
    return mir_emit1(fn, MIR_SIMD_SLP_MAC, mir_op_imm(K), mir_op_none(),
                     mir_op_none(), elem[1], 0, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_fill_counted_writeback(MirFunction *fn, CodeGenerator *g,
                                      BinaryFunctionContext *ctx,
                                      MirNameMap *map,
                                      const IRInstruction *in,
                                      MirOperand cnt, MirOperand m0_start,
                                      int m0_start_zero) {
    /* Final iv = start + max(bound-start, 0); cnt already holds bound-start
     * (or the plain bound when start is 0). */
    MirOperand iv = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirVregId mask = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (mask == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_SAR, mir_op_vreg(mask), cnt, mir_op_imm(63), 8, 0,
                   0) ||
        !mir_emit1(fn, MIR_NOT, mir_op_vreg(mask), mir_op_vreg(mask),
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    if (m0_start_zero) {
      if (!mir_emit1(fn, MIR_AND, iv, cnt, mir_op_vreg(mask), 8, 0, 0)) {
        return 0;
      }
    } else {
      MirVregId w = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (w == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_AND, mir_op_vreg(w), cnt, mir_op_vreg(mask), 8,
                     0, 0) ||
          !mir_emit1(fn, MIR_ADD, iv, mir_op_vreg(w), m0_start, 8, 0, 0)) {
        return 0;
      }
    }
  return 1;
}

static int mir_fill_offset_writeback(MirFunction *fn, CodeGenerator *g,
                                     BinaryFunctionContext *ctx,
                                     MirNameMap *map,
                                     const IRInstruction *in,
                                     MirOperand cnt, MirOperand m2_start,
                                     int m2_start_zero, long long size) {
    MirOperand iv = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand walked = cnt;
    if (size > 1) {
      MirVregId w1 = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId w2 = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (w1 == MIR_VREG_NONE || w2 == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_ADD, mir_op_vreg(w1), cnt, mir_op_imm(size - 1),
                     8, 0, 0) ||
          !mir_emit1(fn, MIR_AND, mir_op_vreg(w2), mir_op_vreg(w1),
                     mir_op_imm(-size), 8, 0, 0)) {
        return 0;
      }
      walked = mir_op_vreg(w2);
    }
    MirVregId mask = mir_new_vreg(fn, MIR_RC_GP, 8);
    MirVregId wm = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (mask == MIR_VREG_NONE || wm == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_SAR, mir_op_vreg(mask), cnt, mir_op_imm(63), 8, 0,
                   0) ||
        !mir_emit1(fn, MIR_NOT, mir_op_vreg(mask), mir_op_vreg(mask),
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_AND, mir_op_vreg(wm), walked, mir_op_vreg(mask), 8,
                   0, 0)) {
      return 0;
    }
    if (m2_start_zero) {
      if (!mir_emit1(fn, MIR_MOV, iv, mir_op_vreg(wm), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
    } else if (!mir_emit1(fn, MIR_ADD, iv, mir_op_vreg(wm), m2_start, 8, 0,
                          0)) {
      return 0;
    }
  return 1;
}

static int mir_fill_fold_counted(MirFunction *fn, CodeGenerator *g,
                                 BinaryFunctionContext *ctx, MirNameMap *map,
                                 const IRInstruction *in, MirOperand *base_io,
                                 MirOperand *cnt_io, MirOperand *m0_start_io,
                                 int *m0_start_zero_io, long long size) {
  MirOperand base = *base_io;
  MirOperand cnt = *cnt_io;
  MirOperand m0_start = *m0_start_io;
  int m0_start_zero = *m0_start_zero_io;
  m0_start_zero = (in->arguments[3].kind == IR_OPERAND_INT &&
                   in->arguments[3].int_value == 0);
  int m0_off_zero = (in->arguments[4].kind == IR_OPERAND_INT &&
                     in->arguments[4].int_value == 0);
  if (!m0_start_zero) {
    int m0_wide = in->argument_count > 5 &&
                  in->arguments[5].kind == IR_OPERAND_INT &&
                  in->arguments[5].int_value == 64;
    m0_start = mir_value_operand(fn, g, ctx, map, &in->arguments[3]);
    if (cnt.kind != MIR_OPK_VREG) {
      MirVregId lc = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (lc == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_MOV, mir_op_vreg(lc), cnt, mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
      cnt = mir_op_vreg(lc);
    }
    MirVregId nc = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (nc == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_SUB, mir_op_vreg(nc), cnt, m0_start,
                   m0_wide ? 8 : 4, 0, 0)) {
      return 0;
    }
    if (!m0_wide &&
        !mir_emit1(fn, MIR_MOVSX, mir_op_vreg(nc), mir_op_vreg(nc),
                   mir_op_none(), 4, 0, 0)) {
      return 0;
    }
    cnt = mir_op_vreg(nc);
  }
  if (!m0_off_zero || !m0_start_zero) {
    MirOperand eff;
    if (m0_off_zero) {
      eff = m0_start;
    } else if (m0_start_zero) {
      eff = mir_value_operand(fn, g, ctx, map, &in->arguments[4]);
    } else {
      MirOperand off = mir_value_operand(fn, g, ctx, map, &in->arguments[4]);
      MirVregId sum = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (sum == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_ADD, mir_op_vreg(sum), off, m0_start, 8, 0,
                     0)) {
        return 0;
      }
      eff = mir_op_vreg(sum);
    }
    if (eff.kind == MIR_OPK_IMM) {
      MirVregId ev = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (ev == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_MOV, mir_op_vreg(ev), eff, mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
      eff = mir_op_vreg(ev);
    }
    MirVregId scaled = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (scaled == MIR_VREG_NONE) {
      return 0;
    }
    int shift = (size == 8) ? 3 : (size == 4) ? 2 : (size == 2) ? 1 : 0;
    if (shift > 0) {
      if (!mir_emit1(fn, MIR_SHL, mir_op_vreg(scaled), eff,
                     mir_op_imm(shift), 8, 0, 0)) {
        return 0;
      }
    } else if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(scaled), eff,
                          mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    MirVregId adj = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (adj == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_ADD, mir_op_vreg(adj), base, mir_op_vreg(scaled),
                   8, 0, 0)) {
      return 0;
    }
    base = mir_op_vreg(adj);
  }
  *base_io = base;
  *cnt_io = cnt;
  *m0_start_io = m0_start;
  *m0_start_zero_io = m0_start_zero;
  return 1;
}

static int mir_fill_fold_offset(MirFunction *fn, CodeGenerator *g,
                                BinaryFunctionContext *ctx, MirNameMap *map,
                                const IRInstruction *in, MirOperand *base_io,
                                MirOperand *cnt_io, MirOperand *m2_start_io,
                                int *m2_start_zero_io) {
  MirOperand base = *base_io;
  MirOperand cnt = *cnt_io;
  MirOperand m2_start = *m2_start_io;
  int m2_start_zero = *m2_start_zero_io;
  m2_start_zero = (in->arguments[3].kind == IR_OPERAND_INT &&
                   in->arguments[3].int_value == 0);
  if (cnt.kind != MIR_OPK_VREG) {
    MirVregId lc = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (lc == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(lc), cnt, mir_op_none(), 8, 0,
                   0)) {
      return 0;
    }
    cnt = mir_op_vreg(lc);
  }
  if (!m2_start_zero) {
    m2_start = mir_value_operand(fn, g, ctx, map, &in->arguments[3]);
    MirVregId ab = mir_new_vreg(fn, MIR_RC_GP, 8);
    MirVregId lb = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (ab == MIR_VREG_NONE || lb == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_ADD, mir_op_vreg(ab), base, m2_start, 8, 0, 0) ||
        !mir_emit1(fn, MIR_SUB, mir_op_vreg(lb), cnt, m2_start, 8, 0, 0)) {
      return 0;
    }
    base = mir_op_vreg(ab);
    cnt = mir_op_vreg(lb);
  }
  *base_io = base;
  *cnt_io = cnt;
  *m2_start_io = m2_start;
  *m2_start_zero_io = m2_start_zero;
  return 1;
}

static int mir_lower_fill(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_FILL: {
    /* Inline fill. Marshal base->RCX, element count (mode 0) / end pointer
     * (mode 1) / byte length (mode 2)->R8, value->RAX, then emit the kernel.
     * The value is parked into RAX LAST so it cannot clobber a base/count
     * source that the allocator happened to place in RAX (the only poolable
     * register among the three targets). */
    MirOperand base = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand val = mir_gp_value_operand(fn, g, ctx, map, &in->arguments[2]);
    long long size = in->arguments[0].int_value;
    long long mode = in->arguments[1].int_value;
    /* Mode-0 with a runtime offset and/or nonzero start (int64 index): fold
     * `base + (offset+start)*size` in 64-bit MIR, and elements = bound-start,
     * so the kernel runs the plain element loop. */
    MirOperand m0_start = mir_op_imm(0);
    int m0_start_zero = 1;
    if (mode == 0 &&
        !mir_fill_fold_counted(fn, g, ctx, map, in, &base, &cnt, &m0_start,
                               &m0_start_zero, size)) {
      return 0;
    }
    /* Mode-2 byte-offset walk: fold `base + start` and the byte length
     * `bound - start` here in 64-bit MIR (the kernel receives the length
     * precomputed); keep the length vreg for the live-iv write-back below. */
    MirOperand m2_start = mir_op_imm(0);
    int m2_start_zero = 1;
    if (mode == 2 &&
        !mir_fill_fold_offset(fn, g, ctx, map, in, &base, &cnt, &m2_start,
                              &m2_start_zero)) {
      return 0;
    }
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), base,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), val,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    /* dst.imm = element size; a.imm = fill mode (0 element-counted, 1 byte-walk). */
    if (!mir_emit1(fn, MIR_SIMD_FILL, mir_op_imm(size), mir_op_imm(mode),
                   mir_op_none(), (int)size, 0, 0)) {
      return 0;
    }
    /* Live induction variable (mode 0, start 0): the unit-stride loop leaves
     * iv = max(count, 0) (the count for an empty loop, else the bound). Fold it
     * branchlessly as `cnt & ~(cnt >> 63)` so a later use of the counter reads
     * the right value -- matching the fallback's cmov write-back exactly. */
    if (mode == 0 && in->dest.kind == IR_OPERAND_SYMBOL &&
        !mir_fill_counted_writeback(fn, g, ctx, map, in, cnt, m0_start,
                                    m0_start_zero)) {
      return 0;
    }
    /* Mode-2 live iv: the scalar loop leaves iv = start when the walk is
     * empty (len <= 0), else start + len rounded up to the stride (the tail
     * store overshoots exactly as `i += size` does). Fold branchlessly:
     * walked = ((len + size-1) & -size) & ~(len >> 63); iv = start + walked. */
    if (mode == 2 && in->dest.kind == IR_OPERAND_SYMBOL &&
        !mir_fill_offset_writeback(fn, g, ctx, map, in, cnt, m2_start,
                                   m2_start_zero, size)) {
      return 0;
    }
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_affine_map(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_AFFINE_MAP_F32: {
    /* Inline float32 affine map: marshal src->RCX, dst->RDX, count->R8, then emit
     * the kernel with the (compile-time) a/b/c coefficient bits in dst/a/b.imm
     * and the b_is_one/b_is_zero/c_is_zero flags in cc. */
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), src,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), dst,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    int a32_runtime = in->arguments[1].kind != IR_OPERAND_FLOAT;
    if (a32_runtime) {
      MirOperand av =
          coerce_float_operand(fn, g, ctx, map, &in->arguments[1], 4);
      if (!mir_emit_fmov(fn, mir_op_phys(BINARY_XMM4, MIR_RC_XMM), av, 4)) {
        return 0;
      }
    }
    long long a_bits =
        a32_runtime ? 0
                    : (long long)(uint32_t)mir_float_bits_at(
                          in->arguments[1].float_value, 4);
    long long b_bits = (long long)(uint32_t)mir_float_bits_at(
        in->arguments[2].float_value, 4);
    long long c_bits = (long long)(uint32_t)mir_float_bits_at(
        in->arguments[3].float_value, 4);
    int b_is_one = in->arguments[2].float_value == 1.0;
    int b_is_zero = in->arguments[2].float_value == 0.0;
    int c_is_zero = in->arguments[3].float_value == 0.0;
    unsigned char flags = (unsigned char)((b_is_one ? 1 : 0) |
                                          (b_is_zero ? 2 : 0) |
                                          (c_is_zero ? 4 : 0) |
                                          (a32_runtime ? 8 : 0));
    return mir_emit1(fn, MIR_SIMD_AFFINE_MAP_F32, mir_op_imm(a_bits),
                     mir_op_imm(b_bits), mir_op_imm(c_bits), 4, 0, flags);
  }

  case IR_OP_SIMD_AFFINE_MAP_F64: {
    /* Inline float64 affine map: marshal src->RCX, dst->RDX, count->R8, then
     * emit the kernel with the (compile-time) a/b/c coefficient 64-bit bits in
     * dst/a/b.imm and the b_is_one/b_is_zero/c_is_zero flags in cc. */
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), src,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), dst,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    /* Runtime `a`: marshal its scalar value into XMM4 (the kernel's `a` lane,
     * a caller-saved register the kernel clobbers anyway) right before the
     * kernel, which then broadcasts it instead of materializing an immediate. */
    int a_runtime = in->arguments[1].kind != IR_OPERAND_FLOAT;
    if (a_runtime) {
      MirOperand av = mir_value_operand(fn, g, ctx, map, &in->arguments[1]);
      if (!mir_emit_fmov(fn, mir_op_phys(BINARY_XMM4, MIR_RC_XMM), av, 8)) {
        return 0;
      }
    }
    long long a_bits =
        a_runtime ? 0
                  : (long long)mir_float_bits_at(in->arguments[1].float_value, 8);
    long long b_bits = (long long)mir_float_bits_at(in->arguments[2].float_value, 8);
    long long c_bits = (long long)mir_float_bits_at(in->arguments[3].float_value, 8);
    int b_is_one = in->arguments[2].float_value == 1.0;
    int b_is_zero = in->arguments[2].float_value == 0.0;
    int c_is_zero = in->arguments[3].float_value == 0.0;
    unsigned char flags = (unsigned char)((b_is_one ? 1 : 0) |
                                          (b_is_zero ? 2 : 0) |
                                          (c_is_zero ? 4 : 0) |
                                          (a_runtime ? 8 : 0));
    return mir_emit1(fn, MIR_SIMD_AFFINE_MAP_F64, mir_op_imm(a_bits),
                     mir_op_imm(b_bits), mir_op_imm(c_bits), 8, 0, flags);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_vloop(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_VLOOP_F64: {
    {
      int bridge = in->argument_count > 5 &&
                   (in->arguments[0].int_value != 0 ||
                    in->arguments[5].int_value != 0);
      if (!bridge) {
        const char *bn[4];
        const IROperand *bs[4];
        int bvn = 0;
        bridge = code_generator_vloop_collect_dist(in, 0, bn, bs, &bvn) >= 0 &&
                 bvn > 3;
      }
      if (bridge) {
        return mir_lower_ir_kernel(fn, g, ctx, map, in);
      }
    }
    /* Inline general vloop (any lane width, maps only): marshal the <=3 distinct
     * base pointers into RCX/RDX/R8/R9 (kGp order, matching the kernel's dist)
     * and the element count into the next arg register; the kernel reads its DAG
     * from the borrowed IRInstruction in `aux`. */
    static const int kGp[4] = {BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8,
                               BINARY_GP_R9};
    const char *vnames[4];
    const IROperand *vsrcs[4];
    int vn = 0;
    if (code_generator_vloop_collect_dist(in, 0, vnames, vsrcs, &vn) < 0 ||
        vn > 3) {
      return 0;
    }
    for (int vk = 0; vk < vn; vk++) {
      MirOperand v = mir_value_operand(fn, g, ctx, map, vsrcs[vk]);
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(kGp[vk], MIR_RC_GP), v,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->lhs);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(kGp[vn], MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    MirInst v;
    memset(&v, 0, sizeof(v));
    v.op = MIR_SIMD_VLOOP;
    v.ir_index = -1;
    v.aux = in; /* borrowed: the IR outlives this function's codegen */
    return mir_emit(fn, &v);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_silu(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_SIMD_SILU_F32: {
    /* Inline SiLU/SwiGLU gate: marshal g/out->RCX, count->R8, u->RDX (SwiGLU),
     * then emit the kernel with has_mul in dst.imm. */
    int has_mul = (in->rhs.kind == IR_OPERAND_TEMP ||
                   in->rhs.kind == IR_OPERAND_SYMBOL);
    MirOperand gbase = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), gbase,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    if (has_mul) {
      MirOperand ubase = mir_value_operand(fn, g, ctx, map, &in->rhs);
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), ubase,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    return mir_emit1(fn, MIR_SIMD_SILU_F32, mir_op_imm(has_mul ? 1 : 0),
                     mir_op_none(), mir_op_none(), 4, 0, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_address_of(MirFunction *fn, CodeGenerator *g,
                        BinaryFunctionContext *ctx, MirNameMap *map,
                        const IRInstruction *in,
                        const MirGlobalWriteback *wb, int *handled) {
  (void)g;
  (void)ctx;
  (void)map;
  (void)wb;
  *handled = 1;
  switch (in->op) {
  case IR_OP_ADDRESS_OF: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    const IRFunction *irf =
        ctx && ctx->function_name
            ? code_generator_find_ir_function_binary(g, ctx->function_name)
            : NULL;
    MirAddrofKind ak = mir_addressof_kind(g, irf, in);
    if (ak == MIR_ADDROF_INDIRECT_PARAM) {
      /* &@p of a by-reference (INDIRECT) struct param: the param already holds
       * the struct's address, so the address-of is just a copy of the pointer. */
      MirOperand ptr = mir_value_operand(fn, g, ctx, map, &in->lhs);
      return mir_emit1(fn, MIR_MOV, dst, ptr, mir_op_none(), 8, 0, 0);
    }
    if (ak == MIR_ADDROF_GLOBAL) {
      /* &global: lea its RIP-relative address (is_unsigned carries the
       * declare-external flag for the encoder). The global stays cached; the
       * main loop flushes/reloads address-taken globals around pointer memory
       * ops so the alias and the cache vreg stay coherent. */
      const CgSym *s = g->ir_program
                      ? code_generator_lookup_symbol(g, in->lhs.name)
                      : NULL;
      int is_extern = (s && s->is_extern) ? 1 : 0;
      return mir_emit1(fn, MIR_LEA_GLOBAL, dst, mir_op_symbol(in->lhs.name),
                       mir_op_none(), 8, is_extern, 0);
    }
    if (ak == MIR_ADDROF_FUNCTION) {
      const CgSym *s = g->ir_program
                      ? code_generator_lookup_symbol(g, in->lhs.name)
                      : NULL;
      int is_extern = (s && s->is_extern) ? 1 : 0;
      return mir_emit1(fn, MIR_LEA_FUNC, dst, mir_op_symbol(in->lhs.name),
                       mir_op_none(), 8, is_extern, 0);
    }
    /* &local / &param: mark the target memory-resident and lea its stack home. */
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    if (src.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    fn->vregs[src.vreg].address_taken = 1;
    /* An INDIRECT struct local needs a home large enough for the whole struct,
     * since field stores reach past the first 8 bytes. Size it to the struct
     * size rounded up to an 8-byte slot. (Scalars and DIRECT small aggregates
     * keep home_bytes == 0, i.e. the default single slot.) The type is resolved
     * from the IR (function scope has popped from the symbol table by now). */
    {
      int is_param = 0;
      MtlcType *lt = mir_local_or_param_type(g, irf, in->lhs.name, &is_param);
      if (lt && !is_param && code_generator_type_is_aggregate(lt) &&
          code_generator_abi_classify(lt) == ABI_PASS_INDIRECT) {
        size_t sz = code_generator_abi_type_size(lt);
        fn->vregs[src.vreg].home_bytes = (int)((sz + 7) & ~(size_t)7);
      }
      /* A narrow scalar's home is authoritative only at its declared width:
       * the aliasing pointer writes exactly those bytes, so a by-name read
       * must extend from them rather than load the whole 8-byte slot. */
      if (lt && !code_generator_type_is_aggregate(lt) &&
          code_generator_binary_resolved_type_float_bits(lt) == 0) {
        int w = code_generator_binary_resolved_type_scalar_size(lt);
        if (w == 1 || w == 2 || w == 4) {
          fn->vregs[src.vreg].home_width = w;
          fn->vregs[src.vreg].home_signed =
              code_generator_binary_resolved_type_is_signed_integer(lt);
        }
      }
      if (lt && (lt->kind == MTLC_TYPE_FLOAT16 || lt->kind == MTLC_TYPE_BFLOAT16)) {
        fn->vregs[src.vreg].home_width = 2;
        fn->vregs[src.vreg].home_signed = 0;
      }
      /* Read off the IR, exactly as the fallback layout does: the safety pass
       * has already said which locals it describes, by emitting a registration
       * whose argument is the address of one. A described local's home must
       * cover whole granules, so the neighbour it sits next to cannot share
       * one and blind them both. */
      if (!is_param &&
          binary_function_local_is_safety_described(irf, in->lhs.name)) {
        fn->vregs[src.vreg].home_granule = 1;
        if (lt) {
          size_t sz = code_generator_abi_type_size(lt);
          int need = (int)((sz + 7) & ~(size_t)7);
          if (need > fn->vregs[src.vreg].home_bytes) {
            fn->vregs[src.vreg].home_bytes = need;
          }
        }
      }
    }
    return mir_emit1(fn, MIR_LEA_LOCAL, dst, src, mir_op_none(), 8, 0, 0);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int mir_lower_instruction(MirFunction *fn, CodeGenerator *g,
                                 BinaryFunctionContext *ctx, MirNameMap *map,
                                 const IRInstruction *in,
                                 const MirGlobalWriteback *wb) {
  static int (*const LOWERERS[])(MirFunction *, CodeGenerator *,
                                 BinaryFunctionContext *, MirNameMap *,
                                 const IRInstruction *,
                                 const MirGlobalWriteback *, int *) = {
      mir_lower_control,
      mir_lower_assign,
      mir_lower_binary,
      mir_lower_unary,
      mir_lower_cast,
      mir_lower_load,
      mir_lower_store,
      mir_lower_select,
      mir_lower_alloc,
      mir_lower_return,
      mir_lower_call,
      mir_lower_call_indirect,
      mir_lower_slp_mac,
      mir_lower_fill,
      mir_lower_affine_map,
      mir_lower_vloop,
      mir_lower_silu,
      mir_lower_address_of};
  size_t lowerer;

  for (lowerer = 0; lowerer < sizeof(LOWERERS) / sizeof(LOWERERS[0]);
       lowerer++) {
    int handled = 0;
    int lowered = LOWERERS[lowerer](fn, g, ctx, map, in, wb, &handled);
    if (handled) {
      return lowered;
    }
  }
  if (mir_ir_kernel_index_for_op(in->op) >= 0) {
    return mir_lower_ir_kernel(fn, g, ctx, map, in);
  }
  fn->has_error = 1;
  return 0;
}

/* ---- scaled-address (SIB) folding --------------------------------------- *
 * An array access lowers to three IR ops: a shift/multiply that scales the
 * index, an add that offsets the base pointer, and the load/store itself. x86
 * addresses that whole thing in one [base + index*scale] memory operand, so we
 * detect the pattern and let the load/store carry a SIB MirMem, dropping the
 * two address-computation instructions. This is the single biggest scalar
 * codegen win for index-heavy loops (e.g. matmul): it removes a shift, an add,
 * and (when the base would otherwise spill) a reload every memory access. */

typedef struct {
  int valid;
  IROperand base;
  IROperand index;
  int scale;
  /* Constant byte offset to add on top of base + index*scale. Non-zero when the
   * index was itself `j + k`: x86 addresses carry that k for free in their
   * displacement, so `a[j + 1]` need not compute a second index register. */
  long long disp;
} MirAddrFold;

/* Per-function index over TEMP names, recording for each temp how many
 * instructions READ it and which instruction DEFINES it.
 *
 * A read is any lhs/rhs operand, plus a STORE's dest (that dest is the store's
 * address, so it is read, not defined). A producer's own dest is a definition.
 *
 * The address- and compare-fold passes below ask both questions once per
 * candidate access. Answering each by scanning the whole instruction list made
 * those passes quadratic, with a strcmp per instruction visited -- profiling a
 * 226k-line compile put that single call site at 3.8% of total wall clock, the
 * largest source of strcmp in the compiler. One pass builds the index; each
 * query is then a hash lookup. */
typedef struct {
  const char *name;
  int reads;
  /* Reads that occur as the ADDRESS operand of a non-float LOAD/STORE. When
   * this equals `reads`, every consumer of the temp is an access that can carry
   * the address in its own memory operand, so the address computation can be
   * folded into all of them at once rather than only a single one. */
  int addr_reads;
  long def_index; /* -1 until an instruction defines it */
  int def_count;  /* number of instructions defining it (non-SSA names exist) */
} MirTempUse;

typedef struct {
  MirTempUse *items;
  size_t count;
  size_t capacity;
  size_t *buckets; /* open addressing over items: slot+1, 0 = empty */
  size_t bucket_count;
} MirTempUseIndex;

static void mir_temp_use_destroy(MirTempUseIndex *ix) {
  free(ix->items);
  free(ix->buckets);
  ix->items = NULL;
  ix->buckets = NULL;
  ix->count = ix->capacity = ix->bucket_count = 0;
}

/* Slot for `name`, appending an empty entry when absent. NULL only on OOM. */
static MirTempUse *mir_temp_use_slot(MirTempUseIndex *ix, const char *name) {
  size_t h = mettle_fnv1a_hash(name);
  size_t b = h & (ix->bucket_count - 1);
  while (ix->buckets[b]) {
    MirTempUse *e = &ix->items[ix->buckets[b] - 1];
    if (strcmp(e->name, name) == 0) {
      return e;
    }
    b = (b + 1) & (ix->bucket_count - 1);
  }
  if (ix->count >= ix->capacity) {
    size_t nc = ix->capacity ? ix->capacity * 2 : 32;
    MirTempUse *grown = (MirTempUse *)realloc(ix->items, nc * sizeof(MirTempUse));
    if (!grown) {
      return NULL;
    }
    ix->items = grown;
    ix->capacity = nc;
  }
  ix->items[ix->count].name = name;
  ix->items[ix->count].reads = 0;
  ix->items[ix->count].addr_reads = 0;
  ix->items[ix->count].def_index = -1;
  ix->items[ix->count].def_count = 0;
  ix->count++;
  ix->buckets[b] = ix->count;

  if ((ix->count + 1) * 4 >= ix->bucket_count * 3) {
    /* Rehash before the table gets dense enough for probes to lengthen. */
    size_t nb = ix->bucket_count * 2;
    size_t *fresh = (size_t *)calloc(nb, sizeof(size_t));
    if (!fresh) {
      return NULL;
    }
    for (size_t i = 0; i < ix->count; i++) {
      size_t nbk = mettle_fnv1a_hash(ix->items[i].name) & (nb - 1);
      while (fresh[nbk]) {
        nbk = (nbk + 1) & (nb - 1);
      }
      fresh[nbk] = i + 1;
    }
    free(ix->buckets);
    ix->buckets = fresh;
    ix->bucket_count = nb;
  }
  return &ix->items[ix->count - 1];
}

static int mir_temp_use_build(const IRFunction *f, MirTempUseIndex *ix) {
  memset(ix, 0, sizeof(*ix));
  ix->bucket_count = 64;
  while (ix->bucket_count < (f->instruction_count + 1) * 2) {
    ix->bucket_count *= 2;
  }
  ix->buckets = (size_t *)calloc(ix->bucket_count, sizeof(size_t));
  if (!ix->buckets) {
    return 0;
  }
  for (size_t i = 0; i < f->instruction_count; i++) {
    const IRInstruction *in = &f->instructions[i];
    const IROperand *reads[3];
    int nreads = 0;
    if (in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name) {
      reads[nreads++] = &in->lhs;
    }
    if (in->rhs.kind == IR_OPERAND_TEMP && in->rhs.name) {
      reads[nreads++] = &in->rhs;
    }
    if (in->op == IR_OP_STORE && in->dest.kind == IR_OPERAND_TEMP &&
        in->dest.name) {
      reads[nreads++] = &in->dest;
    }
    /* The address operand of a non-float LOAD/STORE: a read that an x86 memory
     * operand can absorb (see mir_compute_address_folds). */
    const IROperand *addr_read = NULL;
    if (!in->is_float) {
      if (in->op == IR_OP_LOAD && in->lhs.kind == IR_OPERAND_TEMP &&
          in->lhs.name) {
        addr_read = &in->lhs;
      } else if (in->op == IR_OP_STORE && in->dest.kind == IR_OPERAND_TEMP &&
                 in->dest.name) {
        addr_read = &in->dest;
      }
    }
    /* The argument vector is an input vector. Call arguments live there, and
     * so do the third operand of a SELECT and the operands of every SIMD
     * kernel. Counting only lhs/rhs/dest made a temp whose only reader is a
     * call argument look unread, and the address folds below retire a
     * producer they believe has exactly one reader -- so `%t = i << 2;
     * f(%t); ... = base[%t]` folded `i*4` into the load's SIB and deleted the
     * shift, leaving the call reading a register nothing wrote. Rare in
     * ordinary code and universal under --safe, where every checked access is
     * `check(base, off, ...)` followed by a load through the same `off`. */
    for (size_t a = 0; a < in->argument_count; a++) {
      const IROperand *arg = &in->arguments[a];
      if (arg->kind != IR_OPERAND_TEMP || !arg->name) {
        continue;
      }
      MirTempUse *e = mir_temp_use_slot(ix, arg->name);
      if (!e) {
        mir_temp_use_destroy(ix);
        return 0;
      }
      e->reads++;
    }
    for (int k = 0; k < nreads; k++) {
      MirTempUse *e = mir_temp_use_slot(ix, reads[k]->name);
      if (!e) {
        mir_temp_use_destroy(ix);
        return 0;
      }
      e->reads++;
      if (reads[k] == addr_read) {
        e->addr_reads++;
      }
    }
    /* Any op with a TEMP dest, STORE included: the two queries are independent,
     * and the scan this replaces treated a STORE's dest as both a read (its
     * address) and a candidate definition. */
    if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name) {
      MirTempUse *e = mir_temp_use_slot(ix, in->dest.name);
      if (!e) {
        mir_temp_use_destroy(ix);
        return 0;
      }
      if (e->def_index < 0) {
        e->def_index = (long)i; /* first definition wins, as the scan did */
      }
      /* A STORE's dest is the address it writes THROUGH, not a value it
       * defines, so it must not count towards the "exactly one producer"
       * test -- `*%t <- v` would otherwise make every stored-through address
       * look multiply-defined. (def_index keeps its historical behaviour.) */
      if (in->op != IR_OP_STORE) {
        e->def_count++;
      }
    }
  }
  return 1;
}

static const MirTempUse *mir_temp_use_find(const MirTempUseIndex *ix,
                                           const char *name) {
  size_t b = mettle_fnv1a_hash(name) & (ix->bucket_count - 1);
  while (ix->buckets[b]) {
    const MirTempUse *e = &ix->items[ix->buckets[b] - 1];
    if (strcmp(e->name, name) == 0) {
      return e;
    }
    b = (b + 1) & (ix->bucket_count - 1);
  }
  return NULL;
}

static int mir_temp_read_count(const MirTempUseIndex *ix, const char *name) {
  const MirTempUse *e = mir_temp_use_find(ix, name);
  return e ? e->reads : 0;
}

/* Index of the instruction whose dest defines temp `name`, or -1. */
static long mir_temp_def_index(const MirTempUseIndex *ix, const char *name) {
  const MirTempUse *e = mir_temp_use_find(ix, name);
  return e ? e->def_index : -1;
}

/* True when every read of `name` is the address operand of a non-float
 * LOAD/STORE, and the name has exactly one definition. Both conditions are
 * needed before an address computation may be folded into more than one
 * access: a non-address read would still need the value in a register, and a
 * second definition means `def_index` does not identify the producer that
 * reaches those reads. */
static int mir_temp_reads_are_all_addresses(const MirTempUseIndex *ix,
                                            const char *name, int *reads_out) {
  const MirTempUse *e = mir_temp_use_find(ix, name);
  if (!e || e->reads < 1) {
    return 0;
  }
  if (reads_out) {
    *reads_out = e->reads;
  }
  if (e->reads == 1) {
    return 1; /* the long-standing single-access fold; unchanged */
  }
  return e->def_count == 1 && e->reads == e->addr_reads;
}

/* True when `operand` names the value written by `in`. Symbols are the mutable
 * ones -- a temp is written by its own producer, which the caller has already
 * accounted for. */
static int mir_instruction_defines_operand(const IRInstruction *in,
                                           const IROperand *operand) {
  /* A NOP is a deleted instruction: the optimizer blanks the opcode but leaves
   * the operands in place, so its `dest` names a value it no longer writes.
   * A STORE's `dest` is its address -- read, not written. */
  if (in->op == IR_OP_NOP || in->op == IR_OP_STORE) {
    return 0;
  }
  if (!operand || !operand->name || in->dest.kind != operand->kind ||
      !in->dest.name) {
    return 0;
  }
  if (operand->kind != IR_OPERAND_SYMBOL && operand->kind != IR_OPERAND_TEMP) {
    return 0;
  }
  return strcmp(in->dest.name, operand->name) == 0;
}

/* An access folds the address computation into its own memory operand, so the
 * operands are re-read at the access rather than at the original add. Folding
 * into SEVERAL accesses is therefore only sound while `base` and `index` still
 * hold the values they had at the add. Walk forward from the producer and
 * require all `expected` accesses to be reached before either operand is
 * rewritten (a store through a pointer cannot change a register value, so only
 * direct writes and calls -- which may write a global -- matter).
 *
 * The scan is bounded: a candidate whose uses are far apart is left alone
 * rather than paying an unbounded walk per access on a large function. */
#define MIR_ADDR_FOLD_SCAN_LIMIT 256

static int mir_addr_fold_multiuse_safe(const IRFunction *f, size_t def_index,
                                       const char *addr_name,
                                       const IROperand *base,
                                       const IROperand *index, int expected) {
  int seen = 0;
  int base_is_symbol = (base->kind == IR_OPERAND_SYMBOL);
  int index_is_symbol = (index->kind == IR_OPERAND_SYMBOL);
  size_t limit = def_index + 1 + MIR_ADDR_FOLD_SCAN_LIMIT;
  if (limit > f->instruction_count) {
    limit = f->instruction_count;
  }
  for (size_t k = def_index + 1; k < limit; k++) {
    const IRInstruction *in = &f->instructions[k];
    const IROperand *addr = NULL;
    if (in->op == IR_OP_LOAD) {
      addr = &in->lhs;
    } else if (in->op == IR_OP_STORE) {
      addr = &in->dest;
    }
    if (addr && addr->kind == IR_OPERAND_TEMP && addr->name &&
        strcmp(addr->name, addr_name) == 0) {
      seen++;
      if (seen == expected) {
        return 1;
      }
      continue;
    }
    /* A call may write any global, and the base/index of an array access are
     * routinely globals or parameters spilled to memory. */
    if ((in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT) &&
        (base_is_symbol || index_is_symbol)) {
      return 0;
    }
    if (mir_instruction_defines_operand(in, base) ||
        mir_instruction_defines_operand(in, index)) {
      return 0;
    }
  }
  return 0;
}

/* An index of the form `j + k` (k a constant) needs no arithmetic of its own:
 * x86 carries `k * scale` in the address displacement. Neighbour accesses --
 * `src[i + 1]`, `dst[o + 2]`, `a[i - 1]` -- are ubiquitous, and each one was
 * paying an ADD and a register for an offset the address encodes for free.
 *
 * Rewrites *index to `j` and adds the byte offset to *disp, but only when the
 * constant-add temp exists solely to feed this address (otherwise its producer
 * still has to run and nothing is saved). Returns the producer's index to skip,
 * or -1 when the index is left alone. */
static long mir_fold_index_constant_offset(const IRFunction *f,
                                           const MirTempUseIndex *uses,
                                           IROperand *index, int scale,
                                           long long *disp) {
  if (index->kind != IR_OPERAND_TEMP || !index->name) {
    return -1;
  }
  if (mir_temp_read_count(uses, index->name) != 1) {
    return -1;
  }
  long pi = mir_temp_def_index(uses, index->name);
  if (pi < 0) {
    return -1;
  }
  const IRInstruction *p = &f->instructions[pi];
  if (p->op != IR_OP_BINARY || p->is_float || !p->text) {
    return -1;
  }
  int subtract = strcmp(p->text, "-") == 0;
  if (!subtract && strcmp(p->text, "+") != 0) {
    return -1;
  }
  /* For subtraction only `j - k` works; `k - j` negates the index. */
  const IROperand *var = NULL;
  long long konst = 0;
  if (p->rhs.kind == IR_OPERAND_INT &&
      (p->lhs.kind == IR_OPERAND_TEMP || p->lhs.kind == IR_OPERAND_SYMBOL)) {
    var = &p->lhs;
    konst = subtract ? -p->rhs.int_value : p->rhs.int_value;
  } else if (!subtract && p->lhs.kind == IR_OPERAND_INT &&
             (p->rhs.kind == IR_OPERAND_TEMP ||
              p->rhs.kind == IR_OPERAND_SYMBOL)) {
    var = &p->rhs;
    konst = p->lhs.int_value;
  } else {
    return -1;
  }
  if (!var->name) {
    return -1;
  }
  long long offset = konst * (long long)scale;
  long long total = *disp + offset;
  if (offset / (scale ? scale : 1) != konst || total < -2147483648LL ||
      total > 2147483647LL) {
    return -1; /* would not fit the displacement */
  }
  *index = *var;
  *disp = total;
  return pi;
}

/* If `p` scales an index by a legal SIB factor (`idx << k`, k in 0..3, or
 * `idx * c`, c in {1,2,4,8}), fill *index/*scale and return 1. */
static int mir_decode_scale(const IRInstruction *p, IROperand *index,
                            int *scale) {
  if (p->op != IR_OP_BINARY || p->is_float || !p->text) {
    return 0;
  }
  if (strcmp(p->text, "<<") == 0 && p->rhs.kind == IR_OPERAND_INT) {
    long long k = p->rhs.int_value;
    if (k < 0 || k > 3) {
      return 0;
    }
    *index = p->lhs;
    *scale = 1 << k;
    return 1;
  }
  if (strcmp(p->text, "*") == 0) {
    const IROperand *konst = NULL, *var = NULL;
    if (p->rhs.kind == IR_OPERAND_INT) {
      konst = &p->rhs;
      var = &p->lhs;
    } else if (p->lhs.kind == IR_OPERAND_INT) {
      konst = &p->lhs;
      var = &p->rhs;
    } else {
      return 0;
    }
    long long c = konst->int_value;
    if (c == 1 || c == 2 || c == 4 || c == 8) {
      *index = *var;
      *scale = (int)c;
      return 1;
    }
  }
  return 0;
}

/* Scan for `LOAD/STORE [ base + (index<<k|index*c) ]` and record a SIB fold for
 * each, marking the two address-producer instructions to be skipped. Only
 * integer accesses fold (the float encoder path does not read mem.index). */
/* Mark for skipping the producer of any loop-bound constant that the fused
 * compare-branch will fold into an imm32 (see mir_fused_cmp_imm). Without this
 * the CAST that materializes the bound stays in the loop as a dead `mov reg,
 * imm` every iteration. Only drops a producer whose temp is read solely by that
 * compare. */
static void mir_compute_const_compare_skips(CodeGenerator *g,
                                            BinaryFunctionContext *ctx,
                                            IRFunction *f,
                                            const MirTempUseIndex *uses,
                                            char *skip) {
  for (size_t i = 0; i + 1 < f->instruction_count; i++) {
    if (!mir_fuses_compare_branch(g, f, i)) {
      continue;
    }
    const IRInstruction *cmp = &f->instructions[i];
    if (cmp->is_float || cmp->rhs.kind != IR_OPERAND_TEMP || !cmp->rhs.name) {
      continue;
    }
    long long imm;
    if (!mir_fused_cmp_imm(g, ctx, f, &cmp->rhs, &imm)) {
      continue;
    }
    if (mir_temp_read_count(uses, cmp->rhs.name) != 1) {
      continue; /* bound temp feeds something else; keep its producer */
    }
    long def = mir_temp_def_index(uses, cmp->rhs.name);
    if (def >= 0) {
      skip[def] = 1;
    }
  }
}

/* What can carry a memory operand's base: a temp, or a pointer-valued symbol
 * (a parameter or local the allocator keeps in a register). */
static int mir_addr_base_operand_kind(const IROperand *operand) {
  return operand && operand->name &&
         (operand->kind == IR_OPERAND_TEMP ||
          operand->kind == IR_OPERAND_SYMBOL);
}

static int mir_addr_fold_through_inner_add(const IRFunction *f,
                                           const MirTempUseIndex *uses,
                                           const IROperand *outer_base,
                                           const char *access_addr_name,
                                           IROperand *base, IROperand *index,
                                           int *scale, long *inner,
                                           long *index_def) {
  if (!outer_base || outer_base->kind != IR_OPERAND_TEMP || !outer_base->name) {
    return 0;
  }
  if (mir_temp_read_count(uses, outer_base->name) != 1) {
    return 0;
  }
  long ii = mir_temp_def_index(uses, outer_base->name);
  if (ii < 0) {
    return 0;
  }
  const IRInstruction *inner_add = &f->instructions[ii];
  if (inner_add->op != IR_OP_BINARY || inner_add->is_float || !inner_add->text ||
      strcmp(inner_add->text, "+") != 0) {
    return 0;
  }
  const IROperand *order[2][2] = {{&inner_add->lhs, &inner_add->rhs},
                                  {&inner_add->rhs, &inner_add->lhs}};
  for (int t = 0; t < 2; t++) {
    const IROperand *b = order[t][0];
    const IROperand *x = order[t][1];
    if (!mir_addr_base_operand_kind(b) || !mir_addr_base_operand_kind(x)) {
      continue;
    }
    IROperand idx = *x;
    int sc = 1;
    long xdef = -1;
    if (x->kind == IR_OPERAND_TEMP && x->name &&
        mir_temp_read_count(uses, x->name) == 1) {
      long xi = mir_temp_def_index(uses, x->name);
      IROperand decoded;
      int decoded_scale;
      if (xi >= 0 && mir_decode_scale(&f->instructions[xi], &decoded,
                                      &decoded_scale) &&
          mir_addr_base_operand_kind(&decoded)) {
        idx = decoded;
        sc = decoded_scale;
        xdef = xi;
      }
    }
    if (!mir_addr_fold_multiuse_safe(f, (size_t)ii, access_addr_name, b, &idx,
                                     1)) {
      continue;
    }
    *base = *b;
    *index = idx;
    *scale = sc;
    *inner = ii;
    *index_def = xdef;
    return 1;
  }
  return 0;
}

static void mir_compute_address_folds(const IRFunction *f,
                                      const MirTempUseIndex *uses, char *skip,
                                      MirAddrFold *folds) {
  for (size_t i = 0; i < f->instruction_count; i++) {
    const IRInstruction *in = &f->instructions[i];
    const IROperand *addr;
    if (in->op == IR_OP_LOAD) {
      addr = &in->lhs;
    } else if (in->op == IR_OP_STORE) {
      addr = &in->dest;
    } else {
      continue;
    }
    if (in->is_float || addr->kind != IR_OPERAND_TEMP || !addr->name) {
      continue;
    }
    /* Every read of the address must be an access that can carry it in its own
     * memory operand, or dropping its producer would lose a value another
     * instruction needs. Several such accesses are fine -- a SIB operand costs
     * no extra instruction, so re-deriving the address per access is strictly
     * cheaper than computing it once into a register. `count[d] = count[d] + 1`
     * (load and store through one address) is the common shape. */
    int addr_reads = 0;
    if (!mir_temp_reads_are_all_addresses(uses, addr->name, &addr_reads)) {
      continue;
    }
    long ai = mir_temp_def_index(uses, addr->name);
    if (ai < 0) {
      continue;
    }
    const IRInstruction *padd = &f->instructions[ai];
    if (padd->op != IR_OP_BINARY || padd->is_float || !padd->text ||
        strcmp(padd->text, "+") != 0) {
      continue;
    }
    /* One operand is the base pointer, the other the scaled index (a temp whose
     * sole use is this add). Try both orderings. */
    const IROperand *order[2][2] = {{&padd->lhs, &padd->rhs},
                                    {&padd->rhs, &padd->lhs}};
    for (int t = 0; t < 2; t++) {
      const IROperand *base = order[t][0];
      const IROperand *scaled = order[t][1];
      if (scaled->kind != IR_OPERAND_TEMP || !scaled->name) {
        continue;
      }
      /* The scaled index may have more than one reader: `a[i] * b[i]` reads one
       * `i << 2` twice once CSE has folded the two copies together. Such an
       * access still folds -- a SIB operand re-derives `i*4` for free -- but its
       * producer has to stay for the other reader, so only retire the shift when
       * this access is its sole reader. Refusing outright would drop the whole
       * pair onto the scale-1 fallback, which folds the SCALED value as an
       * index and gets the address wrong. */
      int scaled_reads = mir_temp_read_count(uses, scaled->name);
      if (scaled_reads < 1) {
        continue;
      }
      long si = mir_temp_def_index(uses, scaled->name);
      if (si < 0) {
        continue;
      }
      IROperand index;
      int scale;
      if (!mir_decode_scale(&f->instructions[si], &index, &scale)) {
        continue;
      }
      long long disp = 0;
      long offset_producer =
          mir_fold_index_constant_offset(f, uses, &index, scale, &disp);
      if (addr_reads > 1 &&
          !mir_addr_fold_multiuse_safe(f, (size_t)ai, addr->name, base, &index,
                                       addr_reads)) {
        continue;
      }
      folds[i].valid = 1;
      folds[i].base = *base;
      folds[i].index = index;
      folds[i].scale = scale;
      folds[i].disp = disp;
      skip[ai] = 1; /* the base+scaled add */
      if (scaled_reads == 1) {
        skip[si] = 1; /* the index scale, read by this access alone */
        if (offset_producer >= 0) {
          skip[offset_producer] = 1; /* the index's constant offset */
        }
      }
      break;
    }

    /* Scale-1 fallback: a plain `base + index` with no explicit scaling, i.e.
     * the unit-stride access `a[i]` on a byte/char/pointer-sized-by-1 buffer
     * (and any loop walking an int8/uint8 array). Both operands must be
     * register-resident values (TEMP or SYMBOL); fold them straight into
     * [op0 + op1*1]. base/index are symmetric at scale 1, so either ordering
     * encodes identically. Unlike the scaled path this consumes no separate
     * producer and leaves both operands live (the index is typically the loop
     * induction variable, still needed by the increment), so only the add
     * itself is dropped. */
    if (!folds[i].valid) {
      const IROperand *o0 = &padd->lhs;
      const IROperand *o1 = &padd->rhs;
      int o0_reg = (o0->kind == IR_OPERAND_TEMP || o0->kind == IR_OPERAND_SYMBOL);
      int o1_reg = (o1->kind == IR_OPERAND_TEMP || o1->kind == IR_OPERAND_SYMBOL);
      if (o0_reg && o1_reg &&
          (addr_reads == 1 ||
           mir_addr_fold_multiuse_safe(f, (size_t)ai, addr->name, o0, o1,
                                       addr_reads))) {
        /* Either side may be the one carrying the `+ k`; the base pointer is
         * whichever is not. Try the second operand first, the usual index
         * position for `buf[i + 1]`. */
        IROperand index = *o1;
        IROperand base = *o0;
        long long disp = 0;
        long offset_producer =
            mir_fold_index_constant_offset(f, uses, &index, 1, &disp);
        if (offset_producer < 0) {
          index = *o0;
          base = *o1;
          offset_producer =
              mir_fold_index_constant_offset(f, uses, &index, 1, &disp);
          if (offset_producer < 0) {
            index = *o1;
            base = *o0;
          }
        }
        folds[i].valid = 1;
        folds[i].base = base;
        folds[i].index = index;
        folds[i].scale = 1;
        folds[i].disp = disp;
        skip[ai] = 1; /* fold the base+index add into the memory operand */
        if (offset_producer >= 0) {
          skip[offset_producer] = 1;
        }
      }
    }

    /* Constant-displacement fallback: `ptr + const_int` -- a struct-field or
     * fixed-offset access (`p->field`, `b[i].field`, `*(ptr + k)`) -- folds the
     * constant straight into the x86 displacement: [base + disp]. The constant
     * is exactly the displacement (the access size is unchanged, so there is no
     * width or aliasing concern). Unlike the scaled/scale-1 paths this consumes
     * no separate producer -- only the add is dropped -- and the base pointer
     * temp stays live, since it is typically shared across several field
     * accesses on the same element (folding base+index*scale here instead would
     * re-derive it per field). mir_lower_folded_access turns an IR_OPERAND_INT
     * index into the displacement. */
    if (!folds[i].valid) {
      const IROperand *o0 = &padd->lhs;
      const IROperand *o1 = &padd->rhs;
      const IROperand *base = NULL;
      const IROperand *cst = NULL;
      int base_is_symbol = 0;
      if (mir_addr_base_operand_kind(o0) && o1->kind == IR_OPERAND_INT) {
        base = o0;
        cst = o1;
      } else if (mir_addr_base_operand_kind(o1) &&
                 o0->kind == IR_OPERAND_INT) {
        base = o1;
        cst = o0;
      }
      base_is_symbol = base && base->kind == IR_OPERAND_SYMBOL;
      /* A symbol base is re-read at the access rather than at the add, so it
       * has to survive the gap even when there is only one access; a temp is
       * written once and cannot. `p->field` through a pointer parameter is the
       * shape this reaches, and it was materializing the address into a
       * register for every field read in the function. */
      if (base && cst->int_value >= -2147483648LL &&
          cst->int_value <= 2147483647LL &&
          ((addr_reads == 1 && !base_is_symbol) ||
           mir_addr_fold_multiuse_safe(f, (size_t)ai, addr->name, base, cst,
                                       addr_reads))) {
        IROperand deep_base;
        IROperand deep_index;
        int deep_scale = 1;
        long inner = -1;
        long index_def = -1;
        if (addr_reads == 1 &&
            mir_addr_fold_through_inner_add(f, uses, base, addr->name,
                                            &deep_base, &deep_index,
                                            &deep_scale, &inner, &index_def)) {
          folds[i].valid = 1;
          folds[i].base = deep_base;
          folds[i].index = deep_index;
          folds[i].scale = deep_scale;
          folds[i].disp = cst->int_value;
          skip[ai] = 1;
          skip[inner] = 1;
          if (index_def >= 0) {
            skip[index_def] = 1;
          }
        } else {
          folds[i].valid = 1;
          folds[i].base = *base;
          folds[i].index = *cst;
          folds[i].scale = 1;
          skip[ai] = 1; /* fold the ptr+const add into the memory displacement */
        }
      }
    }
  }
}

/* Lower a LOAD/STORE whose address folded into a [base + index*scale] SIB. */
static int mir_lower_folded_access(MirFunction *fn, CodeGenerator *g,
                                   BinaryFunctionContext *ctx, MirNameMap *map,
                                   const IRInstruction *in,
                                   const MirAddrFold *fold) {
  MirOperand baseo = mir_value_operand(fn, g, ctx, map, &fold->base);
  if (baseo.kind != MIR_OPK_VREG) {
    fn->has_error = 1;
    return 0;
  }
  MirOperand mem;
  if (fold->index.kind == IR_OPERAND_INT) {
    /* A constant index (e.g. `p[0]`, `arr[5]`) folds into the displacement:
     * [base + index*scale]. mir_decode_scale yields the literal index when the
     * scaled-offset expression is itself constant. */
    long long disp = fold->index.int_value * (long long)fold->scale + fold->disp;
    if (disp < -2147483648LL || disp > 2147483647LL) {
      fn->has_error = 1;
      return 0;
    }
    mem = mir_op_mem_vreg(baseo.vreg, MIR_VREG_NONE, 0, (int)disp);
  } else {
    MirOperand idxo = mir_value_operand(fn, g, ctx, map, &fold->index);
    if (idxo.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    if (fold->disp < -2147483648LL || fold->disp > 2147483647LL) {
      fn->has_error = 1;
      return 0;
    }
    mem = mir_op_mem_vreg(baseo.vreg, idxo.vreg, fold->scale, (int)fold->disp);
  }
  int size = code_generator_binary_get_access_size(g, ctx, &in->rhs);
  if (size <= 0) {
    fn->has_error = 1;
    return 0;
  }
  /* String value convention (same as the unfolded LOAD/STORE paths): an
   * 8-byte string value is a record pointer, so a string-local dest deref-
   * copies and a string-local source stores its home's address. */
  const IRFunction *sirf =
      ctx && ctx->function_name
          ? code_generator_find_ir_function_binary(g, ctx->function_name)
          : NULL;
  if (in->op == IR_OP_LOAD) {
    if (size == 8 && in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        mir_name_is_string_local(g, sirf, in->dest.name)) {
      MirVregId ptr = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirOperand dsym = mir_value_operand(fn, g, ctx, map, &in->dest);
      if (ptr == MIR_VREG_NONE || dsym.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      fn->vregs[dsym.vreg].address_taken = 1;
      if (fn->vregs[dsym.vreg].home_bytes < 16) {
        fn->vregs[dsym.vreg].home_bytes = 16;
      }
      MirVregId db = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (db == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_MOV, mir_op_vreg(ptr), mem, mir_op_none(), 8, 1,
                     0) ||
          !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(db), dsym, mir_op_none(), 8,
                     0, 0) ||
          !mir_emit_struct_copy(fn, db, ptr, 16)) {
        return 0;
      }
      return 1;
    }
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    int sign_ext = !in->is_unsigned &&
                   code_generator_binary_load_needs_sign_extend(g, ctx,
                                                               &in->dest, size);
    return mir_emit1(fn, MIR_MOV, dst, mem, mir_op_none(), size,
                     sign_ext ? 0 : 1, 0);
  }
  if (size == 8 &&
      (in->lhs.kind == IR_OPERAND_SYMBOL || in->lhs.kind == IR_OPERAND_TEMP) &&
      in->lhs.name) {
    int hsz = (in->lhs.kind == IR_OPERAND_SYMBOL)
                  ? (mir_name_is_string_local(g, sirf, in->lhs.name) ? 16 : 0)
                  : mir_struct_temp_size(g, sirf, in->lhs.name);
    if (hsz > 0) {
      MirVregId sb =
          mir_emit_indirect_source_addr(fn, g, ctx, map, sirf, &in->lhs, hsz);
      if (sb == MIR_VREG_NONE) {
        return 0;
      }
      return mir_emit1(fn, MIR_MOV, mem, mir_op_vreg(sb), mir_op_none(), 8, 0,
                       0);
    }
  }
  MirOperand val = mir_value_operand(fn, g, ctx, map, &in->lhs);
  return mir_emit1(fn, MIR_MOV, mem, val, mir_op_none(), size, 0, 0);
}

/* ---- loop rotation ------------------------------------------------------ */

/* Rotate top-tested loops to bottom-tested ones. The lowering emits a while
 * loop as `label H; CMPBR cc -> Lexit; <body>; JMP H`, a fall-through test at
 * the top plus an unconditional back-jump every iteration (two branches/iter).
 * This rewrites it to `CMPBR cc -> Lexit (guard); H: <body>; CMPBR !cc -> H`,
 * so the back-edge is a single conditional branch and the top test runs once.
 *
 * Done by (1) converting each backward `JMP H` into a `CMPBR` with the header's
 * compare operands and the inverted condition (x86: cc ^ 1 flips the test),
 * targeting H, and (2) swapping `label H` with its following CMPBR so H now
 * marks the body start. Only safe when H is immediately followed by its CMPBR:
 * then the compare operands are loop-stable live values (a counter and a bound),
 * not temps computed by the header's condition evaluation, so re-testing them
 * at the back-edge (after the body's update) is exactly the loop condition. */
/* Fuse `MOV d, s` immediately followed by `MOVSX/MOVZX d, d` into
 * `MOVSX/MOVZX d, s`: the extend overwrites d with the extension of its low
 * bytes, so reading s directly is identical and the copy is dead. The narrow-
 * integer canonicalization emitted after an ASSIGN is exactly this shape
 * (`MOV cd, b; MOVSX cd, cd`), so this removes a register copy -- and one
 * short-lived value, easing register pressure -- per narrow copy-assign, which
 * is common in inlined and recursive code (e.g. rec_fib's `mov r13,r12; movsxd
 * r13,r13d`). Only vreg->vreg moves (a load/immediate MOV is left alone), and
 * the two are adjacent so s cannot be redefined between them. */
/* True for integer ops whose low 32 result bits depend only on the low 32 bits
 * of their inputs, so evaluating them at operand size 32 gives the same answer
 * as evaluating at 64 and discarding the top half. The right shifts, the
 * divides and MULHI are excluded: they read the bits above 32. */
static int mir_op_low32_is_self_contained(MirOpcode op) {
  switch (op) {
  case MIR_ADD:
  case MIR_SUB:
  case MIR_AND:
  case MIR_OR:
  case MIR_XOR:
  case MIR_NEG:
  case MIR_NOT:
  case MIR_IMUL:
    return 1;
  default:
    return 0;
  }
}

/* uint32 arithmetic is evaluated in 64-bit registers and truncated back after
 * every step, so each operation is followed by a zero-extend of its own result.
 * That extend is pure overhead: a 32-bit-operand-size instruction already
 * zero-extends into the full register. Where the extend immediately follows the
 * op that defines it -- so nothing can observe the untruncated value -- mark the
 * op as 32-bit and delete the extend.
 *
 * Two things follow. The instruction disappears, and so does its cycle: an
 * in-place `mov r8d, r8d` is one of the few register moves the hardware cannot
 * rename away, and in a serial recurrence (a bit-at-a-time CRC, a hash step)
 * that cycle sits on the loop-carried path. The 32-bit form also takes a full
 * 32-bit immediate, so masking constants like 0xEDB88320 stop needing a
 * register: at width 8 they do not fit a sign-extended imm32.
 *
 * Only MOVZX qualifies. MOVSX asks for sign extension, which a 32-bit operation
 * does not perform. */
static void mir_canonicalize_commutative(MirFunction *fn) {
  if (!fn) {
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    int commutative = in->op == MIR_ADD || in->op == MIR_IMUL ||
                      in->op == MIR_AND || in->op == MIR_OR ||
                      in->op == MIR_XOR;
    if (!commutative || in->is_float) {
      continue;
    }
    if (in->a.kind == MIR_OPK_IMM && in->b.kind != MIR_OPK_IMM) {
      MirOperand swap = in->a;
      in->a = in->b;
      in->b = swap;
    }
  }
}

static void mir_narrow_zero_extended_ops(MirFunction *fn) {
  if (!fn) {
    return;
  }
  for (size_t i = 1; i < fn->insn_count; i++) {
    MirInst *ext = &fn->insns[i];
    if (ext->op != MIR_MOVZX || ext->is_float || ext->width != 4 ||
        ext->dst.kind != MIR_OPK_VREG || ext->a.kind != MIR_OPK_VREG ||
        ext->dst.vreg != ext->a.vreg) {
      continue;
    }
    /* The defining op must be the previous real instruction: anything in
     * between could read the value before it is truncated. */
    size_t d = i;
    while (d > 0 && fn->insns[d - 1].op == MIR_NOP) {
      d--;
    }
    if (d == 0) {
      continue;
    }
    MirInst *def = &fn->insns[d - 1];
    if (def->is_float || def->width != 8 || def->dst.kind != MIR_OPK_VREG ||
        def->dst.vreg != ext->dst.vreg ||
        !mir_op_low32_is_self_contained(def->op)) {
      continue;
    }
    def->width = 4;
    ext->op = MIR_NOP;
  }
}

/* ---- demanded bits: drop extensions nothing looks at ---------------------
 *
 * int32 values live in 64-bit registers, so the frontend re-extends after every
 * step to keep the register agreeing with the declared type. Most of those
 * extensions are dead: an `i` that is only ever compared as int32 and fed to
 * more int32 arithmetic never has its upper half read, and the sign extension
 * that guards it is pure cost -- an instruction, and in a recurrence a cycle,
 * per step.
 *
 * The analysis asks one question per value: does EVERY reader of it look only
 * at its low 32 bits? Start by assuming yes for all, then let each instruction
 * veto the values it reads in full. A 32-bit ALU result depends only on the low
 * halves of its inputs, so those readers pass the question down to their own
 * destination -- which is why this iterates to a fixpoint. The answer only ever
 * moves from yes to no, so it converges, and anything not explicitly understood
 * vetoes, so an unmodelled opcode is safe by construction.
 *
 * Where the answer is yes, a MOVSX/MOVZX of that value becomes a plain copy,
 * which the register allocator's coalescer then usually removes outright. */

/* Reads of these ops' operands only need the low 32 bits when the op's own
 * result does. Right shifts, divides and MULHI are excluded: they read the
 * bits above 32 even when their result does not. */
static int mir_op_demand_passes_through(MirOpcode op) {
  switch (op) {
  case MIR_ADD:
  case MIR_SUB:
  case MIR_AND:
  case MIR_OR:
  case MIR_XOR:
  case MIR_NEG:
  case MIR_NOT:
  case MIR_IMUL:
  case MIR_SHL:
  case MIR_MOV:
    return 1;
  default:
    return 0;
  }
}

static void mir_demand_veto_operand(const MirOperand *op, char *low32,
                                    size_t n) {
  if (!op) {
    return;
  }
  if (op->kind == MIR_OPK_VREG && op->vreg != MIR_VREG_NONE &&
      (size_t)op->vreg < n) {
    low32[op->vreg] = 0;
  }
  if (op->kind == MIR_OPK_MEM) {
    /* An address is always used in full. */
    if (op->mem.base != MIR_VREG_NONE && (size_t)op->mem.base < n) {
      low32[op->mem.base] = 0;
    }
    if (op->mem.index != MIR_VREG_NONE && (size_t)op->mem.index < n) {
      low32[op->mem.index] = 0;
    }
  }
}

/* True when `in` writes a GP vreg whose own readers all want just 32 bits. */
static int mir_demand_dst_is_low32(const MirInst *in, const char *low32,
                                   size_t n) {
  return in->dst.kind == MIR_OPK_VREG && in->dst.vreg != MIR_VREG_NONE &&
         (size_t)in->dst.vreg < n && low32[in->dst.vreg];
}

static void mir_drop_dead_extensions(MirFunction *fn) {
  if (!fn || fn->vreg_count == 0) {
    return;
  }
  size_t n = fn->vreg_count;
  char *low32 = (char *)malloc(n);
  if (!low32) {
    return;
  }
  for (size_t v = 0; v < n; v++) {
    /* An address-taken value lives in memory, where a full-width load elsewhere
     * can see the bits this analysis would let go undefined. Float and vector
     * values are not in scope at all. */
    low32[v] = (fn->vregs[v].rclass == MIR_RC_GP && !fn->vregs[v].address_taken)
                   ? 1
                   : 0;
  }

  size_t last_low32_count = (size_t)-1;
  for (int changed = 1; changed;) {
    changed = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (in->op == MIR_NOP || in->op == MIR_LABEL) {
        continue;
      }
      int reads_low32_only = 0;
      if ((in->op == MIR_MOVZX || in->op == MIR_MOVSX) && in->width <= 4) {
        reads_low32_only = 1; /* only the low `width` bytes are extended */
      } else if ((in->op == MIR_CMP || in->op == MIR_CMPBR ||
                  in->op == MIR_TEST) &&
                 in->width == 4) {
        reads_low32_only = 1;
      } else if (mir_op_demand_passes_through(in->op)) {
        /* A store (`mov [mem], a`) has no vreg destination to pass the question
         * to, and writes `width` bytes; treat only the narrow store as narrow. */
        reads_low32_only = mir_demand_dst_is_low32(in, low32, n) ||
                           (in->op == MIR_MOV && in->dst.kind == MIR_OPK_MEM &&
                            in->width <= 4);
      }
      if (reads_low32_only) {
        /* The value operands are read narrowly, but an address inside them is
         * still an address. */
        if (in->a.kind == MIR_OPK_MEM) {
          mir_demand_veto_operand(&in->a, low32, n);
        }
        if (in->b.kind == MIR_OPK_MEM) {
          mir_demand_veto_operand(&in->b, low32, n);
        }
        if (in->dst.kind == MIR_OPK_MEM) {
          mir_demand_veto_operand(&in->dst, low32, n);
        }
        continue;
      }
      mir_demand_veto_operand(&in->a, low32, n);
      mir_demand_veto_operand(&in->b, low32, n);
      if (in->dst.kind == MIR_OPK_MEM) {
        mir_demand_veto_operand(&in->dst, low32, n);
      }
    }
    /* A veto can turn a pass-through op into a full reader of its own inputs,
     * so re-run until the flags stop moving. They only ever move one way. */
    size_t still_low32 = 0;
    for (size_t v = 0; v < n; v++) {
      still_low32 += (size_t)low32[v];
    }
    if (still_low32 != last_low32_count) {
      last_low32_count = still_low32;
      changed = 1;
    }
  }

  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    if ((in->op != MIR_MOVSX && in->op != MIR_MOVZX) || in->is_float ||
        in->width != 4 || in->a.kind != MIR_OPK_VREG ||
        in->dst.kind != MIR_OPK_VREG || !low32[in->dst.vreg]) {
      continue;
    }
    /* Nothing reads above bit 31, so the extension is just a copy. */
    in->op = MIR_MOV;
    in->width = 8;
    in->is_unsigned = 0;
  }

  free(low32);
}

static size_t mir_label_index(const MirFunction *fn, const char *name);

static int mir_vreg_defs_are_sext32(const MirFunction *fn, MirVregId v,
                                    const unsigned char *guarded_add) {
  int defs = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP || in->dst.kind != MIR_OPK_VREG ||
        in->dst.vreg != v) {
      continue;
    }
    if (in->op == MIR_CMPBR || in->op == MIR_JCC || in->op == MIR_JMP) {
      continue;
    }
    defs++;
    if (in->op == MIR_MOVSX && in->width == 4) {
      continue;
    }
    if (in->op == MIR_MOV && in->width == 4 && !in->is_unsigned &&
        !in->is_float && in->a.kind == MIR_OPK_MEM) {
      continue;
    }
    if (in->op == MIR_MOV && !in->is_float && in->a.kind == MIR_OPK_IMM &&
        in->a.imm >= -2147483648LL && in->a.imm <= 2147483647LL) {
      continue;
    }
    if (in->op == MIR_ADD && in->width == 8 && !in->is_float &&
        in->a.kind == MIR_OPK_VREG && in->a.vreg == v &&
        in->b.kind == MIR_OPK_IMM && in->b.imm == 1) {
      if (guarded_add && guarded_add[i]) {
        continue;
      }
      size_t nx = i + 1;
      while (nx < fn->insn_count && fn->insns[nx].op == MIR_NOP) {
        nx++;
      }
      if (nx < fn->insn_count && fn->insns[nx].op == MIR_MOVSX &&
          fn->insns[nx].width == 4 &&
          fn->insns[nx].dst.kind == MIR_OPK_VREG &&
          fn->insns[nx].dst.vreg == v &&
          fn->insns[nx].a.kind == MIR_OPK_VREG &&
          fn->insns[nx].a.vreg == v) {
        continue;
      }
    }
    return 0;
  }
  return defs > 0;
}

static int mir_operand_reads_vreg(const MirOperand *op, MirVregId v) {
  if (op->kind == MIR_OPK_VREG && op->vreg == v) {
    return 1;
  }
  if (op->kind == MIR_OPK_MEM &&
      (op->mem.base == v || op->mem.index == v)) {
    return 1;
  }
  return 0;
}

static int mir_sext_label_covered(const MirFunction *fn, size_t l, size_t g,
                                  const unsigned char *visited,
                                  unsigned char *memo);

static int mir_sext_site_covered(const MirFunction *fn, size_t at, size_t g,
                                 const unsigned char *visited,
                                 unsigned char *memo) {
  size_t i = at;
  while (i > 0) {
    if (i == g) {
      return 1;
    }
    if (fn->insns[i].op == MIR_LABEL) {
      return mir_sext_label_covered(fn, i, g, visited, memo);
    }
    i--;
  }
  return 0;
}

static int mir_sext_label_covered(const MirFunction *fn, size_t l, size_t g,
                                  const unsigned char *visited,
                                  unsigned char *memo) {
  if (memo[l] == 1 || memo[l] == 3) {
    return 1;
  }
  if (memo[l] == 2) {
    return 0;
  }
  memo[l] = 3;
  const char *name = fn->insns[l].dst.sym;
  if (!name) {
    memo[l] = 2;
    return 0;
  }
  if (l > 0) {
    size_t p = l - 1;
    while (p > 0 && fn->insns[p].op == MIR_NOP) {
      p--;
    }
    const MirInst *prev = &fn->insns[p];
    if (prev->op != MIR_JMP && prev->op != MIR_RET) {
      if (!visited[p] || !mir_sext_site_covered(fn, p, g, visited, memo)) {
        memo[l] = 2;
        return 0;
      }
    }
  }
  for (size_t j = 0; j < fn->insn_count; j++) {
    const MirInst *jj = &fn->insns[j];
    if (jj->op != MIR_JMP && jj->op != MIR_JCC && jj->op != MIR_CMPBR &&
        jj->op != MIR_FCMPBR) {
      continue;
    }
    if (jj->dst.kind != MIR_OPK_LABEL || !jj->dst.sym ||
        strcmp(jj->dst.sym, name) != 0) {
      continue;
    }
    if (j == g) {
      continue;
    }
    if (!visited[j] || !mir_sext_site_covered(fn, j, g, visited, memo)) {
      memo[l] = 2;
      return 0;
    }
  }
  memo[l] = 1;
  return 1;
}

static void mir_elide_guarded_sext(MirFunction *fn) {
  if (!fn || fn->insn_count == 0) {
    return;
  }
  size_t n = fn->insn_count;
  unsigned char *visited = calloc(n, 1);
  unsigned char *covered = calloc(n, 1);
  unsigned char *guarded_add = calloc(n, 1);
  size_t *stack = malloc(n * sizeof(size_t));
  if (!visited || !covered || !guarded_add || !stack) {
    free(visited);
    free(covered);
    free(guarded_add);
    free(stack);
    return;
  }
  for (size_t g = 0; g < n; g++) {
    const MirInst *guard = &fn->insns[g];
    if (guard->op != MIR_CMPBR ||
        (guard->width != 8 && guard->width != 4) || guard->cc != 0x8D ||
        guard->is_float || guard->a.kind != MIR_OPK_VREG) {
      continue;
    }
    MirVregId A = guard->a.vreg;
    MirVregId B = MIR_VREG_NONE;
    int b_is_mem = 0;
    if (guard->b.kind == MIR_OPK_VREG) {
      B = guard->b.vreg;
      if (A == B) {
        continue;
      }
      if (guard->width == 8 &&
          !mir_vreg_defs_are_sext32(fn, B, guarded_add)) {
        continue;
      }
    } else if (guard->width == 4 && (guard->b.kind == MIR_OPK_MEM ||
                                     guard->b.kind == MIR_OPK_STACKHOME)) {
      b_is_mem = 1;
    } else {
      continue;
    }
    if (!mir_vreg_defs_are_sext32(fn, A, guarded_add)) {
      continue;
    }
    memset(visited, 0, n);
    visited[g] = 1;
    size_t cand_movsx[8];
    size_t cand_add[8];
    unsigned char cand_inplace[8];
    /* 0: the add already targets A, just drop the sext.
     * 1: the add targets a temp nothing else reads, so retarget it to A.
     * 2: the temp IS read elsewhere -- json_parse stores `pos + 1` back
     *    through the Parser between the add and the sext -- so leave both
     *    the add and that reader alone and weaken the sext to a plain move.
     *    The guard proves the value fits in int32, which is the whole content
     *    of the sign extension; the copy that remains is what the allocator
     *    coalesces away. */
    unsigned char cand_weaken[8];
    size_t cand_count = 0;
    size_t sp = 0;
    if (g + 1 < n) {
      stack[sp++] = g + 1;
    }
    while (sp) {
      size_t i = stack[--sp];
      if (i >= n || visited[i]) {
        continue;
      }
      visited[i] = 1;
      MirInst *in = &fn->insns[i];
      if (in->op == MIR_NOP || in->op == MIR_LABEL) {
        if (i + 1 < n) {
          stack[sp++] = i + 1;
        }
        continue;
      }
      if (in->op == MIR_RET || in->op == MIR_CALL) {
        continue;
      }
      if (in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR) {
        if (in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
          size_t t = mir_label_index(fn, in->dst.sym);
          if (t != (size_t)-1 && t > g) {
            stack[sp++] = t;
          }
        }
        if (in->op != MIR_JMP && i + 1 < n) {
          stack[sp++] = i + 1;
        }
        continue;
      }
      if (b_is_mem && (in->dst.kind == MIR_OPK_MEM ||
                       in->dst.kind == MIR_OPK_STACKHOME)) {
        continue;
      }
      if (in->dst.kind == MIR_OPK_VREG &&
          (in->dst.vreg == A || (B != MIR_VREG_NONE && in->dst.vreg == B))) {
        if (in->dst.vreg == A && in->op == MIR_ADD && in->width == 8 &&
            !in->is_float && in->a.kind == MIR_OPK_VREG && in->a.vreg == A &&
            in->b.kind == MIR_OPK_IMM && in->b.imm == 1 && cand_count < 8) {
          size_t nx = i + 1;
          while (nx < n && fn->insns[nx].op == MIR_NOP) {
            nx++;
          }
          if (nx < n && fn->insns[nx].op == MIR_MOVSX &&
              fn->insns[nx].width == 4 &&
              fn->insns[nx].dst.kind == MIR_OPK_VREG &&
              fn->insns[nx].dst.vreg == A &&
              fn->insns[nx].a.kind == MIR_OPK_VREG &&
              fn->insns[nx].a.vreg == A) {
            cand_movsx[cand_count] = nx;
            cand_add[cand_count] = i;
            cand_inplace[cand_count] = 1;
            cand_weaken[cand_count] = 0;
            cand_count++;
          }
          continue;
        }
        if (in->dst.vreg == A && in->op == MIR_MOVSX && in->width == 4 &&
            in->a.kind == MIR_OPK_VREG) {
          MirVregId C = in->a.vreg;
          size_t def_at = (size_t)-1;
          size_t use_count = 0;
          int multi_def = 0;
          for (size_t k = 0; k < n; k++) {
            const MirInst *kk = &fn->insns[k];
            if (kk->op == MIR_NOP) {
              continue;
            }
            if (kk->dst.kind == MIR_OPK_VREG && kk->dst.vreg == C) {
              if (def_at != (size_t)-1) {
                multi_def = 1;
                break;
              }
              def_at = k;
            }
            if (k != i &&
                (mir_operand_reads_vreg(&kk->a, C) ||
                 mir_operand_reads_vreg(&kk->b, C) ||
                 (kk->dst.kind == MIR_OPK_MEM &&
                  mir_operand_reads_vreg(&kk->dst, C)))) {
              use_count++;
            }
          }
          if (!multi_def && def_at != (size_t)-1 && use_count == 0 &&
              visited[def_at] && cand_count < 8) {
            MirInst *add = &fn->insns[def_at];
            if (add->op == MIR_ADD && add->width == 8 && !add->is_float &&
                add->dst.kind == MIR_OPK_VREG && add->dst.vreg == C &&
                add->a.kind == MIR_OPK_VREG && add->a.vreg == A &&
                add->b.kind == MIR_OPK_IMM && add->b.imm == 1) {
              int clean = 1;
              for (size_t k = def_at + 1; k < i; k++) {
                const MirInst *kk = &fn->insns[k];
                if (kk->op == MIR_NOP) {
                  continue;
                }
                clean = 0;
                break;
              }
              if (clean) {
                cand_movsx[cand_count] = i;
                cand_add[cand_count] = def_at;
                cand_inplace[cand_count] = 0;
                cand_weaken[cand_count] = 0;
                cand_count++;
              }
            }
          }
          /* The retarget above needs the temp to be private and adjacent.
           * When it is neither, the sign extension is still redundant. */
          if (!multi_def && def_at != (size_t)-1 && visited[def_at] &&
              cand_count < 8) {
            const MirInst *add = &fn->insns[def_at];
            int already = 0;
            for (size_t q = 0; q < cand_count; q++) {
              if (cand_movsx[q] == i) {
                already = 1;
                break;
              }
            }
            if (!already && add->op == MIR_ADD && add->width == 8 &&
                !add->is_float && add->dst.kind == MIR_OPK_VREG &&
                add->dst.vreg == C && add->a.kind == MIR_OPK_VREG &&
                add->a.vreg == A && add->b.kind == MIR_OPK_IMM &&
                add->b.imm == 1) {
              cand_movsx[cand_count] = i;
              cand_add[cand_count] = def_at;
              cand_inplace[cand_count] = 0;
              cand_weaken[cand_count] = 1;
              cand_count++;
            }
          }
        }
        continue;
      }
      if (i + 1 < n) {
        stack[sp++] = i + 1;
      }
    }
    if (!cand_count) {
      continue;
    }
    memset(covered, 0, n);
    for (size_t c = 0; c < cand_count; c++) {
      if (!mir_sext_site_covered(fn, cand_add[c], g, visited, covered)) {
        continue;
      }

      MirInst *sext = &fn->insns[cand_movsx[c]];
      if (cand_weaken[c]) {
        sext->op = MIR_MOV;
        sext->width = 8;
        sext->is_unsigned = 0;
        guarded_add[cand_add[c]] = 1;
        continue;
      }
      if (!cand_inplace[c]) {
        fn->insns[cand_add[c]].dst = mir_op_vreg(A);
      }
      guarded_add[cand_add[c]] = 1;
      sext->op = MIR_NOP;
      sext->dst = mir_op_none();
      sext->a = mir_op_none();
      sext->b = mir_op_none();
    }
  }
  free(visited);
  free(covered);
  free(guarded_add);
  free(stack);
}

static void mir_fuse_mov_then_extend(MirFunction *fn) {
  if (!fn) {
    return;
  }
  for (size_t i = 1; i < fn->insn_count; i++) {
    MirInst *ext = &fn->insns[i];
    if ((ext->op != MIR_MOVSX && ext->op != MIR_MOVZX) || ext->is_float ||
        ext->dst.kind != MIR_OPK_VREG || ext->a.kind != MIR_OPK_VREG ||
        ext->dst.vreg != ext->a.vreg) {
      continue;
    }
    MirInst *mov = &fn->insns[i - 1];
    if (mov->op != MIR_MOV || mov->is_float || mov->dst.kind != MIR_OPK_VREG ||
        mov->dst.vreg != ext->dst.vreg || mov->a.kind != MIR_OPK_VREG ||
        mov->a.vreg == ext->dst.vreg) {
      continue;
    }
    ext->a.vreg = mov->a.vreg; /* extend reads the copy's source directly */
    mov->op = MIR_NOP;         /* the copy is now dead */
  }
}

/* Every vreg an operand READS (a MEM operand reads its base and index). */
static void mir_operand_reads_pair(const MirOperand *op, MirVregId out[2]) {
  out[0] = MIR_VREG_NONE;
  out[1] = MIR_VREG_NONE;
  if (!op) {
    return;
  }
  if (op->kind == MIR_OPK_VREG) {
    out[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    out[0] = op->mem.base;
    out[1] = op->mem.index;
  }
}

/* Rewrite every vreg an operand READS from `from` to `to`. */
static void mir_operand_rename_read(MirOperand *op, MirVregId from,
                                    MirVregId to) {
  if (!op) {
    return;
  }
  if (op->kind == MIR_OPK_VREG && op->vreg == from) {
    op->vreg = to;
  } else if (op->kind == MIR_OPK_MEM) {
    if (op->mem.base == from) {
      op->mem.base = to;
    }
    if (op->mem.index == from) {
      op->mem.index = to;
    }
  }
}

/* ---- fold a constant address adjustment into the access -----------------
 *
 * Reading a struct field lowers to "compute the base, add the field offset,
 * load through it". x86 addressing already has that offset field, so the add
 * is free to absorb: `add rax, 4; mov edx, [rax]` becomes `mov edx, [rax+4]`.
 * A three-field node read pays this three times per visit.
 *
 * Only an add whose result is used exactly once, by that one access, can move
 * -- otherwise the address is still needed in a register. */
static void mir_fold_address_offsets(MirFunction *fn) {
  if (!fn || fn->insn_count < 2 || fn->vreg_count == 0) {
    return;
  }
  int *uses = (int *)calloc(fn->vreg_count, sizeof(int));
  int *defs = (int *)calloc(fn->vreg_count, sizeof(int));
  if (!uses || !defs) {
    free(uses);
    free(defs);
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP) {
      continue; /* retired: its leftover operands are neither reads nor writes */
    }
    const MirOperand *ops[3] = {&in->a, &in->b, &in->dst};
    for (int k = 0; k < 3; k++) {
      MirVregId r[2];
      if (ops[k] == &in->dst && in->dst.kind == MIR_OPK_VREG) {
        defs[in->dst.vreg]++;
        continue;
      }
      mir_operand_reads_pair(ops[k], r);
      for (int e = 0; e < 2; e++) {
        if (r[e] >= 0 && (size_t)r[e] < fn->vreg_count) {
          uses[r[e]]++;
        }
      }
    }
  }

  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *add = &fn->insns[i];
    if (add->op != MIR_ADD || add->is_float || add->width != 8 ||
        add->dst.kind != MIR_OPK_VREG || add->a.kind != MIR_OPK_VREG ||
        add->b.kind != MIR_OPK_IMM) {
      continue;
    }
    MirVregId addr = add->dst.vreg;
    if (addr == add->a.vreg || uses[addr] != 1 || defs[addr] != 1 ||
        fn->vregs[addr].address_taken) {
      continue;
    }
    if (add->b.imm < INT32_MIN / 2 || add->b.imm > INT32_MAX / 2) {
      continue;
    }
    /* The access must be the very next instruction -- anything in between could
     * redefine the base the offset would now be applied to. The lowering does
     * leave plain `vC <- addr` copies in the way, though, so walk through any
     * that are themselves used exactly once: they are links in the same chain,
     * and retiring them leaves nothing behind. */
    MirVregId cur = addr;
    size_t u = i;
    size_t chain[4];
    size_t chain_n = 0;
    MirOperand *mem = NULL;
    for (;;) {
      u++;
      while (u < fn->insn_count && fn->insns[u].op == MIR_NOP) {
        u++;
      }
      if (u >= fn->insn_count) {
        break;
      }
      MirInst *use = &fn->insns[u];
      if (use->a.kind == MIR_OPK_MEM && use->a.mem.base == cur) {
        mem = &use->a;
      } else if (use->dst.kind == MIR_OPK_MEM && use->dst.mem.base == cur) {
        mem = &use->dst;
      }
      if (mem) {
        break;
      }
      if (chain_n < 4 && use->op == MIR_MOV && use->dst.kind == MIR_OPK_VREG &&
          use->a.kind == MIR_OPK_VREG && use->a.vreg == cur &&
          use->b.kind == MIR_OPK_NONE && uses[cur] == 1 &&
          defs[use->dst.vreg] == 1 && !fn->vregs[use->dst.vreg].address_taken) {
        chain[chain_n++] = u;
        cur = use->dst.vreg;
        continue;
      }
      break;
    }
    /* The float access path has no scaled-index form, so never hand it one. */
    if (!mem || uses[cur] != 1 || mem->mem.index == cur ||
        mem->mem.phys_base_valid ||
        (fn->insns[u].is_float && mem->mem.index != MIR_VREG_NONE)) {
      continue;
    }
    long long disp = (long long)mem->mem.disp + add->b.imm;
    if (disp < INT32_MIN || disp > INT32_MAX) {
      continue;
    }
    mem->mem.base = add->a.vreg;
    mem->mem.disp = (int)disp;
    add->op = MIR_NOP;
    for (size_t c = 0; c < chain_n; c++) {
      fn->insns[chain[c]].op = MIR_NOP;
    }
    uses[add->a.vreg]++;
    uses[addr] = 0;
  }

  free(uses);
  free(defs);
}

/* ---- redundant load elimination -----------------------------------------
 *
 * `if (data[i] <= data[j]) { tmp[k] = data[i]; }` loads data[i] twice: once to
 * compare it and once to store it. The second load is reached only through the
 * first, and nothing writes memory in between, so it can read the register the
 * first one already filled. Merge sort's inner loop pays this on every
 * iteration, and the shape -- test a value, then use it -- is everywhere.
 *
 * This is a linear scan carrying a small table of loads whose results are still
 * valid. An entry dies when anything could have changed what it loaded (a
 * store, a call, an inline kernel), when one of its address registers is
 * rewritten, when its own destination is rewritten, or at a label the value
 * might not have reached along every incoming edge. */

#define MIR_LOAD_TABLE_MAX 12

typedef struct {
  int def;        /* MIR index of the load */
  MirVregId dst;  /* vreg it filled */
  MirMem mem;     /* address it read */
  int width;
  int is_unsigned;
  int is_float;
} MirAvailableLoad;

/* A plain register load: `dst(vreg) <- [mem]`, no scaling of the result beyond
 * the width/signedness the instruction already carries. */
static int mir_is_plain_load(const MirInst *in) {
  return in->op == MIR_MOV && in->a.kind == MIR_OPK_MEM &&
         in->dst.kind == MIR_OPK_VREG;
}

static int mir_mem_same(const MirMem *a, const MirMem *b) {
  return a->base == b->base && a->index == b->index && a->scale == b->scale &&
         a->disp == b->disp && a->phys_base_valid == b->phys_base_valid &&
         a->phys_base == b->phys_base;
}

/* Could this instruction change what some earlier load returned? Stores are the
 * obvious case; a call or an inline kernel can write anywhere. */
static int mir_clobbers_memory(const MirInst *in) {
  if (in->dst.kind == MIR_OPK_MEM) {
    return 1;
  }
  switch (in->op) {
  case MIR_CALL:
  case MIR_CALL_INDIRECT:
  case MIR_TRAP:
  case MIR_STORE_GLOBAL:
  case MIR_STORE_OUTARG:
    return 1;
  default:
    return mir_op_is_inline_kernel(in->op);
  }
}

/* Keep only the entries `keep` also has: what is available where two paths meet
 * is what was available on both. Identity is the destination vreg -- the same
 * vreg is the same value. */
static size_t mir_load_table_intersect(MirAvailableLoad *dst, size_t dst_n,
                                       const MirAvailableLoad *keep,
                                       size_t keep_n) {
  size_t n = 0;
  for (size_t a = 0; a < dst_n; a++) {
    for (size_t b = 0; b < keep_n; b++) {
      if (dst[a].dst == keep[b].dst) {
        dst[n++] = dst[a];
        break;
      }
    }
  }
  return n;
}

/* The instruction before `index`, skipping NOPs; -1 if there is none. */
static int mir_prev_real(const MirFunction *fn, size_t index) {
  for (size_t k = index; k > 0; k--) {
    if (fn->insns[k - 1].op != MIR_NOP) {
      return (int)(k - 1);
    }
  }
  return -1;
}

static void mir_cse_loads(MirFunction *fn) {
  if (!fn || fn->insn_count < 2) {
    return;
  }
  /* Per label: the highest index that branches to it (a back-edge, i.e. a loop
   * header, if it is at or past the label), and an ordinal for the snapshot
   * table below. */
  int *pred_hi = (int *)malloc(fn->insn_count * sizeof(int));
  int *label_ord = (int *)malloc(fn->insn_count * sizeof(int));
  if (!pred_hi || !label_ord) {
    free(pred_hi);
    free(label_ord);
    return;
  }
  size_t label_count = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    pred_hi[i] = -1;
    label_ord[i] = (fn->insns[i].op == MIR_LABEL) ? (int)label_count++ : -1;
  }
  for (size_t b = 0; b < fn->insn_count; b++) {
    const MirInst *in = &fn->insns[b];
    if (in->op != MIR_JMP && in->op != MIR_JCC && in->op != MIR_CMPBR &&
        in->op != MIR_FCMPBR) {
      continue;
    }
    if (in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      continue;
    }
    for (size_t d = 0; d < fn->insn_count; d++) {
      const MirInst *lb = &fn->insns[d];
      if (lb->op == MIR_LABEL && lb->dst.kind == MIR_OPK_LABEL && lb->dst.sym &&
          strcmp(lb->dst.sym, in->dst.sym) == 0) {
        if ((int)b > pred_hi[d]) pred_hi[d] = (int)b;
        break;
      }
    }
  }

  /* What was available at each forward branch into a label. Without this, an
   * `if (p) { ...store... } else { ...reuse... }` loses the reuse: the linear
   * walk reaches the else-arm's label having passed through the then-arm's
   * store, which the branched-to path never executes. */
  MirAvailableLoad *snap = NULL;
  size_t *snap_n = NULL;
  char *snap_seen = NULL;
  if (label_count > 0 && label_count <= 1024) {
    snap = (MirAvailableLoad *)malloc(label_count * MIR_LOAD_TABLE_MAX *
                                      sizeof(MirAvailableLoad));
    snap_n = (size_t *)calloc(label_count, sizeof(size_t));
    snap_seen = (char *)calloc(label_count, 1);
    if (!snap || !snap_n || !snap_seen) {
      free(snap);
      free(snap_n);
      free(snap_seen);
      snap = NULL;
      snap_n = NULL;
      snap_seen = NULL;
    }
  }

  MirAvailableLoad table[MIR_LOAD_TABLE_MAX];
  size_t table_n = 0;

  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP) {
      continue;
    }

    if (mir_clobbers_memory(in)) {
      table_n = 0;
      continue;
    }

    if (snap && (in->op == MIR_JMP || in->op == MIR_JCC ||
                 in->op == MIR_CMPBR || in->op == MIR_FCMPBR) &&
        in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      for (size_t d = i + 1; d < fn->insn_count; d++) {
        const MirInst *lb = &fn->insns[d];
        if (lb->op == MIR_LABEL && lb->dst.kind == MIR_OPK_LABEL &&
            lb->dst.sym && strcmp(lb->dst.sym, in->dst.sym) == 0) {
          int o = label_ord[d];
          MirAvailableLoad *slot = snap + (size_t)o * MIR_LOAD_TABLE_MAX;
          if (!snap_seen[o]) {
            memcpy(slot, table, table_n * sizeof(MirAvailableLoad));
            snap_n[o] = table_n;
            snap_seen[o] = 1;
          } else {
            snap_n[o] = mir_load_table_intersect(slot, snap_n[o], table,
                                                 table_n);
          }
          break;
        }
      }
    }

    if (in->op == MIR_LABEL) {
      int o = label_ord[i];
      if (pred_hi[i] >= (int)i) {
        table_n = 0; /* loop header: memory may change across the back-edge */
      } else if (pred_hi[i] < 0) {
        /* Only reachable by falling through: the walked table is exact. */
      } else if (!snap || !snap_seen[o]) {
        table_n = 0;
      } else {
        const MirAvailableLoad *slot = snap + (size_t)o * MIR_LOAD_TABLE_MAX;
        int prev = mir_prev_real(fn, i);
        int falls_through =
            prev >= 0 && fn->insns[prev].op != MIR_JMP &&
            fn->insns[prev].op != MIR_RET;
        if (falls_through) {
          table_n = mir_load_table_intersect(table, table_n, slot, snap_n[o]);
        } else {
          memcpy(table, slot, snap_n[o] * sizeof(MirAvailableLoad));
          table_n = snap_n[o];
        }
      }
      continue;
    }

    /* Reuse: an identical load whose value is still around becomes a copy. */
    if (mir_is_plain_load(in)) {
      for (size_t e = 0; e < table_n; e++) {
        if (table[e].width == in->width &&
            table[e].is_unsigned == in->is_unsigned &&
            table[e].is_float == in->is_float &&
            table[e].dst != in->dst.vreg &&
            mir_mem_same(&table[e].mem, &in->a.mem)) {
          in->a = mir_op_vreg(table[e].dst);
          in->b = mir_op_none();
          break;
        }
      }
    }

    /* Anything this instruction writes invalidates the entries that depend on
     * it, whether as an address register or as the cached result itself. */
    if (in->dst.kind == MIR_OPK_VREG) {
      MirVregId w = in->dst.vreg;
      size_t keep = 0;
      for (size_t e = 0; e < table_n; e++) {
        if (table[e].dst != w && table[e].mem.base != w &&
            table[e].mem.index != w) {
          table[keep++] = table[e];
        }
      }
      table_n = keep;
    }

    if (mir_is_plain_load(in) && in->a.kind == MIR_OPK_MEM &&
        table_n < MIR_LOAD_TABLE_MAX) {
      table[table_n].def = (int)i;
      table[table_n].dst = in->dst.vreg;
      table[table_n].mem = in->a.mem;
      table[table_n].width = in->width;
      table[table_n].is_unsigned = in->is_unsigned;
      table[table_n].is_float = in->is_float;
      table_n++;
    }
  }

  free(pred_hi);
  free(label_ord);
  free(snap);
  free(snap_n);
  free(snap_seen);
}


/* ---- float64 pair vectorizer (SLP) --------------------------------------- */
/*
 * Structs of the form {x, y, ...} in float64 produce statement pairs that
 * differ only in the field offset: `p.vx += f*dx; p.vy += f*dy` is two loads,
 * two multiplies, two adds and two stores that clang runs as one movupd,
 * mulpd, addpd, movupd. This pass finds ADJACENT float64 store pairs (same
 * base register, displacements 8 apart), grows the operation DAG upward while
 * both lanes stay isomorphic, and rewrites the pair lanes into one width-16
 * instruction each: loads become movupd, arithmetic becomes the packed VEX
 * form, a scalar appearing in both lanes becomes one vmovddup.
 *
 * Soundness rules:
 *  - Everything stays inside one straight-line region (no labels, branches,
 *    or calls between the earliest and latest instruction touched).
 *  - The fused instruction sits at the LATER lane's original index, so each
 *    earlier lane conceptually moves down: any memory access strictly between
 *    the two lanes must be provably disjoint (same base register with
 *    non-overlapping displacements). A different base register may alias and
 *    refuses the pair.
 *  - An original whose value is read outside the graph is KEPT (the pair
 *    recomputes its lanes); only fully-internal originals are dropped. That
 *    trades a little duplicate scalar work for never needing lane extraction,
 *    and the dead-code sweep already removes what turns out unread.
 */

#define MIR_SLP_MAX_NODES 24

typedef struct {
  MirVregId lo;      /* lane-0 value (MIR_VREG_NONE for a store node) */
  MirVregId hi;
  size_t lo_at;      /* defining instruction indices */
  size_t hi_at;
  MirVregId pair;    /* the width-16 vreg carrying both lanes */
  int kind;          /* 0 load, 1 binop, 2 dup, 3 store */
  MirOpcode op;      /* for binops */
  int child_a;       /* node indices, -1 = none */
  int child_b;
  int keep_originals;
} MirSlpNode;

typedef struct {
  MirFunction *fn;
  const int *def_count;  /* per-vreg definition count */
  const size_t *def_at;  /* index of the single def (valid when count==1) */
  const int *use_count;  /* per-vreg read count */
  MirSlpNode nodes[MIR_SLP_MAX_NODES];
  int node_count;
  size_t region_lo;
  size_t region_hi;
} MirSlpGraph;

static int mir_slp_region_ok(const MirFunction *fn, size_t lo, size_t hi) {
  for (size_t i = lo; i <= hi; i++) {
    switch (fn->insns[i].op) {
    case MIR_LABEL:
    case MIR_JMP:
    case MIR_JCC:
    case MIR_CMPBR:
    case MIR_FCMPBR:
    case MIR_CALL:
    case MIR_CALL_INDIRECT:
    case MIR_RET:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

static int mir_slp_is_f64_load(const MirInst *in) {
  return in->op == MIR_MOV && in->is_float && in->width == 8 &&
         in->a.kind == MIR_OPK_MEM && in->a.mem.index == MIR_VREG_NONE &&
         in->dst.kind == MIR_OPK_VREG;
}

static int mir_slp_is_f64_store(const MirInst *in) {
  return in->op == MIR_MOV && in->is_float && in->width == 8 &&
         in->dst.kind == MIR_OPK_MEM && in->dst.mem.index == MIR_VREG_NONE &&
         in->a.kind == MIR_OPK_VREG;
}

static int mir_slp_is_f64_binop(const MirInst *in) {
  /* the opcode is float by definition; is_float is not set uniformly */
  return (in->op == MIR_FADD || in->op == MIR_FSUB || in->op == MIR_FMUL ||
          in->op == MIR_FDIV) &&
         in->width == 8 && in->dst.kind == MIR_OPK_VREG &&
         in->a.kind == MIR_OPK_VREG && in->b.kind == MIR_OPK_VREG;
}

/* Address bases are equal when they are the same register, or when both are
 * single-def registers computed by the same operation over equal operands:
 * the lowering recomputes `live + i*32` per field access, so the .x and .y
 * addresses arrive in different vregs holding one value. */
static int mir_slp_same_base(const MirFunction *fn, const int *def_count,
                             const size_t *def_at, MirVregId a, MirVregId b,
                             int depth);
static int mir_slp_mem_disjoint(const MirFunction *fn, const int *def_count,
                                const size_t *def_at, const MirOperand *acc,
                                MirVregId base, int disp, int depth);

static int mir_slp_operand_equal(const MirFunction *fn, const int *def_count,
                                 const size_t *def_at, const MirOperand *x,
                                 const MirOperand *y, int depth) {
  if (x->kind != y->kind) {
    return 0;
  }
  switch (x->kind) {
  case MIR_OPK_VREG:
    return mir_slp_same_base(fn, def_count, def_at, x->vreg, y->vreg, depth);
  case MIR_OPK_IMM:
    return x->imm == y->imm;
  case MIR_OPK_NONE:
    return 1;
  default:
    return 0;
  }
}

static int mir_slp_same_base(const MirFunction *fn, const int *def_count,
                             const size_t *def_at, MirVregId a, MirVregId b,
                             int depth) {
  if (a == b) {
    return 1;
  }
  if (depth > 4 || a == MIR_VREG_NONE || b == MIR_VREG_NONE ||
      (size_t)a >= fn->vreg_count || (size_t)b >= fn->vreg_count ||
      def_count[a] != 1 || def_count[b] != 1) {
    return 0;
  }
  const MirInst *da = &fn->insns[def_at[a]];
  const MirInst *db = &fn->insns[def_at[b]];
  if (da->op != db->op || da->width != db->width ||
      da->is_float != db->is_float) {
    return 0;
  }
  switch (da->op) {
  case MIR_MOV:
    if (da->a.kind == MIR_OPK_VREG && db->a.kind == MIR_OPK_VREG) {
      return mir_slp_same_base(fn, def_count, def_at, da->a.vreg, db->a.vreg,
                               depth + 1);
    }
    /* Two loads of one location are one value when nothing can have written
     * it in between: same width, same address (base equivalence + equal
     * displacement, no index), and every store between the two positions is
     * provably disjoint from it. The lowering reloads `w->live` per field
     * access, so every address chain bottoms out here. */
    if (da->a.kind == MIR_OPK_MEM && db->a.kind == MIR_OPK_MEM &&
        da->a.mem.index == MIR_VREG_NONE && db->a.mem.index == MIR_VREG_NONE &&
        da->a.mem.disp == db->a.mem.disp &&
        mir_slp_same_base(fn, def_count, def_at, da->a.mem.base,
                          db->a.mem.base, depth + 1)) {
      size_t lo_at = def_at[a] < def_at[b] ? def_at[a] : def_at[b];
      size_t hi_at = def_at[a] < def_at[b] ? def_at[b] : def_at[a];
      for (size_t i = lo_at + 1; i < hi_at; i++) {
        const MirInst *in = &fn->insns[i];
        switch (in->op) {
        case MIR_LABEL:
        case MIR_JMP:
        case MIR_JCC:
        case MIR_CMPBR:
        case MIR_FCMPBR:
        case MIR_CALL:
        case MIR_CALL_INDIRECT:
        case MIR_RET:
          return 0;
        default:
          break;
        }
        if (in->dst.kind == MIR_OPK_MEM &&
            !mir_slp_mem_disjoint(fn, def_count, def_at, &in->dst,
                                  da->a.mem.base, da->a.mem.disp,
                                  depth + 1)) {
          return 0;
        }
      }
      return 1;
    }
    return 0;
  case MIR_ADD:
  case MIR_SHL:
  case MIR_IMUL:
  case MIR_LEA:
    return mir_slp_operand_equal(fn, def_count, def_at, &da->a, &db->a,
                                 depth + 1) &&
           mir_slp_operand_equal(fn, def_count, def_at, &da->b, &db->b,
                                 depth + 1);
  default:
    return 0;
  }
}

/* mem accesses [base+disp, +8) provably disjoint: equal base value, ranges
 * apart. A base that may differ may alias. */
static void mir_slp_resolve_addr(const MirFunction *fn, const int *def_count,
                                 const size_t *def_at, MirVregId base,
                                 int disp, MirVregId *root_out, int *disp_out);

static int mir_slp_mem_disjoint(const MirFunction *fn, const int *def_count,
                                const size_t *def_at, const MirOperand *acc,
                                MirVregId base, int disp, int depth) {
  MirVregId acc_root, base_root;
  int acc_disp, base_disp;
  if (depth > 6 || acc->mem.index != MIR_VREG_NONE) {
    return 0;
  }
  mir_slp_resolve_addr(fn, def_count, def_at, acc->mem.base, acc->mem.disp,
                       &acc_root, &acc_disp);
  mir_slp_resolve_addr(fn, def_count, def_at, base, disp, &base_root,
                       &base_disp);
  if (!mir_slp_same_base(fn, def_count, def_at, acc_root, base_root, depth)) {
    return 0;
  }
  return acc_disp + 8 <= base_disp || base_disp + 8 <= acc_disp;
}

/* The earlier lane at `from` conceptually moves down to `to`: every memory
 * access strictly between must be disjoint with (base, disp). Accesses that
 * belong to the graph itself are checked too -- the graph's own lanes are
 * same-base-adjacent pairs, and adjacent is NOT disjoint, so partner lanes are
 * skipped by index. */
/* moving_is_store: a moving STORE conflicts with both reads and writes of its
 * location; a moving LOAD conflicts only with writes (loads reorder freely
 * against loads). */
static int mir_slp_can_cross(const MirFunction *fn, const int *def_count,
                             const size_t *def_at, size_t from, size_t to,
                             MirVregId base, int disp, size_t partner,
                             int moving_is_store) {
  for (size_t i = from + 1; i < to; i++) {
    const MirInst *in = &fn->insns[i];
    if (i == partner) {
      continue;
    }
    if (moving_is_store && in->a.kind == MIR_OPK_MEM &&
        !mir_slp_mem_disjoint(fn, def_count, def_at, &in->a, base, disp, 0)) {
      return 0;
    }
    if (in->dst.kind == MIR_OPK_MEM &&
        !mir_slp_mem_disjoint(fn, def_count, def_at, &in->dst, base, disp,
                              0)) {
      return 0;
    }
  }
  return 1;
}

/* Normalize an address to (root, disp): the lowering splits `p + 8` into its
 * own ADD as often as it folds it into the displacement, so both spellings
 * must compare equal. Follows single-def `ADD vreg, imm` and register copies. */
static void mir_slp_resolve_addr(const MirFunction *fn, const int *def_count,
                                 const size_t *def_at, MirVregId base,
                                 int disp, MirVregId *root_out,
                                 int *disp_out) {
  for (int depth = 0; depth < 6; depth++) {
    if (base == MIR_VREG_NONE || (size_t)base >= fn->vreg_count ||
        def_count[base] != 1) {
      break;
    }
    const MirInst *d = &fn->insns[def_at[base]];
    if (d->op == MIR_ADD && d->width == 8 && !d->is_float &&
        d->a.kind == MIR_OPK_VREG && d->b.kind == MIR_OPK_IMM) {
      disp += (int)d->b.imm;
      base = d->a.vreg;
      continue;
    }
    if (d->op == MIR_MOV && !d->is_float && d->width == 8 &&
        d->a.kind == MIR_OPK_VREG) {
      base = d->a.vreg;
      continue;
    }
    break;
  }
  *root_out = base;
  *disp_out = disp;
}

static int mir_slp_find_node(const MirSlpGraph *g, MirVregId lo, MirVregId hi) {
  for (int i = 0; i < g->node_count; i++) {
    if (g->nodes[i].kind != 3 && g->nodes[i].lo == lo && g->nodes[i].hi == hi) {
      return i;
    }
  }
  return -1;
}

/* Build (or find) the pair node for lanes (lo, hi). Returns the node index or
 * -1 when the lanes cannot run in lockstep. */
static int mir_slp_pair_value(MirSlpGraph *g, MirVregId lo, MirVregId hi) {
  MirFunction *fn = g->fn;
  int found = mir_slp_find_node(g, lo, hi);
  if (found >= 0) {
    return found;
  }
  if (g->node_count >= MIR_SLP_MAX_NODES) {
    return -1;
  }

  /* One scalar feeding both lanes broadcasts. Zero definitions is a
   * parameter: defined at entry, stable everywhere. More than one is a
   * mutable local and refuses. */
  if (lo == hi) {
    if (g->def_count[lo] > 1) {
      return -1;
    }
    int n = g->node_count++;
    g->nodes[n].lo = lo;
    g->nodes[n].hi = hi;
    g->nodes[n].lo_at = g->def_count[lo] == 1 ? g->def_at[lo] : 0;
    g->nodes[n].hi_at = g->nodes[n].lo_at;
    g->nodes[n].pair = MIR_VREG_NONE;
    g->nodes[n].kind = 2;
    g->nodes[n].child_a = -1;
    g->nodes[n].child_b = -1;
    g->nodes[n].keep_originals = 1; /* the scalar def always stays */
    return n;
  }

  if (g->def_count[lo] != 1 || g->def_count[hi] != 1) {
    return -1;
  }
  size_t la = g->def_at[lo];
  size_t ha = g->def_at[hi];
  if (la < g->region_lo || ha < g->region_lo || la > g->region_hi ||
      ha > g->region_hi || la == ha) {
    return -1;
  }
  const MirInst *li = &fn->insns[la];
  const MirInst *hi_in = &fn->insns[ha];

  /* Adjacent loads: lane 0 at [base+d], lane 1 at [base+d+8]. The fused load
   * runs at the EARLIER lane's slot, so the LATER lane conceptually moves up:
   * everything between must be provably disjoint from the later lane's
   * address (its own lane may legitimately be stored to in between -- the
   * scalar code loaded before that store, and so does the fused load). */
  MirVregId lo_root = MIR_VREG_NONE, hi_root = MIR_VREG_NONE;
  int lo_disp = 0, hi_disp = 0;
  if (mir_slp_is_f64_load(li) && mir_slp_is_f64_load(hi_in)) {
    mir_slp_resolve_addr(fn, g->def_count, g->def_at, li->a.mem.base,
                         li->a.mem.disp, &lo_root, &lo_disp);
    mir_slp_resolve_addr(fn, g->def_count, g->def_at, hi_in->a.mem.base,
                         hi_in->a.mem.disp, &hi_root, &hi_disp);
  }
  if (mir_slp_is_f64_load(li) && mir_slp_is_f64_load(hi_in) &&
      hi_disp == lo_disp + 8 &&
      mir_slp_same_base(fn, g->def_count, g->def_at, lo_root, hi_root, 0)) {
    size_t early = la < ha ? la : ha;
    size_t late = la < ha ? ha : la;
    const MirInst *late_in = &fn->insns[late];
    if (!mir_slp_can_cross(fn, g->def_count, g->def_at, early, late,
                           late_in->a.mem.base, late_in->a.mem.disp, early,
                           0)) {
      return -1;
    }
    int n = g->node_count++;
    g->nodes[n].lo = lo;
    g->nodes[n].hi = hi;
    g->nodes[n].lo_at = la;
    g->nodes[n].hi_at = ha;
    g->nodes[n].pair = MIR_VREG_NONE;
    g->nodes[n].kind = 0;
    g->nodes[n].child_a = -1;
    g->nodes[n].child_b = -1;
    g->nodes[n].keep_originals = 0;
    return n;
  }

  /* Isomorphic binops: same opcode, lanes pair recursively. FDIV pairs too --
   * both lanes divide, so the packed form raises exactly the same traps. */
  if (mir_slp_is_f64_binop(li) && mir_slp_is_f64_binop(hi_in) &&
      li->op == hi_in->op && la < ha) {
    int ca = mir_slp_pair_value(g, li->a.vreg, hi_in->a.vreg);
    if (ca < 0) {
      return -1;
    }
    int cb = mir_slp_pair_value(g, li->b.vreg, hi_in->b.vreg);
    if (cb < 0) {
      return -1;
    }
    int n = g->node_count++;
    g->nodes[n].lo = lo;
    g->nodes[n].hi = hi;
    g->nodes[n].lo_at = la;
    g->nodes[n].hi_at = ha;
    g->nodes[n].pair = MIR_VREG_NONE;
    g->nodes[n].kind = 1;
    g->nodes[n].op = li->op;
    g->nodes[n].child_a = ca;
    g->nodes[n].child_b = cb;
    g->nodes[n].keep_originals = 0;
    return n;
  }

  return -1;
}

/* Internal uses: reads of a node's lanes by other graph originals. A lane
 * read anywhere else forces the originals to stay. */
static void mir_slp_mark_escapes(MirSlpGraph *g, size_t st_lo, size_t st_hi) {
  MirFunction *fn = g->fn;
  for (int n = 0; n < g->node_count; n++) {
    MirSlpNode *node = &g->nodes[n];
    if (node->kind == 2 || node->kind == 3 || node->keep_originals) {
      continue;
    }
    int internal_lo = 0;
    int internal_hi = 0;
    /* the two original stores read the root's lanes and are dropped */
    if (fn->insns[st_lo].a.kind == MIR_OPK_VREG &&
        fn->insns[st_lo].a.vreg == node->lo) {
      internal_lo++;
    }
    if (fn->insns[st_hi].a.kind == MIR_OPK_VREG &&
        fn->insns[st_hi].a.vreg == node->hi) {
      internal_hi++;
    }
    for (int m = 0; m < g->node_count; m++) {
      if (m == n || g->nodes[m].keep_originals) {
        continue;
      }
      const MirInst *ml = &fn->insns[g->nodes[m].lo_at];
      const MirInst *mh = &fn->insns[g->nodes[m].hi_at];
      const MirInst *pair[2] = {ml, mh};
      for (int k = 0; k < 2; k++) {
        const MirInst *in = pair[k];
        if (in->a.kind == MIR_OPK_VREG && in->a.vreg == node->lo) {
          internal_lo++;
        }
        if (in->b.kind == MIR_OPK_VREG && in->b.vreg == node->lo) {
          internal_lo++;
        }
        if (in->a.kind == MIR_OPK_VREG && in->a.vreg == node->hi) {
          internal_hi++;
        }
        if (in->b.kind == MIR_OPK_VREG && in->b.vreg == node->hi) {
          internal_hi++;
        }
      }
    }
    if (g->use_count[node->lo] != internal_lo ||
        g->use_count[node->hi] != internal_hi) {
      node->keep_originals = 1;
    }
  }
  /* Keeping a parent's originals means its lanes still read the children's
   * lanes, so the children's originals must stay too. Propagate down. */
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int n = 0; n < g->node_count; n++) {
      const MirSlpNode *node = &g->nodes[n];
      if (!node->keep_originals || node->kind != 1) {
        continue;
      }
      int kids[2] = {node->child_a, node->child_b};
      for (int k = 0; k < 2; k++) {
        if (kids[k] >= 0 && !g->nodes[kids[k]].keep_originals &&
            g->nodes[kids[k]].kind != 2) {
          g->nodes[kids[k]].keep_originals = 1;
          changed = 1;
        }
      }
    }
  }
}

/* One emitted pair instruction, targeted at a slot in the original stream:
 * it is inserted immediately before whatever remains at that index. Loads
 * anchor at the EARLIER lane (later lane proved able to move up); arithmetic
 * anchors at the LATER lane (its inputs' anchors are strictly earlier);
 * broadcasts anchor with their first consumer. */
typedef struct {
  size_t at;
  MirInst inst;
} MirSlpEmit;

static int mir_slp_emit_node(MirSlpGraph *g, int n, size_t consumer_anchor,
                             MirSlpEmit *out, int *out_count,
                             size_t *anchor_out) {
  MirFunction *fn = g->fn;
  MirSlpNode *node = &g->nodes[n];
  size_t anchor;
  if (node->pair != MIR_VREG_NONE) {
    if (anchor_out) {
      *anchor_out = node->lo_at; /* already placed; anchor irrelevant */
    }
    return 1;
  }
  switch (node->kind) {
  case 0:
    anchor = node->lo_at < node->hi_at ? node->lo_at : node->hi_at;
    break;
  case 1:
    anchor = node->lo_at > node->hi_at ? node->lo_at : node->hi_at;
    break;
  default:
    anchor = consumer_anchor;
    /* the broadcast scalar must exist by then (parameters exist at entry) */
    if (g->def_count[node->lo] == 1 && g->def_at[node->lo] >= anchor) {
      return 0;
    }
    break;
  }
  if (node->child_a >= 0 &&
      !mir_slp_emit_node(g, node->child_a, anchor, out, out_count, NULL)) {
    return 0;
  }
  if (node->child_b >= 0 &&
      !mir_slp_emit_node(g, node->child_b, anchor, out, out_count, NULL)) {
    return 0;
  }
  node->pair = mir_new_vreg(fn, MIR_RC_XMM, 16);
  if (node->pair == MIR_VREG_NONE) {
    return 0;
  }
  MirSlpEmit *slot = &out[(*out_count)++];
  MirInst *in = &slot->inst;
  slot->at = anchor;
  memset(in, 0, sizeof(*in));
  in->is_float = 1;
  in->width = 16;
  in->ir_index = -1;
  switch (node->kind) {
  case 0: { /* load pair: movupd from lane 0's (lower) address */
    const MirInst *l0 = &fn->insns[node->lo_at];
    in->op = MIR_MOV;
    in->dst = mir_op_vreg(node->pair);
    in->a = l0->a;
    break;
  }
  case 1:
    in->op = node->op;
    in->dst = mir_op_vreg(node->pair);
    in->a = mir_op_vreg(g->nodes[node->child_a].pair);
    in->b = mir_op_vreg(g->nodes[node->child_b].pair);
    break;
  case 2:
    in->op = MIR_FDUP;
    in->dst = mir_op_vreg(node->pair);
    in->a = mir_op_vreg(node->lo);
    break;
  default:
    return 0;
  }
  if (anchor_out) {
    *anchor_out = anchor;
  }
  return 1;
}

/* Try to vectorize the store pair at (s_lo, s_hi). Returns 1 and fills the
 * rewrite plan when the whole graph pairs. */
static int mir_slp_try_store_pair(MirFunction *fn, const int *def_count,
                                  const size_t *def_at, const int *use_count,
                                  size_t s_lo, size_t s_hi, int *changed) {
  MirSlpGraph g = {0};
  g.fn = fn;
  g.def_count = def_count;
  g.def_at = def_at;
  g.use_count = use_count;

  const MirInst *st_lo = &fn->insns[s_lo];
  const MirInst *st_hi = &fn->insns[s_hi];

  int root = -1;
  {
    /* Region: from the earliest def the graph can reach back to, up to the
     * later store. Start wide (the enclosing straight-line run). */
    size_t lo = s_lo;
    while (lo > 0 && mir_slp_region_ok(fn, lo - 1, lo - 1)) {
      lo--;
    }
    g.region_lo = lo;
    g.region_hi = s_hi;
    if (!mir_slp_region_ok(fn, s_lo, s_hi)) {
      return 0;
    }
    root = mir_slp_pair_value(&g, st_lo->a.vreg, st_hi->a.vreg);
  }
  if (root < 0) {
    return 0;
  }
  /* The earlier store moves down to the later one. */
  if (!mir_slp_can_cross(fn, def_count, def_at, s_lo, s_hi,
                         st_lo->dst.mem.base, st_lo->dst.mem.disp, s_hi, 1)) {
    return 0;
  }
  /* A store pair with a bare broadcast root gains nothing. */
  if (g.nodes[root].kind == 2) {
    return 0;
  }

  mir_slp_mark_escapes(&g, s_lo, s_hi);

  /* Emit the pair chain, each instruction targeted at its own slot. */
  MirSlpEmit emitted[MIR_SLP_MAX_NODES + 2];
  int emitted_count = 0;
  size_t root_anchor = 0;
  if (!mir_slp_emit_node(&g, root, s_hi, emitted, &emitted_count,
                         &root_anchor)) {
    return 0;
  }
  if (root_anchor >= s_hi) {
    return 0; /* the root value must exist before the fused store runs */
  }
  {
    MirSlpEmit *slot = &emitted[emitted_count++];
    MirInst *st = &slot->inst;
    slot->at = s_hi;
    memset(st, 0, sizeof(*st));
    st->op = MIR_MOV;
    st->is_float = 1;
    st->width = 16;
    st->ir_index = -1;
    st->dst = st_lo->dst; /* lane 0 = the lower resolved address */
    st->a = mir_op_vreg(g.nodes[root].pair);
  }

  /* Rebuild: droppable originals disappear, each emitted instruction lands
   * just before whatever remains at its slot. */
  unsigned char *drop = calloc(fn->insn_count, 1);
  if (!drop) {
    return 0;
  }
  drop[s_lo] = 1;
  drop[s_hi] = 1;
  for (int n = 0; n < g.node_count; n++) {
    if (!g.nodes[n].keep_originals && g.nodes[n].kind != 2) {
      drop[g.nodes[n].lo_at] = 1;
      drop[g.nodes[n].hi_at] = 1;
    }
  }

  size_t new_cap = fn->insn_count + (size_t)emitted_count;
  MirInst *rebuilt = malloc(new_cap * sizeof(MirInst));
  if (!rebuilt) {
    free(drop);
    return 0;
  }
  size_t w = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    for (int e = 0; e < emitted_count; e++) {
      if (emitted[e].at == i) {
        rebuilt[w++] = emitted[e].inst;
      }
    }
    if (drop[i]) {
      continue;
    }
    rebuilt[w++] = fn->insns[i];
  }
  free(fn->insns);
  fn->insns = rebuilt;
  fn->insn_count = w;
  fn->insn_capacity = new_cap;
  free(drop);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

/* Pair adjacent float64 stores and the operation DAGs behind them. One
 * rewrite per scan; def/use tables go stale at the first change. */
static void mir_slp_pair_f64(MirFunction *fn) {
  if (!fn || fn->insn_count < 4) {
    return;
  }
  for (int round = 0; round < 8; round++) {
    size_t n_vregs = fn->vreg_count;
    int *def_count = calloc(n_vregs, sizeof(int));
    size_t *def_at = calloc(n_vregs, sizeof(size_t));
    int *use_count = calloc(n_vregs, sizeof(int));
    if (!def_count || !def_at || !use_count) {
      free(def_count);
      free(def_at);
      free(use_count);
      return;
    }
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (in->dst.kind == MIR_OPK_VREG) {
        if (def_count[in->dst.vreg]++ == 0) {
          def_at[in->dst.vreg] = i;
        }
      }
      const MirOperand *reads[3] = {&in->a, &in->b,
                                    in->dst.kind == MIR_OPK_MEM ? &in->dst
                                                                : NULL};
      for (int k = 0; k < 3; k++) {
        const MirOperand *op = reads[k];
        if (!op) {
          continue;
        }
        if (op->kind == MIR_OPK_VREG) {
          use_count[op->vreg]++;
        } else if (op->kind == MIR_OPK_MEM) {
          if (op->mem.base != MIR_VREG_NONE) {
            use_count[op->mem.base]++;
          }
          if (op->mem.index != MIR_VREG_NONE) {
            use_count[op->mem.index]++;
          }
        }
      }
    }

    int changed = 0;
    int dbg = getenv("METTLE_SLP_TRACE") != NULL;
    for (size_t i = 0; i + 1 < fn->insn_count && !changed; i++) {
      const MirInst *a = &fn->insns[i];
      if (!mir_slp_is_f64_store(a)) {
        continue;
      }
      for (size_t j = i + 1; j < fn->insn_count && j < i + 24; j++) {
        const MirInst *b = &fn->insns[j];
        if (b->op == MIR_LABEL || b->op == MIR_JMP || b->op == MIR_JCC ||
            b->op == MIR_CMPBR || b->op == MIR_FCMPBR || b->op == MIR_CALL ||
            b->op == MIR_CALL_INDIRECT || b->op == MIR_RET) {
          break;
        }
        MirVregId ar = MIR_VREG_NONE, br = MIR_VREG_NONE;
        int adp = 0, bdp = 0;
        if (mir_slp_is_f64_store(b)) {
          mir_slp_resolve_addr(fn, def_count, def_at, a->dst.mem.base,
                               a->dst.mem.disp, &ar, &adp);
          mir_slp_resolve_addr(fn, def_count, def_at, b->dst.mem.base,
                               b->dst.mem.disp, &br, &bdp);
        }
        if (mir_slp_is_f64_store(b) && bdp == adp + 8 &&
            mir_slp_same_base(fn, def_count, def_at, ar, br, 0)) {
          if (dbg) {
            fprintf(stderr, "[slp] pair candidate @%zu/@%zu disp %d/%d\n", i,
                    j, a->dst.mem.disp, b->dst.mem.disp);
          }
          /* lane order == program order: the .x store first, .y second */
          if (mir_slp_try_store_pair(fn, def_count, def_at, use_count, i, j,
                                     &changed)) {
            if (dbg) {
              fprintf(stderr, "[slp] FUSED @%zu/@%zu\n", i, j);
            }
            break;
          }
        }
      }
    }

    free(def_count);
    free(def_at);
    free(use_count);
    if (!changed) {
      return;
    }
  }
}

static size_t mir_label_index(const MirFunction *fn, const char *name);

#define MIR_JUMP_TABLE_MIN_CASES 5
#define MIR_JUMP_TABLE_MAX_SPAN 512

static int mir_jt_case(const MirInst *in, MirVregId *key, long long *value) {
  if (in->op != MIR_CMPBR || in->is_float || in->cc != 0x84 || in->width != 8 ||
      in->a.kind != MIR_OPK_VREG || in->b.kind != MIR_OPK_IMM ||
      in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
    return 0;
  }
  if (*key != MIR_VREG_NONE && in->a.vreg != *key) {
    return 0;
  }
  *key = in->a.vreg;
  *value = in->b.imm;
  return 1;
}

static char *mir_jt_own_name(MirFunction *fn, const char *name) {
  char *copy = mettle_strdup(name);
  if (!copy) {
    return NULL;
  }
  if (fn->owned_sym_count >= fn->owned_sym_capacity) {
    size_t nc = fn->owned_sym_capacity ? fn->owned_sym_capacity * 2 : 8;
    char **grown = (char **)realloc(fn->owned_syms, nc * sizeof(char *));
    if (!grown) {
      free(copy);
      return NULL;
    }
    fn->owned_syms = grown;
    fn->owned_sym_capacity = nc;
  }
  fn->owned_syms[fn->owned_sym_count++] = copy;
  return copy;
}

static void mir_build_jump_tables(MirFunction *fn) {
  if (!fn || fn->insn_count == 0) {
    return;
  }
  for (size_t i = 0; i + MIR_JUMP_TABLE_MIN_CASES < fn->insn_count; i++) {
    MirVregId key = MIR_VREG_NONE;
    long long value = 0;
    if (!mir_jt_case(&fn->insns[i], &key, &value)) {
      continue;
    }
    size_t j = i;
    long long lo = value;
    long long hi = value;
    while (j < fn->insn_count && mir_jt_case(&fn->insns[j], &key, &value)) {
      if (value < lo) {
        lo = value;
      }
      if (value > hi) {
        hi = value;
      }
      j++;
    }
    size_t cases = j - i;
    long long span = hi - lo + 1;
    if (cases < MIR_JUMP_TABLE_MIN_CASES || span > MIR_JUMP_TABLE_MAX_SPAN ||
        span > 2 * (long long)cases || j >= fn->insn_count) {
      i = j > i ? j - 1 : i;
      continue;
    }

    const char *fallthrough = NULL;
    if (fn->insns[j].op == MIR_LABEL && fn->insns[j].dst.kind == MIR_OPK_LABEL) {
      fallthrough = fn->insns[j].dst.sym;
    } else if (fn->insns[j].op == MIR_JMP &&
               fn->insns[j].dst.kind == MIR_OPK_LABEL) {
      fallthrough = fn->insns[j].dst.sym;
    }
    if (!fallthrough) {
      i = j - 1;
      continue;
    }

    char **slots = (char **)calloc((size_t)span, sizeof(char *));
    MirJumpTable *table = (MirJumpTable *)calloc(1, sizeof(MirJumpTable));
    if (!slots || !table) {
      free(slots);
      free(table);
      return;
    }
    int duplicate = 0;
    for (size_t k = i; k < j; k++) {
      long long slot = fn->insns[k].b.imm - lo;
      if (slots[slot]) {
        duplicate = 1;
        break;
      }
      slots[slot] = mir_jt_own_name(fn, fn->insns[k].dst.sym);
      if (!slots[slot]) {
        duplicate = 1;
        break;
      }
    }
    char *deflt = duplicate ? NULL : mir_jt_own_name(fn, fallthrough);
    int forward = 1;
    for (size_t k = i; k < j && forward; k++) {
      size_t at = mir_label_index(fn, fn->insns[k].dst.sym);
      if (at == (size_t)-1 || at <= j) {
        forward = 0;
      }
    }
    MirVregId biased = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (duplicate || !deflt || !forward || biased == MIR_VREG_NONE) {
      free(slots);
      free(table);
      i = j - 1;
      continue;
    }
    for (long long k = 0; k < span; k++) {
      if (!slots[k]) {
        slots[k] = deflt;
      }
    }
    table->labels = slots;
    table->count = (size_t)span;
    if (!mir_function_own_aux(fn, table)) {
      free(slots);
      i = j - 1;
      continue;
    }
    if (!mir_function_own_aux(fn, slots)) {
      i = j - 1;
      continue;
    }

    MirInst bias = {0};
    bias.op = lo == 0 ? MIR_MOV : MIR_SUB;
    bias.dst = mir_op_vreg(biased);
    bias.a = mir_op_vreg(key);
    bias.b = lo == 0 ? mir_op_none() : mir_op_imm(lo);
    bias.width = 8;
    bias.ir_index = fn->insns[i].ir_index;

    MirInst guard = {0};
    guard.op = MIR_CMPBR;
    guard.dst = mir_op_label(deflt);
    guard.a = mir_op_vreg(biased);
    guard.b = mir_op_imm(span - 1);
    guard.width = 8;
    guard.is_unsigned = 1;
    guard.cc = 0x87;
    guard.ir_index = fn->insns[i].ir_index;

    MirInst dispatch = {0};
    dispatch.op = MIR_JMP_TABLE;
    dispatch.a = mir_op_vreg(biased);
    dispatch.width = 8;
    dispatch.aux = table;
    dispatch.ir_index = fn->insns[i].ir_index;

    fn->insns[i] = bias;
    fn->insns[i + 1] = guard;
    fn->insns[i + 2] = dispatch;
    for (size_t k = i + 3; k < j; k++) {
      MirInst nop = {0};
      nop.op = MIR_NOP;
      nop.ir_index = -1;
      fn->insns[k] = nop;
    }
    i = j - 1;
  }
}

static void mir_rotate_loops(MirFunction *fn) {
  if (!fn || fn->insn_count < 3) {
    return;
  }
  for (size_t j = 0; j + 1 < fn->insn_count; j++) {
    /* A rotatable header is `label H` immediately followed by its `CMPBR cc ->
     * E`. (Immediate adjacency means the compare operands are loop-stable live
     * values, not header-computed temps.) */
    if (fn->insns[j].op != MIR_LABEL ||
        fn->insns[j].dst.kind != MIR_OPK_LABEL || !fn->insns[j].dst.sym ||
        fn->insns[j + 1].op != MIR_CMPBR ||
        fn->insns[j + 1].dst.kind != MIR_OPK_LABEL ||
        !fn->insns[j + 1].dst.sym) {
      continue;
    }
    const char *hname = fn->insns[j].dst.sym;     /* header / body-start label */
    const char *ename = fn->insns[j + 1].dst.sym; /* loop exit target          */

    /* Require that the only edge into H is a single backward `JMP H`: the latch.
     * Rotation moves the test above the label, so H stops being tested on entry
     * and every other edge reaching it would run the body without ever
     * evaluating the loop condition. That covers conditional back-edges as well
     * as unconditional ones -- a `while (i <= j) { ...; if (i <= j) { ... } }`
     * lowers the `if`'s false arm to a `CMPBR H`, which is a back-edge the JMP
     * scan alone does not see -- and forward jumps into the header. */
    size_t be = 0;
    int nbe = 0;
    int other_edge = 0;
    for (size_t k = 0; k < fn->insn_count; k++) {
      if (k == j || fn->insns[k].dst.kind != MIR_OPK_LABEL ||
          !fn->insns[k].dst.sym ||
          strcmp(fn->insns[k].dst.sym, hname) != 0) {
        continue;
      }
      if (fn->insns[k].op == MIR_JMP && k > j + 1) {
        be = k;
        nbe++;
      } else {
        other_edge = 1;
      }
    }
    for (size_t k = 0; k < fn->insn_count && !other_edge; k++) {
      const MirJumpTable *tbl;
      if (fn->insns[k].op != MIR_JMP_TABLE || !fn->insns[k].aux) {
        continue;
      }
      tbl = (const MirJumpTable *)fn->insns[k].aux;
      for (size_t e = 0; e < tbl->count; e++) {
        if (tbl->labels[e] && strcmp(tbl->labels[e], hname) == 0) {
          other_edge = 1;
          break;
        }
      }
    }
    if (nbe != 1 || other_edge) {
      continue;
    }
    /* The instruction right after the back-edge must be the header's exit label.
     * Otherwise the rotated loop's fall-through (the not-taken bottom test) would
     * land on the wrong block, e.g. when the loop is the last statement in an
     * `if` and its exit is the enclosing block's end, not a `while_end` here. */
    if (be + 1 >= fn->insn_count || fn->insns[be + 1].op != MIR_LABEL ||
        fn->insns[be + 1].dst.kind != MIR_OPK_LABEL ||
        !fn->insns[be + 1].dst.sym ||
        strcmp(fn->insns[be + 1].dst.sym, ename) != 0) {
      continue;
    }

    /* Convert the back-edge `JMP H` into the bottom test `CMPBR !cc -> H` (loop
     * while the condition still holds; fall through to the exit label when it
     * fails). cc ^ 1 inverts the x86 condition. dst already targets H. */
    fn->insns[be].op = MIR_CMPBR;
    fn->insns[be].a = fn->insns[j + 1].a;
    fn->insns[be].b = fn->insns[j + 1].b;
    fn->insns[be].width = fn->insns[j + 1].width;
    fn->insns[be].is_unsigned = fn->insns[j + 1].is_unsigned;
    fn->insns[be].cc = (unsigned char)(fn->insns[j + 1].cc ^ 1u);

    /* Swap `label H` with its CMPBR so H marks the body and the CMPBR is a
     * one-time entry guard. */
    MirInst tmp = fn->insns[j];
    fn->insns[j] = fn->insns[j + 1];
    fn->insns[j + 1] = tmp;
  }
}

/* MIR index of the LABEL defining `name`, or (size_t)-1. */
static int mir_insn_defines_label(const MirInst *in, const char *name) {
  return in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
         strcmp(in->dst.sym, name) == 0;
}

static size_t mir_label_index_scan(const MirFunction *fn, const char *name) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_insn_defines_label(&fn->insns[i], name)) {
      return i;
    }
  }
  return (size_t)-1;
}

static size_t mir_label_hash(const char *s) {
  size_t h = (size_t)1469598103934665603ULL;
  for (; *s; s++) {
    h ^= (size_t)(unsigned char)*s;
    h *= (size_t)1099511628211ULL;
  }
  return h;
}

static void mir_label_index_build(MirFunction *fn) {
  size_t labels = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      labels++;
    }
  }
  size_t capacity = 16;
  while (capacity < labels * 2u) {
    capacity *= 2u;
  }
  size_t *slots = (size_t *)calloc(capacity, sizeof(size_t));
  if (!slots) {
    return;
  }
  size_t mask = capacity - 1u;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op != MIR_LABEL || in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      continue;
    }
    size_t h = mir_label_hash(in->dst.sym) & mask;
    int duplicate = 0;
    while (slots[h]) {
      if (mir_insn_defines_label(&fn->insns[slots[h] - 1u], in->dst.sym)) {
        duplicate = 1;
        break;
      }
      h = (h + 1u) & mask;
    }
    if (!duplicate) {
      slots[h] = i + 1u;
    }
  }
  free(fn->label_slots);
  fn->label_slots = slots;
  fn->label_slot_capacity = capacity;
  fn->label_slot_insns = fn->insn_count;
}

/* Answered from an index built in one walk. This was a full scan of the
 * instruction stream per lookup, and mir_enclosing_loop resolves every branch
 * target through it while itself being called once per branch, so an N-arm
 * if/else function cost O(N^3): at 1600 arms, 2.56M calls totalling 13.7
 * BILLION compares, and codegen was 99.8% of the compile. */
static size_t mir_label_index(const MirFunction *fn, const char *name) {
  if (!fn || !name) {
    return (size_t)-1;
  }
  MirFunction *mutable_fn = (MirFunction *)fn;
  if (!mutable_fn->label_slots ||
      mutable_fn->label_slot_insns != fn->insn_count) {
    mir_label_index_build(mutable_fn);
  }
  if (mutable_fn->label_slots) {
    size_t mask = mutable_fn->label_slot_capacity - 1u;
    size_t h = mir_label_hash(name) & mask;
    while (mutable_fn->label_slots[h]) {
      size_t candidate = mutable_fn->label_slots[h] - 1u;
      if (candidate < fn->insn_count &&
          mir_insn_defines_label(&fn->insns[candidate], name)) {
        return candidate;
      }
      h = (h + 1u) & mask;
    }
  }
  /* Absent from the map, which a rewrite that left insn_count alone can also
   * mean. Rescanning keeps the answer right whatever a pass did, and rebuilds
   * so the next lookup is cheap again. A branch to a label this function does
   * not define is the only case that pays this twice. */
  size_t found = mir_label_index_scan(fn, name);
  if (found != (size_t)-1) {
    mir_label_index_build(mutable_fn);
  }
  return found;
}

/* True if MIR index p sits inside a loop body: some JMP/CMPBR back-edge after p
 * targets a label defined at or before p (it spans p). */
/* Bounds of the tightest loop containing `p`: the back edge at `hi` jumps to the
 * header at `lo`, and lo <= p < hi. The tightest is the one whose header sits
 * latest, which is the innermost loop `p` belongs to. */
/* One back edge: the branch at `branch` jumps to a label defined at `target`,
 * at or before it. Collected once per pass so the enclosing-loop query does not
 * walk the whole function per candidate -- that walk was the O(N^2) left after
 * the label lookup itself became an index. */
typedef struct {
  size_t target;
  size_t branch;
} MirBackEdge;

static size_t mir_collect_back_edges(const MirFunction *fn,
                                     MirBackEdge **out_edges) {
  *out_edges = NULL;
  size_t count = 0;
  for (int pass = 0; pass < 2; pass++) {
    size_t seen = 0;
    for (size_t k = 0; k < fn->insn_count; k++) {
      const MirInst *in = &fn->insns[k];
      if ((in->op != MIR_JMP && in->op != MIR_CMPBR) ||
          in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
        continue;
      }
      size_t t = mir_label_index(fn, in->dst.sym);
      if (t == (size_t)-1 || t > k) {
        continue;
      }
      if (pass == 1) {
        (*out_edges)[seen].target = t;
        (*out_edges)[seen].branch = k;
      }
      seen++;
    }
    if (pass == 0) {
      count = seen;
      if (count == 0) {
        return 0;
      }
      *out_edges = (MirBackEdge *)malloc(count * sizeof(MirBackEdge));
      if (!*out_edges) {
        return 0;
      }
    }
  }
  return count;
}

/* Ascending in `branch`, so a tie on the header keeps the earliest back edge,
 * exactly as the walk it replaces did. */
static int mir_enclosing_loop_from(const MirBackEdge *edges, size_t edge_count,
                                   size_t p, size_t *lo, size_t *hi) {
  int found = 0;
  for (size_t e = 0; e < edge_count; e++) {
    size_t t = edges[e].target;
    size_t k = edges[e].branch;
    if (k <= p || t > p) {
      continue;
    }
    if (!found || t > *lo) {
      *lo = t;
      *hi = k;
      found = 1;
    }
  }
  return found;
}

static int mir_enclosing_loop(const MirFunction *fn, size_t p, size_t *lo,
                              size_t *hi) {
  MirBackEdge *edges = NULL;
  size_t edge_count = mir_collect_back_edges(fn, &edges);
  int found = mir_enclosing_loop_from(edges, edge_count, p, lo, hi);
  free(edges);
  return found;
}

static int mir_index_in_loop(const MirFunction *fn, size_t p) {
  size_t lo = 0, hi = 0;
  return mir_enclosing_loop(fn, p, &lo, &hi);
}

static int mir_operand_uses_vreg(const MirOperand *op, MirVregId v) {
  if (!op || v == MIR_VREG_NONE) {
    return 0;
  }
  if (op->kind == MIR_OPK_VREG) {
    return op->vreg == v;
  }
  if (op->kind == MIR_OPK_MEM) {
    return op->mem.base == v || op->mem.index == v;
  }
  return 0;
}

static int mir_inst_uses_vreg(const MirInst *in, MirVregId v) {
  if (!in) {
    return 0;
  }
  if (mir_operand_uses_vreg(&in->a, v) ||
      mir_operand_uses_vreg(&in->b, v)) {
    return 1;
  }
  return in->dst.kind == MIR_OPK_MEM && mir_operand_uses_vreg(&in->dst, v);
}

static size_t mir_first_use_index(const MirFunction *fn, MirVregId v,
                                  size_t def) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (i == def) {
      continue;
    }
    if (mir_inst_uses_vreg(&fn->insns[i], v)) {
      return i;
    }
  }
  return (size_t)-1;
}

static size_t mir_const_def_index(const MirFunction *fn, MirVregId v,
                                  int is_float) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op != MIR_MOV || in->is_float != is_float ||
        in->dst.kind != MIR_OPK_VREG || in->dst.vreg != v) {
      continue;
    }
    if (is_float) {
      if (in->a.kind == MIR_OPK_FIMM) {
        return i;
      }
    } else if (in->a.kind == MIR_OPK_IMM) {
      return i;
    }
  }
  return (size_t)-1;
}

static int mir_label_has_forward_target(const MirFunction *fn, const char *name,
                                        size_t label_index) {
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < label_index; i++) {
    const MirInst *in = &fn->insns[i];
    if ((in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
         in->op == MIR_FCMPBR) &&
        in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* True iff every use of `v` lies within the inclusive instruction range
 * [lo, hi]. Used to prove a pooled constant is confined to a single loop body
 * before its materialization is sunk to that loop's header. */
static int mir_all_uses_in_range(const MirFunction *fn, MirVregId v, size_t lo,
                                 size_t hi) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_inst_uses_vreg(&fn->insns[i], v) && (i < lo || i > hi)) {
      return 0;
    }
  }
  return 1;
}

/* Returns the loop header to sink the constant to, and its loop-body end via
 * *loop_end. Returns first_use (and leaves *loop_end = first_use) when no
 * enclosing loop encloses first_use -- the caller must not relocate then, since
 * a non-loop position need not dominate the constant's other uses. */
static int mir_insert_point_is_reached(const MirFunction *fn, size_t insert) {
  const MirInst *previous = NULL;

  if (insert == 0 || insert >= fn->insn_count) {
    return 1;
  }
  previous = &fn->insns[insert - 1];
  return previous->op != MIR_JMP && previous->op != MIR_RET;
}

static size_t mir_const_insert_index(const MirFunction *fn, size_t first_use,
                                     size_t *loop_end) {
  size_t insert = first_use;
  *loop_end = first_use;
  for (size_t b = 0; b < fn->insn_count; b++) {
    const MirInst *br = &fn->insns[b];
    if ((br->op != MIR_JMP && br->op != MIR_JCC && br->op != MIR_CMPBR &&
         br->op != MIR_FCMPBR) ||
        br->dst.kind != MIR_OPK_LABEL || !br->dst.sym) {
      continue;
    }
    size_t l = mir_label_index(fn, br->dst.sym);
    if (l == (size_t)-1 || l >= b || first_use < l || first_use > b) {
      continue;
    }
    if (mir_label_has_forward_target(fn, br->dst.sym, l)) {
      continue;
    }
    if (insert == first_use || l > insert) {
      insert = l;
      *loop_end = b;
    }
  }
  return insert;
}

/* Loop-pooled constants are discovered before lowering, so their original MOVs
 * land near function entry. Relocate those materializations to the nearest safe
 * preheader of the loop that first uses them: back-edges jump to the label, so
 * an instruction before that label runs once on loop entry and not per
 * iteration. This keeps magic div/mod constants and pooled float literals out of
 * unrelated setup calls and gives the allocator much shorter live ranges. */
static void mir_place_const_pool(MirFunction *fn) {
  if (!fn || (!fn->fconst_count && !fn->iconst_count) || fn->insn_count == 0) {
    return;
  }
  typedef struct {
    size_t def;
    size_t insert;
    MirInst inst;
  } ConstMove;
  size_t max_moves = fn->fconst_count + fn->iconst_count;
  ConstMove *moves = (ConstMove *)calloc(max_moves, sizeof(ConstMove));
  char *skip = (char *)calloc(fn->insn_count, 1);
  if (!moves || !skip) {
    free(moves);
    free(skip);
    return;
  }
  size_t nmove = 0;

  for (size_t i = 0; i < fn->iconst_count; i++) {
    MirVregId v = fn->iconsts[i].vreg;
    size_t def = mir_const_def_index(fn, v, 0);
    if (def == (size_t)-1) {
      continue;
    }
    size_t first = mir_first_use_index(fn, v, def);
    if (first == (size_t)-1) {
      continue;
    }
    size_t loop_end = first;
    size_t insert = mir_const_insert_index(fn, first, &loop_end);
    if (insert <= def + 1 || insert == first ||
        !mir_insert_point_is_reached(fn, insert) ||
        !mir_all_uses_in_range(fn, v, insert, loop_end)) {
      continue;
    }
    moves[nmove].def = def;
    moves[nmove].insert = insert;
    moves[nmove].inst = fn->insns[def];
    skip[def] = 1;
    nmove++;
  }
  for (size_t i = 0; i < fn->fconst_count; i++) {
    MirVregId v = fn->fconsts[i].vreg;
    size_t def = mir_const_def_index(fn, v, 1);
    if (def == (size_t)-1) {
      continue;
    }
    size_t first = mir_first_use_index(fn, v, def);
    if (first == (size_t)-1) {
      continue;
    }
    size_t loop_end = first;
    size_t insert = mir_const_insert_index(fn, first, &loop_end);
    if (insert <= def + 1 || insert == first ||
        !mir_insert_point_is_reached(fn, insert) ||
        !mir_all_uses_in_range(fn, v, insert, loop_end)) {
      continue;
    }
    moves[nmove].def = def;
    moves[nmove].insert = insert;
    moves[nmove].inst = fn->insns[def];
    skip[def] = 1;
    nmove++;
  }

  if (nmove == 0) {
    free(moves);
    free(skip);
    return;
  }

  MirInst *out = (MirInst *)malloc(fn->insn_count * sizeof(MirInst));
  if (!out) {
    free(moves);
    free(skip);
    return;
  }
  size_t w = 0;
  for (size_t i = 0; i <= fn->insn_count; i++) {
    for (size_t m = 0; m < nmove; m++) {
      if (moves[m].insert == i) {
        out[w++] = moves[m].inst;
      }
    }
    if (i == fn->insn_count) {
      break;
    }
    if (!skip[i]) {
      out[w++] = fn->insns[i];
    }
  }
  free(fn->insns);
  fn->insns = out;
  fn->insn_count = w;
  fn->insn_capacity = fn->insn_count;
  free(moves);
  free(skip);
}

/* Cold-exit sinking. An in-loop early-return guard lowers to a forward CMPBR
 * that skips a short straight-line block ending in RET; the loop continuation
 * is the branch TARGET, so the hot path pays a taken forward branch every
 * iteration (on top of the back-edge -- two taken branches/iter). Invert the
 * branch to jump to the return block, sink that block to the function tail, and
 * fall through to the continuation. The back-edge is then the loop's only taken
 * branch (matching what gcc/clang do for search/validation loops). The move is
 * pure relabel + relocate of a straight-line exit block, so it is value- and
 * control-equivalent regardless of the branch's real probability; the loop gate
 * only restricts WHERE it pays off. */
/* `jcc A; jmp B; A:` is a branch over a branch: five bytes of unconditional
 * jump on one of the two ways through every if/else and every rotated loop
 * exit that lands here. Inverting the condition reaches both targets with one
 * branch: `j!cc B; A:`. x86 condition inversion is the exact complement
 * (opcode ^ 1), and for the float branches the complement also routes the
 * unordered (NaN) case to the side the original fall-through took, so the
 * rewrite is value-equivalent for every input including NaN. */
static void mir_thread_branch_over_jump(MirFunction *fn) {
  if (!fn || fn->insn_count < 3) {
    return;
  }
  for (size_t p = 0; p + 2 < fn->insn_count; p++) {
    MirInst *br = &fn->insns[p];
    MirInst *jmp = &fn->insns[p + 1];
    const MirInst *label = &fn->insns[p + 2];
    if ((br->op != MIR_JCC && br->op != MIR_CMPBR && br->op != MIR_FCMPBR) ||
        br->dst.kind != MIR_OPK_LABEL || !br->dst.sym) {
      continue;
    }
    if (jmp->op != MIR_JMP || jmp->dst.kind != MIR_OPK_LABEL || !jmp->dst.sym) {
      continue;
    }
    if (label->op != MIR_LABEL || label->dst.kind != MIR_OPK_LABEL ||
        !label->dst.sym || strcmp(label->dst.sym, br->dst.sym) != 0 ||
        strcmp(jmp->dst.sym, br->dst.sym) == 0) {
      continue;
    }
    br->cc ^= 1;
    br->dst = jmp->dst;
    memset(jmp, 0, sizeof(*jmp));
    jmp->op = MIR_NOP;
    jmp->ir_index = -1;
  }
}

static void mir_sink_cold_exits(MirFunction *fn) {
  if (!fn || fn->insn_count < 4) {
    return;
  }
  /* An appended tail block must be unreachable by fall-through, so the function
   * must already end in a terminator. */
  MirOpcode last = fn->insns[fn->insn_count - 1].op;
  if (last != MIR_RET && last != MIR_JMP && last != MIR_TRAP) {
    return;
  }

  typedef struct {
    size_t p;      /* the CMPBR to invert */
    size_t lo, hi; /* sunk region [lo, hi) -- ends in RET */
    char *label;   /* fresh target name (owned by fn) */
  } Sink;
  Sink *sinks = NULL;
  size_t nsink = 0, cap = 0;
  char *moved = (char *)calloc(fn->insn_count, 1);
  if (!moved) {
    return;
  }
  /* Collected once: this loop only reads the stream, and the rewrite below runs
   * after it finishes, so the edges stay accurate for every query here. */
  MirBackEdge *back_edges = NULL;
  size_t back_edge_count = mir_collect_back_edges(fn, &back_edges);

  for (size_t p = 0; p + 1 < fn->insn_count; p++) {
    const MirInst *br = &fn->insns[p];
    if (br->op != MIR_CMPBR || br->dst.kind != MIR_OPK_LABEL || !br->dst.sym) {
      continue;
    }
    size_t q = mir_label_index(fn, br->dst.sym);
    if (q == (size_t)-1 || q < p + 2) {
      continue; /* backward branch, or empty fall-through region */
    }
    if (q - 1 - p > 16) {
      continue; /* keep this to short early-exit blocks */
    }
    int ok = 1;
    for (size_t r = p + 1; r < q; r++) {
      MirOpcode op = fn->insns[r].op;
      if (op == MIR_LABEL || op == MIR_JCC || op == MIR_CMPBR ||
          op == MIR_FCMPBR) {
        ok = 0;
        break;
      }
      if ((op == MIR_RET || op == MIR_JMP) && r != q - 1) {
        ok = 0;
        break;
      }
    }
    if (!ok) {
      continue;
    }
    /* The region has to end itself, or sinking it would fall into whatever
     * follows at the end of the function. A RET arm is cold on the return
     * heuristic. A JMP arm is worth sinking on either of two grounds: it
     * leaves the loop, which a loop by definition does once however many times
     * it goes around; or the branch guarding it tests equality against a
     * constant, which is false far more often than not, so the equal case is
     * the arm to move out. Either way the common path stops jumping twice to
     * reach the code after the arm. */
    MirOpcode tail = fn->insns[q - 1].op;
    size_t loop_lo = 0, loop_hi = 0;
    if (!mir_enclosing_loop_from(back_edges, back_edge_count, p, &loop_lo,
                                 &loop_hi)) {
      continue;
    }
    int arm_calls = 0;
    for (size_t r = p + 1; r < q; r++) {
      if (fn->insns[r].op == MIR_CALL) {
        arm_calls = 1;
        break;
      }
    }
    /* An arm that calls is not a rare fixup, it is the work. A dispatch chain's
     * arms all look like equality tests, but exactly one of them runs every
     * time, so moving them out of line costs the case that hits a jump it did
     * not pay before, and buys nothing the call does not already swamp. */
    int arm_is_cold = 0;
    if (arm_calls) {
      continue;
    }
    if (tail == MIR_RET) {
      arm_is_cold = 1;
    } else if (tail == MIR_JMP) {
      if (br->cc == 0x85 && br->b.kind == MIR_OPK_IMM) {
        arm_is_cold = 1;
      } else {
        const MirInst *out_jmp = &fn->insns[q - 1];
        size_t t = (out_jmp->dst.kind == MIR_OPK_LABEL && out_jmp->dst.sym)
                       ? mir_label_index(fn, out_jmp->dst.sym)
                       : (size_t)-1;
        arm_is_cold = t != (size_t)-1 && (t < loop_lo || t > loop_hi);
      }
    }
    if (!arm_is_cold) {
      continue;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), ".mcsink_%zu", nsink);
    char *name = mettle_strdup(buf);
    if (!name) {
      continue;
    }
    if (fn->owned_sym_count >= fn->owned_sym_capacity) {
      size_t nc = fn->owned_sym_capacity ? fn->owned_sym_capacity * 2 : 4;
      char **grown = (char **)realloc(fn->owned_syms, nc * sizeof(char *));
      if (!grown) {
        free(name);
        continue;
      }
      fn->owned_syms = grown;
      fn->owned_sym_capacity = nc;
    }
    fn->owned_syms[fn->owned_sym_count++] = name;

    if (nsink >= cap) {
      size_t nc = cap ? cap * 2 : 4;
      Sink *grown = (Sink *)realloc(sinks, nc * sizeof(Sink));
      if (!grown) {
        break;
      }
      sinks = grown;
      cap = nc;
    }
    sinks[nsink].p = p;
    sinks[nsink].lo = p + 1;
    sinks[nsink].hi = q;
    sinks[nsink].label = name;
    nsink++;
    for (size_t r = p + 1; r < q; r++) {
      moved[r] = 1;
    }
  }

  if (nsink == 0) {
    free(moved);
    free(back_edges);
    free(sinks);
    return;
  }

  size_t total = fn->insn_count + nsink; /* one new label per sunk block */
  MirInst *out = (MirInst *)malloc(total * sizeof(MirInst));
  if (!out) {
    free(moved);
    free(back_edges);
    free(sinks);
    return;
  }
  size_t w = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (moved[i]) {
      continue;
    }
    out[w] = fn->insns[i];
    for (size_t s = 0; s < nsink; s++) {
      if (sinks[s].p == i) {
        out[w].cc = (unsigned char)(out[w].cc ^ 1u);
        out[w].dst = mir_op_label(sinks[s].label);
        break;
      }
    }
    w++;
  }
  for (size_t s = 0; s < nsink; s++) {
    MirInst lbl;
    memset(&lbl, 0, sizeof(lbl));
    lbl.op = MIR_LABEL;
    lbl.dst = mir_op_label(sinks[s].label);
    lbl.ir_index = -1;
    out[w++] = lbl;
    for (size_t r = sinks[s].lo; r < sinks[s].hi; r++) {
      out[w++] = fn->insns[r];
    }
  }

  free(fn->insns);
  fn->insns = out;
  fn->insn_count = w;
  fn->insn_capacity = total;
  free(moved);
  free(back_edges);
  free(sinks);
}

/* ---- emit entry --------------------------------------------------------- */

static int mir_emit_volatile_global_reads(MirFunction *fn, CodeGenerator *g,
                                          MirNameMap *map,
                                          const IRInstruction *in,
                                          const MirAddrFold *fold) {
  const IROperand *reads[5];
  size_t count = 0;
  reads[count++] = &in->lhs;
  reads[count++] = &in->rhs;
  if (in->op == IR_OP_STORE) {
    reads[count++] = &in->dest;
  }
  if (fold && fold->valid) {
    reads[count++] = &fold->base;
    reads[count++] = &fold->index;
  }
  for (size_t k = 0; k < count + in->argument_count; k++) {
    const IROperand *op =
        k < count ? reads[k] : &in->arguments[k - count];
    const char *name = op->name;
    if (op->kind != IR_OPERAND_SYMBOL || !name ||
        (in->op == IR_OP_ADDRESS_OF && op == &in->lhs) ||
        !mir_name_is_volatile_global_scalar(g, name)) {
      continue;
    }
    if (!mir_emit_global_reload_names(fn, g, map, &name, 1)) {
      return 0;
    }
  }
  return 1;
}

static int mir_emit_volatile_global_write(MirFunction *fn, CodeGenerator *g,
                                          MirNameMap *map,
                                          const IRInstruction *in) {
  const char *name = in->dest.name;
  if (in->op == IR_OP_STORE || in->op == IR_OP_DECLARE_LOCAL ||
      in->dest.kind != IR_OPERAND_SYMBOL || !name ||
      !mir_name_is_volatile_global_scalar(g, name)) {
    return 1;
  }
  return mir_emit_global_flush_names(fn, g, map, &name, 1);
}

int code_generator_binary_emit_function_via_mir(
    CodeGenerator *generator,
    IRFunction *ir_function, BinaryFunctionContext *context) {
  MirFunction fn;
  MirNameMap map;
  void *vr_oracle = NULL;
  mir_function_init(&fn, context);
  fn.generator = generator;
  fn.ir_function = ir_function;
  fn.reserve_rbx = ir_function->is_interrupt ? 1 : 0;
  memset(&map, 0, sizeof(map));

  /* Globals this function writes: register-promoted (cached at entry, written
   * back before each return). Eligibility has proven these are leaf-function
   * scalar-global writes with no aliasing pointer in scope. */
  MirGlobalWriteback wb = {0};
  size_t wb_cap = 0;
  size_t wb_all_cap = 0;
  size_t wb_at_cap = 0;
  unsigned long long *dirty_masks = NULL;

  /* MIR owns saved registers and the frame; discard anything the legacy
   * promoter left in the context. */
  context->saved_register_count = 0;
  context->saved_xmm_count = 0;
  context->raw_frame_size = 0;
  context->frame_size = 0;
  context->return_float_bits = 0;
  /* Frame-pointer omission is DISABLED: a controlled A/B (same C baseline)
   * showed it is performance-neutral across the benchmark suite (~0% on ~11
   * benches, +3% on const_mod, but -6% on saxpy and -3% on func_ptr) -- no net
   * win, with downside on a couple of leaf loops, plus the added rsp-addressing
   * complexity. The freed rbp rarely binds and rsp-relative slots cost a SIB
   * byte. Set unconditionally to 0 so the allocator keeps the rbp frame and rbp
   * stays reserved. (The FPO machinery in mir_encode/mir_regalloc is inert while
   * this is 0; opt back in via METTLE_FPO if a future change makes it pay off.) */
  {
    static int fpo = -1;
    if (fpo < 0) {
      fpo = getenv("METTLE_FPO") ? 1 : 0;
    }
    context->omit_frame_pointer = fpo;
  }

  /* Bind parameters to vregs and record their incoming extension. */
  const BinaryAbi *abi = code_generator_binary_active_abi();
  (void)abi;
  for (size_t i = 0; i < ir_function->parameter_count; i++) {
    const char *pname = ir_function->parameter_names[i];
    MtlcType *pt = code_generator_binary_get_resolved_type(
        generator, ir_function->parameter_types
                       ? ir_function->parameter_types[i]
                       : NULL,
        0);
    int pfb = pt ? code_generator_binary_resolved_type_float_bits(pt) : 0;
    int is_agg = pt && code_generator_type_is_aggregate(pt);
    int w = pfb ? pfb / 8 : (pt ? code_generator_binary_resolved_type_scalar_size(pt) : 8);
    if (is_agg || (!pfb && w != 1 && w != 2 && w != 4 && w != 8)) {
      /* A DIRECT small aggregate arrives in a full GP register; home all 8 bytes
       * with no integer extension (field access reads within the struct size). */
      w = 8;
    }
    MirVregId v = mir_name_map_get_or_add(&map, &fn, pname, 0,
                                          pfb ? MIR_RC_XMM : MIR_RC_GP,
                                          pfb ? w : 8);
    if (v == MIR_VREG_NONE) {
      goto oom;
    }
    fn.params[fn.param_count].vreg = v;
    fn.params[fn.param_count].arg_index = (int)i;
    fn.params[fn.param_count].width = w;
    fn.params[fn.param_count].is_float = pfb ? 1 : 0;
    fn.params[fn.param_count].is_signed =
        (!is_agg && pt)
            ? code_generator_binary_resolved_type_is_signed_integer(pt)
            : 0;
    fn.param_count++;
  }

  /* INDIRECT struct return: the caller passes a hidden out-pointer (Win64: RCX,
   * SysV: RDI) ahead of the user arguments. Reserve a vreg for it; the prologue
   * homes that register into it (shifting user params up one ABI slot) and each
   * RETURN copies the struct there and returns the pointer in RAX. */
  {
    MtlcType *rt = code_generator_binary_get_resolved_type(
        generator, ir_function->return_type_name, 1);
    if (rt && code_generator_type_is_aggregate(rt) &&
        code_generator_abi_classify(rt) == ABI_PASS_INDIRECT) {
      fn.returns_indirect = 1;
      fn.indirect_return_size = (int)code_generator_abi_type_size(rt);
      fn.indirect_return_vreg = mir_new_vreg(&fn, MIR_RC_GP, 8);
      if (fn.indirect_return_vreg == MIR_VREG_NONE) {
        goto oom;
      }
    } else if (rt && code_generator_binary_resolved_type_float_bits(rt) != 0) {
      fn.float_return_bits = code_generator_binary_resolved_type_float_bits(rt);
    } else if (rt && code_generator_binary_resolved_type_float_bits(rt) == 0 &&
               !code_generator_type_is_aggregate(rt)) {
      /* A narrow integer return (int32/uint32/int16/...) must be canonicalized
       * before `mov rax`: MIR computes in 64-bit, so the value can carry garbage
       * above its width, and the Win64/SysV ABI leaves the high RAX bits
       * undefined for a sub-64-bit return, a caller that uses the full register
       * (e.g. `(int64)narrow_fn()`) would then read the garbage. Record the
       * return width/signedness so the RETURN lowering extends to canonical
       * 64-bit form. */
      int rw = code_generator_binary_resolved_type_scalar_size(rt);
      if (rw == 1 || rw == 2 || rw == 4) {
        fn.scalar_return_width = rw;
        fn.scalar_return_signed =
            code_generator_binary_resolved_type_is_signed_integer(rt);
      }
    }
  }

  /* Cache global scalars: load each referenced global once at entry into a vreg
   * so body references (reads AND writes) resolve to that register instead of a
   * per-use RIP-relative memory access. A read-only global is just cached; a
   * written global is additionally recorded for write-back before each return.
   * Eligibility has proven every global access here is a leaf-function scalar
   * global with no aliasing pointer in scope. Emitted before the body so the
   * cache vreg is defined at index 0 (live across the whole function, like a
   * parameter); the loop-extension in the allocator then keeps it live across
   * loop back-edges. */
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    /* Record a written global scalar for write-back (deduped). */
    if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        mir_name_is_global_scalar(generator, in->dest.name)) {
      int present = 0;
      for (size_t j = 0; j < wb.count; j++) {
        if (strcmp(wb.names[j], in->dest.name) == 0) {
          present = 1;
          break;
        }
      }
      if (!present) {
        if (wb.count >= wb_cap) {
          size_t nc = wb_cap ? wb_cap * 2 : 4;
          const char **grown =
              (const char **)realloc(wb.names, nc * sizeof(*grown));
          if (!grown) {
            goto oom;
          }
          wb.names = grown;
          wb_cap = nc;
        }
        wb.names[wb.count++] = in->dest.name;
      }
    }
    /* Load each global (read or written) into its cache vreg once at entry.
     * Scans dest/lhs/rhs AND call arguments, a global used only as a call
     * argument (f(g)) must still be loaded, or the value path would resolve it
     * to an undefined vreg holding a stale register. */
    for (int k = 0;; k++) {
      const IROperand *op;
      if (k == 0) {
        op = &in->dest;
      } else if (k == 1) {
        op = &in->lhs;
      } else if (k == 2) {
        op = &in->rhs;
      } else if ((size_t)(k - 3) < in->argument_count) {
        op = &in->arguments[k - 3];
      } else {
        break;
      }
      if (in->op == IR_OP_ADDRESS_OF && op == &in->lhs) {
        continue; /* ADDRESS_OF lowers through MIR_LEA_*; no value preload. */
      }
      if (op->kind != IR_OPERAND_SYMBOL || !op->name ||
          mir_name_map_has(&map, op->name) ||
          !mir_name_is_global_scalar(generator, op->name)) {
        continue;
      }
      const CgSym *s = code_generator_lookup_symbol(generator, op->name);
      int size = s ? code_generator_binary_resolved_type_scalar_size(s->type) : 0;
      if (size != 1 && size != 2 && size != 4 && size != 8) {
        continue;
      }
      int is_signed =
          code_generator_binary_resolved_type_is_signed_integer(s->type);
      /* A float global must be cached in an XMM vreg so float consumers read it
       * via the XMM path; a GP cache leaves the bits in a GP register the float
       * ops never read (reading an uninitialized xmm instead). */
      int fbits = code_generator_binary_resolved_type_float_bits(s->type);
      MirVregId v = mir_name_map_get_or_add(
          &map, &fn, op->name, 0, fbits ? MIR_RC_XMM : MIR_RC_GP,
          fbits ? fbits / 8 : 8);
      if (v == MIR_VREG_NONE) {
        goto oom;
      }
      if (!mir_emit1(&fn, MIR_LOAD_GLOBAL, mir_op_vreg(v),
                     mir_op_symbol(op->name), mir_op_none(), size,
                     is_signed ? 0 : 1, 0)) {
        goto oom;
      }
      /* Record this cached global for reload-after-call. The map-has guard
       * above means each global is loaded (and recorded) exactly once. */
      if (wb.all_count >= wb_all_cap) {
        size_t nc = wb_all_cap ? wb_all_cap * 2 : 4;
        const char **grown = (const char **)realloc(wb.all, nc * sizeof(*grown));
        if (!grown) {
          goto oom;
        }
        wb.all = grown;
        wb_all_cap = nc;
      }
      wb.all[wb.all_count++] = op->name;
    }
  }

  /* Address-taken globals (&g): a pointer can read/write their memory, so the
   * cache vreg must be flushed before a pointer LOAD/STORE and reloaded after a
   * pointer STORE. Collect them once (deduped).
   *
   * Only globals the loop above actually cached belong here. One that is merely
   * address-taken -- `&g` with no read of `g` by name anywhere in the function,
   * which is every global aggregate, since those are only ever reached through
   * an address -- has no cache vreg, and memory is already authoritative. Its
   * name still maps to a fresh vreg on demand, so flushing it would store an
   * undefined register over the global's own storage: `var p: int32* = &g;
   * return *p;` read back whatever the allocator had left in that register. */
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (in->op != IR_OP_ADDRESS_OF || in->lhs.kind != IR_OPERAND_SYMBOL ||
        !in->lhs.name || !mir_name_is_global_scalar(generator, in->lhs.name) ||
        !mir_name_map_has(&map, in->lhs.name)) {
      continue;
    }
    if (mir_address_only_feeds_calls(ir_function, in)) {
      continue;
    }
    int present = 0;
    for (size_t j = 0; j < wb.at_count; j++) {
      if (strcmp(wb.at[j], in->lhs.name) == 0) {
        present = 1;
        break;
      }
    }
    if (present) {
      continue;
    }
    if (wb.at_count >= wb_at_cap) {
      size_t nc = wb_at_cap ? wb_at_cap * 2 : 4;
      const char **grown = (const char **)realloc(wb.at, nc * sizeof(*grown));
      if (!grown) {
        goto oom;
      }
      wb.at = grown;
      wb_at_cap = nc;
    }
    wb.at[wb.at_count++] = in->lhs.name;
  }
  /* A cached global can also be aliased by a pointer built at MODULE scope
   * (`var p: int32* = &g;`): no IR_OP_ADDRESS_OF appears in any function, but
   * a pointer LOAD/STORE can still reach its memory. Give those the same
   * address-taken flush/reload discipline. */
  for (size_t i = 0; i < wb.all_count; i++) {
    if (!mir_global_address_escapes_via_initializer(generator, wb.all[i]) &&
        !mir_global_address_taken_in_module(generator, wb.all[i])) {
      continue;
    }
    int present = 0;
    for (size_t j = 0; j < wb.at_count; j++) {
      if (strcmp(wb.at[j], wb.all[i]) == 0) {
        present = 1;
        break;
      }
    }
    if (present) {
      continue;
    }
    if (wb.at_count >= wb_at_cap) {
      size_t nc = wb_at_cap ? wb_at_cap * 2 : 4;
      const char **grown = (const char **)realloc(wb.at, nc * sizeof(*grown));
      if (!grown) {
        goto oom;
      }
      wb.at = grown;
      wb_at_cap = nc;
    }
    wb.at[wb.at_count++] = wb.all[i];
  }

  /* Dirty-global flow analysis: lets the flush before each call/return write
   * only the globals actually dirtied since the last cleaning point, instead
   * of the whole written set (whose clean members a flush could stomp under
   * concurrency). A NULL result (no writes, >64 written globals, malformed
   * CFG) degrades to flushing everything -- but eligibility bails the
   * write+call combination in that case, so calls never see the degraded
   * flush. */
  dirty_masks = mir_compute_global_dirty_masks(ir_function, wb.names, wb.count);
  wb.dirty = dirty_masks;

  /* Hoist loop-invariant constants into pooled vregs. Their materialization
   * starts here and is relocated to hot-loop preheaders after MIR layout. */
  if (!mir_build_const_pool(&fn, generator, context, ir_function)) {
    goto oom;
  }

  /* Detect [base + index*scale] address folds before lowering: the producers
   * are marked to skip and each access carries its SIB descriptor. */
  char *fold_skip = NULL;
  MirAddrFold *folds = NULL;
  if (ir_function->instruction_count > 0) {
    fold_skip = (char *)calloc(ir_function->instruction_count, sizeof(char));
    folds = (MirAddrFold *)calloc(ir_function->instruction_count,
                                  sizeof(MirAddrFold));
    if (!fold_skip || !folds) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    MirTempUseIndex uses;
    if (!mir_temp_use_build(ir_function, &uses)) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    mir_compute_address_folds(ir_function, &uses, fold_skip, folds);
    mir_compute_const_compare_skips(generator, context, ir_function, &uses,
                                    fold_skip);
    mir_temp_use_destroy(&uses);
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    /* --annotate-asm: attribute every op emitted while lowering this IR
     * instruction back to it (inert when the annotator is off). */
    fn.cur_ir_index = (int)i;
    if (fold_skip[i]) {
      continue; /* address sub-expression folded into a SIB access */
    }
    /* A pointer LOAD/STORE may alias an address-taken global: flush the cached
     * address-taken globals to memory first (so the access sees pending by-name
     * writes), and reload them after a STORE (so a later by-name read sees what
     * the store wrote through the alias). Empty set => no overhead. */
    /* An inline kernel reads and writes arrays through pointers, so it is a
     * pointer memory op for this purpose too: an address-taken global it walks
     * over must be flushed from its cache vreg first and reloaded after. */
    int kernel_op =
        mir_ir_kernel_index_for_op(ir_function->instructions[i].op) >= 0 ||
        ir_function->instructions[i].op == IR_OP_INLINE_ASM;
    int mem_op = ir_function->instructions[i].op == IR_OP_LOAD ||
                 ir_function->instructions[i].op == IR_OP_STORE || kernel_op;
    int store_op = ir_function->instructions[i].op == IR_OP_STORE || kernel_op;
    if (mem_op && wb.at_count > 0 &&
        !mir_emit_global_alias_flush(&fn, generator, &map, &wb)) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    if (!mir_emit_volatile_global_reads(&fn, generator, &map,
                                        &ir_function->instructions[i],
                                        &folds[i])) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    if (folds[i].valid) {
      if (!mir_lower_folded_access(&fn, generator, context, &map,
                                   &ir_function->instructions[i], &folds[i]) ||
          !mir_emit_volatile_global_write(&fn, generator, &map,
                                          &ir_function->instructions[i])) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
    } else if (mir_fuses_compare_branch(generator, ir_function, i)) {
      if (!mir_lower_compare_branch(&fn, generator, context, &map, ir_function,
                                    &ir_function->instructions[i],
                                    &ir_function->instructions[i + 1])) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
      i++; /* consumed the branch_zero too */
    } else {
      /* Around a call, memory is the source of truth for cached globals: flush
       * the written ones first (the callee may read them), lower the call, then
       * reload cached globals only when the callee may have written them. */
      const IRInstruction *cin = &ir_function->instructions[i];
      int is_call = cin->op == IR_OP_CALL || cin->op == IR_OP_CALL_INDIRECT ||
                    cin->op == IR_OP_INLINE_ASM;
      int call_writes_globals =
          cin->op == IR_OP_INLINE_ASM
              ? 1
              : (is_call ? mir_call_may_write_globals(generator, ir_function,
                                                      i, cin)
                         : 0);
      if (is_call && wb.all_count > 0 &&
          !mir_emit_global_writebacks(&fn, generator, &map, &wb)) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
      if (!mir_lower_instruction(&fn, generator, context, &map,
                                 &ir_function->instructions[i], &wb) ||
          !mir_emit_volatile_global_write(&fn, generator, &map, cin)) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
      if (is_call && call_writes_globals && wb.all_count > 0) {
        /* If the call's result is assigned straight to a global (@g = f()), the
         * call lowering already captured RAX into @g's cache vreg; don't reload
         * @g from its stale memory (that would drop the just-stored result). */
        const IROperand *cd = &cin->dest;
        const char *except =
            (cd->kind == IR_OPERAND_SYMBOL && cd->name &&
             mir_name_is_global_scalar(generator, cd->name))
                ? cd->name
                : NULL;
        if (!mir_emit_global_reloads_except(&fn, generator, &map, &wb, except)) {
          free(fold_skip);
          free(folds);
          goto oom;
        }
      }
      /* Keep narrow integer homes canonical: an ASSIGN/BINARY/UNARY/CALL
       * result written to an int32/uint32/int16/etc. variable was computed in
       * 64 bits and may carry garbage above the type's width. LOAD already
       * extends at the access width, CAST canonicalizes itself, and an
       * in-range literal is canonical as materialized, so those are skipped. */
      {
        if (cin->op == IR_OP_ASSIGN || cin->op == IR_OP_BINARY ||
            cin->op == IR_OP_UNARY || cin->op == IR_OP_CALL ||
            cin->op == IR_OP_CALL_INDIRECT) {
          int signed_home = 0;
          int cw =
              mir_dest_integer_narrow_width(generator, context, &cin->dest,
                                            &signed_home);
          int literal_canonical = 0;
          if (cw && cin->op == IR_OP_ASSIGN &&
              cin->lhs.kind == IR_OPERAND_INT) {
            int bits = cw * 8;
            if (signed_home) {
              int64_t minv = -(1ll << (bits - 1));
              int64_t maxv = (1ll << (bits - 1)) - 1;
              literal_canonical = cin->lhs.int_value >= minv &&
                                  cin->lhs.int_value <= maxv;
            } else {
              literal_canonical = cin->lhs.int_value >= 0 &&
                                  (uint64_t)cin->lhs.int_value <
                                      (1ull << bits);
            }
          }
          /* Range-proven elision: when the operand ranges show the exact
           * 64-bit result already fits the home's width, the computed bits
           * ARE canonical and the re-extension is dropped. This is what takes
           * the `movsx` off every int32 loop counter's step (`i = i + 1`
           * under an `i < n` guard cannot leave int32). */
          int range_canonical = 0;
          if (cw && !literal_canonical && !fn.has_error &&
              (cin->op == IR_OP_BINARY || cin->op == IR_OP_ASSIGN)) {
            if (!vr_oracle) {
              vr_oracle = ir_value_range_oracle_create(ir_function);
            }
            range_canonical =
                vr_oracle && ir_value_range_result_is_narrow(
                                 vr_oracle, i, cw * 8, !signed_home);
          }
          if (cw && !literal_canonical && !range_canonical && !fn.has_error) {
            MirOperand cd =
                mir_value_operand(&fn, generator, context, &map, &cin->dest);
            if (cd.kind == MIR_OPK_VREG &&
                !mir_emit1(&fn, signed_home ? MIR_MOVSX : MIR_MOVZX, cd, cd,
                           mir_op_none(), cw, !signed_home, 0)) {
              free(fold_skip);
              free(folds);
              goto oom;
            }
          }
        }
      }
    }
    if (store_op && wb.at_count > 0 &&
        !mir_emit_global_reload_names(&fn, generator, &map, wb.at,
                                      wb.at_count)) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    if (fn.has_error) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
  }
  free(fold_skip);
  free(folds);

  mir_fuse_mov_then_extend(&fn);
  mir_drop_dead_extensions(&fn);
  mir_canonicalize_commutative(&fn);
  mir_narrow_zero_extended_ops(&fn);
  mir_elide_guarded_sext(&fn);
  mir_fold_address_offsets(&fn);
  mir_cse_loads(&fn);
  mir_slp_pair_f64(&fn);
  mir_build_jump_tables(&fn);
  mir_rotate_loops(&fn);
  mir_thread_branch_over_jump(&fn);
  /* The const pool decides where a materialization dominates its uses from the
   * linear order, so it has to run while that order still reflects the control
   * flow. Sinking moves a use out of line, where it is reached by a branch from
   * above rather than by falling through, and a pool placed afterwards can land
   * a definition on a path the use never takes. */
  mir_place_const_pool(&fn);
  mir_sink_cold_exits(&fn);

  g_mir_ra_trace_name = ir_function->name;
  if (!mir_regalloc(&fn) || fn.has_error) {
    goto oom;
  }
  {
    static int dump = -1;
    static const char *dump_only = NULL;
    if (dump < 0) {
      const char *env = getenv("METTLE_MIR_DUMP");
      dump = env ? 1 : 0;
      if (env && env[0] && strcmp(env, "1") != 0) {
        dump_only = env;
      }
    }
    if (dump && (!dump_only || (ir_function->name &&
                                strcmp(ir_function->name, dump_only) == 0))) {
      fprintf(stderr, "; MIR function %s\n",
              ir_function->name ? ir_function->name : "?");
      mir_function_dump(&fn, stderr);
    }
  }
  /* --annotate-asm: open a capture context so mir_encode's per-instruction
   * records land under this function (inert when the annotator is off). */
  fn.cur_ir_index = -1;
  if (mir_annotate_enabled()) {
    mir_annotate_begin_function(
        ir_function->name, ir_function, mir_function_filename(ir_function),
        (ir_function->location.line));
    mir_annotate_note_backend("register-allocated", NULL);
  }
  if (!mir_encode(&fn) || fn.has_error) {
    mir_annotate_end_function();
    goto oom;
  }
  mir_annotate_end_function();

  if (ir_machine_collecting() && ir_function->name) {
    ir_machine_note_frame(ir_function->name,
                          (long long)context->frame_size,
                          mir_encode_last_spills);
  }
  free(dirty_masks);
  free(wb.names);
  free(wb.all);
  free(wb.at);
  mir_name_map_destroy(&map);
  mir_function_destroy(&fn);
  ir_value_range_oracle_destroy(vr_oracle);
  return 1;

oom:
  if (!generator->has_error) {
    code_generator_set_error(generator,
                             "Out of memory or unsupported construct while "
                             "emitting MIR for function '%s'",
                             ir_function->name ? ir_function->name : "?");
  }
  free(dirty_masks);
  free(wb.names);
  free(wb.all);
  free(wb.at);
  mir_name_map_destroy(&map);
  mir_function_destroy(&fn);
  ir_value_range_oracle_destroy(vr_oracle);
  return 0;
}
