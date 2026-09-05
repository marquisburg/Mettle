/* IR -> NVIDIA PTX text emitter. See ptx_emitter.h.
 *
 * Strategy: PTX is a typed virtual ISA with unlimited registers, so there is no
 * register allocation. Each IR value (SSA temp or mutable local/param) is bound
 * to one PTX register of a class derived from its type:
 *   PC_PRED -> %p   PC_B32 -> %r   PC_B64 -> %rd (also pointers)
 *   PC_F32  -> %f   PC_F64 -> %fd
 * Types come from backend-owned module symbols/value_type descriptors, with
 * parameter/local names retained as a compatibility fallback, plus
 * per-instruction inference of temps. No frontend symbol table is consulted.
 * The function body is buffered so the .reg declarations (which need the final
 * per-class counts) can be emitted at the top. */
#include "ptx_emitter.h"
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { PC_NONE, PC_PRED, PC_B16, PC_B32, PC_B64, PC_F32, PC_F64 } PtxClass;

typedef struct {
  PtxClass cls;
  int idx;         /* register index within its class */
  int is_unsigned; /* integer signedness hint */
  int is_ptr;      /* pointer value (still PC_B64) */
  MtlcTypeKind elem;   /* pointed-to scalar kind, when is_ptr */
  MtlcAddressSpace address_space;
  /* A local whose home is per-thread `.local` storage rather than a register:
   * either an aggregate (a struct, array, or tagged enum has no register form)
   * or a scalar whose address is taken. `mem_addr` is the .b64 register holding
   * its address; the remaining fields describe the *value* for a scalar, and
   * `mem_aggregate` says there is no scalar value at all. */
  int mem_local;
  int mem_aggregate;
  int mem_addr;
  size_t mem_size;
  size_t mem_align;
} PtxVal;

typedef struct {
  char *name;
  PtxVal val;
} PtxBinding;

typedef struct {
  uint32_t id;
  IRTensorResidencyScope scope;
  int resident;
  int tuple_peak;
  PtxClass accumulator_class;
  int accumulator_base;
  int accumulator_count;
  const IRInstruction *consumed_epilogue;
} PtxTensorResidency;

/* growable text buffer */
typedef struct {
  char *data;
  size_t len, cap;
} Sb;

typedef struct {
  Sb body;
  Sb declarations;
  int count[8]; /* register counts indexed by PtxClass */
  PtxBinding *binds;
  size_t nbinds, capbinds;
  PtxTensorResidency *tensor_residencies;
  size_t tensor_residency_count, tensor_residency_capacity;
  IRProgram *program;
  IRFunction *function;
  const IRModuleSymbol *function_symbol;
  PtxVal return_desc;
  size_t call_count;
  int target_arch;
  char target_variant; /* '\0' compatible, 'a' architecture-, 'f' family-specific */
  int isa_major;
  int isa_minor;
  int tensor_tuple_budget;
  char *error;
} PtxFn;

static void sb_ensure(Sb *sb, size_t extra) {
  if (sb->len + extra + 1 <= sb->cap) {
    return;
  }
  size_t ncap = sb->cap ? sb->cap * 2 : 1024;
  while (ncap < sb->len + extra + 1) {
    ncap *= 2;
  }
  sb->data = realloc(sb->data, ncap);
  sb->cap = ncap;
}
static void sb_puts(Sb *sb, const char *s) {
  size_t n = strlen(s);
  sb_ensure(sb, n);
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = 0;
}
static void sb_printf(Sb *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (need < 0) {
    va_end(ap);
    return;
  }
  sb_ensure(sb, (size_t)need);
  vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap);
  sb->len += (size_t)need;
  va_end(ap);
}

static void fn_error(PtxFn *fn, const char *fmt, ...) {
  if (fn->error) {
    return;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fn->error = strdup(buf);
}

/* ---- register allocation (just bump a per-class counter) ---- */
static const char *cls_prefix(PtxClass c) {
  switch (c) {
  case PC_PRED:
    return "%p";
  case PC_B16:
    return "%rs";
  case PC_B32:
    return "%r";
  case PC_B64:
    return "%rd";
  case PC_F32:
    return "%f";
  case PC_F64:
    return "%fd";
  default:
    return "%?";
  }
}
static const char *cls_regtype(PtxClass c) {
  switch (c) {
  case PC_PRED:
    return ".pred";
  case PC_B16:
    return ".b16";
  case PC_B32:
    return ".b32";
  case PC_B64:
    return ".b64";
  case PC_F32:
    return ".f32";
  case PC_F64:
    return ".f64";
  default:
    return ".b32";
  }
}
static int new_reg(PtxFn *fn, PtxClass c) { return fn->count[c]++; }
static PtxClass elem_class(MtlcTypeKind elem, int *is_unsigned);
static const char *mem_type_suffix(MtlcTypeKind elem);
static int ptx_type_is_aggregate(const MtlcType *type);
static PtxVal operand_desc(PtxFn *fn, const IROperand *op);
static void use_as(PtxFn *fn, const IROperand *op, PtxClass want, char *out);
static void coerce(PtxFn *fn, PtxClass scls, int s_unsigned, const char *srcreg,
                   PtxClass want, char *out);
static void reg_name(PtxClass c, int idx, char *out) {
  snprintf(out, 24, "%s%d", cls_prefix(c), idx);
}

/* ---- value bindings ---- */
static PtxBinding *find_binding(PtxFn *fn, const char *name) {
  for (size_t i = 0; i < fn->nbinds; i++) {
    if (strcmp(fn->binds[i].name, name) == 0) {
      return &fn->binds[i];
    }
  }
  return NULL;
}
/* Temps and mutable symbols share one binding table and differ only in operand
 * kind, so anything that asks "what is this operand bound to" has to accept
 * both. A record-valued call result is a temp; the local it lands in is a
 * symbol; the copy between them is the same copy. */
static PtxBinding *named_binding(PtxFn *fn, const IROperand *op) {
  if (!op || !op->name ||
      (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL)) {
    return NULL;
  }
  return find_binding(fn, op->name);
}

static PtxVal *bind_value(PtxFn *fn, const char *name, PtxVal v) {
  PtxBinding *b = find_binding(fn, name);
  if (b) {
    /* A memory-homed local keeps its home. Producers write a scratch register
     * (see destination_value) and land here, which is the one place every
     * definition of a symbol passes through, so the store back is written
     * once instead of at each producer. */
    if (b->val.mem_local && !v.mem_local) {
      if (!b->val.mem_aggregate) {
        int u = 0;
        PtxClass want = elem_class(b->val.elem, &u);
        char src[24], addr[24], use[24];
        reg_name(v.cls, v.idx, src);
        reg_name(PC_B64, b->val.mem_addr, addr);
        coerce(fn, v.cls, v.is_unsigned, src, want, use);
        sb_printf(&fn->body, "\tst.local.%s [%s], %s;\n",
                  mem_type_suffix(b->val.elem), addr, use);
      }
      return &b->val;
    }
    b->val = v;
    return &b->val;
  }
  if (fn->nbinds == fn->capbinds) {
    fn->capbinds = fn->capbinds ? fn->capbinds * 2 : 16;
    fn->binds = realloc(fn->binds, fn->capbinds * sizeof(PtxBinding));
  }
  fn->binds[fn->nbinds].name = strdup(name);
  fn->binds[fn->nbinds].val = v;
  return &fn->binds[fn->nbinds++].val;
}

/* Mutable IR symbols have a stable register home. Most frontend-lowered IR
 * writes them through IR_OP_ASSIGN, but target-neutral coalescing may rewrite a
 * producer to write the symbol directly. Rebinding the name to a fresh PTX
 * register is not equivalent across a loop back-edge or a conditional merge:
 * paths that did not execute the producer would observe an undefined register.
 * Temps remain single-definition values and receive a fresh register. */
static PtxVal destination_value(PtxFn *fn, const IROperand *dest,
                                PtxVal computed) {
  if (dest && dest->kind == IR_OPERAND_SYMBOL && dest->name) {
    PtxBinding *home = find_binding(fn, dest->name);
    if (home) {
      if (home->val.mem_local) {
        computed.idx = new_reg(fn, computed.cls);
        return computed;
      }
      if (home->val.cls != computed.cls) {
        fn_error(fn,
                 "PTX: direct write to symbol '%s' changes register class %d -> %d",
                 dest->name, (int)home->val.cls, (int)computed.cls);
        return computed;
      }
      return home->val;
    }
  }
  computed.idx = new_reg(fn, computed.cls);
  return computed;
}

/* ---- type-name parsing (no symbol table) ---- */
static MtlcTypeKind base_kind_from_name(const char *s, int *is_unsigned) {
  *is_unsigned = (strstr(s, "uint") != NULL || strstr(s, "bool") != NULL);
  if (strstr(s, "float16")) {
    return MTLC_TYPE_FLOAT16;
  }
  if (strstr(s, "bfloat16") || strstr(s, "bf16")) {
    return MTLC_TYPE_BFLOAT16;
  }
  if (strstr(s, "float32") || strstr(s, "f32")) {
    return MTLC_TYPE_FLOAT32;
  }
  if (strstr(s, "float64") || strstr(s, "double") || strstr(s, "float")) {
    return MTLC_TYPE_FLOAT64;
  }
  if (strstr(s, "int64") || strstr(s, "uint64")) {
    return *is_unsigned ? MTLC_TYPE_UINT64 : MTLC_TYPE_INT64;
  }
  if (strstr(s, "int16") || strstr(s, "uint16")) {
    return *is_unsigned ? MTLC_TYPE_UINT16 : MTLC_TYPE_INT16;
  }
  if (strstr(s, "int8") || strstr(s, "uint8")) {
    return *is_unsigned ? MTLC_TYPE_UINT8 : MTLC_TYPE_INT8;
  }
  if (strstr(s, "int32") || strstr(s, "uint32")) {
    return *is_unsigned ? MTLC_TYPE_UINT32 : MTLC_TYPE_INT32;
  }
  if (strstr(s, "bool")) {
    return MTLC_TYPE_BOOL;
  }
  return MTLC_TYPE_INT32;
}
static PtxClass class_of_kind(MtlcTypeKind k, int *is_unsigned) {
  switch (k) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
    *is_unsigned = 0;
    return PC_B32;
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_BOOL:
    *is_unsigned = 1;
    return PC_B32;
  case MTLC_TYPE_INT64:
    *is_unsigned = 0;
    return PC_B64;
  case MTLC_TYPE_UINT64:
    *is_unsigned = 1;
    return PC_B64;
  case MTLC_TYPE_FLOAT32:
    *is_unsigned = 0;
    return PC_F32;
  case MTLC_TYPE_FLOAT64:
    *is_unsigned = 0;
    return PC_F64;
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    *is_unsigned = 0;
    return PC_F32;
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_FUNCTION_POINTER:
    *is_unsigned = 1;
    return PC_B64;
  default:
    *is_unsigned = 0;
    return PC_B32;
  }
}
/* Build a PtxVal descriptor (class/ptr/elem) from a type-name string, without
 * allocating a register. */
static PtxVal descriptor_from_typename(const char *name) {
  PtxVal v = {0};
  if (!name) {
    name = "int64";
  }
  int ptr = (strchr(name, '*') != NULL) || strstr(name, "cstring") ||
            strstr(name, "string");
  int isu = 0;
  MtlcTypeKind base = base_kind_from_name(name, &isu);
  if (strstr(name, "cstring")) {
    base = MTLC_TYPE_UINT8;
  }
  if (ptr) {
    v.cls = PC_B64;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    /* pointer-to-pointer (two or more '*') -> element is itself a pointer */
    const char *firstStar = strchr(name, '*');
    v.elem = (firstStar && strchr(firstStar + 1, '*')) ? MTLC_TYPE_POINTER : base;
    v.address_space = MTLC_ADDRESS_SPACE_GENERIC;
  } else {
    int u = 0;
    v.cls = class_of_kind(base, &u);
    v.is_unsigned = u;
    v.elem = base;
  }
  return v;
}

static PtxVal descriptor_from_type(const MtlcType *type) {
  PtxVal v = {0};
  if (!type) {
    return descriptor_from_typename(NULL);
  }
  if (type->kind == MTLC_TYPE_POINTER) {
    v.cls = PC_B64;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    v.elem = type->base_type ? type->base_type->kind : MTLC_TYPE_VOID;
    /* A pointer to an aggregate has no single element width or class. Reporting
     * void makes each load and store take its type from the access's own size
     * and float flag, which is what a field access carries. */
    if (ptx_type_is_aggregate(type->base_type)) {
      v.elem = MTLC_TYPE_VOID;
    }
    v.address_space = type->address_space == MTLC_ADDRESS_SPACE_DEFAULT
                          ? MTLC_ADDRESS_SPACE_GLOBAL
                          : type->address_space;
  } else {
    int u = 0;
    v.cls = class_of_kind(type->kind, &u);
    v.is_unsigned = u;
    v.elem = type->kind;
  }
  return v;
}

/* An aggregate has no register form: a struct, a fixed array, or a tagged enum
 * is only ever reached through its address. */
static int ptx_type_is_aggregate(const MtlcType *type) {
  return type && (type->kind == MTLC_TYPE_STRUCT ||
                  type->kind == MTLC_TYPE_ARRAY ||
                  type->kind == MTLC_TYPE_TAGGED_ENUM);
}

static size_t ptx_class_width(PtxClass cls) {
  switch (cls) {
  case PC_B16: return 2;
  case PC_B64:
  case PC_F64: return 8;
  case PC_PRED:
  case PC_B32:
  case PC_F32: return 4;
  case PC_NONE: return 0;
  }
  return 0;
}

/* Copy `size` bytes between two addressed locations, each named by a state
 * space suffix (".local", ".param", or "" for generic) and a base that is either
 * a register or a symbol. Aggregates have static sizes, so the copy unrolls to
 * naturally aligned chunks and never needs a loop or a runtime length. */
static void ptx_block_copy(PtxFn *fn, const char *dst_space,
                           const char *dst_base, const char *src_space,
                           const char *src_base, size_t size,
                           size_t alignment) {
  size_t widest = alignment ? alignment : 1;
  if (widest > 8) {
    widest = 8;
  }
  for (size_t offset = 0; offset < size;) {
    size_t chunk = widest;
    while (chunk > 1 && (chunk > size - offset || offset % chunk != 0)) {
      chunk /= 2;
    }
    PtxClass cls = (chunk == 8) ? PC_B64 : PC_B32;
    const char *type = (chunk == 8)   ? "b64"
                       : (chunk == 4) ? "b32"
                       : (chunk == 2) ? "u16"
                                      : "u8";
    char reg[24];
    reg_name(cls, new_reg(fn, cls), reg);
    sb_printf(&fn->body, "\tld%s.%s %s, [%s+%zu];\n", src_space, type, reg,
              src_base, offset);
    sb_printf(&fn->body, "\tst%s.%s [%s+%zu], %s;\n", dst_space, type,
              dst_base, offset, reg);
    offset += chunk;
  }
}

/* `.local` addresses are only meaningful to the thread's own local window. Once
 * one leaves the function -- as a call argument or stored into memory -- it has
 * to be a generic address, which is what the receiving side assumes. Rewrites
 * `reg` in place when a conversion is needed. */
static void ptx_generic_address(PtxFn *fn, const IROperand *op, char *reg) {
  PtxVal v = operand_desc(fn, op);
  if (!v.is_ptr || v.address_space != MTLC_ADDRESS_SPACE_PRIVATE) {
    return;
  }
  char converted[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), converted);
  sb_printf(&fn->body, "\tcvta.local.u64 %s, %s;\n", converted, reg);
  snprintf(reg, 24, "%s", converted);
}

static int ptx_local_address_taken(const IRFunction *func, const char *name) {
  if (!func || !name) {
    return 0;
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name && strcmp(in->lhs.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static const char *ptx_memory_space(MtlcAddressSpace address_space) {
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_GLOBAL: return ".global";
  case MTLC_ADDRESS_SPACE_WORKGROUP: return ".shared";
  /* A `constant` pointer names device memory the launch allocated and the
   * kernel only reads. The constant bank is not where that memory is, so the
   * load takes the non-coherent path through the read-only cache instead:
   * the same address, read without coherence traffic. */
  case MTLC_ADDRESS_SPACE_CONSTANT: return ".global";
  case MTLC_ADDRESS_SPACE_PRIVATE: return ".local";
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
    return "";
  }
  return NULL;
}

/* --- widening adjacent loads into one vector access ---------------------
 *
 * Four `ld.global.f32` from consecutive elements are one `ld.global.v4.f32`
 * when the first address is 16-byte aligned. Nothing in the arithmetic says
 * that; the pointer's declared `align(N)` does, which is why this reads the
 * type and refuses to guess. */
typedef struct {
  unsigned char width;    /* 2 or 4 on the load that heads a group */
  unsigned char absorbed; /* this load is emitted by an earlier group */
  size_t members[4];      /* instruction indices the group covers */
} PtxVectorPlan;

/* Fold an operand to a constant by walking the producers that define it. Only
 * the shapes an address computation makes: an integer, a copy, a widening
 * cast, and a product or sum of constants. */
static int ptx_fold_constant(const IRFunction *func, size_t before,
                             const IROperand *operand, long long *out,
                             unsigned depth) {
  if (!func || !operand || depth > 8) return 0;
  if (operand->kind == IR_OPERAND_INT) {
    *out = operand->int_value;
    return 1;
  }
  if ((operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL) ||
      !operand->name)
    return 0;
  for (size_t i = before; i > 0; i--) {
    const IRInstruction *producer = &func->instructions[i - 1];
    if (!producer->dest.name || !producer->dest.kind ||
        strcmp(producer->dest.name, operand->name) != 0)
      continue;
    if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST)
      return ptx_fold_constant(func, i - 1, &producer->lhs, out, depth + 1);
    if (producer->op == IR_OP_BINARY && producer->text) {
      long long lhs = 0, rhs = 0;
      if (!ptx_fold_constant(func, i - 1, &producer->lhs, &lhs, depth + 1) ||
          !ptx_fold_constant(func, i - 1, &producer->rhs, &rhs, depth + 1))
        return 0;
      if (strcmp(producer->text, "*") == 0) { *out = lhs * rhs; return 1; }
      if (strcmp(producer->text, "+") == 0) { *out = lhs + rhs; return 1; }
      if (strcmp(producer->text, "-") == 0) { *out = lhs - rhs; return 1; }
      if (strcmp(producer->text, "<<") == 0 && rhs >= 0 && rhs < 32) {
        *out = lhs << rhs;
        return 1;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

/* Split a load address into a base value and a constant byte offset. */
static void ptx_address_parts(const IRFunction *func, size_t index,
                              const IROperand *address,
                              const IROperand **base, long long *offset) {
  *base = address;
  *offset = 0;
  if (!func || !address || !address->name ||
      (address->kind != IR_OPERAND_TEMP && address->kind != IR_OPERAND_SYMBOL))
    return;
  for (size_t i = index; i > 0; i--) {
    const IRInstruction *producer = &func->instructions[i - 1];
    long long constant = 0;
    if (!producer->dest.name ||
        strcmp(producer->dest.name, address->name) != 0)
      continue;
    if (producer->op != IR_OP_BINARY || !producer->text ||
        strcmp(producer->text, "+") != 0)
      return;
    if (ptx_fold_constant(func, i - 1, &producer->rhs, &constant, 0)) {
      *base = &producer->lhs;
      *offset = constant;
    }
    return;
  }
}

/* Whether an instruction between two loads keeps them adjacent. The list is
 * what is allowed rather than what is not: an unfamiliar opcode ends the group
 * instead of being assumed harmless. */
static int ptx_vector_gap_is_safe(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_ASSIGN:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_LOAD:
  case IR_OP_SELECT:
  case IR_OP_ADDRESS_OF:
    return 1;
  default:
    return 0;
  }
}

static int ptx_vector_element_ok(MtlcTypeKind elem) {
  return elem == MTLC_TYPE_FLOAT32 || elem == MTLC_TYPE_INT32 ||
         elem == MTLC_TYPE_UINT32 || elem == MTLC_TYPE_FLOAT64 ||
         elem == MTLC_TYPE_INT64 || elem == MTLC_TYPE_UINT64;
}

/* The element type a load reads, from the pointer's own type. */
static const MtlcType *ptx_load_pointer_type(const IRProgram *program,
                                             const IRFunction *func,
                                             const IROperand *base) {
  const IRModuleSymbol *symbol = NULL;
  if (!func || !base || !base->name) return NULL;
  symbol = program ? ir_program_lookup_symbol(program, func->name) : NULL;
  if (base->kind == IR_OPERAND_SYMBOL && symbol &&
      symbol->kind == IR_MODSYM_FUNCTION) {
    for (size_t p = 0; p < func->parameter_count && p < symbol->param_count;
         p++) {
      if (func->parameter_names && func->parameter_names[p] &&
          strcmp(func->parameter_names[p], base->name) == 0)
        return symbol->param_types ? symbol->param_types[p] : NULL;
    }
  }
  for (size_t i = func->instruction_count; i > 0; i--) {
    const IRInstruction *producer = &func->instructions[i - 1];
    if (!producer->dest.name || strcmp(producer->dest.name, base->name) != 0)
      continue;
    if (producer->op == IR_OP_LOAD || producer->op == IR_OP_ASSIGN ||
        producer->op == IR_OP_CAST || producer->op == IR_OP_BINARY ||
        producer->op == IR_OP_ADDRESS_SPACE_ALLOC)
      return producer->value_type;
    break;
  }
  if (program && base->kind == IR_OPERAND_SYMBOL) {
    const IRModuleSymbol *global = ir_program_lookup_symbol(program, base->name);
    if (global && global->kind == IR_MODSYM_VARIABLE) return global->type;
  }
  return NULL;
}

/* Plan the vector loads for one function. A group is a run of loads from one
 * base at consecutive element offsets, in one block, through a pointer whose
 * declared alignment covers the whole access. */
static PtxVectorPlan *ptx_plan_vector_loads(const IRProgram *program,
                                            const IRFunction *func) {
  PtxVectorPlan *plan;
  if (!func || func->instruction_count == 0) return NULL;
  plan = calloc(func->instruction_count, sizeof(*plan));
  if (!plan) return NULL;
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *head = &func->instructions[i];
    const IROperand *base = NULL;
    const MtlcType *pointer_type = NULL;
    long long offset = 0;
    size_t stride = 0;
    size_t members[4];
    size_t found = 1;
    if (head->op != IR_OP_LOAD || plan[i].absorbed) continue;
    ptx_address_parts(func, i, &head->lhs, &base, &offset);
    pointer_type = ptx_load_pointer_type(program, func, base);
    if (!pointer_type || pointer_type->kind != MTLC_TYPE_POINTER ||
        !pointer_type->base_type || !pointer_type->pointee_align ||
        !ptx_vector_element_ok(pointer_type->base_type->kind))
      continue;
    if (pointer_type->address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
        pointer_type->address_space != MTLC_ADDRESS_SPACE_WORKGROUP &&
        pointer_type->address_space != MTLC_ADDRESS_SPACE_CONSTANT)
      continue;
    stride = pointer_type->base_type->size;
    if (!stride || (size_t)offset % (stride * 2) != 0) continue;
    members[0] = i;
    for (size_t j = i + 1; j < func->instruction_count && found < 4; j++) {
      const IRInstruction *next = &func->instructions[j];
      const IROperand *next_base = NULL;
      long long next_offset = 0;
      if (next->op == IR_OP_LOAD && !plan[j].absorbed) {
        ptx_address_parts(func, j, &next->lhs, &next_base, &next_offset);
        if (next_base && base && next_base->name && base->name &&
            next_base->kind == base->kind &&
            strcmp(next_base->name, base->name) == 0 &&
            next_offset == offset + (long long)(stride * found)) {
          members[found++] = j;
          continue;
        }
      }
      if (!ptx_vector_gap_is_safe(next)) break;
    }
    if (found >= 4 && pointer_type->pointee_align >= stride * 4 &&
        (size_t)offset % (stride * 4) == 0) {
      plan[i].width = 4;
    } else if (found >= 2 && pointer_type->pointee_align >= stride * 2) {
      plan[i].width = 2;
    } else {
      continue;
    }
    for (size_t k = 1; k < plan[i].width; k++) {
      plan[members[k]].absorbed = 1;
      plan[members[k]].width = 0;
    }
    for (size_t k = 0; k < plan[i].width; k++) {
      plan[i].members[k] = members[k];
    }
  }
  return plan;
}

/* The space a load is issued in. A `constant` pointer adds `.nc`, which is
 * what makes it a read-only-cache load; every other space loads plainly. */
static const char *ptx_load_space(MtlcAddressSpace address_space) {
  if (address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
    return ".global.nc";
  }
  return ptx_memory_space(address_space);
}

/* element class from a pointer value's elem kind */
static PtxClass elem_class(MtlcTypeKind elem, int *is_unsigned) {
  return class_of_kind(elem, is_unsigned);
}

/* PTX ld/st type suffix for a load/store of element kind */
static const char *mem_type_suffix(MtlcTypeKind elem) {
  switch (elem) {
  case MTLC_TYPE_INT8:
    return "s8";
  case MTLC_TYPE_UINT8:
    return "u8";
  case MTLC_TYPE_INT16:
    return "s16";
  case MTLC_TYPE_UINT16:
    return "u16";
  case MTLC_TYPE_INT32:
    return "s32";
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_BOOL:
    return "u32";
  case MTLC_TYPE_INT64:
    return "s64";
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_FUNCTION_POINTER:
    return "u64";
  case MTLC_TYPE_FLOAT32:
    return "f32";
  case MTLC_TYPE_FLOAT64:
    return "f64";
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    return "b16";
  default:
    return "u32";
  }
}

/* ---- conversions ---- */
/* Coerce a value in register (src) of class scls to class want; emits cvt as
 * needed; writes the resulting register name into out. */
static void coerce(PtxFn *fn, PtxClass scls, int s_unsigned, const char *srcreg,
                   PtxClass want, char *out) {
  if (scls == want) {
    snprintf(out, 24, "%s", srcreg);
    return;
  }
  int idx = new_reg(fn, want);
  reg_name(want, idx, out);
  /* integer width changes */
  if (scls == PC_B32 && want == PC_B64) {
    sb_printf(&fn->body, "\tcvt.%s.%s %s, %s;\n", s_unsigned ? "u64" : "s64",
              s_unsigned ? "u32" : "s32", out, srcreg);
  } else if (scls == PC_B64 && want == PC_B32) {
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_B32 && want == PC_F32) {
    sb_printf(&fn->body, "\tcvt.rn.f32.%s %s, %s;\n", s_unsigned ? "u32" : "s32",
              out, srcreg);
  } else if (scls == PC_B32 && want == PC_F64) {
    sb_printf(&fn->body, "\tcvt.rn.f64.%s %s, %s;\n", s_unsigned ? "u32" : "s32",
              out, srcreg);
  } else if (scls == PC_B64 && want == PC_F32) {
    sb_printf(&fn->body, "\tcvt.rn.f32.%s %s, %s;\n", s_unsigned ? "u64" : "s64",
              out, srcreg);
  } else if (scls == PC_B64 && want == PC_F64) {
    sb_printf(&fn->body, "\tcvt.rn.f64.%s %s, %s;\n", s_unsigned ? "u64" : "s64",
              out, srcreg);
  } else if (scls == PC_F32 && want == PC_B32) {
    sb_printf(&fn->body, "\tcvt.rzi.s32.f32 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F64 && want == PC_B32) {
    sb_printf(&fn->body, "\tcvt.rzi.s32.f64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F32 && want == PC_B64) {
    sb_printf(&fn->body, "\tcvt.rzi.s64.f32 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F64 && want == PC_B64) {
    sb_printf(&fn->body, "\tcvt.rzi.s64.f64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F32 && want == PC_F64) {
    sb_printf(&fn->body, "\tcvt.f64.f32 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F64 && want == PC_F32) {
    sb_printf(&fn->body, "\tcvt.rn.f32.f64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_PRED && want == PC_B32) {
    sb_printf(&fn->body, "\tselp.u32 %s, 1, 0, %s;\n", out, srcreg);
  } else if (scls == PC_B32 && want == PC_PRED) {
    sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", out, srcreg);
  } else {
    fn_error(fn, "unsupported PTX coercion class %d -> %d", scls, want);
  }
}

static uint32_t f32_bits(double v) {
  float f = (float)v;
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}
static uint64_t f64_bits(double v) {
  uint64_t b;
  memcpy(&b, &v, 8);
  return b;
}

/* Resolve a source operand into a register of class `want`, materializing
 * immediates and coercing as needed. Writes the register name into out. */
static void use_as(PtxFn *fn, const IROperand *op, PtxClass want, char *out) {
  if (op->kind == IR_OPERAND_INT) {
    int idx = new_reg(fn, want);
    reg_name(want, idx, out);
    if (want == PC_B32) {
      sb_printf(&fn->body, "\tmov.u32 %s, %lld;\n", out,
                (long long)op->int_value);
    } else if (want == PC_B64) {
      sb_printf(&fn->body, "\tmov.u64 %s, %lld;\n", out,
                (long long)op->int_value);
    } else if (want == PC_F32) {
      sb_printf(&fn->body, "\tmov.f32 %s, 0f%08X;\n", out,
                f32_bits((double)op->int_value));
    } else if (want == PC_F64) {
      sb_printf(&fn->body, "\tmov.f64 %s, 0d%016llX;\n", out,
                (unsigned long long)f64_bits((double)op->int_value));
    } else if (want == PC_PRED) {
      sb_printf(&fn->body, "\tsetp.ne.u32 %s, %lld, 0;\n", out,
                (long long)op->int_value);
    }
    return;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    PtxClass fc = (op->float_bits == 32) ? PC_F32 : PC_F64;
    int idx = new_reg(fn, fc);
    char tmp[24];
    reg_name(fc, idx, tmp);
    if (fc == PC_F32) {
      sb_printf(&fn->body, "\tmov.f32 %s, 0f%08X;\n", tmp,
                f32_bits(op->float_value));
    } else {
      sb_printf(&fn->body, "\tmov.f64 %s, 0d%016llX;\n", tmp,
                (unsigned long long)f64_bits(op->float_value));
    }
    coerce(fn, fc, 0, tmp, want, out);
    return;
  }
  /* temp or symbol */
  PtxBinding *b = (op->name) ? find_binding(fn, op->name) : NULL;
  if (!b) {
    fn_error(fn, "PTX: use of undefined value '%s'",
             op->name ? op->name : "?");
    snprintf(out, 24, "%%r0");
    return;
  }
  if (b->val.mem_local) {
    if (b->val.mem_aggregate) {
      fn_error(fn, "PTX: aggregate local '%s' has no scalar value", op->name);
      snprintf(out, 24, "%%r0");
      return;
    }
    int u = 0;
    PtxClass sc = elem_class(b->val.elem, &u);
    char tmp[24], addr[24];
    reg_name(sc, new_reg(fn, sc), tmp);
    reg_name(PC_B64, b->val.mem_addr, addr);
    sb_printf(&fn->body, "\tld.local.%s %s, [%s];\n",
              mem_type_suffix(b->val.elem), tmp, addr);
    coerce(fn, sc, b->val.is_unsigned, tmp, want, out);
    return;
  }
  char src[24];
  reg_name(b->val.cls, b->val.idx, src);
  coerce(fn, b->val.cls, b->val.is_unsigned, src, want, out);
}

/* class of an operand's current value (for result-type inference) */
/* Module-scope string pool for kernel-side printf. PTX has no string operand
 * and requires a global byte array declared before any use, so every literal a
 * reachable kernel prints is collected up front, emitted once, and referenced
 * by index. Module state, not function state: the pool spans the file. */
static char **g_ptx_strings = NULL;
static size_t g_ptx_string_count = 0;
static size_t g_ptx_string_capacity = 0;
static int g_ptx_uses_vprintf = 0;
/* --gpu-checks: whether `gpu_assert` emits its trap. */
static int g_ptx_emit_checks = 0;
/* --report-gpu-types: what the type-directed device analyses concluded, and
 * what they cost. Counted while the module is emitted and printed once. */
static int g_ptx_report_types = 0;
static long long g_ptx_spaced_accesses = 0;
static long long g_ptx_generic_accesses = 0;
static long long g_ptx_vector_groups = 0;
static long long g_ptx_vector_loads_saved = 0;
static double g_ptx_analysis_seconds = 0.0;

static int ptx_string_index(const char *text) {
  if (!text) return -1;
  for (size_t i = 0; i < g_ptx_string_count; i++) {
    if (strcmp(g_ptx_strings[i], text) == 0) return (int)i;
  }
  return -1;
}

static int ptx_string_intern(const char *text) {
  int existing = ptx_string_index(text);
  if (existing >= 0) return existing;
  if (g_ptx_string_count == g_ptx_string_capacity) {
    size_t grown = g_ptx_string_capacity ? g_ptx_string_capacity * 2 : 8;
    char **resized = realloc(g_ptx_strings, grown * sizeof(*resized));
    if (!resized) return -1;
    g_ptx_strings = resized;
    g_ptx_string_capacity = grown;
  }
  g_ptx_strings[g_ptx_string_count] = strdup(text ? text : "");
  if (!g_ptx_strings[g_ptx_string_count]) return -1;
  return (int)g_ptx_string_count++;
}

static void ptx_string_pool_reset(void) {
  for (size_t i = 0; i < g_ptx_string_count; i++) free(g_ptx_strings[i]);
  free(g_ptx_strings);
  g_ptx_strings = NULL;
  g_ptx_string_count = 0;
  g_ptx_string_capacity = 0;
  g_ptx_uses_vprintf = 0;
}

/* A kernel's format string reaches the IR as a load from a string operand
 * (the frontend takes the char pointer out of the string value), so resolve a
 * temp back to the literal that produced it. Anything else is not a literal
 * and the print rejects it. */
static const char *ptx_literal_string(const IRFunction *function,
                                      const IROperand *operand) {
  if (!operand) return NULL;
  if (operand->kind == IR_OPERAND_STRING) return operand->name;
  if (!function || operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return NULL;
  }
  for (size_t i = function->instruction_count; i > 0;) {
    i--;
    const IRInstruction *in = &function->instructions[i];
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name ||
        strcmp(in->dest.name, operand->name) != 0) {
      continue;
    }
    if (in->op == IR_OP_LOAD && in->lhs.kind == IR_OPERAND_STRING) {
      return in->lhs.name;
    }
    return NULL;
  }
  return NULL;
}

static int ptx_intrinsic_is_print(MtlcIntrinsic intrinsic) {
  return intrinsic == MTLC_INTRINSIC_GPU_PRINT ||
         intrinsic == MTLC_INTRINSIC_GPU_PRINT_I32 ||
         intrinsic == MTLC_INTRINSIC_GPU_PRINT_F32 ||
         intrinsic == MTLC_INTRINSIC_GPU_PRINT_2I32;
}

static PtxVal operand_desc(PtxFn *fn, const IROperand *op) {
  PtxVal v = {0};
  if (op->kind == IR_OPERAND_INT) {
    v.cls = (op->int_value > 2147483647LL || op->int_value < -2147483648LL)
                ? PC_B64
                : PC_B32;
    return v;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    v.cls = (op->float_bits == 32) ? PC_F32 : PC_F64;
    return v;
  }
  PtxBinding *b = (op->name) ? find_binding(fn, op->name) : NULL;
  if (b) {
    return b->val;
  }
  v.cls = PC_B32;
  return v;
}

static int sanitize_into(const char *s, char *out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; s[i] && j + 1 < cap; i++) {
    char c = s[i];
    out[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '$')
                   ? c
                   : '_';
  }
  out[j] = 0;
  return (int)j;
}

/* ---- target-neutral GPU index intrinsics -> PTX special registers ---- */
static const char *sreg_for_intrinsic(MtlcIntrinsic intrinsic) {
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_LOCAL_ID_X: return "%tid.x";
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Y: return "%tid.y";
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Z: return "%tid.z";
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_X: return "%ntid.x";
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Y: return "%ntid.y";
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Z: return "%ntid.z";
  case MTLC_INTRINSIC_GPU_GROUP_ID_X: return "%ctaid.x";
  case MTLC_INTRINSIC_GPU_GROUP_ID_Y: return "%ctaid.y";
  case MTLC_INTRINSIC_GPU_GROUP_ID_Z: return "%ctaid.z";
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_X: return "%nctaid.x";
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Y: return "%nctaid.y";
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Z: return "%nctaid.z";
  case MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID: return "%laneid";
  default: return NULL;
  }
}

static const char *ptx_atomic_scope(MtlcMemoryScope scope) {
  switch (scope) {
  /* PTX has no warp- or thread-scoped atomic suffix. CTA is a safe
   * strengthening for both narrower neutral scopes. */
  case MTLC_MEMORY_SCOPE_WORK_ITEM:
  case MTLC_MEMORY_SCOPE_SUBGROUP:
  case MTLC_MEMORY_SCOPE_WORKGROUP:
    return "cta";
  case MTLC_MEMORY_SCOPE_DEVICE: return "gpu";
  case MTLC_MEMORY_SCOPE_SYSTEM: return "sys";
  case MTLC_MEMORY_SCOPE_DEFAULT: return "gpu";
  }
  return NULL;
}

static const char *ptx_atomic_order(MtlcMemoryOrder order) {
  switch (order) {
  case MTLC_MEMORY_ORDER_DEFAULT:
  case MTLC_MEMORY_ORDER_RELAXED:
    return "relaxed";
  case MTLC_MEMORY_ORDER_ACQUIRE: return "acquire";
  case MTLC_MEMORY_ORDER_RELEASE: return "release";
  case MTLC_MEMORY_ORDER_ACQ_REL: return "acq_rel";
  /* Sequential consistency is a two-instruction ABI sequence. The RMW half
   * uses acquire after a fence.sc, per NVIDIA's current PTX atomics ABI. */
  case MTLC_MEMORY_ORDER_SEQ_CST: return "acquire";
  }
  return NULL;
}

static const char *ptx_atomic_space(MtlcAddressSpace address_space) {
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GLOBAL:
    return ".global";
  case MTLC_ADDRESS_SPACE_GENERIC: return "";
  /* Legacy `.shared` is CTA/workgroup storage and remains valid back to the
   * portable PTX 6.4 profile. The explicit `::cta` sub-qualifier would need
   * PTX 7.8 and would needlessly break compute_75 output. */
  case MTLC_ADDRESS_SPACE_WORKGROUP: return ".shared";
  case MTLC_ADDRESS_SPACE_CONSTANT:
  case MTLC_ADDRESS_SPACE_PRIVATE:
    return NULL;
  }
  return NULL;
}

static int ptx_workgroup_barrier_contract(const IRInstruction *instruction) {
  const unsigned supported = MTLC_MEMORY_REGION_WORKGROUP |
                             MTLC_MEMORY_REGION_GLOBAL;
  return instruction &&
         instruction->memory_scope == MTLC_MEMORY_SCOPE_WORKGROUP &&
         instruction->memory_order >= MTLC_MEMORY_ORDER_ACQUIRE &&
         instruction->memory_order <= MTLC_MEMORY_ORDER_SEQ_CST &&
         instruction->memory_regions != 0 &&
         (instruction->memory_regions & ~supported) == 0;
}

/* binary op classification */
static int is_compare_op(const char *t) {
  return !strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, "<=") ||
         !strcmp(t, ">=") || !strcmp(t, "==") || !strcmp(t, "!=");
}

static const char *setp_cmp(const char *t, int is_float, int is_unsigned) {
  /* returns the comparison mnemonic component */
  if (!strcmp(t, "==")) return "eq";
  if (!strcmp(t, "!=")) return "ne";
  if (!strcmp(t, "<")) return is_float ? "lt" : (is_unsigned ? "lo" : "lt");
  if (!strcmp(t, ">")) return is_float ? "gt" : (is_unsigned ? "hi" : "gt");
  if (!strcmp(t, "<=")) return is_float ? "le" : (is_unsigned ? "ls" : "le");
  if (!strcmp(t, ">=")) return is_float ? "ge" : (is_unsigned ? "hs" : "ge");
  return "eq";
}

static const char *type_suffix_for_class(PtxClass c, int is_unsigned) {
  switch (c) {
  case PC_B32:
    return is_unsigned ? "u32" : "s32";
  case PC_B64:
    return is_unsigned ? "u64" : "s64";
  case PC_F32:
    return "f32";
  case PC_F64:
    return "f64";
  default:
    return "s32";
  }
}

typedef enum {
  PTX_WMMA_F16,
  PTX_WMMA_BF16,
  PTX_WMMA_TF32,
  PTX_WMMA_F64,
  PTX_WMMA_I8,
  PTX_WMMA_I4,
  PTX_WMMA_B1
} PtxWmmaKind;

typedef struct {
  PtxWmmaKind kind;
  const char *shape;
  unsigned tile_m;
  unsigned tile_n;
  unsigned tile_k;
  int m_tiles;
  int n_tiles;
  const char *a_type;
  const char *b_type;
  const char *c_type;
  const char *d_type;
  PtxClass a_class;
  PtxClass b_class;
  PtxClass c_class;
  PtxClass d_class;
  int a_registers;
  int b_registers;
  int c_registers;
  int d_registers;
  int min_arch;
  int min_ptx_major;
  int min_ptx_minor;
} PtxWmmaProfile;

typedef enum {
  PTX_MMA_FP8,
  PTX_MMA_MXF8F6F4,
  PTX_MMA_MXFP4,
  PTX_MMA_NVFP4,
  PTX_MMA_SPARSE_F16,
  PTX_MMA_SPARSE_BF16
} PtxMmaKind;

/* A direct warp-level MMA profile. Unlike PtxWmmaProfile, this describes one
 * or more logical m16n8 subtiles whose register fragments are populated from
 * the backend-neutral whole-tile memory contract. Fragment layouts remain a
 * PTX backend detail; neither source syntax nor shared IR exposes them. */
typedef struct {
  PtxMmaKind kind;
  const char *shape;
  const char *a_type;
  const char *b_type;
  const char *c_type;
  const char *d_type;
  const char *scale_type;
  int scale_vectors;
  int a_bits;
  int b_bits;
  int a_registers;
  int b_registers;
  int accumulator_registers;
  int m_tiles;
  int n_tiles;
} PtxMmaProfile;

static int ptx_version_at_least(const PtxFn *fn, int major, int minor) {
  return fn->isa_major > major ||
         (fn->isa_major == major && fn->isa_minor >= minor);
}

static int ptx_async_copy_native(const PtxFn *fn) {
  return fn && fn->target_arch >= 80 && ptx_version_at_least(fn, 7, 0);
}

static int ptx_scalar_kind_bytes(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return 1;
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
    return 2;
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_FLOAT32:
    return 4;
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

static void ptx_async_address(char *buffer, size_t capacity,
                              const char *base, size_t offset) {
  if (!buffer || capacity == 0) return;
  if (offset == 0) {
    snprintf(buffer, capacity, "%s", base ? base : "0");
  } else {
    snprintf(buffer, capacity, "%s+%llu", base ? base : "0",
             (unsigned long long)offset);
  }
}

static void ptx_emit_async_copy(PtxFn *fn, const IRInstruction *in) {
  if (!fn || !in || in->argument_count != 2 ||
      in->async_copy_element_count == 0 ||
      (in->async_copy_transaction_bytes != 4 &&
       in->async_copy_transaction_bytes != 8 &&
       in->async_copy_transaction_bytes != 16)) {
    fn_error(fn, "PTX received an invalid asynchronous-copy instruction");
    return;
  }
  PtxVal destination = operand_desc(fn, &in->arguments[0]);
  PtxVal source = operand_desc(fn, &in->arguments[1]);
  int element_bytes = ptx_scalar_kind_bytes(destination.elem);
  if (!destination.is_ptr || !source.is_ptr ||
      destination.address_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
      (source.address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
       source.address_space != MTLC_ADDRESS_SPACE_GENERIC) ||
      destination.elem != source.elem || element_bytes <= 0) {
    fn_error(fn,
             "PTX async copy requires matching global-to-workgroup scalar pointers");
    return;
  }
  size_t bytes = (size_t)element_bytes *
                 (size_t)in->async_copy_element_count;
  size_t transaction = in->async_copy_transaction_bytes;
  if (bytes == 0 || bytes > 65536 || bytes % transaction != 0 ||
      (in->async_copy_cache == MTLC_ASYNC_CACHE_GLOBAL &&
       transaction != 16)) {
    fn_error(fn, "PTX async copy has an invalid byte/transaction contract");
    return;
  }
  char destination_base[24], source_base[24];
  use_as(fn, &in->arguments[0], PC_B64, destination_base);
  use_as(fn, &in->arguments[1], PC_B64, source_base);
  if (ptx_async_copy_native(fn)) {
    const char *cache =
        in->async_copy_cache == MTLC_ASYNC_CACHE_GLOBAL ? "cg" : "ca";
    sb_printf(&fn->body,
              "\t// mtlc.async_copy %snative bytes=%llu transaction=%llu\n",
              in->async_copy_generated ? "auto-promoted " : "",
              (unsigned long long)bytes, (unsigned long long)transaction);
    for (size_t offset = 0; offset < bytes; offset += transaction) {
      char destination_address[48], source_address[48];
      ptx_async_address(destination_address, sizeof(destination_address),
                        destination_base, offset);
      ptx_async_address(source_address, sizeof(source_address), source_base,
                        offset);
      sb_printf(&fn->body,
                "\tcp.async.%s.shared.global [%s], [%s], %llu;\n", cache,
                destination_address, source_address,
                (unsigned long long)transaction);
    }
    return;
  }

  /* Portable replay. Four-byte scalar transfers preserve the exact byte span
   * and make commit/wait no-ops without pretending older targets are async. */
  sb_printf(&fn->body,
            "\t// mtlc.async_copy %ssynchronous-fallback bytes=%llu transaction=%llu\n",
            in->async_copy_generated ? "auto-promoted " : "",
            (unsigned long long)bytes, (unsigned long long)transaction);
  for (size_t offset = 0; offset < bytes; offset += 4) {
    char destination_address[48], source_address[48], value[24];
    ptx_async_address(destination_address, sizeof(destination_address),
                      destination_base, offset);
    ptx_async_address(source_address, sizeof(source_address), source_base,
                      offset);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tld.global.b32 %s, [%s];\n", value,
              source_address);
    sb_printf(&fn->body, "\tst.shared.b32 [%s], %s;\n",
              destination_address, value);
  }
}

static void ptx_emit_async_commit(PtxFn *fn) {
  if (ptx_async_copy_native(fn)) {
    sb_puts(&fn->body, "\tcp.async.commit_group;\n");
  } else {
    sb_puts(&fn->body, "\t// mtlc.async_copy commit synchronous-fallback\n");
  }
}

static void ptx_emit_async_wait(PtxFn *fn, const IRInstruction *in) {
  if (!fn || !in || in->async_copy_pending_groups > 7) {
    if (fn) fn_error(fn, "PTX async-copy wait has an invalid group bound");
    return;
  }
  if (ptx_async_copy_native(fn)) {
    sb_printf(&fn->body, "\tcp.async.wait_group %u;\n",
              in->async_copy_pending_groups);
  } else {
    sb_printf(&fn->body,
              "\t// mtlc.async_copy wait pending=%u synchronous-fallback\n",
              in->async_copy_pending_groups);
  }
}

static int ptx_tensor_transfer_native_capable(
    const PtxFn *fn, const MtlcTensorTransferDesc *desc,
    int has_prepared_view) {
  size_t element_bytes = ir_tensor_transfer_element_bytes(desc->element);
  size_t tile_elements = ir_tensor_transfer_tile_elements(desc);
  if (!fn || !ir_tensor_transfer_desc_valid(desc) || !has_prepared_view ||
      fn->target_arch < 90 || !ptx_version_at_least(fn, 8, 3) ||
      element_bytes == 0 || tile_elements == 0 ||
      desc->global_stride_bytes[0] != element_bytes ||
      desc->element_stride[0] != 1 ||
      ((uint64_t)desc->tile_extent[0] * element_bytes) % 16u != 0 ||
      (tile_elements * element_bytes) % 16u != 0) {
    return 0;
  }
  for (uint8_t dimension = 0; dimension < desc->rank; dimension++) {
    uint64_t traversal =
        (uint64_t)desc->tile_extent[dimension] *
        desc->element_stride[dimension];
    if (desc->global_extent[dimension] > (UINT64_C(1) << 32) ||
        desc->element_stride[dimension] > 8 || traversal > 256) {
      return 0;
    }
    if (dimension != 0) {
      uint64_t stride = desc->global_stride_bytes[dimension];
      uint64_t previous_span = desc->global_stride_bytes[dimension - 1];
      if (stride % 16u != 0 || stride >= (UINT64_C(1) << 40) ||
          desc->global_extent[dimension - 1] >
              UINT64_MAX / previous_span ||
          stride < previous_span * desc->global_extent[dimension - 1]) {
        return 0;
      }
    }
  }
  return 1;
}

static void ptx_tensor_transfer_barrier_name(const PtxFn *fn, char *buffer,
                                             size_t capacity) {
  char function_name[256], raw[512];
  sanitize_into(fn && fn->function && fn->function->name
                    ? fn->function->name
                    : "kernel",
                function_name, sizeof(function_name));
  snprintf(raw, sizeof(raw), "%s_tensor_transfer_barrier", function_name);
  sanitize_into(raw, buffer, capacity);
}

static void ptx_emit_tensor_transfer_fallback(
    PtxFn *fn, const IRInstruction *in, const char *destination,
    const char *source, size_t coordinate_base, size_t label_id) {
  const MtlcTensorTransferDesc *desc = &IR_TENSOR_TRANSFER(in);
  size_t element_bytes = ir_tensor_transfer_element_bytes(desc->element);
  size_t tile_elements = ir_tensor_transfer_tile_elements(desc);
  int r_tid_x = new_reg(fn, PC_B32), r_tid_y = new_reg(fn, PC_B32);
  int r_tid_z = new_reg(fn, PC_B32), r_ntid_x = new_reg(fn, PC_B32);
  int r_ntid_y = new_reg(fn, PC_B32), r_ntid_z = new_reg(fn, PC_B32);
  int r_linear = new_reg(fn, PC_B32), r_threads = new_reg(fn, PC_B32);
  int r_scratch = new_reg(fn, PC_B32), r_quotient = new_reg(fn, PC_B32);
  int p_done = new_reg(fn, PC_PRED), p_in_bounds = new_reg(fn, PC_PRED);
  char tid_x[24], tid_y[24], tid_z[24], ntid_x[24], ntid_y[24], ntid_z[24];
  char linear[24], threads[24], scratch[24], quotient[24];
  char done[24], in_bounds[24];
  reg_name(PC_B32, r_tid_x, tid_x);
  reg_name(PC_B32, r_tid_y, tid_y);
  reg_name(PC_B32, r_tid_z, tid_z);
  reg_name(PC_B32, r_ntid_x, ntid_x);
  reg_name(PC_B32, r_ntid_y, ntid_y);
  reg_name(PC_B32, r_ntid_z, ntid_z);
  reg_name(PC_B32, r_linear, linear);
  reg_name(PC_B32, r_threads, threads);
  reg_name(PC_B32, r_scratch, scratch);
  reg_name(PC_B32, r_quotient, quotient);
  reg_name(PC_PRED, p_done, done);
  reg_name(PC_PRED, p_in_bounds, in_bounds);

  sb_printf(&fn->body,
            "\t// mtlc.tensor_transfer cooperative-fallback rank=%u bytes=%llu\n",
            (unsigned)desc->rank,
            (unsigned long long)(tile_elements * element_bytes));
  if (desc->direction == MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL)
    sb_puts(&fn->body, "\tbar.sync 0;\n");
  sb_printf(&fn->body,
            "\tmov.u32 %s, %%tid.x;\n"
            "\tmov.u32 %s, %%tid.y;\n"
            "\tmov.u32 %s, %%tid.z;\n"
            "\tmov.u32 %s, %%ntid.x;\n"
            "\tmov.u32 %s, %%ntid.y;\n"
            "\tmov.u32 %s, %%ntid.z;\n"
            "\tmad.lo.u32 %s, %s, %s, %s;\n"
            "\tmad.lo.u32 %s, %s, %s, %s;\n"
            "\tmul.lo.u32 %s, %s, %s;\n"
            "\tmul.lo.u32 %s, %s, %s;\n",
            tid_x, tid_y, tid_z, ntid_x, ntid_y, ntid_z, scratch, tid_z,
            ntid_y, tid_y, linear, scratch, ntid_x, tid_x, threads, ntid_x,
            ntid_y, threads, threads, ntid_z);
  sb_printf(&fn->body, "mtlc_tensor_transfer_%llu_loop:\n",
            (unsigned long long)label_id);
  sb_printf(&fn->body,
            "\tsetp.ge.u32 %s, %s, %llu;\n"
            "\t@%s bra mtlc_tensor_transfer_%llu_finish;\n"
            "\tmov.u32 %s, %s;\n"
            "\tmov.pred %s, 1;\n",
            done, linear, (unsigned long long)tile_elements, done,
            (unsigned long long)label_id, quotient, linear, in_bounds);

  int r_global_offset = new_reg(fn, PC_B64);
  char global_offset[24];
  reg_name(PC_B64, r_global_offset, global_offset);
  sb_printf(&fn->body, "\tmov.u64 %s, 0;\n", global_offset);
  for (uint8_t dimension = 0; dimension < desc->rank; dimension++) {
    int r_local = new_reg(fn, PC_B32);
    int r_local_step = new_reg(fn, PC_B32);
    int r_coordinate64 = new_reg(fn, PC_B64);
    int r_local64 = new_reg(fn, PC_B64);
    int r_part = new_reg(fn, PC_B64);
    int p_nonnegative = new_reg(fn, PC_PRED);
    int p_below = new_reg(fn, PC_PRED);
    char local[24], local_step[24], coordinate64[24], local64[24], part[24];
    char nonnegative[24], below[24], coordinate[24];
    reg_name(PC_B32, r_local, local);
    reg_name(PC_B32, r_local_step, local_step);
    reg_name(PC_B64, r_coordinate64, coordinate64);
    reg_name(PC_B64, r_local64, local64);
    reg_name(PC_B64, r_part, part);
    reg_name(PC_PRED, p_nonnegative, nonnegative);
    reg_name(PC_PRED, p_below, below);
    use_as(fn, &in->arguments[coordinate_base + dimension], PC_B32,
           coordinate);
    sb_printf(&fn->body,
              "\trem.u32 %s, %s, %u;\n"
              "\tdiv.u32 %s, %s, %u;\n"
              "\tmul.lo.u32 %s, %s, %u;\n"
              "\tcvt.s64.s32 %s, %s;\n"
              "\tcvt.u64.u32 %s, %s;\n"
              "\tadd.s64 %s, %s, %s;\n"
              "\tsetp.ge.s64 %s, %s, 0;\n"
              "\tsetp.lt.u64 %s, %s, %llu;\n"
              "\tand.pred %s, %s, %s;\n"
              "\tand.pred %s, %s, %s;\n"
              "\tmul.lo.u64 %s, %s, %llu;\n"
              "\tadd.u64 %s, %s, %s;\n",
              local, quotient, (unsigned)desc->tile_extent[dimension],
              quotient, quotient, (unsigned)desc->tile_extent[dimension],
              local_step, local, (unsigned)desc->element_stride[dimension],
              coordinate64, coordinate, local64, local_step, coordinate64,
              coordinate64, local64, nonnegative, coordinate64, below,
              coordinate64,
              (unsigned long long)desc->global_extent[dimension], in_bounds,
              in_bounds, nonnegative, in_bounds, in_bounds, below, part,
              coordinate64,
              (unsigned long long)desc->global_stride_bytes[dimension],
              global_offset, global_offset, part);
  }
  int r_tile_offset = new_reg(fn, PC_B64);
  int r_destination_address = new_reg(fn, PC_B64);
  int r_source_address = new_reg(fn, PC_B64);
  PtxClass value_class = element_bytes == 8 ? PC_B64 : PC_B32;
  int r_value = new_reg(fn, value_class);
  char tile_offset[24], destination_address[24], source_address[24], value[24];
  reg_name(PC_B64, r_tile_offset, tile_offset);
  reg_name(PC_B64, r_destination_address, destination_address);
  reg_name(PC_B64, r_source_address, source_address);
  reg_name(value_class, r_value, value);
  sb_printf(&fn->body,
            "\tcvt.u64.u32 %s, %s;\n"
            "\tmul.lo.u64 %s, %s, %llu;\n",
            tile_offset, linear, tile_offset, tile_offset,
            (unsigned long long)element_bytes);
  const char *load_space;
  const char *store_space;
  const char *load_base;
  const char *store_base;
  const char *load_offset;
  const char *store_offset;
  if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
    load_space = "global";
    store_space = "shared";
    load_base = source;
    store_base = destination;
    load_offset = global_offset;
    store_offset = tile_offset;
  } else {
    load_space = "shared";
    store_space = "global";
    load_base = source;
    store_base = destination;
    load_offset = tile_offset;
    store_offset = global_offset;
  }
  sb_printf(&fn->body,
            "\tadd.u64 %s, %s, %s;\n"
            "\tadd.u64 %s, %s, %s;\n",
            source_address, load_base, load_offset, destination_address,
            store_base, store_offset);
  const char *load_suffix = element_bytes == 1   ? "u8"
                            : element_bytes == 2 ? "u16"
                            : element_bytes == 4 ? "b32"
                                                 : "b64";
  const char *store_suffix = element_bytes == 1   ? "u8"
                             : element_bytes == 2 ? "u16"
                             : element_bytes == 4 ? "b32"
                                                  : "b64";
  if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
    sb_printf(&fn->body, "\tmov.%s %s, 0;\n", element_bytes == 8 ? "b64" : "b32",
              value);
    sb_printf(&fn->body, "\t@%s ld.%s.%s %s, [%s];\n", in_bounds,
              load_space, load_suffix, value, source_address);
    sb_printf(&fn->body, "\tst.%s.%s [%s], %s;\n", store_space,
              store_suffix, destination_address, value);
  } else {
    sb_printf(&fn->body, "\tld.%s.%s %s, [%s];\n", load_space,
              load_suffix, value, source_address);
    sb_printf(&fn->body, "\t@%s st.%s.%s [%s], %s;\n", in_bounds,
              store_space, store_suffix, destination_address, value);
  }
  sb_printf(&fn->body,
            "\tadd.u32 %s, %s, %s;\n"
            "\tbra mtlc_tensor_transfer_%llu_loop;\n"
            "mtlc_tensor_transfer_%llu_finish:\n"
            "\tbar.sync 0;\n",
            linear, linear, threads, (unsigned long long)label_id,
            (unsigned long long)label_id);
}

static void ptx_emit_tensor_transfer(PtxFn *fn, const IRInstruction *in) {
  if (!fn || !in || in->op != IR_OP_TENSOR_TRANSFER ||
      !ir_tensor_transfer_desc_valid(&IR_TENSOR_TRANSFER(in))) {
    if (fn) fn_error(fn, "PTX received an invalid tensor-transfer instruction");
    return;
  }
  const MtlcTensorTransferDesc *desc = &IR_TENSOR_TRANSFER(in);
  int has_view = in->tensor_transfer_has_prepared_view;
  size_t expected = ir_tensor_transfer_operand_count(desc, has_view);
  if (!expected || in->argument_count != expected) {
    fn_error(fn, "PTX tensor transfer has an invalid operand count");
    return;
  }
  PtxVal destination_desc = operand_desc(fn, &in->arguments[0]);
  PtxVal source_desc = operand_desc(fn, &in->arguments[1]);
  MtlcAddressSpace destination_space = destination_desc.address_space;
  MtlcAddressSpace source_space = source_desc.address_space;
  if (!destination_desc.is_ptr || !source_desc.is_ptr ||
      destination_desc.elem != source_desc.elem ||
      (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP &&
       (destination_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
        (source_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         source_space != MTLC_ADDRESS_SPACE_GENERIC))) ||
      (desc->direction == MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL &&
       ((destination_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         destination_space != MTLC_ADDRESS_SPACE_GENERIC) ||
        source_space != MTLC_ADDRESS_SPACE_WORKGROUP))) {
    fn_error(fn, "PTX tensor transfer has invalid pointer address spaces");
    return;
  }
  char destination[24], source[24];
  use_as(fn, &in->arguments[0], PC_B64, destination);
  use_as(fn, &in->arguments[1], PC_B64, source);
  size_t view_index = 2;
  size_t coordinate_base = 2u + (has_view ? 1u : 0u);
  size_t label_id = fn->call_count++;
  int native = ptx_tensor_transfer_native_capable(fn, desc, has_view);
  char map[24] = {0};
  int p_no_view = -1;
  char no_view[24] = {0};
  if (native) {
    PtxVal view_desc = operand_desc(fn, &in->arguments[view_index]);
    if (!view_desc.is_ptr ||
        (view_desc.address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         view_desc.address_space != MTLC_ADDRESS_SPACE_GENERIC)) {
      fn_error(fn, "PTX tensor transfer prepared view is not a global pointer");
      return;
    }
    use_as(fn, &in->arguments[view_index], PC_B64, map);
    p_no_view = new_reg(fn, PC_PRED);
    reg_name(PC_PRED, p_no_view, no_view);
    sb_printf(&fn->body, "\tsetp.eq.u64 %s, %s, 0;\n", no_view, map);
    sb_printf(&fn->body, "\t@%s bra mtlc_tensor_transfer_%llu_fallback;\n",
              no_view, (unsigned long long)label_id);

    /* CUtensorMap is a 128-byte opaque value with 64-byte alignment.  Avoid
     * issuing even the tensor-map acquire fence for a detectably malformed
     * provider handle; the raw geometry remains sufficient for replay. */
    int r_map_misalignment = new_reg(fn, PC_B64);
    int p_map_unaligned = new_reg(fn, PC_PRED);
    char map_misalignment[24], map_unaligned[24];
    reg_name(PC_B64, r_map_misalignment, map_misalignment);
    reg_name(PC_PRED, p_map_unaligned, map_unaligned);
    sb_printf(&fn->body,
              "\tand.b64 %s, %s, 63;\n"
              "\tsetp.ne.u64 %s, %s, 0;\n"
              "\t@%s bra mtlc_tensor_transfer_%llu_fallback;\n",
              map_misalignment, map, map_unaligned, map_misalignment,
              map_unaligned, (unsigned long long)label_id);

    /* TMA requires a 16-byte-aligned shared-memory address.  The neutral IR
     * intentionally permits arbitrary workgroup pointer expressions, so keep
     * alignment as a dynamic native-path precondition and replay otherwise. */
    const char *shared_address =
        desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP
            ? destination
            : source;
    int r_shared_misalignment = new_reg(fn, PC_B64);
    int p_shared_unaligned = new_reg(fn, PC_PRED);
    char shared_misalignment[24], shared_unaligned[24];
    reg_name(PC_B64, r_shared_misalignment, shared_misalignment);
    reg_name(PC_PRED, p_shared_unaligned, shared_unaligned);
    sb_printf(&fn->body,
              "\tand.b64 %s, %s, 15;\n"
              "\tsetp.ne.u64 %s, %s, 0;\n"
              "\t@%s bra mtlc_tensor_transfer_%llu_fallback;\n",
              shared_misalignment, shared_address, shared_unaligned,
              shared_misalignment, shared_unaligned,
              (unsigned long long)label_id);

    int r_tid_x = new_reg(fn, PC_B32), r_tid_y = new_reg(fn, PC_B32);
    int r_tid_z = new_reg(fn, PC_B32), r_election = new_reg(fn, PC_B32);
    int p_elected = new_reg(fn, PC_PRED);
    char tid_x[24], tid_y[24], tid_z[24], election[24], elected[24];
    reg_name(PC_B32, r_tid_x, tid_x);
    reg_name(PC_B32, r_tid_y, tid_y);
    reg_name(PC_B32, r_tid_z, tid_z);
    reg_name(PC_B32, r_election, election);
    reg_name(PC_PRED, p_elected, elected);
    sb_printf(&fn->body,
              "\tmov.u32 %s, %%tid.x;\n"
              "\tmov.u32 %s, %%tid.y;\n"
              "\tmov.u32 %s, %%tid.z;\n"
              "\tor.b32 %s, %s, %s;\n"
              "\tor.b32 %s, %s, %s;\n"
              "\tsetp.eq.u32 %s, %s, 0;\n",
              tid_x, tid_y, tid_z, election, tid_x, tid_y, election, election,
              tid_z, elected, election);
    char coordinates[MTLC_TENSOR_MAX_RANK][24];
    for (uint8_t dimension = 0; dimension < desc->rank; dimension++)
      use_as(fn, &in->arguments[coordinate_base + dimension], PC_B32,
             coordinates[dimension]);
    char coordinate_text[192] = {0};
    size_t used = 0;
    for (uint8_t dimension = 0; dimension < desc->rank; dimension++) {
      int wrote = snprintf(coordinate_text + used, sizeof(coordinate_text) - used,
                           "%s%s", dimension ? ", " : "",
                           coordinates[dimension]);
      if (wrote < 0 || (size_t)wrote >= sizeof(coordinate_text) - used) {
        fn_error(fn, "PTX tensor transfer coordinate list overflowed");
        return;
      }
      used += (size_t)wrote;
    }
    sb_printf(&fn->body,
              "\t// mtlc.tensor_transfer native-tma rank=%u bytes=%llu\n",
              (unsigned)desc->rank,
              (unsigned long long)(ir_tensor_transfer_tile_elements(desc) *
                                   ir_tensor_transfer_element_bytes(
                                       desc->element)));
    sb_printf(&fn->body,
              "\t@%s fence.proxy.tensormap::generic.acquire.sys [%s], 128;\n",
              elected, map);
    if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
      char barrier_name[512], barrier[24], state[24], wait[24];
      ptx_tensor_transfer_barrier_name(fn, barrier_name, sizeof(barrier_name));
      int r_barrier = new_reg(fn, PC_B32);
      int r_state = new_reg(fn, PC_B64);
      int p_wait = new_reg(fn, PC_PRED);
      reg_name(PC_B32, r_barrier, barrier);
      reg_name(PC_B64, r_state, state);
      reg_name(PC_PRED, p_wait, wait);
      sb_printf(&fn->body,
                "\tmov.u32 %s, %s;\n"
                "\t@%s mbarrier.init.shared::cta.b64 [%s], 1;\n"
                /* mbarrier.init is a generic-proxy write.  TMA accesses the
                 * barrier through the async proxy, so NVIDIA requires this
                 * fence before the bulk tensor request can legally observe
                 * the initialized object. */
                "\t@%s fence.proxy.async.shared::cta;\n"
                "\tbar.sync 0;\n"
                "\t@%s cp.async.bulk.tensor.%ud.shared::cta.global.tile.mbarrier::complete_tx::bytes [%s], [%s, {%s}], [%s];\n"
                "\t@%s mbarrier.arrive.expect_tx.release.cta.shared::cta.b64 %s, [%s], %llu;\n"
                "mtlc_tensor_transfer_%llu_wait:\n"
                "\tmbarrier.try_wait.parity.acquire.cta.shared::cta.b64 %s, [%s], 0;\n"
                "\t@!%s bra mtlc_tensor_transfer_%llu_wait;\n"
                "\tbar.sync 0;\n"
                "\t@%s mbarrier.inval.shared::cta.b64 [%s];\n",
                barrier, barrier_name, elected, barrier, elected, elected,
                (unsigned)desc->rank, destination, map, coordinate_text,
                barrier, elected, state, barrier,
                (unsigned long long)(ir_tensor_transfer_tile_elements(desc) *
                                     ir_tensor_transfer_element_bytes(
                                         desc->element)),
                (unsigned long long)label_id, wait, barrier, wait,
                (unsigned long long)label_id, elected, barrier);
    } else {
      sb_printf(&fn->body,
                /* Each producer must order its own shared-memory writes into
                 * the async proxy before the workgroup rendezvous.  A fence
                 * issued only by the elected TMA thread is not transitive. */
                "\tfence.proxy.async.shared::cta;\n"
                "\tbar.sync 0;\n"
                "\t@%s cp.async.bulk.tensor.%ud.global.shared::cta.tile.bulk_group [%s, {%s}], [%s];\n"
                "\t@%s cp.async.bulk.commit_group;\n"
                "\t@%s cp.async.bulk.wait_group 0;\n"
                "\tbar.sync 0;\n",
                elected, (unsigned)desc->rank, map, coordinate_text, source,
                elected, elected);
    }
    sb_printf(&fn->body, "\tbra mtlc_tensor_transfer_%llu_done;\n",
              (unsigned long long)label_id);
    sb_printf(&fn->body, "mtlc_tensor_transfer_%llu_fallback:\n",
              (unsigned long long)label_id);
  }
  ptx_emit_tensor_transfer_fallback(fn, in, destination, source,
                                    coordinate_base, label_id);
  if (native)
    sb_printf(&fn->body, "mtlc_tensor_transfer_%llu_done:\n",
              (unsigned long long)label_id);
}

static const char *ptx_tensor_layout(MtlcTensorLayout layout) {
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) return "row";
  if (layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) return "col";
  return NULL;
}

static int ptx_tensor_shape_is(const MtlcTensorMmaDesc *desc,
                               unsigned m, unsigned n, unsigned k) {
  return desc->m == m && desc->n == n && desc->k == k;
}

static void ptx_wmma_set_shape(const MtlcTensorMmaDesc *desc,
                               PtxWmmaProfile *profile,
                               const char *shape, unsigned m,
                               unsigned n, unsigned k) {
  profile->shape = shape;
  profile->tile_m = m;
  profile->tile_n = n;
  profile->tile_k = k;
  profile->m_tiles = desc->m / m;
  profile->n_tiles = desc->n / n;
}

static int ptx_wmma_shape_or_grid(const MtlcTensorMmaDesc *desc,
                                  PtxWmmaProfile *profile,
                                  const char *shape, unsigned m,
                                  unsigned n, unsigned k) {
  if (!desc || !profile || desc->k != k || desc->m == 0 || desc->n == 0 ||
      desc->m % m != 0 || desc->n % n != 0 ||
      desc->m > 256 || desc->n > 256)
    return 0;
  ptx_wmma_set_shape(desc, profile, shape, m, n, k);
  return 1;
}

static int ptx_wmma_shape_16_family(const MtlcTensorMmaDesc *desc,
                                     PtxWmmaProfile *profile) {
  if (ptx_tensor_shape_is(desc, 16, 16, 16)) {
    ptx_wmma_set_shape(desc, profile, "m16n16k16", 16, 16, 16);
    return 1;
  }
  if (ptx_tensor_shape_is(desc, 8, 32, 16)) {
    ptx_wmma_set_shape(desc, profile, "m8n32k16", 8, 32, 16);
    return 1;
  }
  if (ptx_tensor_shape_is(desc, 32, 8, 16)) {
    ptx_wmma_set_shape(desc, profile, "m32n8k16", 32, 8, 16);
    return 1;
  }
  return ptx_wmma_shape_or_grid(desc, profile, "m16n16k16",
                                16, 16, 16);
}

static int ptx_tensor_uses_narrow_float(const MtlcTensorMmaDesc *desc) {
  if (!desc) return 0;
  return (desc->a_element >= MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 &&
          desc->a_element <= MTLC_TENSOR_ELEMENT_FLOAT4_E2M1) ||
         (desc->b_element >= MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 &&
          desc->b_element <= MTLC_TENSOR_ELEMENT_FLOAT4_E2M1);
}

static int ptx_tensor_uses_direct_mma(const MtlcTensorMmaDesc *desc) {
  return desc &&
         (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ||
          ptx_tensor_uses_narrow_float(desc));
}

static const char *ptx_mma_fp8_type(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
    return "e4m3";
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    return "e5m2";
  default:
    return NULL;
  }
}

/* PTX's block-scaled mxf8f6f4 family admits FP8, FP6, and FP4 operands in
 * any documented A/B combination. The neutral descriptor carries the
 * semantic element kind and packing; this helper is deliberately confined to
 * the PTX backend's instruction spelling and register-container width. */
static const char *ptx_mma_mxf8f6f4_type(MtlcTensorElement element,
                                         int *bits) {
  if (bits) *bits = 0;
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
    if (bits) *bits = 8;
    return "e4m3";
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    if (bits) *bits = 8;
    return "e5m2";
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
    if (bits) *bits = 6;
    return "e2m3";
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
    if (bits) *bits = 6;
    return "e3m2";
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
    if (bits) *bits = 4;
    return "e2m1";
  default:
    return NULL;
  }
}

static int ptx_select_mma_profile(PtxFn *fn,
                                  const MtlcTensorMmaDesc *desc,
                                  PtxMmaProfile *profile,
                                  char *reason, size_t reason_size) {
  memset(profile, 0, sizeof(*profile));
#define PTX_MMA_REJECT(...)                                                    \
  do {                                                                         \
    snprintf(reason, reason_size, __VA_ARGS__);                                \
    return 0;                                                                  \
  } while (0)
  if (!fn || !ir_tensor_mma_desc_valid(desc))
    PTX_MMA_REJECT("invalid target-neutral tensor descriptor");
  if (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP)
    PTX_MMA_REJECT("warp-level MMA requires subgroup scope");
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) {
    if (desc->sparsity != MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4)
      PTX_MMA_REJECT("this PTX sparse MMA profile requires canonical structured 2:4 A");
    if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD)
      PTX_MMA_REJECT("structured-sparse MMA requires multiply-add math");
    if (desc->a_element != desc->b_element ||
        (desc->a_element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
         desc->a_element != MTLC_TENSOR_ELEMENT_BFLOAT16))
      PTX_MMA_REJECT("this PTX sparse MMA profile requires matching f16 or bf16 inputs");
    if (desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
        desc->result_element != MTLC_TENSOR_ELEMENT_FLOAT32)
      PTX_MMA_REJECT("this PTX sparse MMA profile requires f32 accumulation and result");
    if (desc->m % 16 != 0 || desc->n % 8 != 0 || desc->k != 16)
      PTX_MMA_REJECT("f16/bf16 structured-sparse tiles require M divisible by 16, N divisible by 8, and K=16");
    if (desc->m > 256 || desc->n > 256)
      PTX_MMA_REJECT("one structured-sparse collective tile is limited to at most 256x256");
    if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT ||
        desc->overflow != MTLC_TENSOR_OVERFLOW_WRAP)
      PTX_MMA_REJECT("structured-sparse f16/bf16 MMA does not preserve explicit rounding or saturation");
    if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
        desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE)
      PTX_MMA_REJECT("this structured-sparse profile is unscaled");
    if (desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL ||
        desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL)
      PTX_MMA_REJECT("structured-sparse f16/bf16 storage uses logical element packing");
    if (fn->target_arch < 80 || !ptx_version_at_least(fn, 7, 1))
      PTX_MMA_REJECT("structured-sparse mma.sp requires PTX 7.1 and sm_80 or newer");

    profile->kind = desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16
                        ? PTX_MMA_SPARSE_F16
                        : PTX_MMA_SPARSE_BF16;
    profile->shape = "m16n8k16";
    profile->a_type = profile->b_type =
        desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16";
    profile->c_type = profile->d_type = "f32";
    profile->a_bits = profile->b_bits = 16;
    profile->a_registers = profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }
  if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD)
    PTX_MMA_REJECT("narrow floating-point MMA requires multiply-add math");
  if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT)
    PTX_MMA_REJECT("FP8 MMA does not preserve an explicit rounding request");
  if (desc->overflow != MTLC_TENSOR_OVERFLOW_WRAP)
    PTX_MMA_REJECT("FP8 MMA does not provide saturating accumulation");
  if (desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
      desc->result_element != MTLC_TENSOR_ELEMENT_FLOAT32)
    PTX_MMA_REJECT("the current narrow-float native path requires f32 accumulation and result");
  if (desc->m % 16 != 0 || desc->n % 8 != 0)
    PTX_MMA_REJECT("native narrow-float tiles require M divisible by 16 and N divisible by 8");
  if (desc->m > 256 || desc->n > 256)
    PTX_MMA_REJECT("one native narrow-float collective tile is limited to at most 256x256");

  int mxf_a_bits = 0, mxf_b_bits = 0;
  const char *mxf_a_type =
      ptx_mma_mxf8f6f4_type(desc->a_element, &mxf_a_bits);
  const char *mxf_b_type =
      ptx_mma_mxf8f6f4_type(desc->b_element, &mxf_b_bits);
  int requests_mxf8f6f4 =
      desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
      desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
      desc->a_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
      desc->b_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
      (desc->k == 32 || mxf_a_bits == 6 || mxf_b_bits == 6);
  if (requests_mxf8f6f4) {
    if (!mxf_a_type || !mxf_b_type)
      PTX_MMA_REJECT("block-scaled mxf8f6f4 requires FP8, FP6, or FP4 inputs");
    if (desc->k != 32)
      PTX_MMA_REJECT("block-scaled mxf8f6f4 native tiles require K=32");
    if ((mxf_a_bits == 8 &&
         desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL) ||
        (mxf_b_bits == 8 &&
         desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL))
      PTX_MMA_REJECT("FP8 operands use one logical byte per element");
    if ((mxf_a_bits < 8 &&
         desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL &&
         desc->a_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE) ||
        (mxf_b_bits < 8 &&
         desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL &&
         desc->b_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE))
      PTX_MMA_REJECT("FP6/FP4 operands require logical or dense-subbyte packing");
    if (fn->target_arch < 120 ||
        (fn->target_variant != 'a' && fn->target_variant != 'f') ||
        !ptx_version_at_least(fn, 8, 8))
      PTX_MMA_REJECT("block-scaled mxf8f6f4 mma.sync requires PTX 8.8 and an architecture- or family-specific sm_120a/sm_121a target");

    profile->kind = PTX_MMA_MXF8F6F4;
    profile->shape = "m16n8k32";
    profile->a_type = mxf_a_type;
    profile->b_type = mxf_b_type;
    profile->c_type = profile->d_type = "f32";
    profile->scale_type = "ue8m0";
    profile->scale_vectors = 1;
    profile->a_bits = mxf_a_bits;
    profile->b_bits = mxf_b_bits;
    profile->a_registers = 4;
    profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }

  if (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT4_E2M1 &&
      desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT4_E2M1) {
    if (desc->k != 64)
      PTX_MMA_REJECT("MXFP4 native tiles require K=64");
    if (desc->a_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE ||
        desc->b_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE)
      PTX_MMA_REJECT("MXFP4 requires densely packed E2M1 operands");
    int is_mxfp4 =
        desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
        desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
        desc->a_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
        desc->b_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
    int is_nvfp4 =
        desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_16 &&
        desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_16 &&
        desc->a_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE4M3 &&
        desc->b_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE4M3;
    if (!is_mxfp4 && !is_nvfp4)
      PTX_MMA_REJECT("native FP4 requires matched UE8M0 block32 (MXFP4) or UE4M3 block16 (NVFP4) scales");
    if (fn->target_arch < 120 ||
        (fn->target_variant != 'a' && fn->target_variant != 'f') ||
        !ptx_version_at_least(fn, 8, 8))
      PTX_MMA_REJECT("native FP4 block-scale mma.sync requires PTX 8.8 and an architecture- or family-specific sm_120a/sm_121a target");

    profile->kind = is_nvfp4 ? PTX_MMA_NVFP4 : PTX_MMA_MXFP4;
    profile->shape = "m16n8k64";
    profile->a_type = profile->b_type = "e2m1";
    profile->c_type = profile->d_type = "f32";
    profile->scale_type = is_nvfp4 ? "ue4m3" : "ue8m0";
    profile->scale_vectors = is_nvfp4 ? 4 : 2;
    profile->a_bits = profile->b_bits = 4;
    profile->a_registers = 4;
    profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }

  if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
      desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE)
    PTX_MMA_REJECT("this native narrow-float profile does not support the requested scaling mode");
  if (!ptx_mma_fp8_type(desc->a_element) ||
      !ptx_mma_fp8_type(desc->b_element))
    PTX_MMA_REJECT("this native MMA profile requires FP8 e4m3/e5m2 or packed MXFP4 inputs");
  if (desc->k != 16 && desc->k != 32)
    PTX_MMA_REJECT("FP8 native tiles require K=16 or K=32");
  if (fn->target_arch < 89 || !ptx_version_at_least(fn, 8, 4))
    PTX_MMA_REJECT("FP8 mma.sync requires PTX 8.4 and sm_89 or newer");

  profile->kind = PTX_MMA_FP8;
  profile->shape = desc->k == 16 ? "m16n8k16" : "m16n8k32";
  profile->a_type = ptx_mma_fp8_type(desc->a_element);
  profile->b_type = ptx_mma_fp8_type(desc->b_element);
  profile->c_type = profile->d_type = "f32";
  profile->a_bits = profile->b_bits = 8;
  profile->a_registers = desc->k == 16 ? 2 : 4;
  profile->b_registers = desc->k == 16 ? 1 : 2;
  profile->accumulator_registers = 4;
  profile->m_tiles = desc->m / 16;
  profile->n_tiles = desc->n / 8;
  return 1;
#undef PTX_MMA_REJECT
}

static int ptx_select_wmma_profile(PtxFn *fn,
                                   const MtlcTensorMmaDesc *desc,
                                   PtxWmmaProfile *profile,
                                   char *reason, size_t reason_size) {
  memset(profile, 0, sizeof(*profile));
#define PTX_WMMA_REJECT(...)                                                   \
  do {                                                                         \
    snprintf(reason, reason_size, __VA_ARGS__);                                \
    return 0;                                                                  \
  } while (0)
  if (!ir_tensor_mma_desc_valid(desc)) {
    PTX_WMMA_REJECT("invalid target-neutral tensor descriptor");
  }
  if (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP) {
    PTX_WMMA_REJECT("WMMA requires subgroup scope");
  }
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) {
    PTX_WMMA_REJECT("structured sparsity requires the PTX mma/tcgen path");
  }
  if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
      desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE) {
    PTX_WMMA_REJECT("scaled tensor formats require the PTX mma/tcgen path");
  }
  if (desc->transpose_a || desc->transpose_b) {
    PTX_WMMA_REJECT("explicit transpose requires a tiled transform or mma path");
  }
  if (!ptx_tensor_layout(desc->a_layout) ||
      !ptx_tensor_layout(desc->b_layout) ||
      !ptx_tensor_layout(desc->c_layout) ||
      !ptx_tensor_layout(desc->d_layout)) {
    PTX_WMMA_REJECT("invalid tensor layout");
  }

  profile->a_class = PC_B32;
  profile->b_class = PC_B32;
  profile->c_class = PC_B32;
  profile->d_class = PC_B32;
  if (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 &&
      desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT16 &&
      (desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32) &&
      (desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32) &&
      desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
      ptx_wmma_shape_16_family(desc, profile)) {
    profile->kind = PTX_WMMA_F16;
    profile->a_type = profile->b_type = "f16";
    profile->c_type = desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16
                          ? "f16"
                          : "f32";
    profile->d_type = desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT16
                          ? "f16"
                          : "f32";
    profile->a_registers = profile->b_registers = 8;
    profile->c_registers =
        desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? 4 : 8;
    profile->d_registers =
        desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? 4 : 8;
    profile->c_class = desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32
                           ? PC_F32
                           : PC_B32;
    profile->d_class = desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32
                           ? PC_F32
                           : PC_B32;
    profile->min_arch = 70;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor =
        profile->tile_m == 16 && profile->tile_n == 16 ? 0 : 1;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_BFLOAT16 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_16_family(desc, profile)) {
    profile->kind = PTX_WMMA_BF16;
    profile->a_type = profile->b_type = "bf16";
    profile->c_type = profile->d_type = "f32";
    profile->c_class = profile->d_class = PC_F32;
    if (profile->tile_m == 16 && profile->tile_n == 16) {
      profile->a_registers = profile->b_registers = 4;
    } else if (profile->tile_m == 8 && profile->tile_n == 32) {
      profile->a_registers = 2;
      profile->b_registers = 8;
    } else {
      profile->a_registers = 8;
      profile->b_registers = 2;
    }
    profile->c_registers = profile->d_registers = 8;
    profile->min_arch = 80;
    profile->min_ptx_major = 7;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_TFLOAT32 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_TFLOAT32 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_or_grid(desc, profile, "m16n16k8",
                                    16, 16, 8)) {
    profile->kind = PTX_WMMA_TF32;
    profile->a_type = profile->b_type = "tf32";
    profile->c_type = profile->d_type = "f32";
    profile->a_registers = profile->b_registers = 4;
    profile->c_registers = profile->d_registers = 8;
    profile->c_class = profile->d_class = PC_F32;
    profile->min_arch = 80;
    profile->min_ptx_major = 7;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_or_grid(desc, profile, "m8n8k4", 8, 8, 4)) {
    profile->kind = PTX_WMMA_F64;
    profile->a_type = profile->b_type = profile->c_type = profile->d_type =
        "f64";
    profile->a_class = profile->b_class = profile->c_class = profile->d_class =
        PC_F64;
    profile->a_registers = profile->b_registers = 1;
    profile->c_registers = profile->d_registers = 2;
    profile->min_arch = 80;
    profile->min_ptx_major = 7;
  } else if ((desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ||
              desc->a_element == MTLC_TENSOR_ELEMENT_UINT8) &&
             desc->b_element == desc->a_element &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_16_family(desc, profile)) {
    profile->kind = PTX_WMMA_I8;
    profile->a_type = profile->b_type =
        desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ? "s8" : "u8";
    profile->c_type = profile->d_type = "s32";
    if (profile->tile_m == 16 && profile->tile_n == 16) {
      profile->a_registers = profile->b_registers = 2;
    } else if (profile->tile_m == 8 && profile->tile_n == 32) {
      profile->a_registers = 1;
      profile->b_registers = 4;
    } else {
      profile->a_registers = 4;
      profile->b_registers = 1;
    }
    profile->c_registers = profile->d_registers = 8;
    profile->min_arch = 72;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor = 3;
  } else if ((desc->a_element == MTLC_TENSOR_ELEMENT_INT4 ||
              desc->a_element == MTLC_TENSOR_ELEMENT_UINT4) &&
             desc->b_element == desc->a_element &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_tensor_shape_is(desc, 8, 8, 32) &&
             desc->a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
             desc->b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) {
    profile->kind = PTX_WMMA_I4;
    ptx_wmma_set_shape(desc, profile, "m8n8k32", 8, 8, 32);
    profile->a_type = profile->b_type =
        desc->a_element == MTLC_TENSOR_ELEMENT_INT4 ? "s4" : "u4";
    profile->c_type = profile->d_type = "s32";
    profile->a_registers = profile->b_registers = 1;
    profile->c_registers = profile->d_registers = 2;
    profile->min_arch = 75;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor = 3;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_BIT1 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_BIT1 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_INT32 &&
             (desc->math_mode == MTLC_TENSOR_MATH_XOR_POPCOUNT ||
              desc->math_mode == MTLC_TENSOR_MATH_AND_POPCOUNT) &&
             ptx_tensor_shape_is(desc, 8, 8, 128) &&
             desc->a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
             desc->b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) {
    profile->kind = PTX_WMMA_B1;
    ptx_wmma_set_shape(desc, profile, "m8n8k128", 8, 8, 128);
    profile->a_type = profile->b_type = "b1";
    profile->c_type = profile->d_type = "s32";
    profile->a_registers = profile->b_registers = 1;
    profile->c_registers = profile->d_registers = 2;
    profile->min_arch = 75;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor = 3;
  } else {
    PTX_WMMA_REJECT(
        "profile is not a stable PTX WMMA combination; it requires mma/tcgen lowering");
  }
  if (desc->overflow == MTLC_TENSOR_OVERFLOW_SATURATE_FINITE &&
      profile->kind != PTX_WMMA_I8 && profile->kind != PTX_WMMA_I4) {
    PTX_WMMA_REJECT("finite saturation is supported only for integer WMMA");
  }
  if (profile->kind != PTX_WMMA_F64 &&
      desc->rounding != MTLC_TENSOR_ROUND_DEFAULT) {
    PTX_WMMA_REJECT("explicit rounding is supported only for f64 WMMA");
  }
  if (profile->kind == PTX_WMMA_F64 &&
      desc->rounding != MTLC_TENSOR_ROUND_DEFAULT &&
      desc->rounding != MTLC_TENSOR_ROUND_NEAREST_EVEN) {
    PTX_WMMA_REJECT("this PTX WMMA lowering currently supports default/RN f64 rounding");
  }
  if (fn->target_arch < profile->min_arch ||
      !ptx_version_at_least(fn, profile->min_ptx_major,
                            profile->min_ptx_minor)) {
    PTX_WMMA_REJECT("profile requires PTX %d.%d and sm_%d or newer",
                    profile->min_ptx_major, profile->min_ptx_minor,
                    profile->min_arch);
  }
  return 1;
#undef PTX_WMMA_REJECT
}

static void ptx_reg_tuple(PtxFn *fn, PtxClass cls, int count,
                          char *buffer, size_t buffer_size) {
  size_t offset = 0;
  if (!buffer || buffer_size == 0) return;
  offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "{");
  for (int i = 0; i < count && offset < buffer_size; i++) {
    char reg[24];
    reg_name(cls, new_reg(fn, cls), reg);
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset,
                               "%s%s", i ? ", " : "", reg);
  }
  if (offset < buffer_size) snprintf(buffer + offset, buffer_size - offset, "}");
}

static void ptx_reg_tuple_at(PtxClass cls, int base, int count,
                             char *buffer, size_t buffer_size) {
  size_t offset = 0;
  if (!buffer || buffer_size == 0) return;
  offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "{");
  for (int i = 0; i < count && offset < buffer_size; i++) {
    char reg[24];
    reg_name(cls, base + i, reg);
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset,
                              "%s%s", i ? ", " : "", reg);
  }
  if (offset < buffer_size) snprintf(buffer + offset, buffer_size - offset, "}");
}

static PtxTensorResidency *ptx_tensor_residency_find(PtxFn *fn,
                                                      uint32_t id) {
  if (!fn || id == 0) return NULL;
  for (size_t i = 0; i < fn->tensor_residency_count; i++) {
    if (fn->tensor_residencies[i].id == id)
      return &fn->tensor_residencies[i];
  }
  return NULL;
}

static PtxTensorResidency *ptx_tensor_residency_add(PtxFn *fn,
                                                     uint32_t id) {
  if (!fn || id == 0 || ptx_tensor_residency_find(fn, id)) return NULL;
  if (fn->tensor_residency_count == fn->tensor_residency_capacity) {
    size_t capacity = fn->tensor_residency_capacity
                          ? fn->tensor_residency_capacity * 2
                          : 4;
    PtxTensorResidency *grown = realloc(
        fn->tensor_residencies, capacity * sizeof(*grown));
    if (!grown) return NULL;
    fn->tensor_residencies = grown;
    fn->tensor_residency_capacity = capacity;
  }
  PtxTensorResidency *group =
      &fn->tensor_residencies[fn->tensor_residency_count++];
  memset(group, 0, sizeof(*group));
  group->id = id;
  return group;
}

static const char *ptx_tensor_residency_name(
    IRTensorResidencyScope scope) {
  switch (scope) {
  case IR_TENSOR_RESIDENCY_SCOPE_LOOP:
    return "tensor_loop";
  case IR_TENSOR_RESIDENCY_SCOPE_PIPELINE:
    return "tensor_pipeline";
  default:
    return NULL;
  }
}

static const char *ptx_wmma_space(PtxVal pointer) {
  switch (pointer.address_space) {
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
    return "";
  case MTLC_ADDRESS_SPACE_GLOBAL:
    return ".global";
  case MTLC_ADDRESS_SPACE_WORKGROUP:
    return ".shared";
  default:
    return NULL;
  }
}

typedef struct {
  unsigned group_coefficient;
  unsigned thread_coefficient;
  unsigned constant;
} PtxMmaCoordinate;

typedef struct {
  char bases[6][24];
  const char *spaces[6];
  char strides[4][24];
  char scale_strides[2][24];
  char metadata_stride[24];
  int dense_contiguous[2];
} PtxMmaTileMemory;

/* PTX's sparse A fragment and metadata selector depend on M and K, but not N.
 * Keep them live while walking adjacent N subtiles so a wider logical tile
 * does not reload and re-encode identical sparse data for every m16n8 MMA. */
typedef struct {
  int a_base;
  int metadata_register;
} PtxMmaSparseAFragment;

static int ptx_tensor_stride_registers(PtxFn *fn, const IRInstruction *in,
                                       size_t base, size_t per_tile,
                                       char registers[4][24]);

static void ptx_mma_emit_coordinate(PtxFn *fn, PtxMmaCoordinate coordinate,
                                    const char *group, const char *thread,
                                    char result[24]) {
  reg_name(PC_B32, new_reg(fn, PC_B32), result);
  sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", result, coordinate.constant);
  if (coordinate.group_coefficient == 1) {
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", result, result, group);
  } else if (coordinate.group_coefficient > 1) {
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, %u, %s;\n", result, group,
              coordinate.group_coefficient, result);
  }
  if (coordinate.thread_coefficient == 1) {
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", result, result, thread);
  } else if (coordinate.thread_coefficient > 1) {
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, %u, %s;\n", result, thread,
              coordinate.thread_coefficient, result);
  }
}

static void ptx_mma_emit_address(PtxFn *fn, const char *base,
                                 const char *leading_dimension,
                                 MtlcTensorLayout layout, int transpose,
                                 PtxMmaCoordinate logical_row,
                                 PtxMmaCoordinate logical_column,
                                 unsigned element_bytes, const char *group,
                                 const char *thread, char address[24]) {
  PtxMmaCoordinate storage_row = transpose ? logical_column : logical_row;
  PtxMmaCoordinate storage_column = transpose ? logical_row : logical_column;
  char row[24], column[24], linear[24], byte_offset[24];
  ptx_mma_emit_coordinate(fn, storage_row, group, thread, row);
  ptx_mma_emit_coordinate(fn, storage_column, group, thread, column);
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, row,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              column);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, column,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear, row);
  }
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", byte_offset,
            linear, element_bytes);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
}

/* Address one element in a densely nibble-packed logical matrix. Descriptor
 * strides remain logical element counts; the division by two is exclusively a
 * backend storage operation and therefore cannot leak a PTX fragment layout
 * into shared IR. linear_out is retained for selecting the low/high nibble. */
static void ptx_mma_emit_nibble_address(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, PtxMmaCoordinate logical_row,
    PtxMmaCoordinate logical_column, const char *group, const char *thread,
    char address[24], char linear_out[24]) {
  PtxMmaCoordinate storage_row = transpose ? logical_column : logical_row;
  PtxMmaCoordinate storage_column = transpose ? logical_row : logical_column;
  char row[24], column[24], byte_index[24], byte_offset[24];
  ptx_mma_emit_coordinate(fn, storage_row, group, thread, row);
  ptx_mma_emit_coordinate(fn, storage_column, group, thread, column);
  reg_name(PC_B32, new_reg(fn, PC_B32), linear_out);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear_out, row,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear_out,
              linear_out, column);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear_out, column,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear_out,
              linear_out, row);
  }
  reg_name(PC_B32, new_reg(fn, PC_B32), byte_index);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 1;\n", byte_index, linear_out);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", byte_offset, byte_index);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
}

static void ptx_mma_load_packed_nibbles(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[8], const PtxMmaCoordinate columns[8],
    const char *group, const char *thread, int destination_register,
    int contiguous_word) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  if (contiguous_word) {
    char address[24], linear[24];
    ptx_mma_emit_nibble_address(fn, base, leading_dimension, layout,
                                transpose, rows[0], columns[0], group,
                                thread, address, linear);
    sb_printf(&fn->body, "\tld%s.b32 %s, [%s];\n", space, destination,
              address);
    return;
  }
  for (unsigned nibble = 0; nibble < 8; nibble++) {
    char address[24], linear[24], packed_byte[24], shift[24], value[24];
    ptx_mma_emit_nibble_address(fn, base, leading_dimension, layout,
                                transpose, rows[nibble], columns[nibble],
                                group, thread, address, linear);
    reg_name(PC_B32, new_reg(fn, PC_B32), packed_byte);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, packed_byte,
              address);
    reg_name(PC_B32, new_reg(fn, PC_B32), shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 1;\n", shift, linear);
    sb_printf(&fn->body, "\tshl.b32 %s, %s, 2;\n", shift, shift);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, %s;\n", value, packed_byte,
              shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 15;\n", value, value);
    if (nibble == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                nibble * 4);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_u8_pair(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, const char *space,
    const PtxMmaCoordinate rows[2], const PtxMmaCoordinate columns[2],
    const char *group, const char *thread, int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned byte = 0; byte < 2; byte++) {
    char address[24], value[24];
    ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, rows[byte],
                         columns[byte], 1, group, thread, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value, address);
    if (byte == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, 8;\n", value, value);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_u16_pair(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[2], const PtxMmaCoordinate columns[2],
    const char *group, const char *thread, int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned half = 0; half < 2; half++) {
    char address[24], value[24];
    ptx_mma_emit_address(fn, base, leading_dimension, layout, transpose,
                         rows[half], columns[half], 2, group, thread, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tld%s.u16 %s, [%s];\n", space, value, address);
    if (half == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, 16;\n", value, value);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_packed_bytes(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[4], const PtxMmaCoordinate columns[4],
    const char *group, const char *thread, int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned byte = 0; byte < 4; byte++) {
    char address[24], value[24];
    ptx_mma_emit_address(fn, base, leading_dimension, layout, transpose,
                         rows[byte], columns[byte], 1, group, thread, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value, address);
    if (byte == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      char shifted[24];
      reg_name(PC_B32, new_reg(fn, PC_B32), shifted);
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", shifted, value,
                byte * 8);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, shifted);
    }
  }
}

/* Load four logical subbyte elements into PTX's four byte containers. Dense
 * storage is a least-significant-bit-first bitstream over the logical matrix,
 * including logical leading-dimension padding. A six-bit value can straddle a
 * byte boundary; the second byte is predicated so a non-straddling final
 * element never performs an out-of-bounds speculative load. */
static void ptx_mma_load_dense_subbytes(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[4], const PtxMmaCoordinate columns[4],
    const char *group, const char *thread, unsigned bits,
    int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned container = 0; container < 4; container++) {
    PtxMmaCoordinate storage_row = transpose ? columns[container]
                                             : rows[container];
    PtxMmaCoordinate storage_column = transpose ? rows[container]
                                                : columns[container];
    char row[24], column[24], linear[24], bit_index[24], byte_index[24];
    char bit_shift[24], byte_offset[24], address[24], next_address[24];
    char low[24], high[24], word[24], value[24], crosses[24];
    ptx_mma_emit_coordinate(fn, storage_row, group, thread, row);
    ptx_mma_emit_coordinate(fn, storage_column, group, thread, column);
    reg_name(PC_B32, new_reg(fn, PC_B32), linear);
    if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
      sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, row,
                leading_dimension);
      sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
                column);
    } else {
      sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, column,
                leading_dimension);
      sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear, row);
    }
    reg_name(PC_B32, new_reg(fn, PC_B32), bit_index);
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", bit_index, linear,
              bits);
    reg_name(PC_B32, new_reg(fn, PC_B32), byte_index);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, 3;\n", byte_index, bit_index);
    reg_name(PC_B32, new_reg(fn, PC_B32), bit_shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 7;\n", bit_shift, bit_index);
    reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
    sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", byte_offset,
              byte_index);
    reg_name(PC_B64, new_reg(fn, PC_B64), address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
              byte_offset);
    reg_name(PC_B32, new_reg(fn, PC_B32), low);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, low, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), high);
    sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", high);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), crosses);
    sb_printf(&fn->body, "\tsetp.gt.u32 %s, %s, %u;\n", crosses,
              bit_shift, 8u - bits);
    reg_name(PC_B64, new_reg(fn, PC_B64), next_address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, 1;\n", next_address,
              address);
    sb_printf(&fn->body, "\t@%s ld%s.u8 %s, [%s];\n", crosses, space,
              high, next_address);
    reg_name(PC_B32, new_reg(fn, PC_B32), word);
    sb_printf(&fn->body, "\tshl.b32 %s, %s, 8;\n", word, high);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", word, word, low);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, %s;\n", value, word,
              bit_shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", value, value,
              (1u << bits) - 1u);
    if (container == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                container * 8);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

/* Canonical row-A/column-B fragments begin on a byte boundary and contain
 * four consecutive logical values. Load the exact two (FP4) or three (FP6)
 * bytes and expand locally, avoiding both over-read and the general gather's
 * predicate/address pressure. */
static void ptx_mma_load_contiguous_dense_subbytes(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, const char *space, PtxMmaCoordinate row,
    PtxMmaCoordinate column, const char *group, const char *thread,
    unsigned bits, int destination_register) {
  PtxMmaCoordinate storage_row = row;
  PtxMmaCoordinate storage_column = column;
  char row_register[24], column_register[24], linear[24], bit_index[24];
  char byte_index[24], byte_offset[24], address[24], packed[24];
  ptx_mma_emit_coordinate(fn, storage_row, group, thread, row_register);
  ptx_mma_emit_coordinate(fn, storage_column, group, thread, column_register);
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear,
              row_register, leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              column_register);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear,
              column_register, leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              row_register);
  }
  reg_name(PC_B32, new_reg(fn, PC_B32), bit_index);
  sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", bit_index, linear,
            bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), byte_index);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 3;\n", byte_index, bit_index);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", byte_offset, byte_index);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
  reg_name(PC_B32, new_reg(fn, PC_B32), packed);
  sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", packed);
  unsigned byte_count = (4u * bits + 7u) / 8u;
  for (unsigned byte = 0; byte < byte_count; byte++) {
    char value[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    if (byte == 0)
      sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value,
                address);
    else
      sb_printf(&fn->body, "\tld%s.u8 %s, [%s+%u];\n", space, value,
                address, byte);
    if (byte != 0)
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                byte * 8);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", packed, packed, value);
  }

  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned container = 0; container < 4; container++) {
    char value[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tbfe.u32 %s, %s, %u, %u;\n", value, packed,
              container * bits, bits);
    if (container == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                container * 8);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_narrow_containers(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, MtlcTensorPacking packing,
    const char *space, const PtxMmaCoordinate rows[4],
    const PtxMmaCoordinate columns[4], const char *group, const char *thread,
    unsigned bits, int destination_register, int contiguous) {
  if (packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE && contiguous) {
    ptx_mma_load_contiguous_dense_subbytes(
        fn, base, leading_dimension, layout, space, rows[0], columns[0], group,
        thread, bits, destination_register);
  } else if (packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE) {
    ptx_mma_load_dense_subbytes(fn, base, leading_dimension, layout,
                                transpose, space, rows, columns, group,
                                thread, bits, destination_register);
  } else {
    ptx_mma_load_packed_bytes(fn, base, leading_dimension, layout, transpose,
                              space, rows, columns, group, thread,
                              destination_register);
  }
}

static void ptx_mma_load_u8(PtxFn *fn, const char *base,
                            const char *leading_dimension,
                            MtlcTensorLayout layout, const char *space,
                            PtxMmaCoordinate row, PtxMmaCoordinate column,
                            const char *group, const char *thread,
                            int destination_register) {
  char address[24], destination[24];
  ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, row, column, 1,
                       group, thread, address);
  reg_name(PC_B32, destination_register, destination);
  sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, destination,
            address);
}

static void ptx_mma_load_f32(PtxFn *fn, const char *base,
                             const char *leading_dimension,
                             MtlcTensorLayout layout, const char *space,
                             PtxMmaCoordinate row,
                             PtxMmaCoordinate column, const char *group,
                             const char *thread, int destination_register) {
  char address[24], destination[24];
  ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, row, column, 4,
                       group, thread, address);
  reg_name(PC_F32, destination_register, destination);
  sb_printf(&fn->body, "\tld%s.f32 %s, [%s];\n", space, destination,
            address);
}

static void ptx_mma_store_f32(PtxFn *fn, const char *base,
                              const char *leading_dimension,
                              MtlcTensorLayout layout, const char *space,
                              PtxMmaCoordinate row,
                              PtxMmaCoordinate column, const char *group,
                              const char *thread, int source_register) {
  char address[24], source[24];
  ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, row, column, 4,
                       group, thread, address);
  reg_name(PC_F32, source_register, source);
  sb_printf(&fn->body, "\tst%s.f32 [%s], %s;\n", space, address, source);
}

static void ptx_mma_store_f32_accumulator_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const char *dp, const char *dspace, const char *d_stride,
    const char *group, const char *thread, unsigned m_offset,
    unsigned n_offset, int accumulator_base) {
  for (int element = 0; element < profile->accumulator_registers;
       element++) {
    PtxMmaCoordinate row = {
        1, 0, m_offset + (element >= 2 ? 8u : 0u)};
    PtxMmaCoordinate column = {
        0, 2, n_offset + (unsigned)(element & 1)};
    ptx_mma_store_f32(fn, dp, d_stride, IR_TENSOR_MMA(in).d_layout, dspace, row,
                      column, group, thread, accumulator_base + element);
  }
}

static const char *ptx_mma_kind_name(const PtxMmaProfile *profile) {
  if (!profile) return "unknown";
  if (profile->kind == PTX_MMA_SPARSE_F16) return "sparse-f16-2to4";
  if (profile->kind == PTX_MMA_SPARSE_BF16) return "sparse-bf16-2to4";
  if (profile->kind == PTX_MMA_MXF8F6F4) return "mxf8f6f4";
  if (profile->kind == PTX_MMA_MXFP4) return "mxfp4";
  if (profile->kind == PTX_MMA_NVFP4) return "nvfp4";
  return "fp8";
}

static int ptx_mma_profile_is_sparse(const PtxMmaProfile *profile) {
  return profile &&
         (profile->kind == PTX_MMA_SPARSE_F16 ||
          profile->kind == PTX_MMA_SPARSE_BF16);
}

static int ptx_tensor_tuple_budget(const PtxFn *fn) {
  if (fn && fn->tensor_tuple_budget > 0) return fn->tensor_tuple_budget;
  return fn && fn->target_arch >= 90 ? 96 : 64;
}

static int ptx_mma_prepare_tile_memory(PtxFn *fn,
                                       const IRInstruction *in,
                                       const PtxMmaProfile *profile,
                                       size_t base, size_t per_tile,
                                       PtxMmaTileMemory *memory) {
  memset(memory, 0, sizeof(*memory));
  for (size_t pointer = 0; pointer < 4; pointer++) {
    PtxVal value = operand_desc(fn, &in->arguments[base + pointer]);
    memory->spaces[pointer] = ptx_wmma_space(value);
    if (!value.is_ptr || !memory->spaces[pointer]) return 0;
    use_as(fn, &in->arguments[base + pointer], PC_B64,
           memory->bases[pointer]);
  }
  if (ptx_mma_profile_is_sparse(profile)) {
    PtxVal value = operand_desc(fn, &in->arguments[base + 4]);
    memory->spaces[4] = ptx_wmma_space(value);
    if (!value.is_ptr || !memory->spaces[4]) return 0;
    use_as(fn, &in->arguments[base + 4], PC_B64, memory->bases[4]);
    reg_name(PC_B32, new_reg(fn, PC_B32), memory->metadata_stride);
    sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", memory->metadata_stride,
              (unsigned)IR_TENSOR_MMA(in).k / 4u);
  } else if (profile->kind == PTX_MMA_MXF8F6F4 ||
             profile->kind == PTX_MMA_MXFP4 ||
             profile->kind == PTX_MMA_NVFP4) {
    /* Profile selection guarantees dense operands with both scale pointers,
     * whose neutral-IR order is A/B/C/D, scale_A, scale_B, strides. */
    for (size_t scale = 0; scale < 2; scale++) {
      size_t argument = base + 4 + scale;
      PtxVal value = operand_desc(fn, &in->arguments[argument]);
      memory->spaces[4 + scale] = ptx_wmma_space(value);
      if (!value.is_ptr || !memory->spaces[4 + scale]) return 0;
      use_as(fn, &in->arguments[argument], PC_B64,
             memory->bases[4 + scale]);
      uint32_t leading_dimension =
          scale == 0 ? IR_TENSOR_MMA(in).a_scale_leading_dimension
                     : IR_TENSOR_MMA(in).b_scale_leading_dimension;
      if (!leading_dimension)
        leading_dimension = (uint32_t)profile->scale_vectors;
      reg_name(PC_B32, new_reg(fn, PC_B32), memory->scale_strides[scale]);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n",
                memory->scale_strides[scale], leading_dimension);
    }
  }
  if (!ptx_tensor_stride_registers(fn, in, base, per_tile,
                                   memory->strides))
    return 0;
  memory->dense_contiguous[0] =
      profile->a_bits < 8 &&
      IR_TENSOR_MMA(in).a_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
      IR_TENSOR_MMA(in).a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
      !IR_TENSOR_MMA(in).transpose_a &&
      IR_TENSOR_MMA(in).a_leading_dimension != 0 &&
      ((IR_TENSOR_MMA(in).a_leading_dimension *
        (uint32_t)profile->a_bits) & 7u) == 0;
  memory->dense_contiguous[1] =
      profile->b_bits < 8 &&
      IR_TENSOR_MMA(in).b_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
      IR_TENSOR_MMA(in).b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR &&
      !IR_TENSOR_MMA(in).transpose_b &&
      IR_TENSOR_MMA(in).b_leading_dimension != 0 &&
      ((IR_TENSOR_MMA(in).b_leading_dimension *
        (uint32_t)profile->b_bits) & 7u) == 0;
  return 1;
}

static void ptx_emit_mma_byte_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    unsigned m_offset, unsigned n_offset,
    int accumulator_base, int load_accumulator, int store_accumulator) {
  int a_base = fn->count[PC_B32];
  for (int i = 0; i < profile->a_registers; i++) new_reg(fn, PC_B32);
  int b_base = fn->count[PC_B32];
  for (int i = 0; i < profile->b_registers; i++) new_reg(fn, PC_B32);
  int scale_a = -1, scale_b = -1;
  if (profile->kind == PTX_MMA_MXF8F6F4) {
    scale_a = new_reg(fn, PC_B32);
    scale_b = new_reg(fn, PC_B32);
  }
  if (accumulator_base < 0) {
    accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < profile->accumulator_registers; i++)
      new_reg(fn, PC_F32);
  }

  int direct_a = memory->dense_contiguous[0];
  int direct_b = memory->dense_contiguous[1];

  for (int reg = 0; reg < profile->a_registers; reg++) {
    PtxMmaCoordinate rows[4], columns[4];
    for (int byte = 0; byte < 4; byte++) {
      int element = reg * 4 + byte;
      unsigned second_row = profile->a_registers == 2
                                ? (element >= 4 ? 8u : 0u)
                                : ((element & 7) >= 4 ? 8u : 0u);
      unsigned second_k = profile->a_registers == 4 && element >= 8 ? 16u : 0u;
      rows[byte] = (PtxMmaCoordinate){1, 0, m_offset + second_row};
      columns[byte] =
          (PtxMmaCoordinate){0, 4, second_k + (unsigned)(element & 3)};
    }
    ptx_mma_load_narrow_containers(
        fn, memory->bases[0], memory->strides[0],
        IR_TENSOR_MMA(in).a_layout, IR_TENSOR_MMA(in).transpose_a,
        IR_TENSOR_MMA(in).a_packing, memory->spaces[0], rows, columns, group,
        thread, (unsigned)profile->a_bits, a_base + reg, direct_a);
  }
  for (int reg = 0; reg < profile->b_registers; reg++) {
    PtxMmaCoordinate rows[4], columns[4];
    for (int byte = 0; byte < 4; byte++) {
      int element = reg * 4 + byte;
      unsigned second_k = profile->b_registers == 2 && element >= 4 ? 16u : 0u;
      rows[byte] =
          (PtxMmaCoordinate){0, 4, second_k + (unsigned)(element & 3)};
      columns[byte] = (PtxMmaCoordinate){1, 0, n_offset};
    }
    ptx_mma_load_narrow_containers(
        fn, memory->bases[1], memory->strides[1],
        IR_TENSOR_MMA(in).b_layout, IR_TENSOR_MMA(in).transpose_b,
        IR_TENSOR_MMA(in).b_packing, memory->spaces[1], rows, columns, group,
        thread, (unsigned)profile->b_bits, b_base + reg, direct_b);
  }
  if (profile->kind == PTX_MMA_MXF8F6F4) {
    /* scale_vec::1X uses scale_A[M,1] and scale_B[1,N]. Selector {0,0}
     * chooses the lower A thread pair and thread zero's B byte in each quad. */
    char thread_low[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), thread_low);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 1;\n", thread_low, thread);
    PtxMmaCoordinate scale_a_row = {1, 8, m_offset};
    PtxMmaCoordinate scale_a_column = {0, 0, 0};
    PtxMmaCoordinate scale_b_row = {0, 0, 0};
    PtxMmaCoordinate scale_b_column = {1, 0, n_offset};
    ptx_mma_load_u8(fn, memory->bases[4], memory->scale_strides[0],
                    MTLC_TENSOR_LAYOUT_ROW_MAJOR, memory->spaces[4],
                    scale_a_row, scale_a_column, group, thread_low, scale_a);
    ptx_mma_load_u8(fn, memory->bases[5], memory->scale_strides[1],
                    MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, memory->spaces[5],
                    scale_b_row, scale_b_column, group, thread, scale_b);
  }
  if (load_accumulator) {
    for (int element = 0; element < profile->accumulator_registers;
         element++) {
      PtxMmaCoordinate row = {
          1, 0, m_offset + (element >= 2 ? 8u : 0u)};
      PtxMmaCoordinate column = {
          0, 2, n_offset + (unsigned)(element & 1)};
      ptx_mma_load_f32(fn, memory->bases[2], memory->strides[2],
                       IR_TENSOR_MMA(in).c_layout, memory->spaces[2], row, column,
                       group, thread,
                       accumulator_base + element);
    }
  }

  char a[128], b[128], accumulator[128];
  ptx_reg_tuple_at(PC_B32, a_base, profile->a_registers, a, sizeof(a));
  ptx_reg_tuple_at(PC_B32, b_base, profile->b_registers, b, sizeof(b));
  ptx_reg_tuple_at(PC_F32, accumulator_base,
                   profile->accumulator_registers, accumulator,
                   sizeof(accumulator));
  if (profile->kind == PTX_MMA_MXF8F6F4) {
    char scale_a_register[24], scale_b_register[24];
    reg_name(PC_B32, scale_a, scale_a_register);
    reg_name(PC_B32, scale_b, scale_b_register);
    sb_printf(
        &fn->body,
        "\tmma.sync.aligned.%s.row.col.kind::mxf8f6f4.block_scale.scale_vec::1X.%s.%s.%s.%s.%s %s, %s, %s, %s, %s, {0, 0}, %s, {0, 0};\n",
        profile->shape, profile->d_type, profile->a_type, profile->b_type,
        profile->c_type, profile->scale_type, accumulator, a, b, accumulator,
        scale_a_register, scale_b_register);
  } else {
    sb_printf(&fn->body,
              "\tmma.sync.aligned.%s.row.col.%s.%s.%s.%s %s, %s, %s, %s;\n",
              profile->shape, profile->d_type, profile->a_type,
              profile->b_type, profile->c_type, accumulator, a, b,
              accumulator);
  }

  if (store_accumulator) {
    ptx_mma_store_f32_accumulator_subtile(
        fn, in, profile, memory->bases[3], memory->spaces[3],
        memory->strides[3], group, thread, m_offset, n_offset,
        accumulator_base);
  }
}

static void ptx_emit_mma_fp4_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    unsigned m_offset, unsigned n_offset, int accumulator_base,
    int load_accumulator, int store_accumulator) {
  int a_base = fn->count[PC_B32];
  for (int i = 0; i < profile->a_registers; i++) new_reg(fn, PC_B32);
  int b_base = fn->count[PC_B32];
  for (int i = 0; i < profile->b_registers; i++) new_reg(fn, PC_B32);
  int scale_a = new_reg(fn, PC_B32);
  int scale_b = new_reg(fn, PC_B32);
  if (accumulator_base < 0) {
    accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < profile->accumulator_registers; i++)
      new_reg(fn, PC_F32);
  }

  int direct_a = memory->dense_contiguous[0];
  int direct_b = memory->dense_contiguous[1];
  for (int reg = 0; reg < profile->a_registers; reg++) {
    PtxMmaCoordinate rows[8], columns[8];
    for (int nibble = 0; nibble < 8; nibble++) {
      int element = reg * 8 + nibble;
      rows[nibble] = (PtxMmaCoordinate){
          1, 0, m_offset + ((element & 8) ? 8u : 0u)};
      columns[nibble] =
          (PtxMmaCoordinate){0, 8,
                             (element >= 16 ? 32u : 0u) +
                                 (unsigned)(element & 7)};
    }
    ptx_mma_load_packed_nibbles(
        fn, memory->bases[0], memory->strides[0],
        IR_TENSOR_MMA(in).a_layout, IR_TENSOR_MMA(in).transpose_a,
        memory->spaces[0], rows, columns, group, thread, a_base + reg,
        direct_a);
  }
  for (int reg = 0; reg < profile->b_registers; reg++) {
    PtxMmaCoordinate rows[8], columns[8];
    for (int nibble = 0; nibble < 8; nibble++) {
      int element = reg * 8 + nibble;
      rows[nibble] =
          (PtxMmaCoordinate){0, 8,
                             (element >= 8 ? 32u : 0u) +
                                 (unsigned)(element & 7)};
      columns[nibble] = (PtxMmaCoordinate){1, 0, n_offset};
    }
    ptx_mma_load_packed_nibbles(
        fn, memory->bases[1], memory->strides[1],
        IR_TENSOR_MMA(in).b_layout, IR_TENSOR_MMA(in).transpose_b,
        memory->spaces[1], rows, columns, group, thread, b_base + reg,
        direct_b);
  }

  /* With selector {0,0}, threads 0/1 of each quad contribute the A scale
   * vector and thread 0 contributes the B vector. MXFP4 supplies two UE8M0
   * bytes (block32); NVFP4 supplies four UE4M3 bytes (block16). Duplicating
   * safe loads in non-contributing lanes keeps the instruction converged and
   * branch-free. */
  char thread_low[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), thread_low);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 1;\n", thread_low, thread);
  if (profile->kind == PTX_MMA_NVFP4) {
    PtxMmaCoordinate scale_a_rows[4], scale_a_columns[4];
    PtxMmaCoordinate scale_b_rows[4], scale_b_columns[4];
    for (unsigned scale = 0; scale < 4; scale++) {
      scale_a_rows[scale] =
          (PtxMmaCoordinate){1, 8, m_offset};
      scale_a_columns[scale] =
          (PtxMmaCoordinate){0, 0, scale};
      scale_b_rows[scale] =
          (PtxMmaCoordinate){0, 0, scale};
      scale_b_columns[scale] =
          (PtxMmaCoordinate){1, 0, n_offset};
    }
    ptx_mma_load_packed_bytes(
        fn, memory->bases[4], memory->scale_strides[0],
        MTLC_TENSOR_LAYOUT_ROW_MAJOR, 0, memory->spaces[4], scale_a_rows,
        scale_a_columns, group, thread_low, scale_a);
    ptx_mma_load_packed_bytes(
        fn, memory->bases[5], memory->scale_strides[1],
        MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, 0, memory->spaces[5], scale_b_rows,
        scale_b_columns, group, thread, scale_b);
  } else {
    PtxMmaCoordinate scale_a_rows[2] = {
        {1, 8, m_offset}, {1, 8, m_offset}};
    PtxMmaCoordinate scale_a_columns[2] = {{0, 0, 0}, {0, 0, 1}};
    PtxMmaCoordinate scale_b_rows[2] = {{0, 0, 0}, {0, 0, 1}};
    PtxMmaCoordinate scale_b_columns[2] = {
        {1, 0, n_offset}, {1, 0, n_offset}};
    ptx_mma_load_u8_pair(fn, memory->bases[4], memory->scale_strides[0],
                         MTLC_TENSOR_LAYOUT_ROW_MAJOR, memory->spaces[4],
                         scale_a_rows, scale_a_columns, group, thread_low,
                         scale_a);
    ptx_mma_load_u8_pair(fn, memory->bases[5], memory->scale_strides[1],
                         MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, memory->spaces[5],
                         scale_b_rows, scale_b_columns, group, thread,
                         scale_b);
  }

  if (load_accumulator) {
    for (int element = 0; element < profile->accumulator_registers;
         element++) {
      PtxMmaCoordinate row = {
          1, 0, m_offset + (element >= 2 ? 8u : 0u)};
      PtxMmaCoordinate column = {
          0, 2, n_offset + (unsigned)(element & 1)};
      ptx_mma_load_f32(fn, memory->bases[2], memory->strides[2],
                       IR_TENSOR_MMA(in).c_layout, memory->spaces[2], row,
                       column, group, thread, accumulator_base + element);
    }
  }

  char a[128], b[128], accumulator[128], scale_a_register[24];
  char scale_b_register[24];
  ptx_reg_tuple_at(PC_B32, a_base, profile->a_registers, a, sizeof(a));
  ptx_reg_tuple_at(PC_B32, b_base, profile->b_registers, b, sizeof(b));
  ptx_reg_tuple_at(PC_F32, accumulator_base,
                   profile->accumulator_registers, accumulator,
                   sizeof(accumulator));
  reg_name(PC_B32, scale_a, scale_a_register);
  reg_name(PC_B32, scale_b, scale_b_register);
  const char *mma_kind =
      profile->kind == PTX_MMA_NVFP4 ? "mxf4nvf4" : "mxf4";
  sb_printf(
      &fn->body,
      "\tmma.sync.aligned.%s.row.col.kind::%s.block_scale.scale_vec::%dX.%s.%s.%s.%s.%s %s, %s, %s, %s, %s, {0, 0}, %s, {0, 0};\n",
      profile->shape, mma_kind, profile->scale_vectors, profile->d_type,
      profile->a_type, profile->b_type, profile->c_type,
      profile->scale_type, accumulator, a, b, accumulator, scale_a_register,
      scale_b_register);

  if (store_accumulator) {
    ptx_mma_store_f32_accumulator_subtile(
        fn, in, profile, memory->bases[3], memory->spaces[3],
        memory->strides[3], group, thread, m_offset, n_offset,
        accumulator_base);
  }
}

/* Translate the neutral uint8 2-of-4 group masks into one PTX metadata word.
 * Canonical A stores the selected values in increasing logical-index order, so
 * the ordered-metadata instruction is legal when available. Invalid dynamic
 * masks violate the source contract; clamp them to the safe {0,1} encoding so
 * malformed input cannot feed an architecturally undefined metadata value to
 * the tensor instruction. */
static void ptx_mma_load_sparse_2_to_4_metadata(
    PtxFn *fn, const PtxMmaTileMemory *memory, const char *group,
    unsigned m_offset, int metadata_register) {
  /* PTX Figure 119 for m16n8k16 maps selector-0's contributing thread as:
   * bits  0..15 = four K groups for row groupID,
   * bits 16..31 = four K groups for row groupID + 8.
   * Emitting the same word in every lane of the quad is harmless; selector 0
   * consumes the first lane and avoids divergent metadata preparation. */
  char metadata[24];
  reg_name(PC_B32, metadata_register, metadata);
  sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", metadata);
  for (unsigned chunk = 0; chunk < 8; chunk++) {
    PtxMmaCoordinate row = {
        1, 0, m_offset + (chunk >= 4 ? 8u : 0u)};
    PtxMmaCoordinate column = {0, 0, chunk & 3u};
    char address[24], mask[24], count[24], scratch[24];
    char first[24], second[24], nibble[24], predicate[24];
    ptx_mma_emit_address(fn, memory->bases[4], memory->metadata_stride,
                         MTLC_TENSOR_LAYOUT_ROW_MAJOR, 0, row, column, 1,
                         group, "0", address);
    reg_name(PC_B32, new_reg(fn, PC_B32), mask);
    reg_name(PC_B32, new_reg(fn, PC_B32), count);
    reg_name(PC_B32, new_reg(fn, PC_B32), scratch);
    reg_name(PC_B32, new_reg(fn, PC_B32), first);
    reg_name(PC_B32, new_reg(fn, PC_B32), second);
    reg_name(PC_B32, new_reg(fn, PC_B32), nibble);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), predicate);
    sb_printf(&fn->body,
              "\tld%s.u8 %s, [%s];\n"
              "\tand.b32 %s, %s, 15;\n"
              "\tpopc.b32 %s, %s;\n"
              "\tsetp.ne.u32 %s, %s, 2;\n"
              "\tselp.u32 %s, 3, %s, %s;\n",
              memory->spaces[4], mask, address, mask, mask, count, mask,
              predicate, count, mask, mask, predicate);

    /* first = lowest set bit index, second = highest set bit index. */
    sb_printf(&fn->body,
              "\tand.b32 %s, %s, 2;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 1, 2, %s;\n"
              "\tand.b32 %s, %s, 1;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 0, %s, %s;\n",
              scratch, mask, predicate, scratch, first, predicate, scratch,
              mask, predicate, scratch, first, first, predicate);
    sb_printf(&fn->body,
              "\tand.b32 %s, %s, 4;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 2, 1, %s;\n"
              "\tand.b32 %s, %s, 8;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 3, %s, %s;\n"
              "\tshl.b32 %s, %s, 2;\n"
              "\tor.b32 %s, %s, %s;\n",
              scratch, mask, predicate, scratch, second, predicate, scratch,
              mask, predicate, scratch, second, second, predicate, second,
              second, nibble, first, second);
    if (chunk != 0)
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", nibble, nibble,
                chunk * 4u);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", metadata, metadata,
              nibble);
  }
}

static void ptx_mma_prepare_sparse_f16_a(
    PtxFn *fn, const IRInstruction *in, const PtxMmaTileMemory *memory,
    const char *group, const char *thread, unsigned m_offset,
    PtxMmaSparseAFragment *fragment) {
  fragment->a_base = fn->count[PC_B32];
  for (int reg = 0; reg < 2; reg++) new_reg(fn, PC_B32);
  fragment->metadata_register = new_reg(fn, PC_B32);

  for (int reg = 0; reg < 2; reg++) {
    PtxMmaCoordinate rows[2] = {
        {1, 0, m_offset + (reg ? 8u : 0u)},
        {1, 0, m_offset + (reg ? 8u : 0u)}};
    PtxMmaCoordinate columns[2] = {{0, 2, 0}, {0, 2, 1}};
    ptx_mma_load_u16_pair(
        fn, memory->bases[0], memory->strides[0],
        IR_TENSOR_MMA(in).a_layout, IR_TENSOR_MMA(in).transpose_a,
        memory->spaces[0], rows, columns, group, thread,
        fragment->a_base + reg);
  }
  ptx_mma_load_sparse_2_to_4_metadata(
      fn, memory, group, m_offset, fragment->metadata_register);
}

static void ptx_emit_mma_sparse_f16_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    const PtxMmaSparseAFragment *sparse_a, unsigned m_offset,
    unsigned n_offset, int accumulator_base, int load_accumulator,
    int store_accumulator) {
  int b_base = fn->count[PC_B32];
  for (int reg = 0; reg < 2; reg++) new_reg(fn, PC_B32);
  if (accumulator_base < 0) {
    accumulator_base = fn->count[PC_F32];
    for (int element = 0; element < 4; element++) new_reg(fn, PC_F32);
  }

  for (int reg = 0; reg < 2; reg++) {
    PtxMmaCoordinate rows[2] = {
        {0, 2, (unsigned)reg * 8u},
        {0, 2, (unsigned)reg * 8u + 1u}};
    PtxMmaCoordinate columns[2] = {
        {1, 0, n_offset}, {1, 0, n_offset}};
    ptx_mma_load_u16_pair(
        fn, memory->bases[1], memory->strides[1],
        IR_TENSOR_MMA(in).b_layout, IR_TENSOR_MMA(in).transpose_b,
        memory->spaces[1], rows, columns, group, thread, b_base + reg);
  }

  if (load_accumulator) {
    for (int element = 0; element < 4; element++) {
      PtxMmaCoordinate row = {
          1, 0, m_offset + (element >= 2 ? 8u : 0u)};
      PtxMmaCoordinate column = {
          0, 2, n_offset + (unsigned)(element & 1)};
      ptx_mma_load_f32(fn, memory->bases[2], memory->strides[2],
                       IR_TENSOR_MMA(in).c_layout, memory->spaces[2], row,
                       column, group, thread, accumulator_base + element);
    }
  }

  char a[64], b[64], accumulator[128], metadata[24];
  ptx_reg_tuple_at(PC_B32, sparse_a->a_base, 2, a, sizeof(a));
  ptx_reg_tuple_at(PC_B32, b_base, 2, b, sizeof(b));
  ptx_reg_tuple_at(PC_F32, accumulator_base, 4, accumulator,
                   sizeof(accumulator));
  reg_name(PC_B32, sparse_a->metadata_register, metadata);
  const char *sparse_variant =
      ptx_version_at_least(fn, 8, 5) ? "sp::ordered_metadata" : "sp";
  sb_printf(
      &fn->body,
      "\tmma.%s.sync.aligned.%s.row.col.f32.%s.%s.f32 %s, %s, %s, %s, %s, 0;\n",
      sparse_variant, profile->shape, profile->a_type, profile->b_type,
      accumulator, a, b, accumulator, metadata);

  if (store_accumulator) {
    ptx_mma_store_f32_accumulator_subtile(
        fn, in, profile, memory->bases[3], memory->spaces[3],
        memory->strides[3], group, thread, m_offset, n_offset,
        accumulator_base);
  }
}

static void ptx_emit_mma_native_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    const PtxMmaSparseAFragment *sparse_a, unsigned m_offset,
    unsigned n_offset, int accumulator_base, int load_accumulator,
    int store_accumulator) {
  if (ptx_mma_profile_is_sparse(profile)) {
    ptx_emit_mma_sparse_f16_subtile(
        fn, in, profile, memory, group, thread, sparse_a, m_offset, n_offset,
        accumulator_base, load_accumulator, store_accumulator);
  } else if (profile->kind == PTX_MMA_MXFP4 ||
      profile->kind == PTX_MMA_NVFP4) {
    ptx_emit_mma_fp4_subtile(
        fn, in, profile, memory, group, thread, m_offset, n_offset,
        accumulator_base, load_accumulator, store_accumulator);
  } else {
    ptx_emit_mma_byte_subtile(
        fn, in, profile, memory, group, thread, m_offset, n_offset,
        accumulator_base, load_accumulator, store_accumulator);
  }
}

static void ptx_emit_tensor_mma_native_single(PtxFn *fn,
                                               const IRInstruction *in,
                                               const PtxMmaProfile *profile) {
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  PtxMmaTileMemory memory;
  if (!per_tile || in->argument_count != per_tile) {
    fn_error(fn, "PTX native MMA tensor operand count is inconsistent");
    return;
  }
  if (!ptx_mma_prepare_tile_memory(fn, in, profile, 0, per_tile, &memory)) {
    fn_error(fn,
             "PTX native MMA has invalid tile pointers or stride operands");
    return;
  }

  char lane[24], group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body,
            "\t// mtlc.tensor_mma native-mma %s whole-tile lowering\n",
            ptx_mma_kind_name(profile));
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);
  for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
    PtxMmaSparseAFragment sparse_a;
    PtxMmaSparseAFragment *sparse_a_ptr = NULL;
    if (ptx_mma_profile_is_sparse(profile)) {
      ptx_mma_prepare_sparse_f16_a(
          fn, in, &memory, group, thread, (unsigned)m_tile * 16u, &sparse_a);
      sparse_a_ptr = &sparse_a;
    }
    for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
      ptx_emit_mma_native_subtile(
          fn, in, profile, &memory, group, thread, sparse_a_ptr,
          (unsigned)m_tile * 16u, (unsigned)n_tile * 8u, -1, 1, 1);
    }
  }
}

static void ptx_emit_tensor_mma_native_chain_resident(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    size_t tile_count, size_t per_tile, int tuple_budget,
    int estimated_peak) {
  int subtile_count = profile->m_tiles * profile->n_tiles;
  int accumulator_count = subtile_count * profile->accumulator_registers;
  int accumulator_base = fn->count[PC_F32];
  for (int i = 0; i < accumulator_count; i++) new_reg(fn, PC_F32);

  char lane[24], group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body,
            "\t// mtlc.tensor_chain resident native-mma %s tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
            ptx_mma_kind_name(profile),
            (unsigned long long)tile_count, subtile_count, estimated_peak,
            tuple_budget);
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);

  for (size_t tile = 0; tile < tile_count; tile++) {
    size_t base = tile * per_tile;
    PtxMmaTileMemory memory;
    if (!ptx_mma_prepare_tile_memory(fn, in, profile, base, per_tile,
                                     &memory)) {
      fn_error(fn,
               "PTX native resident chain has invalid pointers or strides");
      return;
    }
    for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
      PtxMmaSparseAFragment sparse_a;
      PtxMmaSparseAFragment *sparse_a_ptr = NULL;
      if (ptx_mma_profile_is_sparse(profile)) {
        ptx_mma_prepare_sparse_f16_a(
            fn, in, &memory, group, thread, (unsigned)m_tile * 16u,
            &sparse_a);
        sparse_a_ptr = &sparse_a;
      }
      for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
        int subtile = m_tile * profile->n_tiles + n_tile;
        ptx_emit_mma_native_subtile(
            fn, in, profile, &memory, group, thread, sparse_a_ptr,
            (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            accumulator_base + subtile * profile->accumulator_registers,
            tile == 0, tile + 1 == tile_count);
      }
    }
  }
}

static void ptx_emit_wmma_mma(PtxFn *fn, const MtlcTensorMmaDesc *desc,
                              const PtxWmmaProfile *profile,
                              const char *d, const char *a, const char *b,
                              const char *c) {
  const char *alayout = ptx_tensor_layout(desc->a_layout);
  const char *blayout = ptx_tensor_layout(desc->b_layout);
  if (profile->kind == PTX_WMMA_F16) {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.%s.%s %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, profile->d_type,
              profile->c_type, d, a, b, c);
  } else if (profile->kind == PTX_WMMA_B1) {
    const char *op = desc->math_mode == MTLC_TENSOR_MATH_XOR_POPCOUNT
                         ? "xor"
                         : "and";
    sb_printf(&fn->body,
              "\twmma.mma.%s.popc.sync.aligned.%s.row.col.s32.b1.b1.s32 %s, %s, %s, %s;\n",
              op, profile->shape, d, a, b, c);
  } else if (profile->kind == PTX_WMMA_I8 ||
             profile->kind == PTX_WMMA_I4) {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.s32.%s.%s.s32%s %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, profile->a_type,
              profile->b_type,
              desc->overflow == MTLC_TENSOR_OVERFLOW_SATURATE_FINITE
                  ? ".satfinite"
                  : "",
              d, a, b, c);
  } else if (profile->kind == PTX_WMMA_F64) {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.f64.f64.f64.f64 %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, d, a, b, c);
  } else {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.f32.%s.%s.f32 %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, profile->a_type,
              profile->b_type, d, a, b, c);
  }
}

static int ptx_tensor_stride_registers(PtxFn *fn, const IRInstruction *in,
                                       size_t base, size_t per_tile,
                                       char registers[4][24]);

static unsigned ptx_wmma_element_bytes(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return 2;
  case MTLC_TENSOR_ELEMENT_TFLOAT32:
  case MTLC_TENSOR_ELEMENT_FLOAT32:
  case MTLC_TENSOR_ELEMENT_INT32:
    return 4;
  case MTLC_TENSOR_ELEMENT_FLOAT64:
    return 8;
  case MTLC_TENSOR_ELEMENT_INT8:
  case MTLC_TENSOR_ELEMENT_UINT8:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    return 1;
  default:
    return 0;
  }
}

/* Compute a physical subtile base from the neutral logical row/column
 * coordinates. Leading dimensions remain logical element counts; only this
 * PTX helper turns the selected element format into a byte address. */
static int ptx_wmma_offset_pointer(PtxFn *fn, const char *base,
                                   const char *stride,
                                   MtlcTensorLayout layout,
                                   unsigned row, unsigned column,
                                   MtlcTensorElement element,
                                   char pointer[24]) {
  if (!fn || !base || !stride || !pointer) return 0;
  if (row == 0 && column == 0) {
    snprintf(pointer, 24, "%s", base);
    return 1;
  }
  unsigned bytes = ptx_wmma_element_bytes(element);
  if (bytes == 0 || (layout != MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
                     layout != MTLC_TENSOR_LAYOUT_COLUMN_MAJOR))
    return 0;
  unsigned major = layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
  unsigned minor = layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
  reg_name(PC_B64, new_reg(fn, PC_B64), pointer);
  if (major == 0) {
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %llu;\n", pointer, base,
              (unsigned long long)minor * bytes);
    return 1;
  }
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", pointer, stride,
            major);
  if (minor != 0)
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %u;\n", pointer, pointer,
              minor);
  if (bytes == 2)
    sb_printf(&fn->body, "\tshl.b64 %s, %s, 1;\n", pointer, pointer);
  else if (bytes == 4)
    sb_printf(&fn->body, "\tshl.b64 %s, %s, 2;\n", pointer, pointer);
  else if (bytes == 8)
    sb_printf(&fn->body, "\tshl.b64 %s, %s, 3;\n", pointer, pointer);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", pointer, base, pointer);
  return 1;
}

static int ptx_emit_wmma_tiled_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    const char *c_base, const char *d_base,
    const char *c_space, const char *d_space, char strides[4][24],
    unsigned m_tile, unsigned n_tile, const char *a, const char *b,
    const char *c, const char *scratch_d, int accumulator_base,
    int load_accumulator, int store_accumulator) {
  unsigned row = m_tile * profile->tile_m;
  unsigned column = n_tile * profile->tile_n;
  int subtile = (int)(m_tile * (unsigned)profile->n_tiles + n_tile);
  char accumulator[256], cp[24], dp[24];
  if (accumulator_base >= 0) {
    ptx_reg_tuple_at(profile->d_class,
                     accumulator_base + subtile * profile->d_registers,
                     profile->d_registers, accumulator,
                     sizeof(accumulator));
  } else {
    snprintf(accumulator, sizeof(accumulator), "%s", scratch_d);
  }
  if (load_accumulator) {
    if (!ptx_wmma_offset_pointer(fn, c_base, strides[2],
                                 IR_TENSOR_MMA(in).c_layout, row, column,
                                 IR_TENSOR_MMA(in).accumulator_element, cp)) {
      fn_error(fn, "PTX tiled WMMA cannot address the C subtile");
      return 0;
    }
    sb_printf(&fn->body,
              "\twmma.load.c.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
              profile->shape, c_space,
              ptx_tensor_layout(IR_TENSOR_MMA(in).c_layout), profile->c_type,
              c, cp, strides[2]);
  }
  ptx_emit_wmma_mma(fn, &IR_TENSOR_MMA(in), profile, accumulator, a, b,
                    load_accumulator ? c : accumulator);
  if (store_accumulator) {
    if (!ptx_wmma_offset_pointer(fn, d_base, strides[3],
                                 IR_TENSOR_MMA(in).d_layout, row, column,
                                 IR_TENSOR_MMA(in).result_element, dp)) {
      fn_error(fn, "PTX tiled WMMA cannot address the D subtile");
      return 0;
    }
    sb_printf(&fn->body,
              "\twmma.store.d.sync.aligned.%s%s.%s.%s [%s], %s, %s;\n",
              profile->shape, d_space,
              ptx_tensor_layout(IR_TENSOR_MMA(in).d_layout), profile->d_type,
              dp, accumulator, strides[3]);
  }
  return 1;
}

/* Emit one logical tile as a grid of stable WMMA tiles. The larger logical
 * shape is shared IR; physical tile selection, pointer offsets, and operand
 * fragment reuse remain entirely inside the PTX backend. */
static int ptx_emit_wmma_tiled_tile(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    size_t base, size_t per_tile, int accumulator_base,
    int load_accumulator, int store_accumulator) {
  if (!fn || !in || !profile || profile->m_tiles <= 0 ||
      profile->n_tiles <= 0 || base > in->argument_count ||
      per_tile > in->argument_count - base) {
    if (fn) fn_error(fn, "PTX tiled WMMA operand range is inconsistent");
    return 0;
  }
  PtxVal av = operand_desc(fn, &in->arguments[base]);
  PtxVal bv = operand_desc(fn, &in->arguments[base + 1]);
  PtxVal cv = operand_desc(fn, &in->arguments[base + 2]);
  PtxVal dv = operand_desc(fn, &in->arguments[base + 3]);
  const char *spaces[4] = {ptx_wmma_space(av), ptx_wmma_space(bv),
                           ptx_wmma_space(cv), ptx_wmma_space(dv)};
  if (!av.is_ptr || !bv.is_ptr || !cv.is_ptr || !dv.is_ptr ||
      !spaces[0] || !spaces[1] || !spaces[2] || !spaces[3]) {
    fn_error(fn,
             "PTX tiled WMMA requires generic/global/workgroup tile pointers");
    return 0;
  }
  char pointers[4][24], strides[4][24];
  for (size_t operand = 0; operand < 4; operand++)
    use_as(fn, &in->arguments[base + operand], PC_B64, pointers[operand]);
  if (!ptx_tensor_stride_registers(fn, in, base, per_tile, strides)) {
    fn_error(fn, "PTX tiled WMMA has inconsistent stride operands");
    return 0;
  }

  char a[256], b[256], c[256] = {0}, scratch_d[256] = {0};
  ptx_reg_tuple(fn, profile->a_class, profile->a_registers, a, sizeof(a));
  ptx_reg_tuple(fn, profile->b_class, profile->b_registers, b, sizeof(b));
  if (load_accumulator)
    ptx_reg_tuple(fn, profile->c_class, profile->c_registers, c, sizeof(c));
  if (accumulator_base < 0)
    ptx_reg_tuple(fn, profile->d_class, profile->d_registers, scratch_d,
                  sizeof(scratch_d));

  int reuse_a = profile->n_tiles >= profile->m_tiles;
  if (profile->m_tiles * profile->n_tiles > 1) {
    sb_printf(&fn->body,
              "\t// mtlc.tensor_mma tiled logical=m%un%uk%u physical=%s subtiles=%d reuse=%s\n",
              (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
              (unsigned)IR_TENSOR_MMA(in).k, profile->shape,
              profile->m_tiles * profile->n_tiles,
              reuse_a ? "A" : "B");
  }
  if (reuse_a) {
    for (int m = 0; m < profile->m_tiles; m++) {
      char ap[24];
      if (!ptx_wmma_offset_pointer(
              fn, pointers[0], strides[0], IR_TENSOR_MMA(in).a_layout,
              (unsigned)m * profile->tile_m, 0,
              IR_TENSOR_MMA(in).a_element, ap)) {
        fn_error(fn, "PTX tiled WMMA cannot address the A subtile");
        return 0;
      }
      sb_printf(&fn->body,
                "\twmma.load.a.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                profile->shape, spaces[0],
                ptx_tensor_layout(IR_TENSOR_MMA(in).a_layout), profile->a_type,
                a, ap, strides[0]);
      for (int n = 0; n < profile->n_tiles; n++) {
        char bp[24];
        if (!ptx_wmma_offset_pointer(
                fn, pointers[1], strides[1], IR_TENSOR_MMA(in).b_layout, 0,
                (unsigned)n * profile->tile_n,
                IR_TENSOR_MMA(in).b_element, bp)) {
          fn_error(fn, "PTX tiled WMMA cannot address the B subtile");
          return 0;
        }
        sb_printf(&fn->body,
                  "\twmma.load.b.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                  profile->shape, spaces[1],
                  ptx_tensor_layout(IR_TENSOR_MMA(in).b_layout),
                  profile->b_type, b, bp, strides[1]);
        if (!ptx_emit_wmma_tiled_subtile(
                fn, in, profile, pointers[2], pointers[3], spaces[2],
                spaces[3], strides, (unsigned)m, (unsigned)n, a, b, c,
                scratch_d, accumulator_base, load_accumulator,
                store_accumulator))
          return 0;
      }
    }
  } else {
    for (int n = 0; n < profile->n_tiles; n++) {
      char bp[24];
      if (!ptx_wmma_offset_pointer(
              fn, pointers[1], strides[1], IR_TENSOR_MMA(in).b_layout, 0,
              (unsigned)n * profile->tile_n,
              IR_TENSOR_MMA(in).b_element, bp)) {
        fn_error(fn, "PTX tiled WMMA cannot address the B subtile");
        return 0;
      }
      sb_printf(&fn->body,
                "\twmma.load.b.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                profile->shape, spaces[1],
                ptx_tensor_layout(IR_TENSOR_MMA(in).b_layout), profile->b_type,
                b, bp, strides[1]);
      for (int m = 0; m < profile->m_tiles; m++) {
        char ap[24];
        if (!ptx_wmma_offset_pointer(
                fn, pointers[0], strides[0], IR_TENSOR_MMA(in).a_layout,
                (unsigned)m * profile->tile_m, 0,
                IR_TENSOR_MMA(in).a_element, ap)) {
          fn_error(fn, "PTX tiled WMMA cannot address the A subtile");
          return 0;
        }
        sb_printf(&fn->body,
                  "\twmma.load.a.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                  profile->shape, spaces[0],
                  ptx_tensor_layout(IR_TENSOR_MMA(in).a_layout),
                  profile->a_type, a, ap, strides[0]);
        if (!ptx_emit_wmma_tiled_subtile(
                fn, in, profile, pointers[2], pointers[3], spaces[2],
                spaces[3], strides, (unsigned)m, (unsigned)n, a, b, c,
                scratch_d, accumulator_base, load_accumulator,
                store_accumulator))
          return 0;
      }
    }
  }
  return !fn->error;
}

static int ptx_emit_wmma_tiled_store(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    size_t base, size_t per_tile, int accumulator_base) {
  PtxVal dv = operand_desc(fn, &in->arguments[base + 3]);
  const char *dspace = ptx_wmma_space(dv);
  char dp_base[24], strides[4][24];
  if (!dv.is_ptr || !dspace || accumulator_base < 0) {
    fn_error(fn, "PTX tiled WMMA commit requires a writable D tile");
    return 0;
  }
  use_as(fn, &in->arguments[base + 3], PC_B64, dp_base);
  if (!ptx_tensor_stride_registers(fn, in, base, per_tile, strides)) {
    fn_error(fn, "PTX tiled WMMA commit has inconsistent strides");
    return 0;
  }
  for (int m = 0; m < profile->m_tiles; m++) {
    for (int n = 0; n < profile->n_tiles; n++) {
      int subtile = m * profile->n_tiles + n;
      char dp[24], accumulator[256];
      if (!ptx_wmma_offset_pointer(
              fn, dp_base, strides[3], IR_TENSOR_MMA(in).d_layout,
              (unsigned)m * profile->tile_m,
              (unsigned)n * profile->tile_n,
              IR_TENSOR_MMA(in).result_element, dp)) {
        fn_error(fn, "PTX tiled WMMA commit cannot address the D subtile");
        return 0;
      }
      ptx_reg_tuple_at(profile->d_class,
                       accumulator_base + subtile * profile->d_registers,
                       profile->d_registers, accumulator,
                       sizeof(accumulator));
      sb_printf(&fn->body,
                "\twmma.store.d.sync.aligned.%s%s.%s.%s [%s], %s, %s;\n",
                profile->shape, dspace,
                ptx_tensor_layout(IR_TENSOR_MMA(in).d_layout),
                profile->d_type, dp, accumulator, strides[3]);
    }
  }
  return !fn->error;
}

static void ptx_emit_tensor_mma_single(PtxFn *fn, const IRInstruction *in) {
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn, "PTX native tensor MMA cannot lower m%un%uk%u: %s",
               (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
               (unsigned)IR_TENSOR_MMA(in).k, native_reason);
      return;
    }
    ptx_emit_tensor_mma_native_single(fn, in, &native_profile);
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor MMA cannot lower m%un%uk%u: %s",
             (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
             (unsigned)IR_TENSOR_MMA(in).k, reason);
    return;
  }
  size_t expected_operands = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (!expected_operands || in->argument_count != expected_operands) {
    fn_error(fn, "PTX stable WMMA tensor operand count is inconsistent");
    return;
  }
  (void)ptx_emit_wmma_tiled_tile(fn, in, &profile, 0,
                                  expected_operands, -1, 1, 1);
}

static int ptx_tensor_stride_registers(PtxFn *fn, const IRInstruction *in,
                                       size_t base, size_t per_tile,
                                       char registers[4][24]) {
  uint32_t static_strides[4] = {
      IR_TENSOR_MMA(in).a_leading_dimension,
      IR_TENSOR_MMA(in).b_leading_dimension,
      IR_TENSOR_MMA(in).c_leading_dimension,
      IR_TENSOR_MMA(in).d_leading_dimension};
  size_t stride_argument =
      base + 4u +
      (IR_TENSOR_MMA(in).sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  for (size_t stride = 0; stride < 4; stride++) {
    if (static_strides[stride] == 0) {
      if (stride_argument >= base + per_tile) return 0;
      use_as(fn, &in->arguments[stride_argument++], PC_B32,
             registers[stride]);
    } else {
      reg_name(PC_B32, new_reg(fn, PC_B32), registers[stride]);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", registers[stride],
                static_strides[stride]);
    }
  }
  return stride_argument == base + per_tile;
}

static void ptx_emit_tensor_mma_chain_resident(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    size_t tile_count, size_t per_tile, int tuple_budget,
    int estimated_peak) {
  int subtile_count = profile->m_tiles * profile->n_tiles;
  int accumulator_count = subtile_count * profile->d_registers;
  int accumulator_base = fn->count[profile->d_class];
  for (int i = 0; i < accumulator_count; i++)
    new_reg(fn, profile->d_class);
  if (subtile_count == 1)
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain resident tiles=%llu tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, estimated_peak, tuple_budget);
  else
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain resident tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, subtile_count, estimated_peak,
              tuple_budget);
  for (size_t tile = 0; tile < tile_count; tile++) {
    if (!ptx_emit_wmma_tiled_tile(
            fn, in, profile, tile * per_tile, per_tile, accumulator_base,
            tile == 0, tile + 1 == tile_count))
      return;
  }
}

static void ptx_emit_tensor_residency_mma_native(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const char *residency_name, size_t per_tile) {
  int subtile_count = profile->m_tiles * profile->n_tiles;
  int accumulator_count = subtile_count * profile->accumulator_registers;
  int estimated_peak = accumulator_count + profile->a_registers +
                       profile->b_registers;
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  PtxTensorResidency *group =
      ptx_tensor_residency_find(fn, in->tensor_residency_id);
  if (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START) {
    if (group) {
      fn_error(fn, "PTX tensor residency group %u has multiple starts",
               in->tensor_residency_id);
      return;
    }
    group = ptx_tensor_residency_add(fn, in->tensor_residency_id);
    if (!group) {
      fn_error(fn, "PTX could not allocate tensor residency group %u",
               in->tensor_residency_id);
      return;
    }
    group->scope = in->tensor_residency_scope;
    group->resident = estimated_peak <= tuple_budget;
    group->tuple_peak = estimated_peak;
    if (!group->resident) {
      sb_printf(
          &fn->body,
          "\t// mtlc.%s replay native-mma %s group=%u tuple_peak=%d budget=%d\n",
          residency_name, ptx_mma_kind_name(profile), group->id,
          estimated_peak, tuple_budget);
      ptx_emit_tensor_mma_single(fn, in);
      return;
    }
    group->accumulator_class = PC_F32;
    group->accumulator_base = fn->count[PC_F32];
    group->accumulator_count = accumulator_count;
    for (int i = 0; i < accumulator_count; i++) new_reg(fn, PC_F32);
    sb_printf(
        &fn->body,
        "\t// mtlc.%s resident native-mma %s group=%u subtiles=%d tuple_peak=%d budget=%d\n",
        residency_name, ptx_mma_kind_name(profile), group->id,
        subtile_count, estimated_peak, tuple_budget);
  } else if (!group) {
    fn_error(fn, "PTX tensor residency update %u precedes its start",
             in->tensor_residency_id);
    return;
  } else if (group->scope != in->tensor_residency_scope) {
    fn_error(fn, "PTX tensor residency group %u changed scope", group->id);
    return;
  } else if (!group->resident) {
    ptx_emit_tensor_mma_single(fn, in);
    return;
  }
  if (group->accumulator_class != PC_F32 ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency group %u changed native MMA profile",
             group->id);
    return;
  }

  PtxMmaTileMemory memory;
  if (!ptx_mma_prepare_tile_memory(fn, in, profile, 0, per_tile, &memory)) {
    fn_error(fn,
             "PTX native tensor residency has invalid pointers or strides");
    return;
  }
  char lane[24], lane_group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), lane_group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", lane_group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);
  for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
    PtxMmaSparseAFragment sparse_a;
    PtxMmaSparseAFragment *sparse_a_ptr = NULL;
    if (ptx_mma_profile_is_sparse(profile)) {
      ptx_mma_prepare_sparse_f16_a(
          fn, in, &memory, lane_group, thread, (unsigned)m_tile * 16u,
          &sparse_a);
      sparse_a_ptr = &sparse_a;
    }
    for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
      int subtile = m_tile * profile->n_tiles + n_tile;
      ptx_emit_mma_native_subtile(
          fn, in, profile, &memory, lane_group, thread, sparse_a_ptr,
          (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
          group->accumulator_base +
              subtile * profile->accumulator_registers,
          in->tensor_residency_role == IR_TENSOR_RESIDENCY_START, 0);
    }
  }
}

static void ptx_emit_tensor_residency_commit_native(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    PtxTensorResidency *group, size_t per_tile) {
  int accumulator_count = profile->m_tiles * profile->n_tiles *
                          profile->accumulator_registers;
  if (group->accumulator_class != PC_F32 ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency commit changed native MMA profile");
    return;
  }
  PtxVal dv = operand_desc(fn, &in->arguments[3]);
  const char *dspace = ptx_wmma_space(dv);
  if (!dv.is_ptr || !dspace) {
    fn_error(fn,
             "PTX native tensor residency commit requires a writable tile pointer");
    return;
  }
  char dp[24], strides[4][24];
  use_as(fn, &in->arguments[3], PC_B64, dp);
  if (!ptx_tensor_stride_registers(fn, in, 0, per_tile, strides)) {
    fn_error(fn, "PTX native tensor residency commit has inconsistent strides");
    return;
  }
  char lane[24], lane_group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), lane_group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", lane_group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);
  for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
    for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
      int subtile = m_tile * profile->n_tiles + n_tile;
      ptx_mma_store_f32_accumulator_subtile(
          fn, in, profile, dp, dspace, strides[3], lane_group, thread,
          (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
          group->accumulator_base +
              subtile * profile->accumulator_registers);
    }
  }
}

static void ptx_emit_tensor_residency_mma(PtxFn *fn,
                                          const IRInstruction *in) {
  const char *residency_name =
      in ? ptx_tensor_residency_name(in->tensor_residency_scope) : NULL;
  if (!fn || !in || in->tensor_residency_id == 0 ||
      !residency_name ||
      (in->tensor_residency_role != IR_TENSOR_RESIDENCY_START &&
       in->tensor_residency_role != IR_TENSOR_RESIDENCY_UPDATE)) {
    fn_error(fn, "PTX received an invalid tensor residency MMA");
    return;
  }
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (!per_tile || in->argument_count != per_tile) {
    fn_error(fn, "PTX tensor residency operand count is inconsistent");
    return;
  }
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn, "PTX native tensor residency cannot lower m%un%uk%u: %s",
               (unsigned)IR_TENSOR_MMA(in).m,
               (unsigned)IR_TENSOR_MMA(in).n,
               (unsigned)IR_TENSOR_MMA(in).k, native_reason);
      return;
    }
    ptx_emit_tensor_residency_mma_native(
        fn, in, &native_profile, residency_name, per_tile);
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor residency cannot lower m%un%uk%u: %s",
             (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
             (unsigned)IR_TENSOR_MMA(in).k, reason);
    return;
  }
  int subtile_count = profile.m_tiles * profile.n_tiles;
  int accumulator_count = subtile_count * profile.d_registers;
  int estimated_peak = accumulator_count + profile.a_registers +
                       profile.b_registers + profile.c_registers;
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  PtxTensorResidency *group = ptx_tensor_residency_find(
      fn, in->tensor_residency_id);
  if (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START) {
    if (group) {
      fn_error(fn, "PTX tensor residency group %u has multiple starts",
               in->tensor_residency_id);
      return;
    }
    group = ptx_tensor_residency_add(fn, in->tensor_residency_id);
    if (!group) {
      fn_error(fn, "PTX could not allocate tensor residency group %u",
               in->tensor_residency_id);
      return;
    }
    group->scope = in->tensor_residency_scope;
    group->resident = estimated_peak <= tuple_budget;
    group->tuple_peak = estimated_peak;
    if (!group->resident) {
      sb_printf(&fn->body,
                "\t// mtlc.%s replay group=%u tuple_peak=%d budget=%d\n",
                residency_name, group->id, estimated_peak, tuple_budget);
      ptx_emit_tensor_mma_single(fn, in);
      return;
    }
    group->accumulator_class = profile.d_class;
    group->accumulator_base = fn->count[profile.d_class];
    group->accumulator_count = accumulator_count;
    for (int i = 0; i < accumulator_count; i++)
      new_reg(fn, profile.d_class);
    if (subtile_count == 1)
      sb_printf(&fn->body,
                "\t// mtlc.%s resident group=%u tuple_peak=%d budget=%d\n",
                residency_name, group->id, estimated_peak, tuple_budget);
    else
      sb_printf(
          &fn->body,
          "\t// mtlc.%s resident group=%u subtiles=%d tuple_peak=%d budget=%d\n",
          residency_name, group->id, subtile_count, estimated_peak,
          tuple_budget);
  } else if (!group) {
    fn_error(fn, "PTX tensor residency update %u precedes its start",
             in->tensor_residency_id);
    return;
  } else if (group->scope != in->tensor_residency_scope) {
    fn_error(fn, "PTX tensor residency group %u changed scope", group->id);
    return;
  } else if (!group->resident) {
    ptx_emit_tensor_mma_single(fn, in);
    return;
  }

  if (group->accumulator_class != profile.d_class ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency group %u changed accumulator profile",
             group->id);
    return;
  }
  (void)ptx_emit_wmma_tiled_tile(
      fn, in, &profile, 0, per_tile, group->accumulator_base,
      in->tensor_residency_role == IR_TENSOR_RESIDENCY_START, 0);
}

static void ptx_emit_tensor_residency_commit(PtxFn *fn,
                                             const IRInstruction *in) {
  if (!fn || !in ||
      in->tensor_residency_role != IR_TENSOR_RESIDENCY_COMMIT ||
      in->tensor_residency_id == 0 ||
      !ptx_tensor_residency_name(in->tensor_residency_scope)) {
    fn_error(fn, "PTX received an invalid tensor residency commit");
    return;
  }
  PtxTensorResidency *group = ptx_tensor_residency_find(
      fn, in->tensor_residency_id);
  if (!group) {
    fn_error(fn, "PTX tensor residency commit %u has no start",
             in->tensor_residency_id);
    return;
  }
  if (group->scope != in->tensor_residency_scope) {
    fn_error(fn, "PTX tensor residency commit %u changed scope", group->id);
    return;
  }
  if (!group->resident) return;
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!per_tile || in->argument_count != per_tile ||
        !ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn,
               "PTX native tensor residency commit has an invalid profile");
      return;
    }
    ptx_emit_tensor_residency_commit_native(
        fn, in, &native_profile, group, per_tile);
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!per_tile || in->argument_count != per_tile ||
      !ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor residency commit has an invalid profile");
    return;
  }
  int accumulator_count = profile.m_tiles * profile.n_tiles *
                          profile.d_registers;
  if (group->accumulator_class != profile.d_class ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency commit changed accumulator profile");
    return;
  }
  (void)ptx_emit_wmma_tiled_store(fn, in, &profile, 0, per_tile,
                                   group->accumulator_base);
}

static void ptx_tensor_epilogue_load(PtxFn *fn, MtlcTensorElement element,
                                     const char *space,
                                     const char *address,
                                     const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
      element == MTLC_TENSOR_ELEMENT_BFLOAT16) {
    char storage[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, storage,
              address);
    sb_printf(&fn->body, "\tcvt.f32.%s %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
              value, storage);
  } else {
    sb_printf(&fn->body, "\tld%s.%s %s, [%s];\n", space,
              element == MTLC_TENSOR_ELEMENT_FLOAT64 ? "f64" : "f32",
              value, address);
  }
}

static void ptx_tensor_epilogue_store(PtxFn *fn, MtlcTensorElement element,
                                      const char *space,
                                      const char *address,
                                      const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
      element == MTLC_TENSOR_ELEMENT_BFLOAT16) {
    char storage[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    sb_printf(&fn->body, "\tcvt.rn.%s.f32 %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
              storage, value);
    sb_printf(&fn->body, "\tst%s.b16 [%s], %s;\n", space, address,
              storage);
  } else {
    sb_printf(&fn->body, "\tst%s.%s [%s], %s;\n", space,
              element == MTLC_TENSOR_ELEMENT_FLOAT64 ? "f64" : "f32",
              address, value);
  }
}

static void ptx_tensor_epilogue_address(
    PtxFn *fn, const char *base, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    unsigned element_bytes, char address[24]) {
  char linear[24], byte_offset[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, row,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              column);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, column,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear, row);
  }
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", byte_offset,
            linear, element_bytes);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
}

static void ptx_emit_tensor_epilogue(PtxFn *fn,
                                     const IRInstruction *in) {
  if (!fn || !in || in->op != IR_OP_TENSOR_EPILOGUE ||
      !ir_tensor_epilogue_desc_valid(&IR_TENSOR_EPILOGUE(in))) {
    if (fn) fn_error(fn, "PTX received an invalid tensor epilogue");
    return;
  }
  const MtlcTensorEpilogueDesc *desc = &IR_TENSOR_EPILOGUE(in);
  size_t expected = ir_tensor_epilogue_operand_count(desc);
  if (!expected || in->argument_count != expected) {
    fn_error(fn, "PTX tensor epilogue has an invalid operand count");
    return;
  }
  if (desc->element == MTLC_TENSOR_ELEMENT_FLOAT16 &&
      (fn->target_arch < 53 || fn->isa_major < 4 ||
       (fn->isa_major == 4 && fn->isa_minor < 2))) {
    fn_error(fn,
             "PTX tensor epilogue f16 conversion requires PTX 4.2 and sm_53");
    return;
  }
  if (desc->element == MTLC_TENSOR_ELEMENT_BFLOAT16 &&
      (fn->target_arch < 80 || fn->isa_major < 7 ||
       (fn->isa_major == 7 && fn->isa_minor < 1))) {
    fn_error(fn,
             "PTX tensor epilogue bf16 conversion requires PTX 7.1 and sm_80");
    return;
  }
  if (desc->scope == MTLC_MEMORY_SCOPE_SUBGROUP &&
      (fn->target_arch < 70 || fn->isa_major < 6)) {
    fn_error(fn,
             "PTX subgroup tensor epilogue ordering requires PTX 6.0 and sm_70");
    return;
  }

  size_t argument = 0;
  PtxVal destination_desc = operand_desc(fn, &in->arguments[argument]);
  const char *destination_space = ptx_wmma_space(destination_desc);
  char destination[24];
  if (!destination_desc.is_ptr || !destination_space) {
    fn_error(fn,
             "PTX tensor epilogue destination must be a generic, global, or workgroup pointer");
    return;
  }
  use_as(fn, &in->arguments[argument++], PC_B64, destination);

  const char *bias_space = NULL;
  char bias[24] = {0};
  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE) {
    PtxVal bias_desc = operand_desc(fn, &in->arguments[argument]);
    bias_space = ptx_wmma_space(bias_desc);
    if (!bias_desc.is_ptr || !bias_space) {
      fn_error(fn,
               "PTX tensor epilogue bias must be a generic, global, or workgroup pointer");
      return;
    }
    use_as(fn, &in->arguments[argument++], PC_B64, bias);
  }

  PtxClass compute_class = desc->element == MTLC_TENSOR_ELEMENT_FLOAT64
                               ? PC_F64
                               : PC_F32;
  const char *compute_type = compute_class == PC_F64 ? "f64" : "f32";
  char alpha[24] = {0}, beta[24] = {0};
  char clamp_minimum[24] = {0}, clamp_maximum[24] = {0};
  if (desc->scale_output)
    use_as(fn, &in->arguments[argument++], compute_class, alpha);
  if (desc->scale_bias)
    use_as(fn, &in->arguments[argument++], compute_class, beta);
  if (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) {
    use_as(fn, &in->arguments[argument++], compute_class, clamp_minimum);
    use_as(fn, &in->arguments[argument++], compute_class, clamp_maximum);
  }

  char leading_dimension[24], bias_leading_dimension[24] = {0};
  if (desc->leading_dimension == 0) {
    use_as(fn, &in->arguments[argument++], PC_B32, leading_dimension);
  } else {
    reg_name(PC_B32, new_reg(fn, PC_B32), leading_dimension);
    sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", leading_dimension,
              desc->leading_dimension);
  }
  if (desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
    if (desc->bias_leading_dimension == 0) {
      use_as(fn, &in->arguments[argument++], PC_B32,
             bias_leading_dimension);
    } else {
      reg_name(PC_B32, new_reg(fn, PC_B32), bias_leading_dimension);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", bias_leading_dimension,
                desc->bias_leading_dimension);
    }
  }
  if (argument != expected) {
    fn_error(fn, "PTX tensor epilogue operand decoding is inconsistent");
    return;
  }

  char linear[24], participants[24];
  char row[24], column[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  reg_name(PC_B32, new_reg(fn, PC_B32), participants);
  reg_name(PC_B32, new_reg(fn, PC_B32), row);
  reg_name(PC_B32, new_reg(fn, PC_B32), column);
  char mask[24] = {0};
  if (desc->scope == MTLC_MEMORY_SCOPE_SUBGROUP) {
    char below[24], active_below[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), mask);
    reg_name(PC_B32, new_reg(fn, PC_B32), below);
    reg_name(PC_B32, new_reg(fn, PC_B32), active_below);
    sb_printf(&fn->body,
              "\tactivemask.b32 %s;\n"
              "\tmov.u32 %s, %%lanemask_lt;\n"
              "\tand.b32 %s, %s, %s;\n"
              "\tpopc.b32 %s, %s;\n"
              "\tpopc.b32 %s, %s;\n"
              "\tbar.warp.sync %s;\n",
              mask, below, active_below, mask, below, linear, active_below,
              participants, mask, mask);
  } else {
    char tid_x[24], tid_y[24], tid_z[24];
    char ntid_x[24], ntid_y[24], ntid_z[24], scratch[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), tid_x);
    reg_name(PC_B32, new_reg(fn, PC_B32), tid_y);
    reg_name(PC_B32, new_reg(fn, PC_B32), tid_z);
    reg_name(PC_B32, new_reg(fn, PC_B32), ntid_x);
    reg_name(PC_B32, new_reg(fn, PC_B32), ntid_y);
    reg_name(PC_B32, new_reg(fn, PC_B32), ntid_z);
    reg_name(PC_B32, new_reg(fn, PC_B32), scratch);
    sb_printf(&fn->body,
              "\tmov.u32 %s, %%tid.x;\n"
              "\tmov.u32 %s, %%tid.y;\n"
              "\tmov.u32 %s, %%tid.z;\n"
              "\tmov.u32 %s, %%ntid.x;\n"
              "\tmov.u32 %s, %%ntid.y;\n"
              "\tmov.u32 %s, %%ntid.z;\n"
              "\tmad.lo.u32 %s, %s, %s, %s;\n"
              "\tmad.lo.u32 %s, %s, %s, %s;\n"
              "\tmul.lo.u32 %s, %s, %s;\n"
              "\tmul.lo.u32 %s, %s, %s;\n"
              "\tbar.sync 0;\n",
              tid_x, tid_y, tid_z, ntid_x, ntid_y, ntid_z, scratch, tid_z,
              ntid_y, tid_y, linear, scratch, ntid_x, tid_x, participants,
              ntid_x, ntid_y, participants, participants, ntid_z);
  }

  size_t label_id = fn->call_count++;
  unsigned element_bytes = desc->element == MTLC_TENSOR_ELEMENT_FLOAT64
                               ? 8u
                           : (desc->element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
                              desc->element == MTLC_TENSOR_ELEMENT_BFLOAT16)
                               ? 2u
                               : 4u;
  unsigned long tile_elements =
      (unsigned long)desc->m * (unsigned long)desc->n;
  char done[24], destination_address[24], value[24];
  reg_name(PC_PRED, new_reg(fn, PC_PRED), done);
  reg_name(compute_class, new_reg(fn, compute_class), value);
  sb_printf(
      &fn->body,
      "\t// mtlc.tensor_epilogue cooperative-memory m=%u n=%u element=%d bias=%d activation=%d scope=%s\n"
      "mtlc_tensor_epilogue_%llu_loop:\n"
      "\tsetp.ge.u32 %s, %s, %lu;\n"
      "\t@%s bra mtlc_tensor_epilogue_%llu_finish;\n"
      "\tdiv.u32 %s, %s, %u;\n"
      "\trem.u32 %s, %s, %u;\n",
      (unsigned)desc->m, (unsigned)desc->n, (int)desc->element,
      (int)desc->bias_mode, (int)desc->activation,
      desc->scope == MTLC_MEMORY_SCOPE_WORKGROUP ? "workgroup" : "subgroup",
      (unsigned long long)label_id, done, linear, tile_elements, done,
      (unsigned long long)label_id, row, linear, (unsigned)desc->n, column,
      linear, (unsigned)desc->n);
  ptx_tensor_epilogue_address(fn, destination, row, column,
                              leading_dimension, desc->layout, element_bytes,
                              destination_address);
  ptx_tensor_epilogue_load(fn, desc->element, destination_space,
                           destination_address, value);
  if (desc->scale_output)
    sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n", compute_type, value,
              value, alpha);

  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE) {
    char bias_address[24], bias_value[24];
    reg_name(compute_class, new_reg(fn, compute_class), bias_value);
    if (desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
      ptx_tensor_epilogue_address(
          fn, bias, row, column, bias_leading_dimension, desc->bias_layout,
          element_bytes, bias_address);
    } else {
      char bias_offset[24];
      const char *bias_index =
          desc->bias_mode == MTLC_TENSOR_BIAS_PER_ROW ? row : column;
      reg_name(PC_B64, new_reg(fn, PC_B64), bias_offset);
      sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", bias_offset,
                bias_index, element_bytes);
      reg_name(PC_B64, new_reg(fn, PC_B64), bias_address);
      sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", bias_address, bias,
                bias_offset);
    }
    ptx_tensor_epilogue_load(fn, desc->element, bias_space, bias_address,
                             bias_value);
    if (desc->scale_bias)
      sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n", compute_type,
                bias_value, bias_value, beta);
    sb_printf(&fn->body, "\tadd.rn.%s %s, %s, %s;\n", compute_type, value,
              value, bias_value);
  }

  if (desc->activation == MTLC_TENSOR_ACTIVATION_RELU) {
    char negative[24];
    const char *positive_zero =
        compute_class == PC_F64 ? "0d0000000000000000" : "0f00000000";
    reg_name(PC_PRED, new_reg(fn, PC_PRED), negative);
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              compute_type, negative, value, positive_zero, compute_type,
              value, positive_zero, value, negative);
  } else if (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) {
    char outside[24];
    reg_name(PC_PRED, new_reg(fn, PC_PRED), outside);
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n"
              "\tsetp.gt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              compute_type, outside, value, clamp_minimum, compute_type,
              value, clamp_minimum, value, outside, compute_type, outside,
              value, clamp_maximum, compute_type, value, clamp_maximum, value,
              outside);
  }
  ptx_tensor_epilogue_store(fn, desc->element, destination_space,
                            destination_address, value);
  sb_printf(&fn->body,
            "\tadd.u32 %s, %s, %s;\n"
            "\tbra mtlc_tensor_epilogue_%llu_loop;\n"
            "mtlc_tensor_epilogue_%llu_finish:\n",
            linear, linear, participants, (unsigned long long)label_id,
            (unsigned long long)label_id);
  if (desc->scope == MTLC_MEMORY_SCOPE_SUBGROUP)
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  else
    sb_puts(&fn->body, "\tbar.sync 0;\n");
}

typedef struct {
  const IRInstruction *instruction;
  const MtlcTensorEpilogueDesc *desc;
  const char *bias_space;
  PtxClass compute_class;
  const char *compute_type;
  char bias[24];
  char bias_value[24];
  char alpha[24];
  char beta[24];
  char clamp_minimum[24];
  char clamp_maximum[24];
  char bias_leading_dimension[24];
  char predicate[24];
} PtxResidentEpilogue;

static size_t ptx_tensor_mma_stride_operand_index(
    const MtlcTensorMmaDesc *desc, unsigned requested) {
  if (!desc) return SIZE_MAX;
  size_t index =
      4u + (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
      (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
      (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  unsigned mask = ir_tensor_mma_runtime_stride_mask(desc);
  for (unsigned bit = MTLC_TENSOR_RUNTIME_STRIDE_A;
       bit <= MTLC_TENSOR_RUNTIME_STRIDE_D; bit <<= 1) {
    if (!(mask & bit)) continue;
    if (bit == requested) return index;
    index++;
  }
  return SIZE_MAX;
}

static size_t ptx_tensor_epilogue_stride_operand_index(
    const MtlcTensorEpilogueDesc *desc) {
  if (!desc || desc->leading_dimension != 0) return SIZE_MAX;
  size_t index = 1u;
  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE) index++;
  if (desc->scale_output) index++;
  if (desc->scale_bias) index++;
  if (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) index += 2u;
  return index;
}

static int ptx_tensor_epilogue_handoff_compatible(
    const IRInstruction *mma, const IRInstruction *epilogue) {
  if (!mma || !epilogue ||
      (mma->op != IR_OP_TENSOR_MMA && mma->op != IR_OP_TENSOR_COMMIT) ||
      epilogue->op != IR_OP_TENSOR_EPILOGUE ||
      !ir_tensor_mma_desc_valid(&IR_TENSOR_MMA(mma)) ||
      !ir_tensor_epilogue_desc_valid(&IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  const MtlcTensorMmaDesc *md = &IR_TENSOR_MMA(mma);
  const MtlcTensorEpilogueDesc *ed = &IR_TENSOR_EPILOGUE(epilogue);
  if (md->m != ed->m || md->n != ed->n ||
      md->result_element != ed->element || md->d_layout != ed->layout ||
      md->scope != ed->scope)
    return 0;
  size_t per_tile = ir_tensor_mma_operand_count(md);
  size_t tile_count = mma->op == IR_OP_TENSOR_MMA
                          ? ir_tensor_mma_instruction_count(mma)
                          : 1u;
  size_t epilogue_count = ir_tensor_epilogue_operand_count(ed);
  if (!per_tile || !tile_count ||
      tile_count > SIZE_MAX / per_tile ||
      mma->argument_count != tile_count * per_tile ||
      !epilogue_count || epilogue->argument_count != epilogue_count)
    return 0;
  size_t base = (tile_count - 1u) * per_tile;
  if (!ir_operand_same(&mma->arguments[base + 3u],
                       &epilogue->arguments[0]))
    return 0;
  if (md->d_leading_dimension != ed->leading_dimension) return 0;
  if (md->d_leading_dimension == 0) {
    size_t mma_stride =
        ptx_tensor_mma_stride_operand_index(md,
                                            MTLC_TENSOR_RUNTIME_STRIDE_D);
    size_t epilogue_stride =
        ptx_tensor_epilogue_stride_operand_index(ed);
    if (mma_stride == SIZE_MAX || epilogue_stride == SIZE_MAX ||
        !ir_operand_same(&mma->arguments[base + mma_stride],
                         &epilogue->arguments[epilogue_stride]))
      return 0;
  }
  return 1;
}

static const IRInstruction *ptx_following_tensor_epilogue(
    const IRFunction *function, size_t instruction_index,
    size_t *epilogue_index) {
  if (!function || instruction_index >= function->instruction_count)
    return NULL;
  for (size_t i = instruction_index + 1u;
       i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_TENSOR_EPILOGUE) return NULL;
    if (epilogue_index) *epilogue_index = i;
    return candidate;
  }
  return NULL;
}

/* A loop residency commit is emitted in its own edge block:
 *
 *   tensor_commit
 *   jump original_exit
 * original_exit:
 *   tensor_epilogue
 *
 * The epilogue may consume the resident accumulator before the jump only when
 * that jump is the label's sole control-flow predecessor and the label has no
 * fallthrough predecessor. An outer guard targeting original_exit makes the
 * predecessor count exceed one and deliberately selects memory replay. */
static const IRInstruction *ptx_loop_exit_tensor_epilogue(
    const IRFunction *function, size_t commit_index,
    size_t *epilogue_index) {
  if (!function || commit_index >= function->instruction_count) return NULL;
  const IRInstruction *commit = &function->instructions[commit_index];
  if (commit->op != IR_OP_TENSOR_COMMIT ||
      commit->tensor_residency_scope != IR_TENSOR_RESIDENCY_SCOPE_LOOP)
    return NULL;

  size_t jump_index = SIZE_MAX;
  for (size_t i = commit_index + 1u; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_JUMP || !candidate->text) return NULL;
    jump_index = i;
    break;
  }
  if (jump_index == SIZE_MAX) return NULL;
  const char *exit_label = function->instructions[jump_index].text;

  size_t label_index = SIZE_MAX;
  for (size_t i = jump_index + 1u; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_LABEL || !candidate->text ||
        strcmp(candidate->text, exit_label) != 0)
      return NULL;
    label_index = i;
    break;
  }
  if (label_index == SIZE_MAX) return NULL;

  size_t predecessor_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if ((candidate->op == IR_OP_JUMP ||
         candidate->op == IR_OP_BRANCH_ZERO ||
         candidate->op == IR_OP_BRANCH_EQ) &&
        candidate->text && strcmp(candidate->text, exit_label) == 0) {
      predecessor_count++;
      if (i != jump_index) return NULL;
    }
  }
  if (predecessor_count != 1u) return NULL;

  for (size_t i = label_index + 1u; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_TENSOR_EPILOGUE) return NULL;
    if (epilogue_index) *epilogue_index = i;
    return candidate;
  }
  return NULL;
}

static int ptx_tensor_epilogue_was_consumed(
    const PtxFn *fn, const IRInstruction *epilogue) {
  if (!fn || !epilogue) return 0;
  for (size_t i = 0; i < fn->tensor_residency_count; i++) {
    if (fn->tensor_residencies[i].consumed_epilogue == epilogue) return 1;
  }
  return 0;
}

static int ptx_prepare_resident_epilogue(PtxFn *fn,
                                         const IRInstruction *epilogue,
                                         PtxResidentEpilogue *state) {
  if (!fn || !epilogue || !state ||
      epilogue->op != IR_OP_TENSOR_EPILOGUE ||
      !ir_tensor_epilogue_desc_valid(&IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  memset(state, 0, sizeof(*state));
  state->instruction = epilogue;
  state->desc = &IR_TENSOR_EPILOGUE(epilogue);
  state->compute_class =
      state->desc->element == MTLC_TENSOR_ELEMENT_FLOAT64 ? PC_F64 : PC_F32;
  state->compute_type = state->compute_class == PC_F64 ? "f64" : "f32";
  size_t argument = 1u; /* The compatible MMA/commit already owns D. */
  if (state->desc->bias_mode != MTLC_TENSOR_BIAS_NONE) {
    PtxVal bias_desc = operand_desc(fn, &epilogue->arguments[argument]);
    state->bias_space = ptx_wmma_space(bias_desc);
    if (!bias_desc.is_ptr || !state->bias_space) return 0;
    use_as(fn, &epilogue->arguments[argument++], PC_B64, state->bias);
    reg_name(state->compute_class, new_reg(fn, state->compute_class),
             state->bias_value);
  }
  if (state->desc->scale_output)
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->alpha);
  if (state->desc->scale_bias)
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->beta);
  if (state->desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) {
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->clamp_minimum);
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->clamp_maximum);
  }
  if (state->desc->leading_dimension == 0) argument++;
  if (state->desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
    if (state->desc->bias_leading_dimension == 0) {
      use_as(fn, &epilogue->arguments[argument++], PC_B32,
             state->bias_leading_dimension);
    } else {
      reg_name(PC_B32, new_reg(fn, PC_B32),
               state->bias_leading_dimension);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n",
                state->bias_leading_dimension,
                state->desc->bias_leading_dimension);
    }
  }
  if (argument != epilogue->argument_count) return 0;
  if (state->desc->activation != MTLC_TENSOR_ACTIVATION_IDENTITY)
    reg_name(PC_PRED, new_reg(fn, PC_PRED), state->predicate);
  return 1;
}

static void ptx_emit_resident_epilogue_barrier(PtxFn *fn, char mask[24]) {
  reg_name(PC_B32, new_reg(fn, PC_B32), mask);
  sb_printf(&fn->body,
            "\tactivemask.b32 %s;\n"
            "\tbar.warp.sync %s;\n",
            mask, mask);
}

static void ptx_apply_resident_epilogue_value(
    PtxFn *fn, const PtxResidentEpilogue *state, const char *value,
    int have_bias) {
  if (state->desc->scale_output)
    sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n",
              state->compute_type, value, value, state->alpha);
  if (have_bias) {
    if (state->desc->scale_bias)
      sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n",
                state->compute_type, state->bias_value, state->bias_value,
                state->beta);
    sb_printf(&fn->body, "\tadd.rn.%s %s, %s, %s;\n",
              state->compute_type, value, value, state->bias_value);
  }
  if (state->desc->activation == MTLC_TENSOR_ACTIVATION_RELU) {
    const char *positive_zero =
        state->compute_class == PC_F64 ? "0d0000000000000000"
                                       : "0f00000000";
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              state->compute_type, state->predicate, value, positive_zero,
              state->compute_type, value, positive_zero, value,
              state->predicate);
  } else if (state->desc->activation ==
             MTLC_TENSOR_ACTIVATION_CLAMP) {
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n"
              "\tsetp.gt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              state->compute_type, state->predicate, value,
              state->clamp_minimum, state->compute_type, value,
              state->clamp_minimum, value, state->predicate,
              state->compute_type, state->predicate, value,
              state->clamp_maximum, state->compute_type, value,
              state->clamp_maximum, value, state->predicate);
  }
}

static int ptx_resident_epilogue_tuple_cost(
    const MtlcTensorEpilogueDesc *desc) {
  if (!desc) return INT_MAX;
  int cost = desc->activation == MTLC_TENSOR_ACTIVATION_IDENTITY ? 0 : 1;
  cost += desc->scale_output ? 1 : 0;
  cost += desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP ? 2 : 0;
  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE)
    cost += 7 + (desc->scale_bias ? 1 : 0);
  return cost;
}

static int ptx_stable_resident_epilogue_capable(
    const PtxWmmaProfile *profile, const MtlcTensorEpilogueDesc *desc) {
  if (!profile || !desc || desc->bias_mode != MTLC_TENSOR_BIAS_NONE)
    return 0;
  return (desc->element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
          profile->d_class == PC_F32) ||
         (desc->element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
          profile->d_class == PC_F64);
}

static int ptx_native_resident_epilogue_capable(
    const PtxMmaProfile *profile, const MtlcTensorEpilogueDesc *desc) {
  return profile && desc && profile->accumulator_registers > 0 &&
         desc->element == MTLC_TENSOR_ELEMENT_FLOAT32;
}

static int ptx_apply_stable_resident_epilogue(
    PtxFn *fn, const IRInstruction *epilogue,
    const PtxWmmaProfile *profile, int accumulator_base,
    int accumulator_count, char mask[24]) {
  if (!ptx_stable_resident_epilogue_capable(
          profile, &IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  PtxResidentEpilogue state;
  if (!ptx_prepare_resident_epilogue(fn, epilogue, &state)) return 0;
  ptx_emit_resident_epilogue_barrier(fn, mask);
  for (int i = 0; i < accumulator_count; i++) {
    char value[24];
    reg_name(profile->d_class, accumulator_base + i, value);
    ptx_apply_resident_epilogue_value(fn, &state, value, 0);
  }
  return !fn->error;
}

static void ptx_native_epilogue_bias_address(
    PtxFn *fn, const PtxResidentEpilogue *state, PtxMmaCoordinate row,
    PtxMmaCoordinate column, const char *group, const char *thread,
    char address[24]) {
  if (state->desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
    ptx_mma_emit_address(fn, state->bias, state->bias_leading_dimension,
                         state->desc->bias_layout, 0, row, column, 4,
                         group, thread, address);
    return;
  }
  char index[24], byte_offset[24];
  ptx_mma_emit_coordinate(
      fn, state->desc->bias_mode == MTLC_TENSOR_BIAS_PER_ROW ? row : column,
      group, thread, index);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, 4;\n", byte_offset,
            index);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address,
            state->bias, byte_offset);
}

static void ptx_apply_store_native_resident_epilogue_subtile(
    PtxFn *fn, const IRInstruction *mma, const PtxMmaProfile *profile,
    const PtxResidentEpilogue *state, const char *dp, const char *dspace,
    const char *d_stride, const char *group, const char *thread,
    unsigned m_offset, unsigned n_offset, int accumulator_base) {
  for (int element = 0; element < profile->accumulator_registers; element++) {
    PtxMmaCoordinate row = {
        1, 0, m_offset + (element >= 2 ? 8u : 0u)};
    PtxMmaCoordinate column = {
        0, 2, n_offset + (unsigned)(element & 1)};
    char value[24];
    reg_name(PC_F32, accumulator_base + element, value);
    int have_bias = state->desc->bias_mode != MTLC_TENSOR_BIAS_NONE;
    if (have_bias) {
      char bias_address[24];
      ptx_native_epilogue_bias_address(fn, state, row, column, group, thread,
                                       bias_address);
      sb_printf(&fn->body, "\tld%s.f32 %s, [%s];\n", state->bias_space,
                state->bias_value, bias_address);
    }
    ptx_apply_resident_epilogue_value(fn, state, value, have_bias);
    ptx_mma_store_f32(fn, dp, d_stride, IR_TENSOR_MMA(mma).d_layout, dspace,
                      row, column, group, thread,
                      accumulator_base + element);
  }
}

static int ptx_try_emit_tensor_mma_resident_epilogue(
    PtxFn *fn, const IRInstruction *mma, const IRInstruction *epilogue) {
  if (!fn || !mma || !epilogue || mma->op != IR_OP_TENSOR_MMA ||
      mma->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
      mma->tensor_residency_id != 0 ||
      !ptx_tensor_epilogue_handoff_compatible(mma, epilogue))
    return 0;
  size_t tile_count = ir_tensor_mma_instruction_count(mma);
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(mma));
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  int epilogue_cost =
      ptx_resident_epilogue_tuple_cost(&IR_TENSOR_EPILOGUE(epilogue));

  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(mma))) {
    PtxMmaProfile profile;
    char reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(mma), &profile, reason,
                                sizeof(reason)) ||
        !ptx_native_resident_epilogue_capable(
            &profile, &IR_TENSOR_EPILOGUE(epilogue)))
      return 0;
    int subtile_count = profile.m_tiles * profile.n_tiles;
    int accumulator_count =
        subtile_count * profile.accumulator_registers;
    int estimated_peak = accumulator_count + profile.a_registers +
                         profile.b_registers + epilogue_cost;
    if (estimated_peak > tuple_budget) return 0;
    int accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < accumulator_count; i++) new_reg(fn, PC_F32);
    char lane[24], group[24], thread[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), lane);
    reg_name(PC_B32, new_reg(fn, PC_B32), group);
    reg_name(PC_B32, new_reg(fn, PC_B32), thread);
    sb_printf(
        &fn->body,
        "\t// mtlc.tensor_epilogue resident native-mma %s tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n"
        "\tmov.u32 %s, %%laneid;\n"
        "\tshr.u32 %s, %s, 2;\n"
        "\tand.b32 %s, %s, 3;\n",
        ptx_mma_kind_name(&profile), (unsigned long long)tile_count,
        subtile_count, estimated_peak, tuple_budget, lane, group, lane,
        thread, lane);
    for (size_t tile = 0; tile < tile_count; tile++) {
      size_t base = tile * per_tile;
      PtxMmaTileMemory memory;
      if (!ptx_mma_prepare_tile_memory(fn, mma, &profile, base, per_tile,
                                       &memory)) {
        fn_error(fn,
                 "PTX native resident epilogue has invalid tile memory");
        return 1;
      }
      for (int m = 0; m < profile.m_tiles; m++) {
        PtxMmaSparseAFragment sparse_a;
        PtxMmaSparseAFragment *sparse_a_ptr = NULL;
        if (ptx_mma_profile_is_sparse(&profile)) {
          ptx_mma_prepare_sparse_f16_a(
              fn, mma, &memory, group, thread, (unsigned)m * 16u, &sparse_a);
          sparse_a_ptr = &sparse_a;
        }
        for (int n = 0; n < profile.n_tiles; n++) {
          int subtile = m * profile.n_tiles + n;
          ptx_emit_mma_native_subtile(
              fn, mma, &profile, &memory, group, thread, sparse_a_ptr,
              (unsigned)m * 16u, (unsigned)n * 8u,
              accumulator_base +
                  subtile * profile.accumulator_registers,
              tile == 0, 0);
        }
      }
    }
    size_t final_base = (tile_count - 1u) * per_tile;
    PtxVal dv = operand_desc(fn, &mma->arguments[final_base + 3u]);
    const char *dspace = ptx_wmma_space(dv);
    char dp[24], strides[4][24], mask[24];
    if (!dv.is_ptr || !dspace) {
      fn_error(fn, "PTX resident tensor epilogue requires a writable D tile");
      return 1;
    }
    use_as(fn, &mma->arguments[final_base + 3u], PC_B64, dp);
    if (!ptx_tensor_stride_registers(fn, mma, final_base, per_tile,
                                     strides)) {
      fn_error(fn, "PTX resident tensor epilogue has inconsistent D stride");
      return 1;
    }
    PtxResidentEpilogue state;
    if (!ptx_prepare_resident_epilogue(fn, epilogue, &state)) {
      fn_error(fn, "PTX resident tensor epilogue has invalid operands");
      return 1;
    }
    ptx_emit_resident_epilogue_barrier(fn, mask);
    for (int m = 0; m < profile.m_tiles; m++) {
      for (int n = 0; n < profile.n_tiles; n++) {
        int subtile = m * profile.n_tiles + n;
        ptx_apply_store_native_resident_epilogue_subtile(
            fn, mma, &profile, &state, dp, dspace, strides[3], group, thread,
            (unsigned)m * 16u, (unsigned)n * 8u,
            accumulator_base +
                subtile * profile.accumulator_registers);
      }
    }
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
    return 1;
  }

  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(mma), &profile, reason,
                               sizeof(reason)) ||
      !ptx_stable_resident_epilogue_capable(
          &profile, &IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  int subtile_count = profile.m_tiles * profile.n_tiles;
  int accumulator_count = subtile_count * profile.d_registers;
  int estimated_peak = accumulator_count + profile.a_registers +
                       profile.b_registers + profile.c_registers +
                       epilogue_cost;
  if (estimated_peak > tuple_budget) return 0;
  int accumulator_base = fn->count[profile.d_class];
  for (int i = 0; i < accumulator_count; i++)
    new_reg(fn, profile.d_class);
  sb_printf(&fn->body,
            "\t// mtlc.tensor_epilogue resident stable-wmma tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
            (unsigned long long)tile_count, subtile_count, estimated_peak,
            tuple_budget);
  for (size_t tile = 0; tile < tile_count; tile++) {
    if (!ptx_emit_wmma_tiled_tile(
            fn, mma, &profile, tile * per_tile, per_tile, accumulator_base,
            tile == 0, 0))
      return 1;
  }
  char mask[24];
  if (!ptx_apply_stable_resident_epilogue(
          fn, epilogue, &profile, accumulator_base, accumulator_count, mask)) {
    fn_error(fn, "PTX could not apply a stable resident tensor epilogue");
    return 1;
  }
  size_t final_base = (tile_count - 1u) * per_tile;
  if (!ptx_emit_wmma_tiled_store(fn, mma, &profile, final_base, per_tile,
                                  accumulator_base))
    return 1;
  sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  return 1;
}

static int ptx_try_emit_tensor_commit_resident_epilogue(
    PtxFn *fn, const IRInstruction *commit,
    const IRInstruction *epilogue) {
  if (!fn || !commit || !epilogue ||
      commit->op != IR_OP_TENSOR_COMMIT ||
      !ptx_tensor_epilogue_handoff_compatible(commit, epilogue))
    return 0;
  PtxTensorResidency *group =
      ptx_tensor_residency_find(fn, commit->tensor_residency_id);
  if (!group || !group->resident) return 0;
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(commit));
  int total_peak =
      group->tuple_peak +
      ptx_resident_epilogue_tuple_cost(&IR_TENSOR_EPILOGUE(epilogue));
  if (total_peak > ptx_tensor_tuple_budget(fn)) return 0;

  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(commit))) {
    PtxMmaProfile profile;
    char reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(commit), &profile, reason,
                                sizeof(reason)) ||
        !ptx_native_resident_epilogue_capable(
            &profile, &IR_TENSOR_EPILOGUE(epilogue)))
      return 0;
    PtxVal dv = operand_desc(fn, &commit->arguments[3]);
    const char *dspace = ptx_wmma_space(dv);
    char dp[24], strides[4][24], lane[24], lane_group[24], thread[24];
    char mask[24];
    if (!dv.is_ptr || !dspace) return 0;
    use_as(fn, &commit->arguments[3], PC_B64, dp);
    if (!ptx_tensor_stride_registers(fn, commit, 0, per_tile, strides))
      return 0;
    reg_name(PC_B32, new_reg(fn, PC_B32), lane);
    reg_name(PC_B32, new_reg(fn, PC_B32), lane_group);
    reg_name(PC_B32, new_reg(fn, PC_B32), thread);
    sb_printf(&fn->body,
              "\t// mtlc.tensor_epilogue resident handoff group=%u native-mma %s tuple_peak=%d budget=%d\n"
              "\tmov.u32 %s, %%laneid;\n"
              "\tshr.u32 %s, %s, 2;\n"
              "\tand.b32 %s, %s, 3;\n",
              group->id, ptx_mma_kind_name(&profile), total_peak,
              ptx_tensor_tuple_budget(fn), lane, lane_group, lane, thread,
              lane);
    PtxResidentEpilogue state;
    if (!ptx_prepare_resident_epilogue(fn, epilogue, &state)) {
      fn_error(fn, "PTX resident tensor epilogue handoff has invalid operands");
      return 1;
    }
    ptx_emit_resident_epilogue_barrier(fn, mask);
    for (int m = 0; m < profile.m_tiles; m++) {
      for (int n = 0; n < profile.n_tiles; n++) {
        int subtile = m * profile.n_tiles + n;
        ptx_apply_store_native_resident_epilogue_subtile(
            fn, commit, &profile, &state, dp, dspace, strides[3],
            lane_group, thread, (unsigned)m * 16u, (unsigned)n * 8u,
            group->accumulator_base +
                subtile * profile.accumulator_registers);
      }
    }
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
    return 1;
  }

  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(commit), &profile, reason,
                               sizeof(reason)) ||
      !ptx_stable_resident_epilogue_capable(
          &profile, &IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  int accumulator_count =
      profile.m_tiles * profile.n_tiles * profile.d_registers;
  if (group->accumulator_class != profile.d_class ||
      group->accumulator_count != accumulator_count)
    return 0;
  sb_printf(&fn->body,
            "\t// mtlc.tensor_epilogue resident handoff group=%u stable-wmma tuple_peak=%d budget=%d\n",
            group->id, total_peak, ptx_tensor_tuple_budget(fn));
  char mask[24];
  if (!ptx_apply_stable_resident_epilogue(
          fn, epilogue, &profile, group->accumulator_base,
          accumulator_count, mask)) {
    fn_error(fn, "PTX could not apply a stable resident epilogue handoff");
    return 1;
  }
  if (!ptx_emit_wmma_tiled_store(fn, commit, &profile, 0, per_tile,
                                  group->accumulator_base))
    return 1;
  sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  return 1;
}

static void ptx_emit_tensor_mma(PtxFn *fn, const IRInstruction *in) {
  if (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START ||
      in->tensor_residency_role == IR_TENSOR_RESIDENCY_UPDATE) {
    ptx_emit_tensor_residency_mma(fn, in);
    return;
  }
  if (in->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
      in->tensor_residency_id != 0 ||
      in->tensor_residency_scope != IR_TENSOR_RESIDENCY_SCOPE_NONE) {
    fn_error(fn, "PTX tensor MMA has inconsistent residency metadata");
    return;
  }
  size_t tile_count = ir_tensor_mma_instruction_count(in);
  if (tile_count <= 1) {
    ptx_emit_tensor_mma_single(fn, in);
    return;
  }
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (!per_tile || tile_count > SIZE_MAX / per_tile ||
      in->argument_count != per_tile * tile_count) {
    fn_error(fn, "PTX tensor chain operand count is inconsistent");
    return;
  }
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn, "PTX native tensor MMA chain cannot lower m%un%uk%u: %s",
               (unsigned)IR_TENSOR_MMA(in).m,
               (unsigned)IR_TENSOR_MMA(in).n,
               (unsigned)IR_TENSOR_MMA(in).k, native_reason);
      return;
    }
    int accumulator_count = native_profile.m_tiles * native_profile.n_tiles *
                            native_profile.accumulator_registers;
    int estimated_peak = accumulator_count + native_profile.a_registers +
                         native_profile.b_registers;
    if (estimated_peak <= tuple_budget) {
      ptx_emit_tensor_mma_native_chain_resident(
          fn, in, &native_profile, tile_count, per_tile, tuple_budget,
          estimated_peak);
      return;
    }
    sb_printf(
        &fn->body,
        "\t// mtlc.tensor_chain replay native-mma %s tiles=%llu tuple_peak=%d budget=%d\n",
        ptx_mma_kind_name(&native_profile),
        (unsigned long long)tile_count, estimated_peak, tuple_budget);
    for (size_t tile = 0; tile < tile_count; tile++) {
      IRInstruction single = *in;
      single.arguments = &in->arguments[tile * per_tile];
      single.argument_types =
          in->argument_types ? &in->argument_types[tile * per_tile] : NULL;
      single.argument_count = per_tile;
      single.tensor_mma_count = 1;
      ptx_emit_tensor_mma_single(fn, &single);
      if (fn->error) return;
    }
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor MMA chain cannot lower m%un%uk%u: %s",
             (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
             (unsigned)IR_TENSOR_MMA(in).k, reason);
    return;
  }
  int subtile_count = profile.m_tiles * profile.n_tiles;
  int estimated_peak = subtile_count * profile.d_registers +
                       profile.a_registers + profile.b_registers +
                       profile.c_registers;
  if (estimated_peak <= tuple_budget) {
    ptx_emit_tensor_mma_chain_resident(fn, in, &profile, tile_count, per_tile,
                                       tuple_budget, estimated_peak);
    return;
  }
  if (subtile_count == 1)
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain replay tiles=%llu tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, estimated_peak, tuple_budget);
  else
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain replay tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, subtile_count, estimated_peak,
              tuple_budget);
  for (size_t tile = 0; tile < tile_count; tile++) {
    IRInstruction single = *in;
    single.arguments = &in->arguments[tile * per_tile];
    single.argument_types = in->argument_types
                                ? &in->argument_types[tile * per_tile]
                                : NULL;
    single.argument_count = per_tile;
    single.tensor_mma_count = 1;
    ptx_emit_tensor_mma_single(fn, &single);
    if (fn->error) return;
  }
}

static int ptx_tensor_matmul_element_bytes(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return 2;
  case MTLC_TENSOR_ELEMENT_FLOAT32:
  case MTLC_TENSOR_ELEMENT_INT32:
    return 4;
  case MTLC_TENSOR_ELEMENT_FLOAT64:
    return 8;
  case MTLC_TENSOR_ELEMENT_INT8:
  case MTLC_TENSOR_ELEMENT_UINT8:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
    return 1;
  default:
    return 0;
  }
}

static unsigned ptx_tensor_matmul_narrow_bits(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    return 8;
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
    return 6;
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
    return 4;
  default:
    return 0;
  }
}

static MtlcTensorLayout
ptx_tensor_matmul_transposed_layout(MtlcTensorLayout layout) {
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR)
    return MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  if (layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR)
    return MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  return MTLC_TENSOR_LAYOUT_INVALID;
}

static int ptx_tensor_matmul_capability(PtxFn *fn,
                                        const MtlcTensorMmaDesc *desc) {
  if (!ir_tensor_mma_desc_valid(desc)) {
    fn_error(fn, "PTX tensor_matmul received an invalid descriptor");
    return 0;
  }
  if (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP) {
    fn_error(fn,
             "PTX tensor_matmul currently requires subgroup scope for exact native/cooperative ordering");
    return 0;
  }
  if (fn->target_arch < 70 || fn->isa_major < 6) {
    fn_error(fn,
             "PTX tensor_matmul subgroup ordering requires PTX 6.0 and sm_70 or newer");
    return 0;
  }
  if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD) {
    fn_error(fn,
             "PTX tensor_matmul exact edge lowering currently supports multiply-add operands only");
    return 0;
  }
  int dense = desc->sparsity == MTLC_TENSOR_SPARSITY_DENSE;
  if (!dense &&
      desc->sparsity != MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4) {
    fn_error(fn,
             "PTX tensor_matmul exact sparse edge lowering currently supports canonical structured 2:4 A only");
    return 0;
  }
  if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT &&
      desc->rounding != MTLC_TENSOR_ROUND_NEAREST_EVEN) {
    fn_error(fn,
             "PTX tensor_matmul exact edge lowering supports default/nearest-even rounding only");
    return 0;
  }
  if (desc->accumulator_element != desc->result_element) {
    fn_error(fn,
             "PTX tensor_matmul requires identical accumulator/result formats so native K chunks and exact tails compose without an intermediate narrowing");
    return 0;
  }

  int unscaled = desc->a_scale_mode == MTLC_TENSOR_SCALE_NONE &&
                 desc->b_scale_mode == MTLC_TENSOR_SCALE_NONE;
  int logical = desc->a_packing == MTLC_TENSOR_PACKING_LOGICAL &&
                desc->b_packing == MTLC_TENSOR_PACKING_LOGICAL;
  int f16_family = dense && unscaled && logical &&
      (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
      (desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32;
  int f64_family = dense && unscaled && logical &&
                    desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
                    desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
                    desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT64;
  int i8_family = dense && unscaled && logical &&
      (desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_UINT8) &&
      (desc->b_element == MTLC_TENSOR_ELEMENT_INT8 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_UINT8) &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
      desc->overflow == MTLC_TENSOR_OVERFLOW_WRAP;
  int fp8_family = dense && unscaled && logical &&
      (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) &&
      (desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32;
  int scaled_narrow_family =
      dense && !unscaled &&
      ptx_tensor_matmul_narrow_bits(desc->a_element) != 0 &&
      ptx_tensor_matmul_narrow_bits(desc->b_element) != 0 &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32;
  int sparse_family = 0;
  if (!dense) {
    PtxMmaProfile sparse_profile;
    char reason[256] = {0};
    int selected = ptx_select_mma_profile(
        fn, desc, &sparse_profile, reason, sizeof(reason));
    if (!selected || !ptx_mma_profile_is_sparse(&sparse_profile)) {
      fn_error(fn,
               "PTX tensor_matmul cannot compose an exact structured-2:4 edge path with its native interior: %s",
               selected ? "selected profile is not structured-sparse" : reason);
      return 0;
    }
    sparse_family = 1;
  }
  if (!f16_family && !f64_family && !i8_family && !fp8_family &&
      !scaled_narrow_family && !sparse_family) {
    fn_error(fn,
             "PTX tensor_matmul exact edge lowering currently supports dense f16/bf16->f32, canonical structured-2:4 matching f16/bf16->f32, unscaled e4m3/e5m2->f32, block-scaled FP8/FP6/FP4->f32, f64, and i8/u8->i32-wrap families; TF32, reduced-precision accumulators, unsupported sparse/scale profiles, and saturating integer tails are rejected");
    return 0;
  }
  if (scaled_narrow_family) {
    PtxMmaProfile scaled_profile;
    char reason[256];
    if (!ptx_select_mma_profile(fn, desc, &scaled_profile, reason,
                                sizeof(reason))) {
      fn_error(fn,
               "PTX tensor_matmul cannot compose an exact scaled edge path with its native interior: %s",
               reason);
      return 0;
    }
    if (scaled_profile.kind != PTX_MMA_MXF8F6F4 &&
        scaled_profile.kind != PTX_MMA_MXFP4 &&
        scaled_profile.kind != PTX_MMA_NVFP4) {
      fn_error(fn,
               "PTX tensor_matmul scaled edge lowering requires a block-scaled FP8/FP6/FP4 native profile");
      return 0;
    }
    if (desc->a_scale_leading_dimension == 0 ||
        desc->b_scale_leading_dimension == 0) {
      fn_error(fn,
               "PTX tensor_matmul block scales require explicit whole-matrix A/B scale leading dimensions because runtime problem K determines the dense minimum");
      return 0;
    }
  }
  if ((desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT16) &&
      (fn->target_arch < 53 || fn->isa_major < 4 ||
       (fn->isa_major == 4 && fn->isa_minor < 2))) {
    fn_error(fn,
             "PTX tensor_matmul f16 edge conversion requires PTX 4.2 and sm_53 or newer");
    return 0;
  }
  if ((desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
      (fn->target_arch < 80 || fn->isa_major < 7 ||
       (fn->isa_major == 7 && fn->isa_minor < 1))) {
    fn_error(fn,
             "PTX tensor_matmul bf16 edge conversion requires PTX 7.1 and sm_80 or newer");
    return 0;
  }
  if ((desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) &&
      (fn->target_arch < 89 || fn->isa_major < 8 ||
       (fn->isa_major == 8 && fn->isa_minor < 1))) {
    fn_error(fn,
             "PTX tensor_matmul FP8 edge conversion requires PTX 8.1 and sm_89 or newer");
    return 0;
  }
  return 1;
}

static int ptx_tensor_matmul_address(
    PtxFn *fn, const char *base, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    unsigned element_bytes, char address[24]) {
  if (!fn || !base || !row || !column || !leading_dimension || !address ||
      (layout != MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
       layout != MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) ||
      (element_bytes != 1 && element_bytes != 2 && element_bytes != 4 &&
       element_bytes != 8))
    return -1;
  const char *major =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
  const char *minor =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
  char linear[24], byte_offset[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), linear);
  sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %s;\n", linear, major,
            leading_dimension);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", linear, linear,
            minor);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  if (element_bytes == 1)
    sb_printf(&fn->body, "\tmov.u64 %s, %s;\n", byte_offset, linear);
  else
    sb_printf(&fn->body, "\tshl.b64 %s, %s, %u;\n", byte_offset,
              linear, element_bytes == 2 ? 1u : element_bytes == 4 ? 2u : 3u);
  int address_index = new_reg(fn, PC_B64);
  reg_name(PC_B64, address_index, address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
  return address_index;
}

static void ptx_tensor_matmul_load_input(PtxFn *fn,
                                         MtlcTensorElement element,
                                         const char *space,
                                         const char *address,
                                         const char *value);

/* A logical sub-byte matrix is either byte-addressable or a target-neutral
 * least-significant-bit-first stream. Native MMA helpers consume a rebased
 * byte pointer, so dense streams additionally return a uniform alignment
 * predicate. Misaligned region origins fail over to exact scalar replay. */
static int ptx_tensor_matmul_storage_address(
    PtxFn *fn, const char *base, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    MtlcTensorElement element, MtlcTensorPacking packing, char address[24],
    char aligned[24]) {
  unsigned bits = ptx_tensor_matmul_narrow_bits(element);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), aligned);
  if (packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE || bits >= 8) {
    sb_printf(&fn->body, "\tmov.pred %s, 1;\n", aligned);
    return ptx_tensor_matmul_address(
        fn, base, row, column, leading_dimension, layout,
        (unsigned)ptx_tensor_matmul_element_bytes(element), address);
  }
  if ((bits != 4 && bits != 6) ||
      (layout != MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
       layout != MTLC_TENSOR_LAYOUT_COLUMN_MAJOR))
    return -1;

  const char *major =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
  const char *minor =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
  char linear[24], bit_index[24], byte_offset[24], remainder[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), linear);
  sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %s;\n", linear, major,
            leading_dimension);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", linear, linear, minor);
  reg_name(PC_B64, new_reg(fn, PC_B64), bit_index);
  sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %u;\n", bit_index, linear,
            bits);
  reg_name(PC_B64, new_reg(fn, PC_B64), remainder);
  sb_printf(&fn->body, "\tand.b64 %s, %s, 7;\n", remainder, bit_index);
  sb_printf(&fn->body, "\tsetp.eq.u64 %s, %s, 0;\n", aligned, remainder);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tshr.u64 %s, %s, 3;\n", byte_offset, bit_index);
  int address_index = new_reg(fn, PC_B64);
  reg_name(PC_B64, address_index, address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
  return address_index;
}

static void ptx_tensor_matmul_decode_narrow(PtxFn *fn,
                                             MtlcTensorElement element,
                                             const char *storage,
                                             const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
      element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) {
    char packed_storage[24], packed_f16[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), packed_storage);
    reg_name(PC_B32, new_reg(fn, PC_B32), packed_f16);
    sb_printf(&fn->body, "\tcvt.u16.u32 %s, %s;\n", packed_storage,
              storage);
    sb_printf(&fn->body, "\tcvt.rn.f16x2.%sx2 %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ? "e4m3"
                                                         : "e5m2",
              packed_f16, packed_storage);
    sb_printf(&fn->body, "\tcvt.f32.f16 %s, %s;\n", value, packed_f16);
    return;
  }

  unsigned bits = ptx_tensor_matmul_narrow_bits(element);
  unsigned exponent_bits = element == MTLC_TENSOR_ELEMENT_FLOAT6_E3M2 ? 3 : 2;
  unsigned mantissa_bits = element == MTLC_TENSOR_ELEMENT_FLOAT4_E2M1
                               ? 1
                               : (element == MTLC_TENSOR_ELEMENT_FLOAT6_E3M2
                                      ? 2
                                      : 3);
  unsigned bias = element == MTLC_TENSOR_ELEMENT_FLOAT6_E3M2 ? 3 : 1;
  if ((bits != 4 && bits != 6) || exponent_bits + mantissa_bits + 1 != bits) {
    fn_error(fn, "PTX tensor_matmul cannot decode this narrow input format");
    return;
  }

  char exponent[24], mantissa[24], normal_significand[24];
  char significand[24], normal_scale_exponent[24], scale_exponent[24];
  char scale_bits[24], scale[24], sign[24], exponent_zero[24], negative[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), exponent);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, %u;\n", exponent, storage,
            mantissa_bits);
  sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", exponent, exponent,
            (1u << exponent_bits) - 1u);
  reg_name(PC_B32, new_reg(fn, PC_B32), mantissa);
  sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", mantissa, storage,
            (1u << mantissa_bits) - 1u);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), exponent_zero);
  sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 0;\n", exponent_zero,
            exponent);
  reg_name(PC_B32, new_reg(fn, PC_B32), normal_significand);
  sb_printf(&fn->body, "\tadd.u32 %s, %s, %u;\n", normal_significand,
            mantissa, 1u << mantissa_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), significand);
  sb_printf(&fn->body, "\tselp.u32 %s, %s, %s, %s;\n", significand,
            mantissa, normal_significand, exponent_zero);
  reg_name(PC_B32, new_reg(fn, PC_B32), normal_scale_exponent);
  sb_printf(&fn->body, "\tadd.u32 %s, %s, %u;\n", normal_scale_exponent,
            exponent, 127u - bias - mantissa_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), scale_exponent);
  sb_printf(&fn->body, "\tselp.u32 %s, %u, %s, %s;\n", scale_exponent,
            127u + 1u - bias - mantissa_bits, normal_scale_exponent,
            exponent_zero);
  reg_name(PC_B32, new_reg(fn, PC_B32), scale_bits);
  sb_printf(&fn->body, "\tshl.b32 %s, %s, 23;\n", scale_bits,
            scale_exponent);
  reg_name(PC_F32, new_reg(fn, PC_F32), scale);
  sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", scale, scale_bits);
  sb_printf(&fn->body, "\tcvt.rn.f32.u32 %s, %s;\n", value, significand);
  sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", value, value, scale);
  reg_name(PC_B32, new_reg(fn, PC_B32), sign);
  sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", sign, storage,
            1u << (bits - 1u));
  reg_name(PC_PRED, new_reg(fn, PC_PRED), negative);
  sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", negative, sign);
  sb_printf(&fn->body, "\t@%s neg.f32 %s, %s;\n", negative, value, value);
}

static int ptx_tensor_matmul_load_operand(
    PtxFn *fn, MtlcTensorElement element, MtlcTensorPacking packing,
    const char *base, const char *space, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    const char *value) {
  unsigned bits = ptx_tensor_matmul_narrow_bits(element);
  if (!bits) {
    char address[24];
    if (ptx_tensor_matmul_address(
            fn, base, row, column, leading_dimension, layout,
            (unsigned)ptx_tensor_matmul_element_bytes(element), address) < 0)
      return 0;
    ptx_tensor_matmul_load_input(fn, element, space, address, value);
    return 1;
  }

  char storage[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), storage);
  if (packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE || bits == 8) {
    char address[24];
    if (ptx_tensor_matmul_address(fn, base, row, column, leading_dimension,
                                  layout, 1, address) < 0)
      return 0;
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, storage,
              address);
  } else {
    const char *major =
        layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
    const char *minor =
        layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
    char linear[24], bit_index[24], byte_index[24], bit_shift64[24];
    char bit_shift[24], address[24], next_address[24];
    char low[24], high[24], word[24], crosses[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), linear);
    sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %s;\n", linear, major,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", linear, linear,
              minor);
    reg_name(PC_B64, new_reg(fn, PC_B64), bit_index);
    sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %u;\n", bit_index,
              linear, bits);
    reg_name(PC_B64, new_reg(fn, PC_B64), byte_index);
    sb_printf(&fn->body, "\tshr.u64 %s, %s, 3;\n", byte_index,
              bit_index);
    reg_name(PC_B64, new_reg(fn, PC_B64), bit_shift64);
    sb_printf(&fn->body, "\tand.b64 %s, %s, 7;\n", bit_shift64,
              bit_index);
    reg_name(PC_B32, new_reg(fn, PC_B32), bit_shift);
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", bit_shift,
              bit_shift64);
    reg_name(PC_B64, new_reg(fn, PC_B64), address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
              byte_index);
    reg_name(PC_B32, new_reg(fn, PC_B32), low);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, low, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), high);
    sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", high);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), crosses);
    sb_printf(&fn->body, "\tsetp.gt.u32 %s, %s, %u;\n", crosses,
              bit_shift, 8u - bits);
    reg_name(PC_B64, new_reg(fn, PC_B64), next_address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, 1;\n", next_address,
              address);
    sb_printf(&fn->body, "\t@%s ld%s.u8 %s, [%s];\n", crosses, space,
              high, next_address);
    reg_name(PC_B32, new_reg(fn, PC_B32), word);
    sb_printf(&fn->body, "\tshl.b32 %s, %s, 8;\n", word, high);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", word, word, low);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, %s;\n", storage, word,
              bit_shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", storage, storage,
              (1u << bits) - 1u);
  }
  ptx_tensor_matmul_decode_narrow(fn, element, storage, value);
  return !fn->error;
}

/* Exact sparse-edge replay consumes the target-neutral representation directly:
 * one uint8 2-of-4 mask per logical A row/group and two stored A values in
 * increasing selected-index order.  Backend metadata words are never exposed
 * here.  Invalid masks violate the source contract; match the native path by
 * clamping them to the safe {0,1} mask before selecting/ranking a value. */
static int ptx_tensor_matmul_load_sparse_a(
    PtxFn *fn, const MtlcTensorMmaDesc *desc, const char *a_base,
    const char *a_space, const char *metadata_base,
    const char *metadata_space, const char *row, const char *q,
    const char *a_leading_dimension, const char *metadata_stride,
    const char *value) {
  if (!fn || !desc || !a_base || !a_space || !metadata_base ||
      !metadata_space || !row || !q || !a_leading_dimension ||
      !metadata_stride || !value ||
      desc->sparsity != MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4 ||
      (desc->a_element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
       desc->a_element != MTLC_TENSOR_ELEMENT_BFLOAT16))
    return 0;

  char group[24], position64[24], position[24], metadata_address[24];
  char mask[24], count[24], selected_bit[24], selected_bits[24];
  char lower_bits[24], rank_bits[24], rank[24], rank64[24];
  char compressed_column[24], a_address[24], storage[24];
  char invalid[24], selected[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), group);
  reg_name(PC_B64, new_reg(fn, PC_B64), position64);
  reg_name(PC_B32, new_reg(fn, PC_B32), position);
  sb_printf(&fn->body,
            "\tshr.u64 %s, %s, 2;\n"
            "\tand.b64 %s, %s, 3;\n"
            "\tcvt.u32.u64 %s, %s;\n",
            group, q, position64, q, position, position64);
  if (ptx_tensor_matmul_address(
          fn, metadata_base, row, group, metadata_stride,
          MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, metadata_address) < 0)
    return 0;
  reg_name(PC_B32, new_reg(fn, PC_B32), mask);
  reg_name(PC_B32, new_reg(fn, PC_B32), count);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), invalid);
  sb_printf(&fn->body,
            "\tld%s.u8 %s, [%s];\n"
            "\tand.b32 %s, %s, 15;\n"
            "\tpopc.b32 %s, %s;\n"
            "\tsetp.ne.u32 %s, %s, 2;\n"
            "\t@%s mov.u32 %s, 3;\n",
            metadata_space, mask, metadata_address, mask, mask, count, mask,
            invalid, count, invalid, mask);

  reg_name(PC_B32, new_reg(fn, PC_B32), selected_bit);
  reg_name(PC_B32, new_reg(fn, PC_B32), selected_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), lower_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), rank_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), rank);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), selected);
  sb_printf(&fn->body,
            "\tshl.b32 %s, 1, %s;\n"
            "\tand.b32 %s, %s, %s;\n"
            "\tsetp.ne.u32 %s, %s, 0;\n"
            "\tsub.u32 %s, %s, 1;\n"
            "\tand.b32 %s, %s, %s;\n"
            "\tpopc.b32 %s, %s;\n",
            selected_bit, position, selected_bits, mask, selected_bit,
            selected, selected_bits, lower_bits, selected_bit, rank_bits, mask,
            lower_bits, rank, rank_bits);
  reg_name(PC_B64, new_reg(fn, PC_B64), rank64);
  reg_name(PC_B64, new_reg(fn, PC_B64), compressed_column);
  sb_printf(&fn->body,
            "\tcvt.u64.u32 %s, %s;\n"
            "\tshl.b64 %s, %s, 1;\n"
            "\tadd.u64 %s, %s, %s;\n",
            rank64, rank, compressed_column, group, compressed_column,
            compressed_column, rank64);

  const char *storage_row = desc->transpose_a ? compressed_column : row;
  const char *storage_column = desc->transpose_a ? row : compressed_column;
  if (ptx_tensor_matmul_address(
          fn, a_base, storage_row, storage_column, a_leading_dimension,
          desc->a_layout, 2, a_address) < 0)
    return 0;
  reg_name(PC_B16, new_reg(fn, PC_B16), storage);
  sb_printf(&fn->body,
            "\tmov.b16 %s, 0;\n"
            "\t@%s ld%s.b16 %s, [%s];\n"
            "\tcvt.f32.%s %s, %s;\n",
            storage, selected, a_space, storage, a_address,
            desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
            value, storage);
  return !fn->error;
}

static void ptx_tensor_matmul_decode_scale(PtxFn *fn,
                                            MtlcTensorElement element,
                                            const char *storage,
  const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_SCALE_UE4M3) {
    char unsigned_storage[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), unsigned_storage);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 127;\n", unsigned_storage,
              storage);
    ptx_tensor_matmul_decode_narrow(
        fn, MTLC_TENSOR_ELEMENT_FLOAT8_E4M3, unsigned_storage, value);
    return;
  }
  if (element != MTLC_TENSOR_ELEMENT_SCALE_UE8M0) {
    fn_error(fn, "PTX tensor_matmul cannot decode this block-scale format");
    return;
  }
  char bits[24], zero[24], nan[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), bits);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), zero);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), nan);
  sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 0;\n", zero, storage);
  sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 255;\n", nan, storage);
  sb_printf(&fn->body, "\tshl.b32 %s, %s, 23;\n", bits, storage);
  sb_printf(&fn->body, "\t@%s mov.u32 %s, 4194304;\n", zero, bits);
  sb_printf(&fn->body, "\t@%s mov.u32 %s, 2143289344;\n", nan, bits);
  sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", value, bits);
}

static int ptx_tensor_matmul_load_scale(
    PtxFn *fn, const MtlcTensorMmaDesc *desc, int a_scale,
    const char *base, const char *space, const char *matrix_coordinate,
    const char *q, const char *leading_dimension, const char *value) {
  MtlcTensorScaleMode mode =
      a_scale ? desc->a_scale_mode : desc->b_scale_mode;
  MtlcTensorElement element =
      a_scale ? desc->a_scale_element : desc->b_scale_element;
  unsigned shift = mode == MTLC_TENSOR_SCALE_BLOCK_16
                       ? 4
                       : mode == MTLC_TENSOR_SCALE_BLOCK_32 ? 5 : 0;
  if (!shift) return 0;
  char chunk[24], address[24], storage[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), chunk);
  sb_printf(&fn->body, "\tshr.u64 %s, %s, %u;\n", chunk, q, shift);
  const char *row = a_scale ? matrix_coordinate : chunk;
  const char *column = a_scale ? chunk : matrix_coordinate;
  MtlcTensorLayout layout = a_scale ? MTLC_TENSOR_LAYOUT_ROW_MAJOR
                                    : MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  if (ptx_tensor_matmul_address(fn, base, row, column, leading_dimension,
                                layout, 1, address) < 0)
    return 0;
  reg_name(PC_B32, new_reg(fn, PC_B32), storage);
  sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, storage, address);
  ptx_tensor_matmul_decode_scale(fn, element, storage, value);
  return !fn->error;
}

static void ptx_tensor_matmul_load_input(PtxFn *fn,
                                         MtlcTensorElement element,
                                         const char *space,
                                         const char *address,
                                         const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
      element == MTLC_TENSOR_ELEMENT_BFLOAT16) {
    char storage[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, storage,
              address);
    sb_printf(&fn->body, "\tcvt.f32.%s %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
              value, storage);
  } else if (element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
             element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) {
    char storage[24], packed_f16[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    reg_name(PC_B32, new_reg(fn, PC_B32), packed_f16);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, storage,
              address);
    sb_printf(&fn->body, "\tcvt.rn.f16x2.%sx2 %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ? "e4m3"
                                                         : "e5m2",
              packed_f16, storage);
    sb_printf(&fn->body, "\tcvt.f32.f16 %s, %s;\n", value, packed_f16);
  } else if (element == MTLC_TENSOR_ELEMENT_FLOAT64) {
    sb_printf(&fn->body, "\tld%s.f64 %s, [%s];\n", space, value,
              address);
  } else if (element == MTLC_TENSOR_ELEMENT_INT8) {
    sb_printf(&fn->body, "\tld%s.s8 %s, [%s];\n", space, value,
              address);
  } else {
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value,
              address);
  }
}

static void ptx_tensor_matmul_load_accumulator(
    PtxFn *fn, MtlcTensorElement element, const char *space,
    const char *address, const char *value) {
  const char *type = element == MTLC_TENSOR_ELEMENT_FLOAT64
                         ? "f64"
                     : element == MTLC_TENSOR_ELEMENT_FLOAT32 ? "f32"
                                                               : "s32";
  sb_printf(&fn->body, "\tld%s.%s %s, [%s];\n", space, type, value,
            address);
}

static void ptx_tensor_matmul_store_result(PtxFn *fn,
                                           MtlcTensorElement element,
                                           const char *space,
                                           const char *address,
                                           const char *value) {
  const char *type = element == MTLC_TENSOR_ELEMENT_FLOAT64
                         ? "f64"
                     : element == MTLC_TENSOR_ELEMENT_FLOAT32 ? "f32"
                                                               : "b32";
  sb_printf(&fn->body, "\tst%s.%s [%s], %s;\n", space, type, address,
            value);
}

static int ptx_tensor_matmul_stride64(PtxFn *fn,
                                      const IRInstruction *in,
                                      size_t per_tile,
                                      char strides[4][24]) {
  uint32_t static_strides[4] = {
      IR_TENSOR_MMA(in).a_leading_dimension,
      IR_TENSOR_MMA(in).b_leading_dimension,
      IR_TENSOR_MMA(in).c_leading_dimension,
      IR_TENSOR_MMA(in).d_leading_dimension};
  size_t argument =
      4u +
      (IR_TENSOR_MMA(in).sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  for (size_t i = 0; i < 4; i++) {
    if (static_strides[i] == 0) {
      if (argument >= per_tile) return 0;
      use_as(fn, &in->arguments[argument++], PC_B64, strides[i]);
    } else {
      reg_name(PC_B64, new_reg(fn, PC_B64), strides[i]);
      sb_printf(&fn->body, "\tmov.u64 %s, %u;\n", strides[i],
                static_strides[i]);
    }
  }
  return argument == per_tile;
}

static void ptx_emit_tensor_matmul_scalar(
    PtxFn *fn, const IRInstruction *in, char bases[6][24],
    const char *spaces[6], char strides[4][24],
    char scale_strides[2][24], const char *metadata_stride,
    const char *row_origin, const char *column_origin,
    const char *problem_m, const char *problem_n, const char *problem_k,
    const char *start_k, int initialize_from_d, const char *lane_rank,
    const char *participants, const char *mask, unsigned long long label_id,
    const char *phase) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  PtxClass compute_class =
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT64
          ? PC_F64
      : desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32
          ? PC_F32
          : PC_B32;
  char linear[24], row_local[24], column_local[24];
  char row_local64[24], column_local64[24], row[24], column[24];
  char q[24], done[24], row_oob[24], column_oob[24];
  char c_address[24], d_address[24];
  char a_value[24], b_value[24], accumulator[24];
  char a_scale_value[24] = {0}, b_scale_value[24] = {0};
  int scaled = desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE;
  int sparse = desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE;
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  reg_name(PC_B32, new_reg(fn, PC_B32), row_local);
  reg_name(PC_B32, new_reg(fn, PC_B32), column_local);
  reg_name(PC_B64, new_reg(fn, PC_B64), row_local64);
  reg_name(PC_B64, new_reg(fn, PC_B64), column_local64);
  reg_name(PC_B64, new_reg(fn, PC_B64), row);
  reg_name(PC_B64, new_reg(fn, PC_B64), column);
  reg_name(PC_B64, new_reg(fn, PC_B64), q);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), done);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), row_oob);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), column_oob);
  reg_name(compute_class, new_reg(fn, compute_class), accumulator);
  reg_name(compute_class, new_reg(fn, compute_class), a_value);
  reg_name(compute_class, new_reg(fn, compute_class), b_value);
  if (scaled) {
    reg_name(PC_F32, new_reg(fn, PC_F32), a_scale_value);
    reg_name(PC_F32, new_reg(fn, PC_F32), b_scale_value);
  }
  sb_printf(&fn->body,
            "\t// mtlc.tensor_matmul cooperative-%s exact M/N/K edge replay\n"
            "\tmov.u32 %s, %s;\n"
            "mtlc_tensor_matmul_%llu_%s_output:\n"
            "\tsetp.ge.u32 %s, %s, %u;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_done;\n"
            "\tdiv.u32 %s, %s, %u;\n"
            "\trem.u32 %s, %s, %u;\n"
            "\tcvt.u64.u32 %s, %s;\n"
            "\tcvt.u64.u32 %s, %s;\n"
            "\tadd.u64 %s, %s, %s;\n"
            "\tadd.u64 %s, %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_next;\n"
            "\tsetp.ge.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_next;\n",
            phase, linear, lane_rank, label_id, phase, done, linear,
            (unsigned)((uint32_t)desc->m * (uint32_t)desc->n), done,
            label_id, phase, row_local, linear, (unsigned)desc->n,
            column_local, linear, (unsigned)desc->n, row_local64, row_local,
            column_local64, column_local, row, row_origin, row_local64,
            column, column_origin, column_local64, row_oob, row, problem_m,
            row_oob, label_id, phase, column_oob, column, problem_n,
            column_oob, label_id, phase);

  int c_bytes = ptx_tensor_matmul_element_bytes(
      initialize_from_d ? desc->result_element : desc->accumulator_element);
  if (ptx_tensor_matmul_address(
          fn, initialize_from_d ? bases[3] : bases[2], row, column,
          initialize_from_d ? strides[3] : strides[2],
          initialize_from_d ? desc->d_layout : desc->c_layout,
          (unsigned)c_bytes, c_address) < 0) {
    fn_error(fn, "PTX tensor_matmul cannot address its accumulator matrix");
    return;
  }
  ptx_tensor_matmul_load_accumulator(
      fn, initialize_from_d ? desc->result_element
                            : desc->accumulator_element,
      initialize_from_d ? spaces[3] : spaces[2], c_address, accumulator);
  sb_printf(&fn->body,
            "\tmov.u64 %s, %s;\n"
            "mtlc_tensor_matmul_%llu_%s_k:\n"
            "\tsetp.ge.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_store;\n",
            q, start_k, label_id, phase, done, q, problem_k, done, label_id,
            phase);
  const char *a_row = desc->transpose_a ? q : row;
  const char *a_column = desc->transpose_a ? row : q;
  const char *b_row = desc->transpose_b ? column : q;
  const char *b_column = desc->transpose_b ? q : column;
  int loaded_a = sparse
                     ? ptx_tensor_matmul_load_sparse_a(
                           fn, desc, bases[0], spaces[0], bases[4], spaces[4],
                           row, q, strides[0], metadata_stride, a_value)
                     : ptx_tensor_matmul_load_operand(
                           fn, desc->a_element, desc->a_packing, bases[0],
                           spaces[0], a_row, a_column, strides[0],
                           desc->a_layout, a_value);
  if (!loaded_a ||
      !ptx_tensor_matmul_load_operand(
          fn, desc->b_element, desc->b_packing, bases[1], spaces[1], b_row,
          b_column, strides[1], desc->b_layout, b_value)) {
    fn_error(fn, "PTX tensor_matmul cannot address A/B edge operands");
    return;
  }
  if (scaled) {
    if (!ptx_tensor_matmul_load_scale(
            fn, desc, 1, bases[4], spaces[4], row, q, scale_strides[0],
            a_scale_value) ||
        !ptx_tensor_matmul_load_scale(
            fn, desc, 0, bases[5], spaces[5], column, q,
            scale_strides[1], b_scale_value)) {
      fn_error(fn, "PTX tensor_matmul cannot address A/B block scales");
      return;
    }
    sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", a_value,
              a_value, a_scale_value);
    sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", b_value,
              b_value, b_scale_value);
  }
  if (compute_class == PC_F64) {
    sb_printf(&fn->body, "\tfma.rn.f64 %s, %s, %s, %s;\n", accumulator,
              a_value, b_value, accumulator);
  } else if (compute_class == PC_F32) {
    sb_printf(&fn->body, "\tfma.rn.f32 %s, %s, %s, %s;\n", accumulator,
              a_value, b_value, accumulator);
  } else {
    sb_printf(&fn->body, "\tmad.lo.s32 %s, %s, %s, %s;\n", accumulator,
              a_value, b_value, accumulator);
  }
  sb_printf(&fn->body,
            "\tadd.u64 %s, %s, 1;\n"
            "\tbra mtlc_tensor_matmul_%llu_%s_k;\n"
            "mtlc_tensor_matmul_%llu_%s_store:\n",
            q, q, label_id, phase, label_id, phase);
  int d_bytes = ptx_tensor_matmul_element_bytes(desc->result_element);
  if (ptx_tensor_matmul_address(fn, bases[3], row, column, strides[3],
                                desc->d_layout, (unsigned)d_bytes,
                                d_address) < 0) {
    fn_error(fn, "PTX tensor_matmul cannot address D edge results");
    return;
  }
  ptx_tensor_matmul_store_result(fn, desc->result_element, spaces[3],
                                 d_address, accumulator);
  sb_printf(&fn->body,
            "mtlc_tensor_matmul_%llu_%s_next:\n"
            "\tadd.u32 %s, %s, %s;\n"
            "\tbra mtlc_tensor_matmul_%llu_%s_output;\n"
            "mtlc_tensor_matmul_%llu_%s_done:\n"
            "\tbar.warp.sync %s;\n"
            "\tbra mtlc_tensor_matmul_%llu_finish;\n",
            label_id, phase, linear, linear, participants, label_id, phase,
            label_id, phase, mask, label_id);
}

static int ptx_tensor_matmul_bind_pointer(PtxFn *fn, const char *name,
                                          PtxVal descriptor,
                                          int register_index) {
  if (!fn || !name || register_index < 0) return 0;
  descriptor.cls = PC_B64;
  descriptor.idx = register_index;
  descriptor.is_ptr = 1;
  return bind_value(fn, name, descriptor) != NULL;
}

static void ptx_emit_tensor_matmul(PtxFn *fn,
                                   const IRInstruction *in) {
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  size_t expected = ir_tensor_matmul_operand_count(&IR_TENSOR_MMA(in));
  if (in->op != IR_OP_TENSOR_MATMUL || !per_tile ||
      expected != per_tile + 5u || in->argument_count != expected ||
      !ptx_tensor_matmul_capability(fn, &IR_TENSOR_MMA(in)))
    return;

  int sparse = IR_TENSOR_MMA(in).sparsity != MTLC_TENSOR_SPARSITY_DENSE;
  int scaled = IR_TENSOR_MMA(in).a_scale_mode != MTLC_TENSOR_SCALE_NONE;
  size_t pointer_count = 4u + (sparse ? 1u : scaled ? 2u : 0u);
  char bases[6][24], strides32[4][24], strides64[4][24];
  char scale_strides64[2][24] = {{0}};
  char metadata_stride64[24] = {0}, metadata_stride32[24] = {0};
  const char *spaces[6];
  PtxVal pointer_descs[6];
  for (size_t i = 0; i < pointer_count; i++) {
    pointer_descs[i] = operand_desc(fn, &in->arguments[i]);
    spaces[i] = ptx_wmma_space(pointer_descs[i]);
    if (!pointer_descs[i].is_ptr || !spaces[i]) {
      fn_error(fn,
               "PTX tensor_matmul requires generic/global/workgroup matrix, metadata, and scale pointers");
      return;
    }
    use_as(fn, &in->arguments[i], PC_B64, bases[i]);
  }
  if (!ptx_tensor_stride_registers(fn, in, 0, per_tile, strides32) ||
      !ptx_tensor_matmul_stride64(fn, in, per_tile, strides64)) {
    fn_error(fn, "PTX tensor_matmul has inconsistent leading dimensions");
    return;
  }
  if (scaled) {
    uint32_t scale_strides[2] = {
        IR_TENSOR_MMA(in).a_scale_leading_dimension,
        IR_TENSOR_MMA(in).b_scale_leading_dimension};
    for (size_t i = 0; i < 2; i++) {
      reg_name(PC_B64, new_reg(fn, PC_B64), scale_strides64[i]);
      sb_printf(&fn->body, "\tmov.u64 %s, %u;\n", scale_strides64[i],
                scale_strides[i]);
    }
  }

  char row_origin[24], column_origin[24];
  char problem_m[24], problem_n[24], problem_k[24], zero64[24];
  use_as(fn, &in->arguments[per_tile], PC_B64, row_origin);
  use_as(fn, &in->arguments[per_tile + 1], PC_B64, column_origin);
  use_as(fn, &in->arguments[per_tile + 2], PC_B64, problem_m);
  use_as(fn, &in->arguments[per_tile + 3], PC_B64, problem_n);
  use_as(fn, &in->arguments[per_tile + 4], PC_B64, problem_k);
  reg_name(PC_B64, new_reg(fn, PC_B64), zero64);
  sb_printf(&fn->body, "\tmov.u64 %s, 0;\n", zero64);
  if (sparse) {
    char remainder[24], has_partial_group[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), metadata_stride64);
    reg_name(PC_B64, new_reg(fn, PC_B64), remainder);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), has_partial_group);
    reg_name(PC_B32, new_reg(fn, PC_B32), metadata_stride32);
    sb_printf(&fn->body,
              "\tshr.u64 %s, %s, 2;\n"
              "\tand.b64 %s, %s, 3;\n"
              "\tsetp.ne.u64 %s, %s, 0;\n"
              "\t@%s add.u64 %s, %s, 1;\n"
              "\tcvt.u32.u64 %s, %s;\n",
              metadata_stride64, problem_k, remainder, problem_k,
              has_partial_group, remainder, has_partial_group,
              metadata_stride64, metadata_stride64, metadata_stride32,
              metadata_stride64);
  }

  char mask[24], below[24], active_below[24];
  char lane_rank[24], participants[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), mask);
  reg_name(PC_B32, new_reg(fn, PC_B32), below);
  reg_name(PC_B32, new_reg(fn, PC_B32), active_below);
  reg_name(PC_B32, new_reg(fn, PC_B32), lane_rank);
  reg_name(PC_B32, new_reg(fn, PC_B32), participants);
  sb_printf(&fn->body,
            "\tactivemask.b32 %s;\n"
            "\tmov.u32 %s, %%lanemask_lt;\n"
            "\tand.b32 %s, %s, %s;\n"
            "\tpopc.b32 %s, %s;\n"
            "\tpopc.b32 %s, %s;\n"
            "\tbar.warp.sync %s;\n",
            mask, below, active_below, mask, below, lane_rank, active_below,
            participants, mask, mask);

  unsigned long long label_id = (unsigned long long)fn->call_count++;
  /* A logical transpose needs no physical staging for stable WMMA: viewing a
   * stored row-major matrix as column-major (or the inverse) gives the exact
   * transposed logical coordinates with the same leading dimension. Keep this
   * normalization backend-local; scalar replay continues to use the original
   * descriptor and explicit coordinate swaps. */
  MtlcTensorMmaDesc native_desc = IR_TENSOR_MMA(in);
  if (native_desc.transpose_a) {
    native_desc.a_layout =
        ptx_tensor_matmul_transposed_layout(native_desc.a_layout);
    native_desc.transpose_a = 0;
  }
  if (native_desc.transpose_b) {
    native_desc.b_layout =
        ptx_tensor_matmul_transposed_layout(native_desc.b_layout);
    native_desc.transpose_b = 0;
  }
  PtxWmmaProfile profile;
  PtxMmaProfile direct_profile;
  char native_reason[256];
  int use_direct_mma = 0;
  int have_native =
      ptx_select_wmma_profile(fn, &native_desc, &profile,
                              native_reason, sizeof(native_reason));
  if (!have_native && ptx_tensor_uses_direct_mma(&native_desc)) {
    char direct_reason[256];
    if (ptx_select_mma_profile(fn, &native_desc, &direct_profile,
                               direct_reason, sizeof(direct_reason))) {
      have_native = 1;
      use_direct_mma = 1;
    } else {
      snprintf(native_reason, sizeof(native_reason), "%s", direct_reason);
    }
  }
  int native_peak = 0;
  if (have_native) {
    native_peak =
        use_direct_mma
             ? direct_profile.m_tiles * direct_profile.n_tiles *
                       direct_profile.accumulator_registers +
                   direct_profile.a_registers + direct_profile.b_registers +
                   (sparse ? 1 : scaled ? 2 : 0)
            : profile.m_tiles * profile.n_tiles * profile.d_registers +
                  profile.a_registers + profile.b_registers +
                  profile.c_registers;
    if (native_peak > ptx_tensor_tuple_budget(fn)) have_native = 0;
  }
  if (!have_native) {
    sb_printf(&fn->body,
              "\t// mtlc.tensor_matmul cooperative-only: %s\n",
              native_peak > ptx_tensor_tuple_budget(fn)
                  ? "native accumulator exceeds tensor tuple budget"
                  : native_reason);
    ptx_emit_tensor_matmul_scalar(
        fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
        row_origin,
        column_origin,
        problem_m, problem_n, problem_k, zero64, 0, lane_rank, participants,
        mask, label_id, "full");
    sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_finish:\n", label_id);
    return;
  }

  char row_remaining[24], column_remaining[24], k_remaining[24];
  char row_origin_ok[24], column_origin_ok[24];
  char row_full[24], column_full[24], k_full[24], mask_full[24];
  char native[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), row_remaining);
  reg_name(PC_B64, new_reg(fn, PC_B64), column_remaining);
  reg_name(PC_B64, new_reg(fn, PC_B64), k_remaining);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), row_origin_ok);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), column_origin_ok);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), row_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), column_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), k_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), mask_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), native);
  sb_printf(&fn->body,
            "\tsetp.le.u64 %s, %s, %s;\n"
            "\tsub.u64 %s, %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %u;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tsetp.le.u64 %s, %s, %s;\n"
            "\tsub.u64 %s, %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %u;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tmov.u64 %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %u;\n"
            "\tsetp.eq.u32 %s, %s, 4294967295;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tand.pred %s, %s, %s;\n",
            row_origin_ok, row_origin, problem_m, row_remaining, problem_m,
            row_origin, row_full, row_remaining, (unsigned)IR_TENSOR_MMA(in).m,
            row_full, row_origin_ok, row_full, column_origin_ok,
            column_origin, problem_n, column_remaining, problem_n,
            column_origin, column_full, column_remaining,
            (unsigned)IR_TENSOR_MMA(in).n, column_full, column_origin_ok,
            column_full, k_remaining, problem_k, k_full, k_remaining,
            (unsigned)IR_TENSOR_MMA(in).k, mask_full, mask, native, row_full,
            column_full, native, native, k_full, native, native, mask_full);
  if (sparse) {
    char metadata_stride_roundtrip[24], metadata_stride_fits[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), metadata_stride_roundtrip);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), metadata_stride_fits);
    sb_printf(&fn->body,
              "\tcvt.u64.u32 %s, %s;\n"
              "\tsetp.eq.u64 %s, %s, %s;\n"
              "\tand.pred %s, %s, %s;\n",
              metadata_stride_roundtrip, metadata_stride32,
              metadata_stride_fits, metadata_stride_roundtrip,
              metadata_stride64, native, native, metadata_stride_fits);
  }
  sb_printf(&fn->body,
            "\t@%s bra mtlc_tensor_matmul_%llu_native;\n"
            "\tbra mtlc_tensor_matmul_%llu_scalar;\n",
            native, label_id, label_id);

  sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_scalar:\n", label_id);
  ptx_emit_tensor_matmul_scalar(
      fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
      row_origin,
      column_origin, problem_m, problem_n, problem_k, zero64, 0, lane_rank,
      participants, mask, label_id, "full");

  sb_printf(&fn->body,
            "mtlc_tensor_matmul_%llu_native:\n"
            "\t// mtlc.tensor_matmul native interior runtime-K resident %s tuple_peak=%d budget=%d\n",
            label_id, use_direct_mma ? "direct-mma" : "stable-wmma",
            native_peak, ptx_tensor_tuple_budget(fn));
  char pointer_names[6][64];
  IROperand tile_arguments[11];
  if (per_tile > 11) {
    fn_error(fn, "PTX tensor_matmul internal operand bundle is too large");
    return;
  }
  memcpy(tile_arguments, in->arguments, per_tile * sizeof(*tile_arguments));
  for (size_t i = 0; i < pointer_count; i++) {
    snprintf(pointer_names[i], sizeof(pointer_names[i]),
             "$mtlc_matmul_%llu_ptr_%llu", label_id,
             (unsigned long long)i);
    tile_arguments[i] = (IROperand){0};
    tile_arguments[i].kind = IR_OPERAND_TEMP;
    tile_arguments[i].name = pointer_names[i];
  }
  IRInstruction tile = *in;
  /* `tile` borrows `in`'s tensor block, so the substituted descriptor goes into a
   * private stack block rather than through the shared pointer -- writing there
   * would rewrite the instruction we are reading from. */
  IRTensorAux tile_tensor;
  ir_instruction_tensor_borrow(&tile, &tile_tensor, in);
  tile_tensor.mma = native_desc;
  tile.op = IR_OP_TENSOR_MMA;
  tile.arguments = tile_arguments;
  tile.argument_count = per_tile;
  tile.tensor_mma_count = 1;
  tile.tensor_residency_id = 0;
  tile.tensor_residency_role = IR_TENSOR_RESIDENCY_NONE;
  tile.tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_NONE;

  char initial_addresses[6][24], a_aligned[24], b_aligned[24];
  int initial_indices[6] = {-1, -1, -1, -1, -1, -1};
  initial_indices[0] = ptx_tensor_matmul_storage_address(
      fn, bases[0], row_origin, zero64, strides64[0], native_desc.a_layout,
      IR_TENSOR_MMA(in).a_element, IR_TENSOR_MMA(in).a_packing,
      initial_addresses[0], a_aligned);
  initial_indices[1] = ptx_tensor_matmul_storage_address(
      fn, bases[1], zero64, column_origin, strides64[1], native_desc.b_layout,
      IR_TENSOR_MMA(in).b_element, IR_TENSOR_MMA(in).b_packing,
      initial_addresses[1], b_aligned);
  initial_indices[2] = ptx_tensor_matmul_address(
      fn, bases[2], row_origin, column_origin, strides64[2],
      IR_TENSOR_MMA(in).c_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(
          IR_TENSOR_MMA(in).accumulator_element),
      initial_addresses[2]);
  initial_indices[3] = ptx_tensor_matmul_address(
      fn, bases[3], row_origin, column_origin, strides64[3],
      IR_TENSOR_MMA(in).d_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(
          IR_TENSOR_MMA(in).result_element),
      initial_addresses[3]);
  if (sparse) {
    initial_indices[4] = ptx_tensor_matmul_address(
        fn, bases[4], row_origin, zero64, metadata_stride64,
        MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, initial_addresses[4]);
  } else if (scaled) {
    initial_indices[4] = ptx_tensor_matmul_address(
        fn, bases[4], row_origin, zero64, scale_strides64[0],
        MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, initial_addresses[4]);
    initial_indices[5] = ptx_tensor_matmul_address(
        fn, bases[5], zero64, column_origin, scale_strides64[1],
        MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, 1, initial_addresses[5]);
  }
  char packed_aligned[24];
  reg_name(PC_PRED, new_reg(fn, PC_PRED), packed_aligned);
  sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", packed_aligned,
            a_aligned, b_aligned);
  int force_dense_contiguous[2] = {
      use_direct_mma && direct_profile.a_bits < 8 &&
          IR_TENSOR_MMA(in).a_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
          native_desc.a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
          !native_desc.transpose_a,
      use_direct_mma && direct_profile.b_bits < 8 &&
          IR_TENSOR_MMA(in).b_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
          native_desc.b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR &&
          !native_desc.transpose_b};
  int direct_bits[2] = {use_direct_mma ? direct_profile.a_bits : 0,
                        use_direct_mma ? direct_profile.b_bits : 0};
  for (size_t operand = 0; operand < 2; operand++) {
    if (!force_dense_contiguous[operand]) continue;
    unsigned stride_mask = direct_bits[operand] == 4 ? 1u : 3u;
    char remainder[24], stride_aligned[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), remainder);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), stride_aligned);
    sb_printf(&fn->body,
              "\tand.b64 %s, %s, %u;\n"
              "\tsetp.eq.u64 %s, %s, 0;\n"
              "\tand.pred %s, %s, %s;\n",
              remainder, strides64[operand], stride_mask, stride_aligned,
              remainder, packed_aligned, packed_aligned, stride_aligned);
  }
  sb_printf(&fn->body,
            "\t// mtlc.tensor_matmul dense-subbyte byte-alignment guard\n"
            "\t@!%s bra mtlc_tensor_matmul_%llu_scalar;\n",
            packed_aligned, label_id);
  for (size_t i = 0; i < pointer_count; i++) {
    if (!ptx_tensor_matmul_bind_pointer(fn, pointer_names[i],
                                        pointer_descs[i],
                                        initial_indices[i])) {
      fn_error(fn, "PTX tensor_matmul could not bind a derived tile pointer");
      return;
    }
  }
  if (use_direct_mma) {
    int direct_accumulator_count =
        direct_profile.m_tiles * direct_profile.n_tiles *
        direct_profile.accumulator_registers;
    int direct_accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < direct_accumulator_count; i++) new_reg(fn, PC_F32);

    char lane[24], group[24], thread[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), lane);
    reg_name(PC_B32, new_reg(fn, PC_B32), group);
    reg_name(PC_B32, new_reg(fn, PC_B32), thread);
    sb_printf(&fn->body,
              "\tmov.u32 %s, %%laneid;\n"
              "\tshr.u32 %s, %s, 2;\n"
              "\tand.b32 %s, %s, 3;\n",
              lane, group, lane, thread, lane);

    PtxMmaTileMemory direct_memory;
    if (!ptx_mma_prepare_tile_memory(fn, &tile, &direct_profile, 0, per_tile,
                                     &direct_memory)) {
      fn_error(fn,
               "PTX tensor_matmul direct MMA has invalid tile pointers or strides");
      return;
    }
    for (size_t operand = 0; operand < 2; operand++)
      if (force_dense_contiguous[operand])
        direct_memory.dense_contiguous[operand] = 1;
    if (sparse)
      snprintf(direct_memory.metadata_stride,
               sizeof(direct_memory.metadata_stride), "%s",
               metadata_stride32);
    for (int m_tile = 0; m_tile < direct_profile.m_tiles; m_tile++) {
      PtxMmaSparseAFragment sparse_a;
      PtxMmaSparseAFragment *sparse_a_ptr = NULL;
      if (sparse) {
        ptx_mma_prepare_sparse_f16_a(
            fn, &tile, &direct_memory, group, thread,
            (unsigned)m_tile * 16u, &sparse_a);
        sparse_a_ptr = &sparse_a;
      }
      for (int n_tile = 0; n_tile < direct_profile.n_tiles; n_tile++) {
        int subtile = m_tile * direct_profile.n_tiles + n_tile;
        ptx_emit_mma_native_subtile(
            fn, &tile, &direct_profile, &direct_memory, group, thread,
            sparse_a_ptr, (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            direct_accumulator_base +
                subtile * direct_profile.accumulator_registers,
            1, 0);
      }
    }
    if (fn->error) return;

    char direct_q[24], direct_remaining[24], direct_loop_done[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), direct_q);
    reg_name(PC_B64, new_reg(fn, PC_B64), direct_remaining);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), direct_loop_done);
    sb_printf(&fn->body,
              "\tmov.u64 %s, %u;\n"
              "mtlc_tensor_matmul_%llu_native_k:\n"
              "\tsub.u64 %s, %s, %s;\n"
              "\tsetp.lt.u64 %s, %s, %u;\n"
              "\t@%s bra mtlc_tensor_matmul_%llu_native_store;\n",
              direct_q, (unsigned)IR_TENSOR_MMA(in).k, label_id,
              direct_remaining, problem_k, direct_q, direct_loop_done,
              direct_remaining, (unsigned)IR_TENSOR_MMA(in).k,
              direct_loop_done, label_id);
    char direct_update_a[24], direct_update_b[24], direct_a_q[24];
    char direct_a_aligned[24], direct_b_aligned[24];
    const char *direct_a_column = direct_q;
    if (sparse) {
      reg_name(PC_B64, new_reg(fn, PC_B64), direct_a_q);
      sb_printf(&fn->body, "\tshr.u64 %s, %s, 1;\n", direct_a_q,
                direct_q);
      direct_a_column = direct_a_q;
    }
    int direct_update_a_index = ptx_tensor_matmul_storage_address(
        fn, bases[0], row_origin, direct_a_column, strides64[0],
        native_desc.a_layout, IR_TENSOR_MMA(in).a_element,
        IR_TENSOR_MMA(in).a_packing, direct_update_a, direct_a_aligned);
    int direct_update_b_index = ptx_tensor_matmul_storage_address(
        fn, bases[1], direct_q, column_origin, strides64[1],
        native_desc.b_layout, IR_TENSOR_MMA(in).b_element,
        IR_TENSOR_MMA(in).b_packing, direct_update_b, direct_b_aligned);
    int direct_bound =
        ptx_tensor_matmul_bind_pointer(fn, pointer_names[0], pointer_descs[0],
                                       direct_update_a_index) &&
        ptx_tensor_matmul_bind_pointer(fn, pointer_names[1], pointer_descs[1],
                                       direct_update_b_index);
    if (sparse) {
      char direct_metadata_group[24], direct_update_metadata[24];
      reg_name(PC_B64, new_reg(fn, PC_B64), direct_metadata_group);
      sb_printf(&fn->body, "\tshr.u64 %s, %s, 2;\n",
                direct_metadata_group, direct_q);
      int direct_update_metadata_index = ptx_tensor_matmul_address(
          fn, bases[4], row_origin, direct_metadata_group,
          metadata_stride64, MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1,
          direct_update_metadata);
      direct_bound =
          direct_bound &&
          ptx_tensor_matmul_bind_pointer(
              fn, pointer_names[4], pointer_descs[4],
              direct_update_metadata_index);
    } else if (scaled) {
      unsigned scale_shift =
          IR_TENSOR_MMA(in).a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_16 ? 4 : 5;
      char direct_scale_chunk[24], direct_update_scale_a[24];
      char direct_update_scale_b[24];
      reg_name(PC_B64, new_reg(fn, PC_B64), direct_scale_chunk);
      sb_printf(&fn->body, "\tshr.u64 %s, %s, %u;\n", direct_scale_chunk,
                direct_q, scale_shift);
      int direct_update_scale_a_index = ptx_tensor_matmul_address(
          fn, bases[4], row_origin, direct_scale_chunk, scale_strides64[0],
          MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, direct_update_scale_a);
      int direct_update_scale_b_index = ptx_tensor_matmul_address(
          fn, bases[5], direct_scale_chunk, column_origin, scale_strides64[1],
          MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, 1, direct_update_scale_b);
      direct_bound =
          direct_bound &&
          ptx_tensor_matmul_bind_pointer(fn, pointer_names[4], pointer_descs[4],
                                         direct_update_scale_a_index) &&
          ptx_tensor_matmul_bind_pointer(fn, pointer_names[5], pointer_descs[5],
                                         direct_update_scale_b_index);
    }
    if (!direct_bound) {
      fn_error(fn,
               "PTX tensor_matmul could not bind direct-MMA K-chunk pointers");
      return;
    }
    PtxMmaTileMemory direct_update_memory;
    if (!ptx_mma_prepare_tile_memory(fn, &tile, &direct_profile, 0, per_tile,
                                     &direct_update_memory)) {
      fn_error(fn,
               "PTX tensor_matmul direct MMA has invalid updated tile pointers");
      return;
    }
    for (size_t operand = 0; operand < 2; operand++)
      if (force_dense_contiguous[operand])
        direct_update_memory.dense_contiguous[operand] = 1;
    if (sparse)
      snprintf(direct_update_memory.metadata_stride,
               sizeof(direct_update_memory.metadata_stride), "%s",
               metadata_stride32);
    for (int m_tile = 0; m_tile < direct_profile.m_tiles; m_tile++) {
      PtxMmaSparseAFragment sparse_a;
      PtxMmaSparseAFragment *sparse_a_ptr = NULL;
      if (sparse) {
        ptx_mma_prepare_sparse_f16_a(
            fn, &tile, &direct_update_memory, group, thread,
            (unsigned)m_tile * 16u, &sparse_a);
        sparse_a_ptr = &sparse_a;
      }
      for (int n_tile = 0; n_tile < direct_profile.n_tiles; n_tile++) {
        int subtile = m_tile * direct_profile.n_tiles + n_tile;
        ptx_emit_mma_native_subtile(
            fn, &tile, &direct_profile, &direct_update_memory, group, thread,
            sparse_a_ptr, (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            direct_accumulator_base +
                subtile * direct_profile.accumulator_registers,
            0, 0);
      }
    }
    if (fn->error) return;
    sb_printf(&fn->body,
              "\tadd.u64 %s, %s, %u;\n"
              "\tbra mtlc_tensor_matmul_%llu_native_k;\n"
              "mtlc_tensor_matmul_%llu_native_store:\n",
              direct_q, direct_q, (unsigned)IR_TENSOR_MMA(in).k, label_id,
              label_id);
    for (int m_tile = 0; m_tile < direct_profile.m_tiles; m_tile++) {
      for (int n_tile = 0; n_tile < direct_profile.n_tiles; n_tile++) {
        int subtile = m_tile * direct_profile.n_tiles + n_tile;
        ptx_mma_store_f32_accumulator_subtile(
            fn, &tile, &direct_profile, direct_memory.bases[3],
            direct_memory.spaces[3], direct_memory.strides[3], group, thread,
            (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            direct_accumulator_base +
                subtile * direct_profile.accumulator_registers);
      }
    }
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
    char direct_has_tail[24];
    reg_name(PC_PRED, new_reg(fn, PC_PRED), direct_has_tail);
    sb_printf(&fn->body,
              "\tsetp.lt.u64 %s, %s, %s;\n"
              "\t@%s bra mtlc_tensor_matmul_%llu_tail;\n"
              "\tbra mtlc_tensor_matmul_%llu_finish;\n"
              "mtlc_tensor_matmul_%llu_tail:\n",
              direct_has_tail, direct_q, problem_k, direct_has_tail, label_id,
              label_id, label_id);
    ptx_emit_tensor_matmul_scalar(
        fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
        row_origin,
        column_origin, problem_m, problem_n, problem_k, direct_q, 1, lane_rank,
        participants, mask, label_id, "tail");
    sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_finish:\n", label_id);
    return;
  }
  int accumulator_count =
      profile.m_tiles * profile.n_tiles * profile.d_registers;
  int accumulator_base = fn->count[profile.d_class];
  for (int i = 0; i < accumulator_count; i++) new_reg(fn, profile.d_class);
  if (!ptx_emit_wmma_tiled_tile(fn, &tile, &profile, 0, per_tile,
                                 accumulator_base, 1, 0))
    return;

  char q[24], remaining[24], loop_done[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), q);
  reg_name(PC_B64, new_reg(fn, PC_B64), remaining);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), loop_done);
  sb_printf(&fn->body,
            "\tmov.u64 %s, %u;\n"
            "mtlc_tensor_matmul_%llu_native_k:\n"
            "\tsub.u64 %s, %s, %s;\n"
            "\tsetp.lt.u64 %s, %s, %u;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_native_store;\n",
            q, (unsigned)IR_TENSOR_MMA(in).k, label_id, remaining, problem_k, q,
            loop_done, remaining, (unsigned)IR_TENSOR_MMA(in).k, loop_done,
            label_id);
  char update_a[24], update_b[24];
  int update_a_index = ptx_tensor_matmul_address(
      fn, bases[0], row_origin, q, strides64[0], native_desc.a_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(IR_TENSOR_MMA(in).a_element),
      update_a);
  int update_b_index = ptx_tensor_matmul_address(
      fn, bases[1], q, column_origin, strides64[1],
      native_desc.b_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(IR_TENSOR_MMA(in).b_element),
      update_b);
  if (!ptx_tensor_matmul_bind_pointer(fn, pointer_names[0], pointer_descs[0],
                                      update_a_index) ||
      !ptx_tensor_matmul_bind_pointer(fn, pointer_names[1], pointer_descs[1],
                                      update_b_index) ||
      !ptx_emit_wmma_tiled_tile(fn, &tile, &profile, 0, per_tile,
                                accumulator_base, 0, 0))
    return;
  sb_printf(&fn->body,
            "\tadd.u64 %s, %s, %u;\n"
            "\tbra mtlc_tensor_matmul_%llu_native_k;\n"
            "mtlc_tensor_matmul_%llu_native_store:\n",
            q, q, (unsigned)IR_TENSOR_MMA(in).k, label_id, label_id);
  if (!ptx_emit_wmma_tiled_store(fn, &tile, &profile, 0, per_tile,
                                  accumulator_base))
    return;
  sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  char has_tail[24];
  reg_name(PC_PRED, new_reg(fn, PC_PRED), has_tail);
  sb_printf(&fn->body,
            "\tsetp.lt.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_tail;\n"
            "\tbra mtlc_tensor_matmul_%llu_finish;\n"
            "mtlc_tensor_matmul_%llu_tail:\n",
            has_tail, q, problem_k, has_tail, label_id, label_id, label_id);
  ptx_emit_tensor_matmul_scalar(
      fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
      row_origin,
      column_origin, problem_m, problem_n, problem_k, q, 1, lane_rank,
      participants, mask, label_id, "tail");
  sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_finish:\n", label_id);
}

static void emit_binary(PtxFn *fn, const IRInstruction *in);
static void emit_function(IRProgram *program, size_t fi, CodeGenerator *gen,
                          FILE *out, int target_arch, char target_variant,
                          int isa_major, int isa_minor,
                          int tensor_tuple_budget, char **error);

static int ptx_target_valid(const char *target) {
  if (!target) {
    return 0;
  }
  int is_sm = strncmp(target, "sm_", 3) == 0;
  const unsigned char *p = (const unsigned char *)target;
  if (is_sm) {
    p += 3;
  } else if (strncmp(target, "compute_", 8) == 0) {
    p += 8;
  } else {
    return 0;
  }
  if (!isdigit(*p)) {
    return 0;
  }
  while (isdigit(*p)) {
    p++;
  }
  if (is_sm && (*p == 'a' || *p == 'f')) {
    p++;
  }
  return *p == '\0';
}

static int ptx_target_arch(const char *target) {
  const char *digits;
  if (!ptx_target_valid(target)) return 0;
  digits = strncmp(target, "sm_", 3) == 0 ? target + 3 : target + 8;
  return atoi(digits);
}

static char ptx_target_variant(const char *target) {
  const char *digits;
  const char *tail;
  if (!ptx_target_valid(target)) return '\0';
  digits = strncmp(target, "sm_", 3) == 0 ? target + 3 : target + 8;
  tail = digits;
  while (isdigit((unsigned char)*tail)) tail++;
  return (*tail == 'a' || *tail == 'f') ? *tail : '\0';
}

int ptx_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                     const PtxEmitOptions *options, char **error) {
  if (error) {
    *error = NULL;
  }
  if (!program || !out) {
    if (error) {
      *error = strdup("ptx_emit_program: null program/out");
    }
    return 0;
  }
  const char *target = options && options->target ? options->target : "sm_121a";
  int isa_major = options ? options->isa_major : 8;
  int isa_minor = options ? options->isa_minor : 8;
  int tensor_tuple_budget = options ? options->tensor_tuple_budget : 0;
  g_ptx_emit_checks = options ? options->checks : 0;
  g_ptx_report_types = options ? options->report_types : 0;
  g_ptx_spaced_accesses = 0;
  g_ptx_generic_accesses = 0;
  g_ptx_vector_groups = 0;
  g_ptx_vector_loads_saved = 0;
  g_ptx_analysis_seconds = 0.0;
  if (!ptx_target_valid(target) || isa_major < 1 || isa_major > 99 ||
      isa_minor < 0 || isa_minor > 9 || tensor_tuple_budget < 0 ||
      tensor_tuple_budget > 4096) {
    if (error) {
      *error = strdup("PTX target options are invalid");
    }
    return 0;
  }
  IRGpuCallGraph graph = {0};
  char *graph_error = NULL;
  if (!ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
    if (error) *error = graph_error ? graph_error
                                    : strdup("PTX: invalid GPU call graph");
    else free(graph_error);
    return 0;
  }
  fprintf(out, "//\n// Generated by the Mettle PTX backend (--emit-ptx).\n//\n");
  fprintf(out, ".version %d.%d\n.target %s\n.address_size 64\n\n",
          isa_major, isa_minor, target);

  /* Kernel-side printf needs its format strings at module scope, declared
   * before the functions that reference them. Collect every literal a
   * reachable function prints, then emit the pool and the vprintf import. */
  ptx_string_pool_reset();
  for (size_t oi = 0; oi < graph.count; oi++) {
    IRFunction *function = program->functions[graph.order[oi]];
    if (!function) continue;
    for (size_t ii = 0; ii < function->instruction_count; ii++) {
      const IRInstruction *in = &function->instructions[ii];
      const char *format = NULL;
      if (in->op != IR_OP_CALL || !ptx_intrinsic_is_print(in->intrinsic) ||
          in->argument_count < 1) {
        continue;
      }
      format = ptx_literal_string(function, &in->arguments[0]);
      if (!format) continue;
      g_ptx_uses_vprintf = 1;
      if (ptx_string_intern(format) < 0) {
        if (error) *error = strdup("PTX: out of memory interning a format string");
        ir_gpu_call_graph_destroy(&graph);
        ptx_string_pool_reset();
        return 0;
      }
    }
  }
  if (g_ptx_uses_vprintf) {
    fprintf(out, ".extern .func (.param .b32 func_retval0) vprintf\n"
                 "(\n\t.param .b64 vprintf_format,\n"
                 "\t.param .b64 vprintf_arguments\n);\n");
    for (size_t i = 0; i < g_ptx_string_count; i++) {
      const char *text = g_ptx_strings[i];
      size_t length = strlen(text);
      fprintf(out, ".global .align 1 .b8 $mtlc_str_%zu[%zu] = {", i,
              length + 1);
      for (size_t c = 0; c < length; c++) {
        fprintf(out, "%s%u", c ? ", " : "", (unsigned)(unsigned char)text[c]);
      }
      fprintf(out, "%s0};\n", length ? ", " : "");
    }
    fputc('\n', out);
  }
  /* PTX requires external shared arrays at module scope. Emit one unique arena
   * symbol per kernel before any function definitions; function lowering binds
   * every zero-extent workgroup view to that symbol. */
  for (size_t oi = 0; oi < graph.count; oi++) {
    IRFunction *function = program->functions[graph.order[oi]];
    size_t alignment = 0;
    if (!function || !function->is_kernel) continue;
    for (size_t ii = 0; ii < function->instruction_count; ii++) {
      const IRInstruction *in = &function->instructions[ii];
      if (in->op != IR_OP_ADDRESS_SPACE_ALLOC ||
          in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value != 0 ||
          !in->value_type || in->value_type->kind != MTLC_TYPE_POINTER ||
          !in->value_type->base_type) {
        continue;
      }
      size_t candidate = mtlc_type_alignment(in->value_type->base_type);
      /* The compiler owns dynamic shared-storage placement. Guarantee the
       * strongest alignment required by neutral 16-byte async transactions
       * and PTX WMMA tile loads instead of inheriting scalar element
       * alignment and relying on an accidental launch-time address. */
      if (candidate < 32) candidate = 32;
      if (candidate > alignment) alignment = candidate;
    }
    if (alignment) {
      char kernel_name[256], raw[512], storage[512];
      sanitize_into(function->name ? function->name : "kernel", kernel_name,
                    sizeof(kernel_name));
      snprintf(raw, sizeof(raw), "%s_dynamic_workgroup_storage", kernel_name);
      sanitize_into(raw, storage, sizeof(storage));
      fprintf(out, ".extern .shared .align %zu .b8 %s[];\n", alignment,
              storage);
    }
  }
  fputc('\n', out);
  for (size_t oi = 0; oi < graph.count; oi++) {
    size_t i = graph.order[oi];
    char *ferr = NULL;
    emit_function(program, i, generator, out, ptx_target_arch(target),
                  ptx_target_variant(target), isa_major, isa_minor,
                  tensor_tuple_budget, &ferr);
    if (ferr) {
      if (error) {
        *error = ferr;
      } else {
        free(ferr);
      }
      ir_gpu_call_graph_destroy(&graph);
      return 0;
    }
  }
  ir_gpu_call_graph_destroy(&graph);
  if (g_ptx_report_types) {
    long long total = g_ptx_spaced_accesses + g_ptx_generic_accesses;
    fprintf(stderr,
            "GPU type report: %lld of %lld device accesses named their space, "
            "%lld stayed generic\n",
            g_ptx_spaced_accesses, total, g_ptx_generic_accesses);
    fprintf(stderr,
            "  %lld vector groups formed from a declared alignment, "
            "%lld separate loads removed\n",
            g_ptx_vector_groups, g_ptx_vector_loads_saved);
    fprintf(stderr, "  the analyses took %.3f ms\n",
            g_ptx_analysis_seconds * 1000.0);
  }
  return 1;
}

/* Map a parameter/local type-name to the PTX .param storage type. */
static const char *param_storage_type(PtxVal value) {
  if (value.is_ptr) {
    return "u64";
  }
  switch (value.elem) {
  case MTLC_TYPE_INT8:
    return "s8";
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return "u8";
  case MTLC_TYPE_INT16:
    return "s16";
  case MTLC_TYPE_UINT16:
    return "u16";
  case MTLC_TYPE_INT32:
    return "s32";
  case MTLC_TYPE_UINT32:
    return "u32";
  case MTLC_TYPE_INT64:
    return "s64";
  case MTLC_TYPE_UINT64:
    return "u64";
  case MTLC_TYPE_FLOAT32:
    return "f32";
  case MTLC_TYPE_FLOAT64:
    return "f64";
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    return "b16";
  default:
    return "u32";
  }
}

/* PTX's device-function ABI does not permit predicate, 8-bit, or 16-bit
 * formal parameters. Promote integer call slots to the register-sized bit
 * representation; kernel entry parameters intentionally keep their natural
 * widths because the host launch ABI passes exact-sized value cells. */
static const char *device_param_storage_type(PtxVal value) {
  switch (value.cls) {
  case PC_PRED:
  case PC_B16:
  case PC_B32:
    return "b32";
  case PC_B64:
    return "b64";
  case PC_F32:
    return "f32";
  case PC_F64:
    return "f64";
  default:
    return "b32";
  }
}

static IRFunction *ptx_lookup_function(IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }
  return NULL;
}

static const MtlcType *ptx_function_return_type(
    const IRProgram *program, const IRFunction *function,
    const IRModuleSymbol *symbol) {
  if (symbol && symbol->kind == IR_MODSYM_FUNCTION && symbol->return_type) {
    return symbol->return_type;
  }
  return function && function->return_type_name
             ? ir_program_lookup_type(program, function->return_type_name)
             : NULL;
}

static int ptx_type_is_void(const MtlcType *type, const char *fallback_name) {
  return (type && type->kind == MTLC_TYPE_VOID) ||
         (!type && fallback_name && strcmp(fallback_name, "void") == 0);
}

static void emit_function(IRProgram *program, size_t fi, CodeGenerator *gen,
                           FILE *out, int target_arch, char target_variant,
                           int isa_major, int isa_minor,
                           int tensor_tuple_budget, char **error) {
  (void)gen;
  IRFunction *func = program->functions[fi];
  PtxFn fn = {0};
  const IRModuleSymbol *function_symbol =
      ir_program_lookup_symbol(program, func->name);
  const MtlcType *return_type =
      ptx_function_return_type(program, func, function_symbol);
  int returns_void =
      ptx_type_is_void(return_type, func ? func->return_type_name : NULL);
  fn.program = program;
  fn.function = func;
  fn.function_symbol = function_symbol;
  fn.target_arch = target_arch;
  fn.target_variant = target_variant;
  fn.isa_major = isa_major;
  fn.isa_minor = isa_minor;
  fn.tensor_tuple_budget = tensor_tuple_budget;
  fn.return_desc = returns_void ? (PtxVal){0}
                                : (return_type
                                       ? descriptor_from_type(return_type)
                                       : descriptor_from_typename(
                                             func->return_type_name));
  if (!returns_void && ptx_type_is_aggregate(return_type)) {
    fn.return_desc = (PtxVal){0};
    fn.return_desc.cls = PC_B64;
    fn.return_desc.elem = MTLC_TYPE_VOID;
    fn.return_desc.mem_local = 1;
    fn.return_desc.mem_aggregate = 1;
    fn.return_desc.mem_size = mtlc_type_size(return_type);
    fn.return_desc.mem_align = mtlc_type_alignment(return_type);
    if (!fn.return_desc.mem_align) {
      fn.return_desc.mem_align = 1;
    }
  }

  char ename[256];
  sanitize_into(func->name ? func->name : "kernel", ename, sizeof(ename));

  /* --- signature --- */
  Sb sig = {0};
  if (func->is_kernel) {
    sb_printf(&sig, ".visible .entry %s(", ename);
  } else if (returns_void) {
    sb_printf(&sig, ".func %s(", ename);
  } else if (fn.return_desc.mem_aggregate) {
    sb_printf(&sig, ".func (.param .align %zu .b8 %s_ret[%zu]) %s(",
              fn.return_desc.mem_align, ename, fn.return_desc.mem_size, ename);
  } else {
    sb_printf(&sig, ".func (.param .%s %s_ret) %s(",
              device_param_storage_type(fn.return_desc), ename, ename);
  }
  /* pre-bind parameters; load them at the top of the body */
  PtxVal *param_descs = calloc(func->parameter_count + 1, sizeof(PtxVal));
  for (size_t p = 0; p < func->parameter_count; p++) {
    const char *tn = func->parameter_types ? func->parameter_types[p] : NULL;
    const MtlcType *pt = function_symbol &&
                                 function_symbol->kind == IR_MODSYM_FUNCTION &&
                                 p < function_symbol->param_count
                             ? function_symbol->param_types[p]
                             : NULL;
    PtxVal d = pt ? descriptor_from_type(pt) : descriptor_from_typename(tn);
    /* A kernel's pointer parameters come from the launch and are device global.
     * A device helper's can be anything the caller had, including the address
     * of one of its own locals, so an unstated space stays generic. */
    if (!func->is_kernel && d.is_ptr &&
        (!pt || pt->address_space == MTLC_ADDRESS_SPACE_DEFAULT)) {
      d.address_space = MTLC_ADDRESS_SPACE_GENERIC;
    }
    if (ptx_type_is_aggregate(pt)) {
      d = (PtxVal){0};
      d.cls = PC_B64;
      d.elem = MTLC_TYPE_VOID;
      d.mem_local = 1;
      d.mem_aggregate = 1;
      d.mem_size = mtlc_type_size(pt);
      d.mem_align = mtlc_type_alignment(pt);
      if (!d.mem_size) {
        fn_error(&fn, "PTX: parameter %zu of '%s' has an empty record type", p,
                 func->name ? func->name : "?");
      }
      if (!d.mem_align) {
        d.mem_align = 1;
      }
    }
    param_descs[p] = d;
    if (p) {
      sb_puts(&sig, ",");
    }
    if (d.mem_aggregate) {
      sb_printf(&sig, "\n    .param .align %zu .b8 %s_p%zu[%zu]", d.mem_align,
                ename, p, d.mem_size);
    } else if (func->is_kernel && d.is_ptr) {
      const char *space = ptx_memory_space(d.address_space);
      /* A declared `align(N)` is a proven fact about the address the launch
       * passes, so the parameter says so and the driver holds the launch to
       * it. Without one the element's own alignment is all that is known. */
      size_t alignment = pt && pt->pointee_align ? pt->pointee_align
                         : pt && pt->base_type && pt->base_type->alignment
                             ? pt->base_type->alignment
                             : 4;
      if (!space) {
        fn_error(&fn, "PTX: invalid address space %d on parameter %zu",
                 (int)d.address_space, p);
      } else {
        sb_printf(&sig, "\n    .param .%s .ptr%s.align %zu %s_p%zu",
                  param_storage_type(d), space, alignment, ename, p);
      }
    } else {
      sb_printf(&sig, "\n    .param .%s %s_p%zu",
                func->is_kernel ? param_storage_type(d)
                                : device_param_storage_type(d),
                ename, p);
    }
  }
  sb_puts(&sig, "\n)\n");
  if (func->is_kernel && func->kernel_block[0] > 0) {
    /* `kernel(block = ...)`: the driver refuses a mismatched launch geometry
     * at cuLaunchKernel instead of running 7/8 of the block with a garbage
     * lane mapping. */
    sb_printf(&sig, ".reqntid %d, %d, %d\n", func->kernel_block[0],
              func->kernel_block[1] > 0 ? func->kernel_block[1] : 1,
              func->kernel_block[2] > 0 ? func->kernel_block[2] : 1);
  }

  /* Address-space allocations are declarations, so emit them before
   * executable instructions and bind each semantic pointer to a .b64 register.
   * A zero extent is the one launch-provided dynamic workgroup arena. Multiple
   * typed views deliberately alias its base. The spelling is PTX-specific; the
   * IR only carries address space, element type, and static/dynamic extent. */
  size_t dynamic_workgroup_alignment = 0;
  char dynamic_workgroup_storage[512] = {0};
  for (size_t i = 0; i < func->instruction_count && !fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC ||
        in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value != 0) {
      continue;
    }
    if (!func->is_kernel || !in->dest.name || !in->value_type ||
        in->value_type->kind != MTLC_TYPE_POINTER ||
        !in->value_type->base_type ||
        in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
        in->value_type->address_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
        mtlc_type_size(in->value_type->base_type) == 0) {
      fn_error(&fn, "PTX: invalid dynamic workgroup view in '%s'",
               func->name ? func->name : "?");
      break;
    }
    size_t alignment = mtlc_type_alignment(in->value_type->base_type);
    if (alignment < 32) alignment = 32;
    if (alignment > dynamic_workgroup_alignment) {
      dynamic_workgroup_alignment = alignment;
    }
  }
  if (dynamic_workgroup_alignment && !fn.error) {
    char raw[512];
    snprintf(raw, sizeof(raw), "%s_dynamic_workgroup_storage", ename);
    sanitize_into(raw, dynamic_workgroup_storage,
                  sizeof(dynamic_workgroup_storage));
  }
  for (size_t i = 0; i < func->instruction_count && !fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC) continue;
    int is_dynamic =
        in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 0;
    if (!func->is_kernel || !in->dest.name ||
        !in->value_type || in->value_type->kind != MTLC_TYPE_POINTER ||
        !in->value_type->base_type || in->rhs.kind != IR_OPERAND_INT ||
        in->rhs.int_value < 0 ||
        (in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP &&
         in->address_space != MTLC_ADDRESS_SPACE_PRIVATE) ||
        in->value_type->address_space != in->address_space ||
        (is_dynamic && in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP)) {
      fn_error(&fn, "PTX: invalid address-space allocation in '%s'",
               func->name ? func->name : "?");
      break;
    }
    size_t elem_size = mtlc_type_size(in->value_type->base_type);
    size_t alignment = mtlc_type_alignment(in->value_type->base_type);
    if (in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP && alignment < 32)
      alignment = 32;
    size_t count = (size_t)in->rhs.int_value;
    if (!elem_size || (!is_dynamic && count > SIZE_MAX / elem_size)) {
      fn_error(&fn, "PTX: address-space allocation '%s' overflows",
               in->dest.name);
      break;
    }
    if (!is_dynamic) {
      char raw[512], storage[512];
      snprintf(raw, sizeof(raw), "%s_%s_storage", ename, in->dest.name);
      sanitize_into(raw, storage, sizeof(storage));
      sb_printf(&fn.body, "\t%s .align %zu .b8 %s[%zu];\n",
                in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP ? ".shared"
                                                                   : ".local",
                alignment ? alignment : 1, storage, elem_size * count);
    }
    PtxVal pointer = descriptor_from_type(in->value_type);
    pointer.idx = new_reg(&fn, PC_B64);
    bind_value(&fn, in->dest.name, pointer);
  }

  /* Rank-aware global->workgroup TMA completes through one compiler-owned
   * transaction barrier. Transfer operations are synchronous at the neutral
   * boundary, so a single barrier can be safely reinitialized and reused by
   * sequential operations in the function. */
  int needs_tensor_transfer_barrier = 0;
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_TENSOR_TRANSFER &&
        IR_TENSOR_TRANSFER(in).direction ==
            MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP &&
        ptx_tensor_transfer_native_capable(
            &fn, &IR_TENSOR_TRANSFER(in),
            in->tensor_transfer_has_prepared_view)) {
      needs_tensor_transfer_barrier = 1;
      break;
    }
  }
  if (needs_tensor_transfer_barrier) {
    char barrier_name[512];
    ptx_tensor_transfer_barrier_name(&fn, barrier_name,
                                     sizeof(barrier_name));
    sb_printf(&fn.body, "\t.shared .align 8 .b8 %s[8];\n", barrier_name);
  }

  /* body: load params into registers and bind by name */
  for (size_t p = 0; p < func->parameter_count && !fn.error; p++) {
    PtxVal d = param_descs[p];
    if (d.mem_aggregate) {
      /* A record parameter arrives by value, so it gets storage of its own and
       * a copy: writing through it must not reach the caller's copy. */
      char raw[512], storage[512], pointer[24], source[512];
      snprintf(raw, sizeof(raw), "%s_p%zu_local", ename, p);
      sanitize_into(raw, storage, sizeof(storage));
      sb_printf(&fn.body, "\t.local .align %zu .b8 %s[%zu];\n", d.mem_align,
                storage, d.mem_size);
      d.mem_addr = new_reg(&fn, PC_B64);
      reg_name(PC_B64, d.mem_addr, pointer);
      sb_printf(&fn.body, "\tmov.u64 %s, %s;\n", pointer, storage);
      snprintf(source, sizeof(source), "%s_p%zu", ename, p);
      ptx_block_copy(&fn, ".local", pointer, ".param", source, d.mem_size,
                     d.mem_align);
      if (func->parameter_names && func->parameter_names[p]) {
        bind_value(&fn, func->parameter_names[p], d);
      }
      continue;
    }
    d.idx = new_reg(&fn, d.cls);
    char rn[24];
    reg_name(d.cls, d.idx, rn);
    if (!d.is_ptr && (d.elem == MTLC_TYPE_FLOAT16 || d.elem == MTLC_TYPE_BFLOAT16)) {
      char tmp[24];
      reg_name(PC_B16, new_reg(&fn, PC_B16), tmp);
      sb_printf(&fn.body, "\tld.param.b16 %s, [%s_p%zu];\n", tmp, ename, p);
      sb_printf(&fn.body, "\tcvt.f32.%s %s, %s;\n",
                d.elem == MTLC_TYPE_FLOAT16 ? "f16" : "bf16", rn, tmp);
    } else {
      sb_printf(&fn.body, "\tld.param.%s %s, [%s_p%zu];\n",
                func->is_kernel ? param_storage_type(d)
                                : device_param_storage_type(d),
                rn, ename, p);
    }
    if (func->parameter_names && func->parameter_names[p]) {
      bind_value(&fn, func->parameter_names[p], d);
    }
  }

  for (size_t i = 0; i < func->instruction_count && !fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC || !in->dest.name) continue;
    PtxBinding *binding = find_binding(&fn, in->dest.name);
    char raw[512], storage[512], pointer[24];
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 0) {
      snprintf(storage, sizeof(storage), "%s", dynamic_workgroup_storage);
    } else {
      snprintf(raw, sizeof(raw), "%s_%s_storage", ename, in->dest.name);
      sanitize_into(raw, storage, sizeof(storage));
    }
    if (!binding) {
      fn_error(&fn, "PTX: allocation '%s' has no pointer binding",
               in->dest.name);
      break;
    }
    reg_name(PC_B64, binding->val.idx, pointer);
    sb_printf(&fn.body, "\tmov.u64 %s, %s;\n", pointer, storage);
  }

  /* Locals with no register form get per-thread `.local` storage: aggregates
   * always, and scalars whose address the function takes. Declaring them here,
   * ahead of the instruction walk, means a use that precedes the declaration
   * in a rotated or threaded CFG still resolves. */
  for (size_t i = 0; i < func->instruction_count && !fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op != IR_OP_DECLARE_LOCAL || !in->dest.name) continue;
    if (find_binding(&fn, in->dest.name)) continue;
    int aggregate = ptx_type_is_aggregate(in->value_type);
    if (!aggregate && !ptx_local_address_taken(func, in->dest.name)) continue;
    size_t size = 0, alignment = 0;
    if (in->value_type) {
      size = mtlc_type_size(in->value_type);
      alignment = mtlc_type_alignment(in->value_type);
    }
    if (!size && !aggregate) {
      PtxVal s = descriptor_from_typename(in->text);
      size = ptx_class_width(s.cls);
      alignment = size;
    }
    if (!size) {
      fn_error(&fn, "PTX: local '%s' has unsupported type '%s'", in->dest.name,
               in->text ? in->text : "?");
      break;
    }
    if (!alignment) alignment = 1;
    char raw[512], storage[512], pointer[24];
    snprintf(raw, sizeof(raw), "%s_%s_local", ename, in->dest.name);
    sanitize_into(raw, storage, sizeof(storage));
    sb_printf(&fn.body, "\t.local .align %zu .b8 %s[%zu];\n", alignment,
              storage, size);
    PtxVal v = aggregate ? (PtxVal){0}
                         : (in->value_type ? descriptor_from_type(in->value_type)
                                           : descriptor_from_typename(in->text));
    v.mem_local = 1;
    v.mem_aggregate = aggregate;
    v.mem_addr = new_reg(&fn, PC_B64);
    v.mem_size = size;
    v.mem_align = alignment;
    if (aggregate) {
      v.cls = PC_B64;
      v.elem = MTLC_TYPE_VOID;
    }
    reg_name(PC_B64, v.mem_addr, pointer);
    sb_printf(&fn.body, "\tmov.u64 %s, %s;\n", pointer, storage);
    bind_value(&fn, in->dest.name, v);
  }

  /* --- walk instructions --- */
  clock_t analysis_start = clock();
  PtxVectorPlan *vector_plan = ptx_plan_vector_loads(program, func);
  g_ptx_analysis_seconds +=
      (double)(clock() - analysis_start) / (double)CLOCKS_PER_SEC;
  for (size_t ii = 0; ii < func->instruction_count && !fn.error; ii++) {
    const IRInstruction *in = &func->instructions[ii];
    if (vector_plan && in->op == IR_OP_LOAD && vector_plan[ii].absorbed) {
      continue;
    }
    if (vector_plan && in->op == IR_OP_LOAD && vector_plan[ii].width) {
      unsigned char width = vector_plan[ii].width;
      const IROperand *base = NULL;
      long long offset = 0;
      const MtlcType *pointer_type;
      char addrreg[24];
      char names[4][24];
      char list[128];
      size_t used = 0;
      int is_unsigned = 0;
      PtxClass cls;
      MtlcTypeKind elem;
      const char *space;
      ptx_address_parts(func, ii, &in->lhs, &base, &offset);
      pointer_type = ptx_load_pointer_type(program, func, base);
      elem = pointer_type && pointer_type->base_type
                 ? pointer_type->base_type->kind
                 : MTLC_TYPE_VOID;
      cls = elem_class(elem, &is_unsigned);
      space = ptx_load_space(pointer_type ? pointer_type->address_space
                                          : MTLC_ADDRESS_SPACE_GENERIC);
      use_as(&fn, base, PC_B64, addrreg);
      list[0] = '\0';
      for (unsigned char k = 0; k < width; k++) {
        const IRInstruction *member =
            &func->instructions[vector_plan[ii].members[k]];
        PtxVal dv = {0};
        dv.cls = cls;
        dv.is_unsigned = is_unsigned;
        dv = destination_value(&fn, &member->dest, dv);
        reg_name(cls, dv.idx, names[k]);
        if (member->dest.name) {
          bind_value(&fn, member->dest.name, dv);
        }
        used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s",
                                 k ? ", " : "", names[k]);
      }
      sb_printf(&fn.body, "\tld%s.v%u.%s {%s}, [%s+%lld];\n", space,
                (unsigned)width, mem_type_suffix(elem), list, addrreg, offset);
      g_ptx_spaced_accesses++;
      g_ptx_vector_groups++;
      g_ptx_vector_loads_saved += width - 1;
      continue;
    }
    switch (in->op) {
    case IR_OP_NOP:
    case IR_OP_ADDRESS_SPACE_ALLOC:
    case IR_OP_DECLARE_LOCAL: {
      if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name) {
        /* pre-allocate a register for the local so refs resolve; aggregates or
         * address-taken locals are unsupported and will surface as errors. */
        PtxVal d = in->value_type ? descriptor_from_type(in->value_type)
                                  : descriptor_from_typename(in->text);
        if (d.cls == PC_NONE) {
          fn_error(&fn, "PTX: local '%s' has unsupported type '%s'",
                   in->dest.name, in->text ? in->text : "?");
          break;
        }
        if (!find_binding(&fn, in->dest.name)) {
          d.idx = new_reg(&fn, d.cls);
          bind_value(&fn, in->dest.name, d);
        }
      }
      break;
    }
    case IR_OP_BARRIER:
      if (!ptx_workgroup_barrier_contract(in)) {
        fn_error(&fn, "PTX: invalid workgroup barrier memory contract");
      } else {
        /* bar.sync is a full CTA execution/memory barrier. It safely
         * strengthens acquire/release-only contracts and covers both shared
         * and global accesses made by participating work-items. */
        sb_puts(&fn.body, "\tbar.sync 0;\n");
      }
      break;
    case IR_OP_ASYNC_COPY:
      ptx_emit_async_copy(&fn, in);
      break;
    case IR_OP_ASYNC_COMMIT:
      ptx_emit_async_commit(&fn);
      break;
    case IR_OP_ASYNC_WAIT:
      ptx_emit_async_wait(&fn, in);
      break;
    case IR_OP_TENSOR_TRANSFER:
      ptx_emit_tensor_transfer(&fn, in);
      break;
    case IR_OP_TENSOR_MMA: {
      size_t epilogue_index = 0;
      const IRInstruction *epilogue =
          ptx_following_tensor_epilogue(func, ii, &epilogue_index);
      if (epilogue &&
          ptx_try_emit_tensor_mma_resident_epilogue(&fn, in, epilogue)) {
        ii = epilogue_index;
      } else {
        ptx_emit_tensor_mma(&fn, in);
      }
      break;
    }
    case IR_OP_TENSOR_MATMUL:
      ptx_emit_tensor_matmul(&fn, in);
      break;
    case IR_OP_TENSOR_EPILOGUE:
      if (!ptx_tensor_epilogue_was_consumed(&fn, in))
        ptx_emit_tensor_epilogue(&fn, in);
      break;
    case IR_OP_TENSOR_COMMIT: {
      size_t epilogue_index = 0;
      const IRInstruction *epilogue =
          ptx_following_tensor_epilogue(func, ii, &epilogue_index);
      int deferred_loop_exit = 0;
      if (!epilogue) {
        epilogue =
            ptx_loop_exit_tensor_epilogue(func, ii, &epilogue_index);
        deferred_loop_exit = epilogue != NULL;
      }
      if (epilogue &&
          ptx_try_emit_tensor_commit_resident_epilogue(&fn, in, epilogue)) {
        if (deferred_loop_exit) {
          PtxTensorResidency *group =
              ptx_tensor_residency_find(&fn, in->tensor_residency_id);
          if (!group) {
            fn_error(&fn,
                     "PTX resident loop epilogue handoff lost residency group");
          } else {
            group->consumed_epilogue = epilogue;
          }
        } else {
          ii = epilogue_index;
        }
      } else {
        ptx_emit_tensor_residency_commit(&fn, in);
      }
      break;
    }
    case IR_OP_LABEL: {
      char lbl[256];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      sb_printf(&fn.body, "%s:\n", lbl);
      break;
    }
    case IR_OP_JUMP: {
      char lbl[256];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      sb_printf(&fn.body, "\tbra %s;\n", lbl);
      break;
    }
    case IR_OP_BRANCH_ZERO: {
      char lbl[256], r[24];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      PtxVal cv = operand_desc(&fn, &in->lhs);
      int p = new_reg(&fn, PC_PRED);
      char pn[24];
      reg_name(PC_PRED, p, pn);
      if (cv.cls == PC_F32 || cv.cls == PC_F64) {
        use_as(&fn, &in->lhs, cv.cls, r);
        /* Zero immediate as a hex bit-pattern: f32 needs 0f + 8 digits, f64
         * needs 0d + 16. (Hand-written literals are an easy off-by-one;
         * formatting from the bits is not.) */
        if (cv.cls == PC_F32) {
          sb_printf(&fn.body, "\tsetp.eq.f32 %s, %s, 0f%08X;\n", pn, r, 0u);
        } else {
          sb_printf(&fn.body, "\tsetp.eq.f64 %s, %s, 0d%016llX;\n", pn, r, 0ull);
        }
      } else {
        PtxClass c = (cv.cls == PC_B64) ? PC_B64 : PC_B32;
        use_as(&fn, &in->lhs, c, r);
        sb_printf(&fn.body, "\tsetp.eq.%s %s, %s, 0;\n",
                  c == PC_B64 ? "s64" : "s32", pn, r);
      }
      sb_printf(&fn.body, "\t@%s bra %s;\n", pn, lbl);
      break;
    }
    case IR_OP_BRANCH_EQ: {
      char lbl[256], a[24], bb[24];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      PtxVal la = operand_desc(&fn, &in->lhs);
      PtxVal lb = operand_desc(&fn, &in->rhs);
      PtxClass c = PC_B32;
      if (la.cls == PC_B64 || lb.cls == PC_B64) {
        c = PC_B64;
      }
      if (la.cls == PC_F32 || lb.cls == PC_F32) {
        c = PC_F32;
      }
      if (la.cls == PC_F64 || lb.cls == PC_F64) {
        c = PC_F64;
      }
      use_as(&fn, &in->lhs, c, a);
      use_as(&fn, &in->rhs, c, bb);
      int p = new_reg(&fn, PC_PRED);
      char pn[24];
      reg_name(PC_PRED, p, pn);
      sb_printf(&fn.body, "\tsetp.eq.%s %s, %s, %s;\n",
                type_suffix_for_class(c, 0), pn, a, bb);
      sb_printf(&fn.body, "\t@%s bra %s;\n", pn, lbl);
      break;
    }
    case IR_OP_ASSIGN: {
      if (!in->dest.name) {
        fn_error(&fn, "PTX: assign with no dest");
        break;
      }
      /* destination class: reuse if symbol already bound, else infer from src */
      PtxBinding *db = find_binding(&fn, in->dest.name);
      PtxBinding *agg = named_binding(&fn, &in->lhs);
      if (agg && agg->val.mem_aggregate) {
        /* A whole-record assignment. An unbound destination is a temp standing
         * in for the same record, so it aliases the storage; a destination with
         * storage of its own receives a copy, which is what by-value means. */
        if (!db) {
          bind_value(&fn, in->dest.name, agg->val);
          break;
        }
        if (!db->val.mem_aggregate) {
          fn_error(&fn, "PTX: cannot assign a record to the scalar '%s'",
                   in->dest.name);
          break;
        }
        size_t size = db->val.mem_size < agg->val.mem_size ? db->val.mem_size
                                                           : agg->val.mem_size;
        char dst[24], src[24];
        reg_name(PC_B64, db->val.mem_addr, dst);
        reg_name(PC_B64, agg->val.mem_addr, src);
        ptx_block_copy(&fn, ".local", dst, ".local", src, size,
                       db->val.mem_align);
        break;
      }
      PtxClass dc;
      if (db) {
        dc = db->val.cls;
      } else {
        PtxVal sv = operand_desc(&fn, &in->lhs);
        dc = (sv.cls == PC_NONE || sv.cls == PC_PRED) ? PC_B32 : sv.cls;
      }
      char src[24];
      use_as(&fn, &in->lhs, dc, src);
      PtxVal dv;
      if (db && db->val.mem_local) {
        /* The home is memory, so the value lands in a scratch register and
         * bind_value writes it through. Reusing the binding here would carry
         * mem_local into the store-back check and drop the write. */
        dv = (PtxVal){0};
        dv.cls = dc;
        dv.is_unsigned = db->val.is_unsigned;
        dv.idx = new_reg(&fn, dc);
      } else if (db) {
        dv = db->val;
      } else {
        dv = operand_desc(&fn, &in->lhs);
        dv.cls = dc;
        dv.idx = new_reg(&fn, dc);
      }
      char dn[24];
      reg_name(dv.cls, dv.idx, dn);
      if (strcmp(dn, src) != 0) {
        const char *mt = (dc == PC_F32)   ? "f32"
                         : (dc == PC_F64) ? "f64"
                         : (dc == PC_B64) ? "u64"
                                          : "u32";
        sb_printf(&fn.body, "\tmov.%s %s, %s;\n", mt, dn, src);
      }
      bind_value(&fn, in->dest.name, dv);
      break;
    }
    case IR_OP_LOAD: {
      /* A load from a string literal is the frontend taking a format string's
       * character pointer. PTX has no string value; the print intrinsic reads
       * the literal directly, so this produces nothing. A temp bound this way
       * and used for anything else stays undefined, which is the right
       * diagnostic: kernels have no strings beyond print formats. */
      if (in->lhs.kind == IR_OPERAND_STRING) {
        break;
      }
      /* dest <- *lhs [rhs size] */
      PtxVal addr = operand_desc(&fn, &in->lhs);
      MtlcTypeKind elem = addr.is_ptr ? addr.elem : MTLC_TYPE_VOID;
      if (elem == MTLC_TYPE_VOID) {
        long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
        if (in->is_float) {
          if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_F16) {
            elem = MTLC_TYPE_FLOAT16;
          } else if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_BF16) {
            elem = MTLC_TYPE_BFLOAT16;
          } else {
            elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
          }
        } else {
          elem = (sz == 8) ? MTLC_TYPE_INT64
                 : (sz == 2) ? MTLC_TYPE_INT16
                 : (sz == 1) ? MTLC_TYPE_UINT8
                             : MTLC_TYPE_INT32;
        }
      }
      char addrreg[24];
      use_as(&fn, &in->lhs, PC_B64, addrreg);
      int u = 0;
      PtxClass dc = elem_class(elem, &u);
      PtxVal dv = {0};
      dv.cls = dc;
      dv.is_unsigned = u;
      dv = destination_value(&fn, &in->dest, dv);
      char dn[24];
      reg_name(dc, dv.idx, dn);
      const char *space = ptx_load_space(addr.address_space);
      if (addr.address_space == MTLC_ADDRESS_SPACE_DEFAULT ||
          addr.address_space == MTLC_ADDRESS_SPACE_GENERIC) {
        g_ptx_generic_accesses++;
      } else {
        g_ptx_spaced_accesses++;
      }
      if (!space) {
        fn_error(&fn, "PTX: invalid load address space %d",
                 (int)addr.address_space);
      } else if (elem == MTLC_TYPE_FLOAT16 || elem == MTLC_TYPE_BFLOAT16) {
        char tmp[24];
        reg_name(PC_B16, new_reg(&fn, PC_B16), tmp);
        sb_printf(&fn.body, "\tld%s.b16 %s, [%s];\n", space, tmp, addrreg);
        sb_printf(&fn.body, "\tcvt.f32.%s %s, %s;\n",
                  elem == MTLC_TYPE_FLOAT16 ? "f16" : "bf16", dn, tmp);
      } else {
        sb_printf(&fn.body, "\tld%s.%s %s, [%s];\n", space,
                  mem_type_suffix(elem), dn, addrreg);
      }
      if (in->dest.name) {
        bind_value(&fn, in->dest.name, dv);
      }
      break;
    }
    case IR_OP_STORE: {
      /* *dest <- lhs [rhs size] */
      PtxVal addr = operand_desc(&fn, &in->dest);
      if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 8) {
        long long total = in->rhs.int_value;
        long long offset = 0;
        PtxVal source = operand_desc(&fn, &in->lhs);
        const char *dst_space = ptx_memory_space(addr.address_space);
        const char *src_space =
            source.is_ptr ? ptx_load_space(source.address_space) : "";
        char dstreg[24], srcreg[24];
        use_as(&fn, &in->dest, PC_B64, dstreg);
        use_as(&fn, &in->lhs, PC_B64, srcreg);
        if (!src_space) {
          src_space = "";
        }
        if (addr.address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
          fn_error(&fn, "PTX: store to constant address space");
          break;
        }
        if (!dst_space) {
          fn_error(&fn, "PTX: invalid store address space %d",
                   (int)addr.address_space);
          break;
        }
        while (offset < total) {
          long long width = total - offset >= 8 ? 8
                            : total - offset >= 4 ? 4
                            : total - offset >= 2 ? 2 : 1;
          const char *suffix = width == 8 ? "b64"
                               : width == 4 ? "b32"
                               : width == 2 ? "u16" : "u8";
          PtxClass cls = width == 8 ? PC_B64 : PC_B32;
          char tmp[24];
          reg_name(cls, new_reg(&fn, cls), tmp);
          sb_printf(&fn.body, "\tld%s.%s %s, [%s+%lld];\n", src_space,
                    suffix, tmp, srcreg, offset);
          sb_printf(&fn.body, "\tst%s.%s [%s+%lld], %s;\n", dst_space,
                    suffix, dstreg, offset, tmp);
          offset += width;
        }
        break;
      }
      MtlcTypeKind elem = addr.is_ptr ? addr.elem : MTLC_TYPE_VOID;
      if (elem == MTLC_TYPE_VOID) {
        long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
        if (in->is_float) {
          if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_F16) {
            elem = MTLC_TYPE_FLOAT16;
          } else if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_BF16) {
            elem = MTLC_TYPE_BFLOAT16;
          } else {
            elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
          }
        } else {
          elem = (sz == 8) ? MTLC_TYPE_INT64
                 : (sz == 2) ? MTLC_TYPE_INT16
                 : (sz == 1) ? MTLC_TYPE_UINT8
                             : MTLC_TYPE_INT32;
        }
      }
      int u = 0;
      PtxClass vc = elem_class(elem, &u);
      char addrreg[24], valreg[24];
      use_as(&fn, &in->dest, PC_B64, addrreg);
      use_as(&fn, &in->lhs, vc, valreg);
      if (vc == PC_B64) {
        ptx_generic_address(&fn, &in->lhs, valreg);
      }
      const char *space = ptx_memory_space(addr.address_space);
      if (addr.address_space == MTLC_ADDRESS_SPACE_DEFAULT ||
          addr.address_space == MTLC_ADDRESS_SPACE_GENERIC) {
        g_ptx_generic_accesses++;
      } else {
        g_ptx_spaced_accesses++;
      }
      if (addr.address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
        fn_error(&fn, "PTX: store to constant address space");
      } else if (!space) {
        fn_error(&fn, "PTX: invalid store address space %d",
                 (int)addr.address_space);
      } else if (elem == MTLC_TYPE_FLOAT16 || elem == MTLC_TYPE_BFLOAT16) {
        char tmp[24];
        reg_name(PC_B16, new_reg(&fn, PC_B16), tmp);
        sb_printf(&fn.body, "\tcvt.rn.%s.f32 %s, %s;\n",
                  elem == MTLC_TYPE_FLOAT16 ? "f16" : "bf16", tmp, valreg);
        sb_printf(&fn.body, "\tst%s.b16 [%s], %s;\n", space, addrreg, tmp);
      } else {
        sb_printf(&fn.body, "\tst%s.%s [%s], %s;\n", space,
                  mem_type_suffix(elem), addrreg, valreg);
      }
      break;
    }
    case IR_OP_BINARY:
      emit_binary(&fn, in);
      break;
    case IR_OP_UNARY: {
      const char *t = in->text ? in->text : "";
      PtxVal sv = operand_desc(&fn, &in->lhs);
      PtxClass c = in->is_float ? (in->float_bits == 32 ? PC_F32 : PC_F64)
                                : (sv.cls == PC_B64 ? PC_B64 : PC_B32);
      char s[24];
      use_as(&fn, &in->lhs, c, s);
      PtxVal dv = {0};
      dv.cls = c;
      dv = destination_value(&fn, &in->dest, dv);
      char dn[24];
      reg_name(c, dv.idx, dn);
      if (!strcmp(t, "-")) {
        sb_printf(&fn.body, "\tneg.%s %s, %s;\n", type_suffix_for_class(c, 0),
                  dn, s);
      } else if (!strcmp(t, "~")) {
        sb_printf(&fn.body, "\tnot.%s %s, %s;\n", c == PC_B64 ? "b64" : "b32",
                  dn, s);
      } else if (!strcmp(t, "!")) {
        int p = new_reg(&fn, PC_PRED);
        char pn[24];
        reg_name(PC_PRED, p, pn);
        sb_printf(&fn.body, "\tsetp.eq.%s %s, %s, 0;\n",
                  c == PC_B64 ? "s64" : "s32", pn, s);
        sb_printf(&fn.body, "\tselp.u32 %s, 1, 0, %s;\n", dn, pn);
      } else {
        fn_error(&fn, "PTX: unsupported unary op '%s'", t);
      }
      if (in->dest.name) {
        bind_value(&fn, in->dest.name, dv);
      }
      break;
    }
    case IR_OP_CAST: {
      /* dest = (text) lhs */
      PtxVal target = in->value_type ? descriptor_from_type(in->value_type)
                                     : descriptor_from_typename(in->text);
      MtlcTypeKind target_elem = target.is_ptr ? MTLC_TYPE_VOID : target.elem;
      if (!target.is_ptr &&
          (target_elem == MTLC_TYPE_FLOAT16 || target_elem == MTLC_TYPE_BFLOAT16) &&
          in->is_float) {
        char s[24];
        PtxClass src_cls = (in->float_bits == 64) ? PC_F64 : PC_F32;
        use_as(&fn, &in->lhs, src_cls, s);
        target.cls = PC_F32;
        target = destination_value(&fn, &in->dest, target);
        char dn[24];
        reg_name(target.cls, target.idx, dn);
        char tmp[24];
        reg_name(PC_B16, new_reg(&fn, PC_B16), tmp);
        const char *to = (target_elem == MTLC_TYPE_FLOAT16) ? "f16" : "bf16";
        char f32tmp[24];
        if (src_cls == PC_F64) {
          reg_name(PC_F32, new_reg(&fn, PC_F32), f32tmp);
          sb_printf(&fn.body, "\tcvt.rn.f32.f64 %s, %s;\n", f32tmp, s);
          sb_printf(&fn.body, "\tcvt.rn.%s.f32 %s, %s;\n", to, tmp, f32tmp);
        } else {
          sb_printf(&fn.body, "\tcvt.rn.%s.f32 %s, %s;\n", to, tmp, s);
        }
        sb_printf(&fn.body, "\tcvt.f32.%s %s, %s;\n", to, dn, tmp);
        if (in->dest.name) {
          bind_value(&fn, in->dest.name, target);
        }
        break;
      }
      char s[24];
      use_as(&fn, &in->lhs, target.cls, s);
      target = destination_value(&fn, &in->dest, target);
      char dn[24];
      reg_name(target.cls, target.idx, dn);
      if (strcmp(dn, s) != 0) {
        const char *mt = (target.cls == PC_F32)   ? "f32"
                         : (target.cls == PC_F64) ? "f64"
                         : (target.cls == PC_B64) ? "u64"
                                                  : "u32";
        sb_printf(&fn.body, "\tmov.%s %s, %s;\n", mt, dn, s);
      }
      if (in->dest.name) {
        bind_value(&fn, in->dest.name, target);
      }
      break;
    }
    case IR_OP_CALL: {
      const char *callee = in->text;
      MtlcIntrinsic intrinsic = in->intrinsic;
      const char *sreg = sreg_for_intrinsic(intrinsic);
      if (sreg) {
        PtxVal dv = {0};
        dv.cls = PC_B32;
        dv.is_unsigned = 1;
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tmov.u32 %s, %s;\n", dn, sreg);
        if (in->dest.name) {
          bind_value(&fn, in->dest.name, dv);
        }
      } else if (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SIZE) {
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        /* A PTX execution subgroup is an NVIDIA warp. This backend-specific
         * width never leaks into the semantic IR or SPIR-V lowering. */
        sb_printf(&fn.body, "\tmov.u32 %s, 32;\n", dn);
        if (in->dest.name) bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32) &&
                 in->argument_count >= 2) {
        int is_float =
            intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32;
        PtxClass cls = is_float ? PC_F32 : PC_B32;
        char value[24], source_lane[24], mask[24], dn[24];
        use_as(&fn, &in->arguments[0], cls, value);
        use_as(&fn, &in->arguments[1], PC_B32, source_lane);
        int mask_index = new_reg(&fn, PC_B32);
        reg_name(PC_B32, mask_index, mask);
        PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
        dv = destination_value(&fn, &in->dest, dv);
        reg_name(cls, dv.idx, dn);
        sb_printf(&fn.body, "\tactivemask.b32 %s;\n", mask);
        sb_printf(&fn.body,
                  "\tshfl.sync.idx.b32 %s, %s, %s, 0x1f, %s;\n",
                  dn, value, source_lane, mask);
        if (in->dest.name) bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32) &&
                 in->argument_count >= 2) {
        int is_float = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32;
        PtxClass cls = is_float ? PC_F32 : PC_B32;
        char value[24], source_lane[24], mask[24], other[24], dn[24];
        char shuffle_ok[24], source_in_range[24], source_active[24];
        char bit[24], active_bits[24], take[24];
        use_as(&fn, &in->arguments[0], cls, value);
        use_as(&fn, &in->arguments[1], PC_B32, source_lane);
        int mask_index = new_reg(&fn, PC_B32);
        int other_index = new_reg(&fn, cls);
        int shuffle_ok_index = new_reg(&fn, PC_PRED);
        int source_in_range_index = new_reg(&fn, PC_PRED);
        int source_active_index = new_reg(&fn, PC_PRED);
        int bit_index = new_reg(&fn, PC_B32);
        int active_bits_index = new_reg(&fn, PC_B32);
        int take_index = new_reg(&fn, PC_PRED);
        reg_name(PC_B32, mask_index, mask);
        reg_name(cls, other_index, other);
        reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
        reg_name(PC_PRED, source_in_range_index, source_in_range);
        reg_name(PC_PRED, source_active_index, source_active);
        reg_name(PC_B32, bit_index, bit);
        reg_name(PC_B32, active_bits_index, active_bits);
        reg_name(PC_PRED, take_index, take);
        PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
        dv = destination_value(&fn, &in->dest, dv);
        reg_name(cls, dv.idx, dn);
        sb_printf(&fn.body, "\tactivemask.b32 %s;\n", mask);
        sb_printf(&fn.body,
                  "\tshfl.sync.idx.b32 %s|%s, %s, %s, 0x1f, %s;\n",
                  other, shuffle_ok, value, source_lane, mask);
        sb_printf(&fn.body, "\tsetp.lt.u32 %s, %s, 32;\n",
                  source_in_range, source_lane);
        sb_printf(&fn.body, "\tmov.u32 %s, 0;\n", bit);
        sb_printf(&fn.body, "\t@%s shl.b32 %s, 1, %s;\n",
                  source_in_range, bit, source_lane);
        sb_printf(&fn.body, "\tand.b32 %s, %s, %s;\n", active_bits,
                  mask, bit);
        sb_printf(&fn.body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                  active_bits);
        sb_printf(&fn.body, "\tand.pred %s, %s, %s;\n", take,
                  shuffle_ok, source_active);
        sb_printf(&fn.body, "\tmov.%s %s, %s;\n",
                  is_float ? "f32" : "u32", dn, value);
        sb_printf(&fn.body, "\t@%s mov.%s %s, %s;\n", take,
                  is_float ? "f32" : "u32", dn, other);
        if (in->dest.name) bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD &&
                 in->argument_count >= 2) {
        char predicate[24], word[24], mask[24], bits[24], word_zero[24], dn[24];
        use_as(&fn, &in->arguments[0], PC_PRED, predicate);
        use_as(&fn, &in->arguments[1], PC_B32, word);
        int mask_index = new_reg(&fn, PC_B32);
        int bits_index = new_reg(&fn, PC_B32);
        int word_zero_index = new_reg(&fn, PC_PRED);
        reg_name(PC_B32, mask_index, mask);
        reg_name(PC_B32, bits_index, bits);
        reg_name(PC_PRED, word_zero_index, word_zero);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tactivemask.b32 %s;\n", mask);
        sb_printf(&fn.body, "\tvote.sync.ballot.b32 %s, %s, %s;\n",
                  bits, predicate, mask);
        sb_printf(&fn.body, "\tsetp.eq.u32 %s, %s, 0;\n", word_zero, word);
        sb_printf(&fn.body, "\tmov.u32 %s, 0;\n", dn);
        sb_printf(&fn.body, "\t@%s mov.u32 %s, %s;\n", word_zero, dn, bits);
        if (in->dest.name) bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ANY ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ALL) &&
                 in->argument_count >= 1) {
        char predicate[24], mask[24], dn[24];
        use_as(&fn, &in->arguments[0], PC_PRED, predicate);
        int mask_index = new_reg(&fn, PC_B32);
        reg_name(PC_B32, mask_index, mask);
        PtxVal dv = {.cls = PC_PRED, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        reg_name(PC_PRED, dv.idx, dn);
        sb_printf(&fn.body, "\tactivemask.b32 %s;\n", mask);
        sb_printf(&fn.body, "\tvote.sync.%s.pred %s, %s, %s;\n",
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ANY ? "any" : "all",
                  dn, predicate, mask);
        if (in->dest.name) bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32) &&
                  in->argument_count >= 1) {
        int is_float = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32 ||
                       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32 ||
                       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32;
        const char *operation =
            (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32 ||
             intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32)
                ? "min"
            : (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32 ||
               intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32)
                ? "max"
                : "add";
        PtxClass cls = is_float ? PC_F32 : PC_B32;
        char input[24], accum[24], mask[24], lane[24];
        use_as(&fn, &in->arguments[0], cls, input);
        PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
        dv = destination_value(&fn, &in->dest, dv);
        reg_name(cls, dv.idx, accum);
        sb_printf(&fn.body, "\tmov.%s %s, %s;\n",
                  is_float ? "f32" : "u32", accum, input);
        int mask_index = new_reg(&fn, PC_B32);
        int lane_index = new_reg(&fn, PC_B32);
        reg_name(PC_B32, mask_index, mask);
        reg_name(PC_B32, lane_index, lane);
        sb_printf(&fn.body, "\tactivemask.b32 %s;\n", mask);
        sb_printf(&fn.body, "\tmov.u32 %s, %%laneid;\n", lane);

        /* Uniform collectives may end in a partial final warp. A plain
         * butterfly reduction would read inactive lanes for non-power-of-two
         * sizes, so each tree edge is guarded by the captured active mask.
         * Lane zero then broadcasts the complete sum to every participant. */
        static const unsigned offsets[] = {16, 8, 4, 2, 1};
        for (size_t oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]); oi++) {
          unsigned offset = offsets[oi];
          char other[24], source[24], bit[24], active_bits[24];
          char shuffle_ok[24], source_in_range[24], source_active[24], take[24];
          int other_index = new_reg(&fn, cls);
          int source_index = new_reg(&fn, PC_B32);
          int bit_index = new_reg(&fn, PC_B32);
          int active_bits_index = new_reg(&fn, PC_B32);
          int shuffle_ok_index = new_reg(&fn, PC_PRED);
          int source_in_range_index = new_reg(&fn, PC_PRED);
          int source_active_index = new_reg(&fn, PC_PRED);
          int take_index = new_reg(&fn, PC_PRED);
          reg_name(cls, other_index, other);
          reg_name(PC_B32, source_index, source);
          reg_name(PC_B32, bit_index, bit);
          reg_name(PC_B32, active_bits_index, active_bits);
          reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
          reg_name(PC_PRED, source_in_range_index, source_in_range);
          reg_name(PC_PRED, source_active_index, source_active);
          reg_name(PC_PRED, take_index, take);
          sb_printf(&fn.body,
                    "\tshfl.sync.down.b32 %s|%s, %s, %u, 0x1f, %s;\n",
                    other, shuffle_ok, accum, offset, mask);
          sb_printf(&fn.body, "\tadd.u32 %s, %s, %u;\n", source, lane,
                    offset);
          sb_printf(&fn.body, "\tsetp.lt.u32 %s, %s, 32;\n",
                    source_in_range, source);
          sb_printf(&fn.body, "\tmov.u32 %s, 0;\n", bit);
          sb_printf(&fn.body, "\t@%s shl.b32 %s, 1, %s;\n",
                    source_in_range, bit, source);
          sb_printf(&fn.body, "\tand.b32 %s, %s, %s;\n", active_bits,
                    mask, bit);
          sb_printf(&fn.body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                    active_bits);
          sb_printf(&fn.body, "\tand.pred %s, %s, %s;\n", take,
                    shuffle_ok, source_active);
          sb_printf(&fn.body, "\t@%s %s.%s %s, %s, %s;\n", take, operation,
                     is_float ? "f32" : "u32", accum, accum, other);
        }
        sb_printf(&fn.body,
                  "\tshfl.sync.idx.b32 %s, %s, 0, 0x1f, %s;\n",
                  accum, accum, mask);
        if (in->dest.name) bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic ==
                      MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32 ||
                  intrinsic ==
                      MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32 ||
                  intrinsic ==
                      MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32 ||
                  intrinsic ==
                      MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32) &&
                 in->argument_count >= 1) {
        int is_float =
            intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32 ||
            intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32;
        int is_exclusive =
            intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32 ||
            intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32;
        PtxClass cls = is_float ? PC_F32 : PC_B32;
        char input[24], accum[24], mask[24], lane[24];
        use_as(&fn, &in->arguments[0], cls, input);
        PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
        dv = destination_value(&fn, &in->dest, dv);
        reg_name(cls, dv.idx, accum);
        sb_printf(&fn.body, "\tmov.%s %s, %s;\n",
                  is_float ? "f32" : "u32", accum, input);
        int mask_index = new_reg(&fn, PC_B32);
        int lane_index = new_reg(&fn, PC_B32);
        reg_name(PC_B32, mask_index, mask);
        reg_name(PC_B32, lane_index, lane);
        sb_printf(&fn.body, "\tactivemask.b32 %s;\n", mask);
        sb_printf(&fn.body, "\tmov.u32 %s, %%laneid;\n", lane);

        static const unsigned scan_offsets[] = {1, 2, 4, 8, 16};
        for (size_t oi = 0;
             oi < sizeof(scan_offsets) / sizeof(scan_offsets[0]); oi++) {
          unsigned offset = scan_offsets[oi];
          char other[24], source[24], bit[24], active_bits[24];
          char shuffle_ok[24], source_in_range[24], source_active[24], take[24];
          int other_index = new_reg(&fn, cls);
          int source_index = new_reg(&fn, PC_B32);
          int bit_index = new_reg(&fn, PC_B32);
          int active_bits_index = new_reg(&fn, PC_B32);
          int shuffle_ok_index = new_reg(&fn, PC_PRED);
          int source_in_range_index = new_reg(&fn, PC_PRED);
          int source_active_index = new_reg(&fn, PC_PRED);
          int take_index = new_reg(&fn, PC_PRED);
          reg_name(cls, other_index, other);
          reg_name(PC_B32, source_index, source);
          reg_name(PC_B32, bit_index, bit);
          reg_name(PC_B32, active_bits_index, active_bits);
          reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
          reg_name(PC_PRED, source_in_range_index, source_in_range);
          reg_name(PC_PRED, source_active_index, source_active);
          reg_name(PC_PRED, take_index, take);
          sb_printf(&fn.body,
                    "\tshfl.sync.up.b32 %s|%s, %s, %u, 0, %s;\n",
                    other, shuffle_ok, accum, offset, mask);
          sb_printf(&fn.body, "\tsetp.ge.u32 %s, %s, %u;\n",
                    source_in_range, lane, offset);
          sb_printf(&fn.body, "\tsub.u32 %s, %s, %u;\n", source, lane,
                    offset);
          sb_printf(&fn.body, "\tmov.u32 %s, 0;\n", bit);
          sb_printf(&fn.body, "\t@%s shl.b32 %s, 1, %s;\n",
                    source_in_range, bit, source);
          sb_printf(&fn.body, "\tand.b32 %s, %s, %s;\n", active_bits,
                    mask, bit);
          sb_printf(&fn.body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                    active_bits);
          sb_printf(&fn.body, "\tand.pred %s, %s, %s;\n", take,
                    shuffle_ok, source_active);
          sb_printf(&fn.body, "\t@%s add.%s %s, %s, %s;\n", take,
                    is_float ? "f32" : "u32", accum, accum, other);
        }

        if (is_exclusive) {
          char other[24], source[24], bit[24], active_bits[24];
          char shuffle_ok[24], has_predecessor[24], source_active[24];
          char take[24], take_active[24];
          int other_index = new_reg(&fn, cls);
          int source_index = new_reg(&fn, PC_B32);
          int bit_index = new_reg(&fn, PC_B32);
          int active_bits_index = new_reg(&fn, PC_B32);
          int shuffle_ok_index = new_reg(&fn, PC_PRED);
          int has_predecessor_index = new_reg(&fn, PC_PRED);
          int source_active_index = new_reg(&fn, PC_PRED);
          int take_index = new_reg(&fn, PC_PRED);
          int take_active_index = new_reg(&fn, PC_PRED);
          reg_name(cls, other_index, other);
          reg_name(PC_B32, source_index, source);
          reg_name(PC_B32, bit_index, bit);
          reg_name(PC_B32, active_bits_index, active_bits);
          reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
          reg_name(PC_PRED, has_predecessor_index, has_predecessor);
          reg_name(PC_PRED, source_active_index, source_active);
          reg_name(PC_PRED, take_index, take);
          reg_name(PC_PRED, take_active_index, take_active);
          sb_printf(&fn.body,
                    "\tshfl.sync.up.b32 %s|%s, %s, 1, 0, %s;\n",
                    other, shuffle_ok, accum, mask);
          sb_printf(&fn.body, "\tsetp.ge.u32 %s, %s, 1;\n",
                    has_predecessor, lane);
          sb_printf(&fn.body, "\tsub.u32 %s, %s, 1;\n", source, lane);
          sb_printf(&fn.body, "\tmov.u32 %s, 0;\n", bit);
          sb_printf(&fn.body, "\t@%s shl.b32 %s, 1, %s;\n",
                    has_predecessor, bit, source);
          sb_printf(&fn.body, "\tand.b32 %s, %s, %s;\n", active_bits,
                    mask, bit);
          sb_printf(&fn.body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                    active_bits);
          sb_printf(&fn.body, "\tand.pred %s, %s, %s;\n", take,
                    shuffle_ok, has_predecessor);
          sb_printf(&fn.body, "\tand.pred %s, %s, %s;\n", take_active,
                    take, source_active);
          sb_printf(&fn.body, "\tmov.%s %s, %s;\n",
                    is_float ? "f32" : "u32", accum,
                    is_float ? "0f00000000" : "0");
          sb_printf(&fn.body, "\t@%s mov.%s %s, %s;\n", take_active,
                    is_float ? "f32" : "u32", accum, other);
        }
        if (in->dest.name) bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_WORKGROUP_BARRIER) {
        sb_puts(&fn.body, "\tbar.sync 0;\n");
      } else if (intrinsic == MTLC_INTRINSIC_GPU_F16_BITS_TO_F32 &&
                 in->argument_count >= 1) {
        /* h2f(bits): reinterpret a uint16 fp16 bit-pattern as float32. The arg
         * arrives as a 32-bit int (zero-extended u16 load); truncate to .b16 and
         * cvt.f32.f16. Lets prefill keep fp16-resident weights and convert on
         * the fly with one PTX instruction. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        int hidx = new_reg(&fn, PC_B16);
        char hn[24];
        reg_name(PC_B16, hidx, hn);
        sb_printf(&fn.body, "\tcvt.u16.u32 %s, %s;\n", hn, a);
        PtxVal dv = {.cls = PC_F32};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_F32, dv.idx, dn);
        sb_printf(&fn.body, "\tcvt.f32.f16 %s, %s;\n", dn, hn);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_F16_BITS &&
                 in->argument_count >= 1) {
        /* f2h(x): float32 -> uint16 fp16 bit-pattern (cvt.rn.f16.f32), returned
         * zero-extended in a 32-bit int so a uint16 store writes the low 16. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_F32, a);
        int hidx = new_reg(&fn, PC_B16);
        char hn[24];
        reg_name(PC_B16, hidx, hn);
        sb_printf(&fn.body, "\tcvt.rn.f16.f32 %s, %s;\n", hn, a);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tcvt.u32.u16 %s, %s;\n", dn, hn);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_FROM_BITS &&
                 in->argument_count >= 1) {
        /* f32_from_bits(u32): reinterpret an IEEE-754 bit pattern as float32.
         * One mov.b32 across register files; no arithmetic reconstruction. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        PtxVal dv = {.cls = PC_F32};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_F32, dv.idx, dn);
        sb_printf(&fn.body, "\tmov.b32 %s, %s;\n", dn, a);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_BITS &&
                 in->argument_count >= 1) {
        /* bits_from_f32(x): the float32's IEEE-754 encoding as uint32. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_F32, a);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tmov.b32 %s, %s;\n", dn, a);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_DP4A_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32) &&
                 in->argument_count >= 3) {
        /* dp4a(a, b, c): four-way packed-byte dot product with 32-bit
         * accumulate. Collapses the shift/mask/convert/FMA chain of quantized
         * decode to one instruction (sm_61+; every supported target qualifies). */
        int is_signed = intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32;
        char a[24];
        char b[24];
        char c[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        use_as(&fn, &in->arguments[1], PC_B32, b);
        use_as(&fn, &in->arguments[2], PC_B32, c);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = !is_signed};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tdp4a.%s %s, %s, %s, %s;\n",
                  is_signed ? "s32.s32" : "u32.u32", dn, a, b, c);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_HADD2 ||
                  intrinsic == MTLC_INTRINSIC_GPU_HMUL2 ||
                  intrinsic == MTLC_INTRINSIC_GPU_HFMA2) &&
                 in->argument_count >= 2) {
        /* Two fp16 lanes per instruction. The carrier is a 32-bit value
         * holding {hi, lo}; f16x2 arithmetic is the reason to keep
         * activations packed rather than widening every element to f32. */
        int is_fma = intrinsic == MTLC_INTRINSIC_GPU_HFMA2;
        char a[24];
        char b[24];
        char c[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        use_as(&fn, &in->arguments[1], PC_B32, b);
        if (is_fma) {
          use_as(&fn, &in->arguments[2], PC_B32, c);
        }
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        if (is_fma) {
          sb_printf(&fn.body, "\tfma.rn.f16x2 %s, %s, %s, %s;\n", dn, a, b, c);
        } else {
          sb_printf(&fn.body, "\t%s.rn.f16x2 %s, %s, %s;\n",
                    intrinsic == MTLC_INTRINSIC_GPU_HADD2 ? "add" : "mul", dn,
                    a, b);
        }
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_H2F_LO ||
                  intrinsic == MTLC_INTRINSIC_GPU_H2F_HI) &&
                 in->argument_count >= 1) {
        /* Widen one lane of a packed pair to f32. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        const char *source = a;
        char shifted_name[24];
        if (intrinsic == MTLC_INTRINSIC_GPU_H2F_HI) {
          int shifted = new_reg(&fn, PC_B32);
          reg_name(PC_B32, shifted, shifted_name);
          sb_printf(&fn.body, "\tshr.u32 %s, %s, 16;\n", shifted_name, a);
          source = shifted_name;
        }
        int half = new_reg(&fn, PC_B16);
        char half_name[24];
        reg_name(PC_B16, half, half_name);
        sb_printf(&fn.body, "\tcvt.u16.u32 %s, %s;\n", half_name, source);
        PtxVal dv = {.cls = PC_F32};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_F32, dv.idx, dn);
        sb_printf(&fn.body, "\tcvt.f32.f16 %s, %s;\n", dn, half_name);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_F2H2 &&
                 in->argument_count >= 2) {
        /* Pack two f32 values into one f16 pair, low lane first. */
        char lo[24];
        char hi[24];
        use_as(&fn, &in->arguments[0], PC_F32, lo);
        use_as(&fn, &in->arguments[1], PC_F32, hi);
        int lo_half = new_reg(&fn, PC_B16);
        int hi_half = new_reg(&fn, PC_B16);
        char lo_name[24];
        char hi_name[24];
        reg_name(PC_B16, lo_half, lo_name);
        reg_name(PC_B16, hi_half, hi_name);
        sb_printf(&fn.body, "\tcvt.rn.f16.f32 %s, %s;\n", lo_name, lo);
        sb_printf(&fn.body, "\tcvt.rn.f16.f32 %s, %s;\n", hi_name, hi);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tmov.b32 %s, {%s, %s};\n", dn, lo_name, hi_name);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_BF2F &&
                 in->argument_count >= 1) {
        /* bfloat16 is the top half of an f32, so widening is exact and needs
         * no conversion instruction or architecture floor. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        int shifted = new_reg(&fn, PC_B32);
        char shifted_name[24];
        reg_name(PC_B32, shifted, shifted_name);
        sb_printf(&fn.body, "\tshl.b32 %s, %s, 16;\n", shifted_name, a);
        PtxVal dv = {.cls = PC_F32};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_F32, dv.idx, dn);
        sb_printf(&fn.body, "\tmov.b32 %s, %s;\n", dn, shifted_name);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_F2BF &&
                 in->argument_count >= 1) {
        /* Round-to-nearest-even down to bfloat16, in integer arithmetic so it
         * holds on every target rather than only where cvt.rn.bf16.f32 does:
         * add half an ulp plus the low bit of the kept mantissa, then drop
         * the low half. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_F32, a);
        int bits = new_reg(&fn, PC_B32);
        int carry = new_reg(&fn, PC_B32);
        int rounded = new_reg(&fn, PC_B32);
        char bits_name[24];
        char carry_name[24];
        char rounded_name[24];
        reg_name(PC_B32, bits, bits_name);
        reg_name(PC_B32, carry, carry_name);
        reg_name(PC_B32, rounded, rounded_name);
        sb_printf(&fn.body, "\tmov.b32 %s, %s;\n", bits_name, a);
        sb_printf(&fn.body, "\tshr.u32 %s, %s, 16;\n", carry_name, bits_name);
        sb_printf(&fn.body, "\tand.b32 %s, %s, 1;\n", carry_name, carry_name);
        sb_printf(&fn.body, "\tadd.u32 %s, %s, 32767;\n", rounded_name,
                  carry_name);
        sb_printf(&fn.body, "\tadd.u32 %s, %s, %s;\n", rounded_name,
                  rounded_name, bits_name);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tshr.u32 %s, %s, 16;\n", dn, rounded_name);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_ASSERT &&
                 in->argument_count >= 1) {
        /* A false condition traps the launch at the offending lane, which is
         * what turns "the numbers are wrong somewhere" into a located fault.
         * Only under --gpu-checks: an assertion left in shipped source must
         * cost nothing. */
        if (g_ptx_emit_checks) {
          char condition[24];
          use_as(&fn, &in->arguments[0], PC_B32, condition);
          int predicate = new_reg(&fn, PC_PRED);
          char predicate_name[24];
          reg_name(PC_PRED, predicate, predicate_name);
          size_t label = fn.call_count++;
          sb_printf(&fn.body, "\tsetp.ne.s32 %s, %s, 0;\n", predicate_name,
                    condition);
          sb_printf(&fn.body, "\t@%s bra $mtlc_assert_ok_%zu;\n",
                    predicate_name, label);
          sb_puts(&fn.body, "\ttrap;\n");
          sb_printf(&fn.body, "$mtlc_assert_ok_%zu:\n", label);
        }
      } else if (ptx_intrinsic_is_print(intrinsic) &&
                 in->argument_count >= 1) {
        /* vprintf(format, argument_buffer). The format is a module-scope byte
         * array interned in the pool; the arguments go through a per-call
         * local buffer laid out to the C varargs rules the device runtime
         * reads them back with (4-byte ints, doubles for floats). */
        const char *format = ptx_literal_string(fn.function, &in->arguments[0]);
        if (!format) {
          fn_error(&fn, "PTX: a kernel print needs a literal format string");
        } else {
          int index = ptx_string_index(format);
          if (index < 0) {
            fn_error(&fn, "PTX: kernel print format was not interned");
          } else {
            size_t site = fn.call_count++;
            int buffer_bytes =
                intrinsic == MTLC_INTRINSIC_GPU_PRINT      ? 0
                : intrinsic == MTLC_INTRINSIC_GPU_PRINT_I32 ? 4
                : intrinsic == MTLC_INTRINSIC_GPU_PRINT_F32 ? 8
                                                            : 8;
            if (buffer_bytes > 0) {
              sb_printf(&fn.declarations,
                        "\t.local .align 8 .b8 $mtlc_print_args_%zu[%d];\n",
                        site, buffer_bytes);
            }
            int format_register = new_reg(&fn, PC_B64);
            char format_name[24];
            reg_name(PC_B64, format_register, format_name);
            sb_printf(&fn.body, "\tmov.u64 %s, $mtlc_str_%d;\n", format_name,
                      index);
            sb_printf(&fn.body, "\tcvta.global.u64 %s, %s;\n", format_name,
                      format_name);
            int args_register = new_reg(&fn, PC_B64);
            char args_name[24];
            reg_name(PC_B64, args_register, args_name);
            if (buffer_bytes > 0) {
              sb_printf(&fn.body, "\tmov.u64 %s, $mtlc_print_args_%zu;\n",
                        args_name, site);
              sb_printf(&fn.body, "\tcvta.local.u64 %s, %s;\n", args_name,
                        args_name);
              if (intrinsic == MTLC_INTRINSIC_GPU_PRINT_I32) {
                char value[24];
                use_as(&fn, &in->arguments[1], PC_B32, value);
                sb_printf(&fn.body, "\tst.u32 [%s], %s;\n", args_name, value);
              } else if (intrinsic == MTLC_INTRINSIC_GPU_PRINT_F32) {
                /* C varargs promote float to double before the callee reads
                 * it, so %f in the format string expects eight bytes. */
                char value[24];
                use_as(&fn, &in->arguments[1], PC_F32, value);
                int widened = new_reg(&fn, PC_F64);
                char widened_name[24];
                reg_name(PC_F64, widened, widened_name);
                sb_printf(&fn.body, "\tcvt.f64.f32 %s, %s;\n", widened_name,
                          value);
                sb_printf(&fn.body, "\tst.f64 [%s], %s;\n", args_name,
                          widened_name);
              } else {
                char first[24];
                char second[24];
                use_as(&fn, &in->arguments[1], PC_B32, first);
                use_as(&fn, &in->arguments[2], PC_B32, second);
                sb_printf(&fn.body, "\tst.u32 [%s], %s;\n", args_name, first);
                sb_printf(&fn.body, "\tst.u32 [%s+4], %s;\n", args_name,
                          second);
              }
            } else {
              sb_printf(&fn.body, "\tmov.u64 %s, 0;\n", args_name);
            }
            sb_puts(&fn.body, "\t{\n");
            sb_puts(&fn.body, "\t.param .b64 $mtlc_print_fmt;\n");
            sb_puts(&fn.body, "\t.param .b64 $mtlc_print_buf;\n");
            sb_puts(&fn.body, "\t.param .b32 $mtlc_print_ret;\n");
            sb_printf(&fn.body, "\tst.param.b64 [$mtlc_print_fmt+0], %s;\n",
                      format_name);
            sb_printf(&fn.body, "\tst.param.b64 [$mtlc_print_buf+0], %s;\n",
                      args_name);
            sb_puts(&fn.body,
                    "\tcall.uni ($mtlc_print_ret), vprintf, "
                    "($mtlc_print_fmt, $mtlc_print_buf);\n");
            sb_puts(&fn.body, "\t}\n");
          }
        }
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32) &&
                 in->argument_count >= 3) {
        /* dp2a: two 16-bit halves of a against two bytes of b (low or high
         * pair), accumulated into c. The mixed-width sibling of dp4a. */
        int is_signed = intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
                        intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32;
        int is_high = intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ||
                      intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32;
        char a[24];
        char b[24];
        char c[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        use_as(&fn, &in->arguments[1], PC_B32, b);
        use_as(&fn, &in->arguments[2], PC_B32, c);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = !is_signed};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tdp2a.%s.%s %s, %s, %s, %s;\n",
                  is_high ? "hi" : "lo", is_signed ? "s32.s32" : "u32.u32", dn,
                  a, b, c);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_PRMT_B32 &&
                 in->argument_count >= 3) {
        /* prmt(a, b, selector): gather four bytes out of the {b:a} byte octet,
         * one selector nibble each. One instruction for the byte shuffling
         * that nibble-quant decode otherwise spends shifts and masks on. */
        char a[24];
        char b[24];
        char sel[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        use_as(&fn, &in->arguments[1], PC_B32, b);
        use_as(&fn, &in->arguments[2], PC_B32, sel);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tprmt.b32 %s, %s, %s, %s;\n", dn, a, b, sel);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if ((intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_STORE4_U32) &&
                 in->argument_count >= 2) {
        /* One 128-bit transaction for four consecutive 32-bit elements. The
         * scalar side is four ordinary accesses against the other pointer,
         * which ptxas keeps in registers when that pointer is a small
         * constant-indexed private array. Both addresses must be 16-byte
         * aligned; that is a source contract, as it is for async copies. */
        int is_load = intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                      intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32;
        int is_float = intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                       intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32;
        PtxClass cls = is_float ? PC_F32 : PC_B32;
        const char *suffix = is_float ? "f32" : "u32";
        /* Argument order is always (vector side, scalar side): load4 reads the
         * vector pointer, store4 writes it. */
        const IROperand *vector_operand = &in->arguments[0];
        const IROperand *scalar_operand = &in->arguments[1];
        PtxVal vector_desc = operand_desc(&fn, vector_operand);
        PtxVal scalar_desc = operand_desc(&fn, scalar_operand);
        const char *vector_space = ptx_memory_space(vector_desc.address_space);
        const char *scalar_space = ptx_memory_space(scalar_desc.address_space);
        char vector_address[24];
        char scalar_address[24];
        use_as(&fn, vector_operand, PC_B64, vector_address);
        use_as(&fn, scalar_operand, PC_B64, scalar_address);
        if (!vector_space || !scalar_space) {
          fn_error(&fn, "PTX: invalid address space in a 4-wide transfer");
        } else {
          int lanes[4];
          char lane_names[4][24];
          for (int lane = 0; lane < 4; lane++) {
            lanes[lane] = new_reg(&fn, cls);
            reg_name(cls, lanes[lane], lane_names[lane]);
          }
          if (is_load) {
            sb_printf(&fn.body, "\tld%s.v4.%s {%s, %s, %s, %s}, [%s];\n",
                      vector_space, suffix, lane_names[0], lane_names[1],
                      lane_names[2], lane_names[3], vector_address);
            for (int lane = 0; lane < 4; lane++) {
              sb_printf(&fn.body, "\tst%s.%s [%s+%d], %s;\n", scalar_space,
                        suffix, scalar_address, lane * 4, lane_names[lane]);
            }
          } else {
            for (int lane = 0; lane < 4; lane++) {
              sb_printf(&fn.body, "\tld%s.%s %s, [%s+%d];\n", scalar_space,
                        suffix, lane_names[lane], scalar_address, lane * 4);
            }
            sb_printf(&fn.body, "\tst%s.v4.%s [%s], {%s, %s, %s, %s};\n",
                      vector_space, suffix, vector_address, lane_names[0],
                      lane_names[1], lane_names[2], lane_names[3]);
          }
        }
      } else if (intrinsic >= MTLC_INTRINSIC_GPU_SQRT_F32 &&
                 intrinsic <= MTLC_INTRINSIC_GPU_EXP_F32 &&
                 in->argument_count >= 1) {
        /* single-arg f32 math -> PTX approximations (inference-grade, mirrors
         * the fast CPU approximations the engine already uses). */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_F32, a);
        PtxVal dv = {.cls = PC_F32};
        dv = destination_value(&fn, &in->dest, dv);
        char dn[24];
        reg_name(PC_F32, dv.idx, dn);
        if (intrinsic == MTLC_INTRINSIC_GPU_SQRT_F32) {
          sb_printf(&fn.body, "\tsqrt.rn.f32 %s, %s;\n", dn, a);
        } else if (intrinsic == MTLC_INTRINSIC_GPU_RSQRT_F32) {
          sb_printf(&fn.body, "\trsqrt.approx.f32 %s, %s;\n", dn, a);
        } else if (intrinsic == MTLC_INTRINSIC_GPU_ABS_F32) {
          sb_printf(&fn.body, "\tabs.f32 %s, %s;\n", dn, a);
        } else if (intrinsic == MTLC_INTRINSIC_GPU_SIN_F32) {
          sb_printf(&fn.body, "\tsin.approx.f32 %s, %s;\n", dn, a);
        } else if (intrinsic == MTLC_INTRINSIC_GPU_COS_F32) {
          sb_printf(&fn.body, "\tcos.approx.f32 %s, %s;\n", dn, a);
        } else if (intrinsic == MTLC_INTRINSIC_GPU_LOG_F32) {
          /* ln(x) = lg2(x) / log2(e) = lg2(x) * 0.6931471805599453 */
          int t = new_reg(&fn, PC_F32);
          char tn[24];
          reg_name(PC_F32, t, tn);
          sb_printf(&fn.body, "\tlg2.approx.f32 %s, %s;\n", tn, a);
          sb_printf(&fn.body, "\tmul.f32 %s, %s, 0f3F317218;\n", dn, tn);
        } else { /* expf: exp(x) = 2^(x * log2(e)), log2(e)=1.4426950408 */
          int t = new_reg(&fn, PC_F32);
          char tn[24];
          reg_name(PC_F32, t, tn);
          sb_printf(&fn.body, "\tmul.f32 %s, %s, 0f3FB8AA3B;\n", tn, a);
          sb_printf(&fn.body, "\tex2.approx.f32 %s, %s;\n", dn, tn);
        }
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (ir_intrinsic_is_atomic(intrinsic) &&
                 in->argument_count >=
                     (size_t)ir_intrinsic_arity(intrinsic)) {
        /* Full unsigned atomic load/store/RMW/CAS family. Element indices stay
         * 64-bit all the way into address generation; large buffers must not
         * silently wrap at 2^31/2^32 elements. */
        int is64 =
            ir_intrinsic_atomic_value_kind(intrinsic) == MTLC_TYPE_UINT64;
        int is_cas = ir_intrinsic_is_compare_exchange(intrinsic);
        int is_load = ir_intrinsic_is_atomic_load(intrinsic);
        int is_store = ir_intrinsic_is_atomic_store(intrinsic);
        int is_sub = intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_SUB_U32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_SUB_U64;
        const char *opn =
            intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_ADD_U32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_ADD_U64 || is_sub
                ? "add"
            : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MIN_U32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MIN_U64
                ? "min"
            : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MAX_U32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MAX_U64
                ? "max"
            : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U64
                ? "and"
            : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U64
                ? "or"
            : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64
                ? "xor"
            : is_cas ? "cas"
                     : "exch";
        int bit_type = is_cas ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U32 ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U64 ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U32 ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U64 ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32 ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64 ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U32 ||
                       intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U64;
        const char *sem = ptx_atomic_order(in->memory_order);
        const char *scope = ptx_atomic_scope(in->memory_scope);
        const char *space = ptx_atomic_space(in->address_space);
        PtxClass vc = is64 ? PC_B64 : PC_B32;
        int elem = is64 ? 8 : 4;
        char bufr[24], idxr[24], valr[24] = {0};
        use_as(&fn, &in->arguments[0], PC_B64, bufr);
        use_as(&fn, &in->arguments[1], PC_B64, idxr);
        if (!is_load) use_as(&fn, &in->arguments[2], vc, valr);
        if (is_sub) {
          int neg = new_reg(&fn, vc);
          char negn[24];
          reg_name(vc, neg, negn);
          sb_printf(&fn.body, "\tneg.%s %s, %s;\n",
                    is64 ? "s64" : "s32", negn, valr);
          strcpy(valr, negn);
        }
        int offset = new_reg(&fn, PC_B64);
        int addr = new_reg(&fn, PC_B64);
        char offn[24], an[24];
        reg_name(PC_B64, offset, offn);
        reg_name(PC_B64, addr, an);
        sb_printf(&fn.body, "\tmul.lo.u64 %s, %s, %d;\n", offn, idxr, elem);
        sb_printf(&fn.body, "\tadd.u64 %s, %s, %s;\n", an, bufr, offn);
        PtxVal dv = {.cls = vc, .is_unsigned = 1};
        char dn[24] = {0};
        if (!is_store) {
          dv = destination_value(&fn, &in->dest, dv);
          reg_name(vc, dv.idx, dn);
        }
        int failure_valid =
            !is_cas ||
            (in->failure_memory_order != MTLC_MEMORY_ORDER_RELEASE &&
             in->failure_memory_order != MTLC_MEMORY_ORDER_ACQ_REL &&
             ((in->memory_order == MTLC_MEMORY_ORDER_RELAXED &&
               in->failure_memory_order == MTLC_MEMORY_ORDER_RELAXED) ||
              ((in->memory_order == MTLC_MEMORY_ORDER_ACQUIRE ||
                in->memory_order == MTLC_MEMORY_ORDER_ACQ_REL) &&
               (in->failure_memory_order == MTLC_MEMORY_ORDER_RELAXED ||
                in->failure_memory_order == MTLC_MEMORY_ORDER_ACQUIRE)) ||
              (in->memory_order == MTLC_MEMORY_ORDER_RELEASE &&
               in->failure_memory_order == MTLC_MEMORY_ORDER_RELAXED) ||
              in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST));
        int order_valid =
            (!is_load || in->memory_order == MTLC_MEMORY_ORDER_RELAXED ||
             in->memory_order == MTLC_MEMORY_ORDER_ACQUIRE ||
             in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST) &&
            (!is_store || in->memory_order == MTLC_MEMORY_ORDER_RELAXED ||
             in->memory_order == MTLC_MEMORY_ORDER_RELEASE ||
             in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST);
        if (!sem || !scope || !space || !failure_valid || !order_valid ||
            (in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP &&
             in->memory_scope > MTLC_MEMORY_SCOPE_WORKGROUP)) {
          fn_error(&fn,
                   "PTX: invalid atomic memory contract (space=%d success=%d failure=%d scope=%d)",
                   (int)in->address_space, (int)in->memory_order,
                   (int)in->failure_memory_order, (int)in->memory_scope);
        } else {
          if (in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST) {
            sb_printf(&fn.body, "\tfence.sc.%s;\n", scope);
          }
          const char *type =
              bit_type ? (is64 ? "b64" : "b32")
                       : (is64 ? "u64" : "u32");
          if (is_load) {
            sb_printf(&fn.body, "\tld.%s.%s%s.%s %s, [%s];\n", sem,
                      scope, space, type, dn, an);
          } else if (is_store) {
            const char *store_sem =
                in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST ? "relaxed"
                                                               : sem;
            sb_printf(&fn.body, "\tst.%s.%s%s.%s [%s], %s;\n", store_sem,
                      scope, space, type, an, valr);
          } else if (is_cas) {
            char desired[24];
            use_as(&fn, &in->arguments[3], vc, desired);
            /* PTX takes comparator then desired value. Its single qualifier
             * strengthens a weaker failure order to the success order. */
            sb_printf(&fn.body,
                      "\tatom.%s.%s%s.%s.%s %s, [%s], %s, %s;\n",
                      sem, scope, space, opn, type, dn, an, valr, desired);
          } else {
            sb_printf(&fn.body, "\tatom.%s.%s%s.%s.%s %s, [%s], %s;\n",
                      sem, scope, space, opn, type, dn, an, valr);
          }
        }
        if (in->dest.name && !is_store)
          bind_value(&fn, in->dest.name, dv);
      } else if (intrinsic == MTLC_INTRINSIC_NONE) {
        IRFunction *callee_function = ptx_lookup_function(program, callee);
        const IRModuleSymbol *callee_symbol =
            ir_program_lookup_symbol(program, callee);
        if (!callee_function || !callee_symbol ||
            callee_symbol->kind != IR_MODSYM_FUNCTION) {
          fn_error(&fn, "PTX: device call target '%s' has no definition",
                   callee ? callee : "?");
          break;
        }
        if (in->argument_count != callee_function->parameter_count ||
            in->argument_count != callee_symbol->param_count) {
          fn_error(&fn,
                   "PTX: device call '%s' expects %zu arguments, received %zu",
                   callee, callee_symbol->param_count, in->argument_count);
          break;
        }

        char callee_name[256];
        sanitize_into(callee, callee_name, sizeof(callee_name));
        size_t call_id = fn.call_count++;
        char **argument_params =
            calloc(in->argument_count ? in->argument_count : 1,
                   sizeof(*argument_params));
        if (!argument_params) {
          fn_error(&fn, "PTX: out of memory lowering device call '%s'", callee);
          break;
        }
        for (size_t a = 0; a < in->argument_count && !fn.error; a++) {
          const MtlcType *argument_type = callee_symbol->param_types[a];
          char parameter[320];
          snprintf(parameter, sizeof(parameter), "__mtlc_call_%zu_arg_%zu",
                   call_id, a);
          argument_params[a] = strdup(parameter);
          if (!argument_params[a]) {
            fn_error(&fn, "PTX: out of memory lowering device call '%s'",
                     callee);
            break;
          }
          if (ptx_type_is_aggregate(argument_type)) {
            PtxBinding *record = named_binding(&fn, &in->arguments[a]);
            if (!record || !record->val.mem_aggregate) {
              fn_error(&fn,
                       "PTX: argument %zu of '%s' is a record with no storage",
                       a, callee);
              break;
            }
            size_t size = mtlc_type_size(argument_type);
            size_t alignment = mtlc_type_alignment(argument_type);
            if (!alignment) {
              alignment = 1;
            }
            char source[24];
            reg_name(PC_B64, record->val.mem_addr, source);
            sb_printf(&fn.declarations, "\t.param .align %zu .b8 %s[%zu];\n",
                      alignment, parameter, size);
            ptx_block_copy(&fn, ".param", parameter, ".local", source, size,
                           alignment);
            continue;
          }
          PtxVal argument_desc = descriptor_from_type(argument_type);
          char value[24];
          use_as(&fn, &in->arguments[a], argument_desc.cls, value);
          ptx_generic_address(&fn, &in->arguments[a], value);
          sb_printf(&fn.declarations, "\t.param .%s %s;\n",
                    device_param_storage_type(argument_desc), parameter);
          sb_printf(&fn.body, "\tst.param.%s [%s], %s;\n",
                    device_param_storage_type(argument_desc), parameter, value);
        }

        const MtlcType *callee_return_type = ptx_function_return_type(
            program, callee_function, callee_symbol);
        int callee_returns_void = ptx_type_is_void(
            callee_return_type, callee_function->return_type_name);
        int result_is_record = ptx_type_is_aggregate(callee_return_type);
        PtxVal result_desc = callee_returns_void
                                 ? (PtxVal){0}
                                 : descriptor_from_type(callee_return_type);
        char return_parameter[320] = {0};
        char result_storage[512] = {0};
        if (result_is_record && !fn.error) {
          result_desc = (PtxVal){0};
          result_desc.cls = PC_B64;
          result_desc.elem = MTLC_TYPE_VOID;
          result_desc.mem_local = 1;
          result_desc.mem_aggregate = 1;
          result_desc.mem_size = mtlc_type_size(callee_return_type);
          result_desc.mem_align = mtlc_type_alignment(callee_return_type);
          if (!result_desc.mem_align) {
            result_desc.mem_align = 1;
          }
          char raw[512];
          snprintf(raw, sizeof(raw), "__mtlc_call_%zu_ret_local", call_id);
          sanitize_into(raw, result_storage, sizeof(result_storage));
          sb_printf(&fn.declarations, "\t.local .align %zu .b8 %s[%zu];\n",
                    result_desc.mem_align, result_storage,
                    result_desc.mem_size);
        }
        if (!callee_returns_void && !fn.error) {
          snprintf(return_parameter, sizeof(return_parameter),
                   "__mtlc_call_%zu_ret", call_id);
          if (result_is_record) {
            sb_printf(&fn.declarations, "\t.param .align %zu .b8 %s[%zu];\n",
                      result_desc.mem_align, return_parameter,
                      result_desc.mem_size);
          } else {
            sb_printf(&fn.declarations, "\t.param .%s %s;\n",
                      device_param_storage_type(result_desc), return_parameter);
          }
        }
        if (!fn.error) {
          if (callee_returns_void) {
            sb_printf(&fn.body, "\tcall.uni %s, (", callee_name);
          } else {
            sb_printf(&fn.body, "\tcall.uni (%s), %s, (", return_parameter,
                      callee_name);
          }
          for (size_t a = 0; a < in->argument_count; a++) {
            sb_printf(&fn.body, "%s%s", a ? ", " : "", argument_params[a]);
          }
          sb_puts(&fn.body, ");\n");

          if (callee_returns_void) {
            if (in->dest.name) {
              fn_error(&fn, "PTX: void device call '%s' has a result", callee);
            }
          } else if (result_is_record) {
            char pointer[24];
            result_desc.mem_addr = new_reg(&fn, PC_B64);
            reg_name(PC_B64, result_desc.mem_addr, pointer);
            sb_printf(&fn.body, "\tmov.u64 %s, %s;\n", pointer, result_storage);
            ptx_block_copy(&fn, ".local", pointer, ".param", return_parameter,
                           result_desc.mem_size, result_desc.mem_align);
            if (in->dest.name) {
              bind_value(&fn, in->dest.name, result_desc);
            }
          } else if (in->dest.name) {
            result_desc = destination_value(&fn, &in->dest, result_desc);
            char result[24];
            reg_name(result_desc.cls, result_desc.idx, result);
            sb_printf(&fn.body, "\tld.param.%s %s, [%s];\n",
                      device_param_storage_type(result_desc), result,
                      return_parameter);
            bind_value(&fn, in->dest.name, result_desc);
          }
        }
        for (size_t a = 0; a < in->argument_count; a++) {
          free(argument_params[a]);
        }
        free(argument_params);
      } else {
        fn_error(&fn, "PTX: unsupported call '%s'", callee ? callee : "?");
      }
      break;
    }
    case IR_OP_RETURN:
      if (func->is_kernel || returns_void) {
        if (in->lhs.kind != IR_OPERAND_NONE) {
          fn_error(&fn, "PTX: void device function '%s' returns a value",
                   func->name ? func->name : "?");
        } else {
          sb_puts(&fn.body, "\tret;\n");
        }
      } else if (in->lhs.kind == IR_OPERAND_NONE) {
        fn_error(&fn, "PTX: non-void device function '%s' has an empty return",
                 func->name ? func->name : "?");
      } else if (fn.return_desc.mem_aggregate) {
        PtxBinding *value = named_binding(&fn, &in->lhs);
        if (!value || !value->val.mem_aggregate) {
          fn_error(&fn, "PTX: device function '%s' returns a record it has no "
                        "storage for",
                   func->name ? func->name : "?");
          break;
        }
        char source[24], destination[512];
        reg_name(PC_B64, value->val.mem_addr, source);
        snprintf(destination, sizeof(destination), "%s_ret", ename);
        ptx_block_copy(&fn, ".param", destination, ".local", source,
                       fn.return_desc.mem_size, fn.return_desc.mem_align);
        sb_puts(&fn.body, "\tret;\n");
      } else {
        char value[24];
        use_as(&fn, &in->lhs, fn.return_desc.cls, value);
        sb_printf(&fn.body, "\tst.param.%s [%s_ret], %s;\n\tret;\n",
                  device_param_storage_type(fn.return_desc), ename, value);
      }
      break;
    case IR_OP_ADDRESS_OF: {
      PtxBinding *home = named_binding(&fn, &in->lhs);
      if (!home || !home->val.mem_local) {
        fn_error(&fn, "PTX: cannot take the address of '%s' in device code",
                 in->lhs.name ? in->lhs.name : "?");
        break;
      }
      PtxVal a = {0};
      a.cls = PC_B64;
      a.idx = home->val.mem_addr;
      a.is_ptr = 1;
      a.is_unsigned = 1;
      a.elem = home->val.mem_aggregate ? MTLC_TYPE_VOID : home->val.elem;
      a.address_space = MTLC_ADDRESS_SPACE_PRIVATE;
      if (in->dest.name) {
        bind_value(&fn, in->dest.name, a);
      }
      break;
    }
    default:
      fn_error(&fn, "PTX: unsupported IR opcode %d in device function '%s'", in->op,
               func->name ? func->name : "?");
      break;
    }
  }

  free(vector_plan);

  if (fn.error) {
    if (error) {
      *error = fn.error;
    } else {
      free(fn.error);
    }
    /* cleanup */
    free(sig.data);
    free(fn.body.data);
    free(fn.declarations.data);
    free(param_descs);
    for (size_t i = 0; i < fn.nbinds; i++) {
      free(fn.binds[i].name);
    }
    free(fn.binds);
    free(fn.tensor_residencies);
    return;
  }

  /* --- assemble: signature { reg-decls body } --- */
  fputs(sig.data, out);
  fputs("{\n", out);
  static const PtxClass classes[6] = {PC_PRED, PC_B16, PC_B32,
                                      PC_B64,  PC_F32, PC_F64};
  for (int c = 0; c < 6; c++) {
    PtxClass cc = classes[c];
    if (fn.count[cc] > 0) {
      fprintf(out, "\t.reg %s %s<%d>;\n", cls_regtype(cc), cls_prefix(cc),
              fn.count[cc]);
    }
  }
  fputs(fn.declarations.data ? fn.declarations.data : "", out);
  fputs(fn.body.data ? fn.body.data : "", out);
  /* Kernel entry points may legally fall through. Non-void helpers were already
   * checked by semantic analysis and must not gain a value-less return here. */
  fputs(func->is_kernel ? "\tret;\n}\n\n" : "}\n\n", out);

  free(sig.data);
  free(fn.body.data);
  free(fn.declarations.data);
  free(param_descs);
  for (size_t i = 0; i < fn.nbinds; i++) {
    free(fn.binds[i].name);
  }
  free(fn.binds);
  free(fn.tensor_residencies);
}

/* ---- BINARY ---- */
static void emit_binary(PtxFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "+";
  PtxVal la = operand_desc(fn, &in->lhs);
  PtxVal ra = operand_desc(fn, &in->rhs);

  if (is_compare_op(t)) {
    /* operand compare class */
    PtxClass c = PC_B32;
    int is_float = 0, is_unsigned = 0;
    if (la.cls == PC_F32 || ra.cls == PC_F32 || in->is_float) {
      c = PC_F32;
      is_float = 1;
    }
    if (la.cls == PC_F64 || ra.cls == PC_F64) {
      c = PC_F64;
      is_float = 1;
    }
    if (!is_float && (la.cls == PC_B64 || ra.cls == PC_B64)) {
      c = PC_B64;
    }
    if (!is_float) {
      /* C "usual arithmetic conversions": if either operand is unsigned the
       * comparison is unsigned. (Integer literals carry is_unsigned=0, so `&&`
       * here would wrongly make `unsigned_var < 10` a signed compare.) */
      is_unsigned = la.is_unsigned || ra.is_unsigned;
    }
    char a[24], b[24];
    use_as(fn, &in->lhs, c, a);
    use_as(fn, &in->rhs, c, b);
    int p = new_reg(fn, PC_PRED);
    char pn[24];
    reg_name(PC_PRED, p, pn);
    sb_printf(&fn->body, "\tsetp.%s.%s %s, %s, %s;\n",
              setp_cmp(t, is_float, is_unsigned),
              type_suffix_for_class(c, is_unsigned), pn, a, b);
    PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
    dv = destination_value(fn, &in->dest, dv);
    char dn[24];
    reg_name(PC_B32, dv.idx, dn);
    sb_printf(&fn->body, "\tselp.u32 %s, 1, 0, %s;\n", dn, pn);
    if (in->dest.name) {
      bind_value(fn, in->dest.name, dv);
    }
    return;
  }

  /* logical && / || : treat as bitwise on 0/1 ints */
  int is_logical = (!strcmp(t, "&&") || !strcmp(t, "||"));

  /* result class */
  PtxVal dv = {0};
  if (in->is_float) {
    dv.cls = (in->float_bits == 32) ? PC_F32 : PC_F64;
  } else if (la.is_ptr || ra.is_ptr) {
    /* pointer arithmetic: result is a pointer, element from whichever side */
    dv.cls = PC_B64;
    dv.is_ptr = 1;
    dv.is_unsigned = 1;
    dv.elem = la.is_ptr ? la.elem : ra.elem;
    dv.address_space =
        la.is_ptr ? la.address_space : ra.address_space;
  } else {
    dv.cls = (la.cls == PC_B64 || ra.cls == PC_B64) ? PC_B64 : PC_B32;
    /* Unsigned if either operand is unsigned (C usual arithmetic conversions).
     * `&&` is wrong: integer literals are is_unsigned=0, so `unsigned_var / 7`
     * or `unsigned_var >> 3` would emit signed div/shr and miscompute for
     * high-bit-set values. */
    dv.is_unsigned = la.is_unsigned || ra.is_unsigned;
  }

  char a[24], b[24];
  use_as(fn, &in->lhs, dv.cls, a);
  use_as(fn, &in->rhs, dv.cls, b);
  dv = destination_value(fn, &in->dest, dv);
  char dn[24];
  reg_name(dv.cls, dv.idx, dn);

  const char *ts = type_suffix_for_class(dv.cls, dv.is_unsigned);
  const char *bts = (dv.cls == PC_B64) ? "b64" : "b32";

  if (!strcmp(t, "+")) {
    sb_printf(&fn->body, "\tadd.%s %s, %s, %s;\n", ts, dn, a, b);
  } else if (!strcmp(t, "-")) {
    sb_printf(&fn->body, "\tsub.%s %s, %s, %s;\n", ts, dn, a, b);
  } else if (!strcmp(t, "*")) {
    if (dv.cls == PC_F32 || dv.cls == PC_F64) {
      sb_printf(&fn->body, "\tmul.%s %s, %s, %s;\n", ts, dn, a, b);
    } else {
      sb_printf(&fn->body, "\tmul.lo.%s %s, %s, %s;\n", ts, dn, a, b);
    }
  } else if (!strcmp(t, "/")) {
    if (dv.cls == PC_F32) {
      sb_printf(&fn->body, "\tdiv.rn.f32 %s, %s, %s;\n", dn, a, b);
    } else if (dv.cls == PC_F64) {
      sb_printf(&fn->body, "\tdiv.rn.f64 %s, %s, %s;\n", dn, a, b);
    } else {
      sb_printf(&fn->body, "\tdiv.%s %s, %s, %s;\n", ts, dn, a, b);
    }
  } else if (!strcmp(t, "%")) {
    sb_printf(&fn->body, "\trem.%s %s, %s, %s;\n", ts, dn, a, b);
  } else if (!strcmp(t, "&") || (is_logical && !strcmp(t, "&&"))) {
    sb_printf(&fn->body, "\tand.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "|") || (is_logical && !strcmp(t, "||"))) {
    sb_printf(&fn->body, "\tor.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "^")) {
    sb_printf(&fn->body, "\txor.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "<<")) {
    /* shift amount is a .u32 in PTX */
    char sh[24];
    use_as(fn, &in->rhs, PC_B32, sh);
    sb_printf(&fn->body, "\tshl.%s %s, %s, %s;\n", bts, dn, a, sh);
  } else if (!strcmp(t, ">>")) {
    /* Arithmetic (signed) vs logical (unsigned) shift is decided by the value
     * being shifted -- the left operand -- not the shift count. */
    char sh[24];
    use_as(fn, &in->rhs, PC_B32, sh);
    sb_printf(&fn->body, "\tshr.%s %s, %s, %s;\n",
              type_suffix_for_class(dv.cls, la.is_unsigned), dn, a, sh);
  } else {
    fn_error(fn, "PTX: unsupported binary op '%s'", t);
  }
  if (in->dest.name) {
    bind_value(fn, in->dest.name, dv);
  }
}
