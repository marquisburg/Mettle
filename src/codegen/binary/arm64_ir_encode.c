#include "codegen/binary/arm64_ir.h"
#include "codegen/binary/strength_rules.h"
#include "codegen/binary_emitter.h"
#include "codegen/binary_emitter_internal.h"
#include "codegen/target.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Load address and header size of the self-contained executable's two PT_LOAD
 * segments (see arm64_write_elf). An embedded string's virtual address is
 * ELF_BASE + ELF_HDRS + its byte offset in the code blob. */
#define ELF_BASE 0x400000u
#define ELF_HDRS 176u /* Ehdr + 2 Phdr */

/* The writable segment loads at a FIXED virtual address, so a global's address
 * is a compile-time constant: this path emits a bare executable with no
 * relocations to defer it to, and the code size is not known while the code
 * that references a global is being emitted. The address is what the text may
 * not grow into, so it sits a gigabyte up -- an address is materialized from
 * 16-bit fields, and one whose only non-zero field is bits 16..31 costs exactly
 * what the old 2MiB gap did. arm64_write_elf still rejects text that reaches
 * it, but nothing this backend can emit gets close. Address space, not file
 * size: the segment's file offset still follows the text. */
#define ELF_DATA_VADDR 0x40000000u
#define ELF_TEXT_LIMIT (ELF_DATA_VADDR - ELF_BASE)

/* The first bytes of the writable segment are the bump allocator's state, which
 * the malloc stub reads and writes; module globals follow. */
#define HEAP_CURSOR_OFF 0u  /* uint64: next free heap byte */
#define HEAP_END_OFF 8u     /* uint64: one past the mapped heap */
#define DATA_RESERVED 16u

/* AArch64 Linux syscall numbers and the `svc #0` word, for the freestanding
 * paths that cannot call libc. */
#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_MMAP 222
#define ARM64_SVC0 0xD4000001u
/* Bytes of address space each heap mmap claims, on top of the request that
 * forced it. Anonymous mappings are lazily backed, so this costs no memory
 * until it is touched. */
#define HEAP_ARENA_BYTES 0x4000000u /* 64 MiB */

/* Scratch registers: lhs, rhs, result, and an aux for msub (modulo). */
#define R_LHS ARM64_X9
#define R_RHS ARM64_X10
#define R_RES ARM64_X11
#define R_AUX ARM64_X12

typedef struct {
  BinaryEmitter *emitter;
  const IRProgram *program;
  size_t text_section;
  size_t rodata_section;
  unsigned string_id;
} Arm64ObjectContext;

/* Stack-slot map: each distinct name gets a byte offset into the frame. Scalars
 * and pointers take 8 bytes; an array local takes count*elem_size (8-aligned)
 * so &array + i*elem_size addresses its elements. */
/* Writable data image for the self-contained executable: the allocator state
 * followed by every module global, each at a fixed offset. The object path
 * leaves this NULL and goes through .data/.bss relocations instead. */
typedef struct {
  unsigned char *bytes;
  size_t len;
  size_t cap;
  const char **names; /* module-variable name -> */
  size_t *offs;       /*   byte offset into `bytes` */
  int count;
  int name_cap;
  /* Globals initialized with a FUNCTION's address. Code addresses are not
   * known while the data image is built, so _start stores these before it
   * calls the entry function. */
  const char **fixup_fns;
  size_t *fixup_offs;
  int fixup_count;
  int fixup_cap;
} Arm64Data;

typedef struct {
  const char **names;
  int *offs;
  int count;
  int cap;
  int frame; /* running total bytes */
  Arm64ObjectContext *object;
  /* Set on both paths, so a module global is recognized as one no matter which
   * output we are producing. Without it every global silently became a
   * per-function stack slot and writes to it were lost between calls. */
  const IRProgram *program;
  Arm64Data *data;        /* self-contained path only; appended to on demand */
  struct LblMap *fns;     /* function name -> entry label, for &function */
  const struct NarrowMap *narrow; /* locals stored at less than 8 bytes */
  const struct AggMap *aggregates; /* locals that ARE an object, not a value */
} SlotMap;

static const IRModuleSymbol *module_variable(const SlotMap *slots,
                                             const char *name) {
  if (!slots || !slots->program || !name) return NULL;
  const IRModuleSymbol *symbol =
      ir_program_lookup_symbol(slots->program, name);
  return symbol && symbol->kind == IR_MODSYM_VARIABLE ? symbol : NULL;
}

/* Every owned runtime entry point the self contained executable provides.
 * `needs_malloc` records the one inter-stub dependency, so referencing calloc
 * also pulls in malloc. */
typedef enum {
  STUB_MALLOC, STUB_CALLOC, STUB_FREE, STUB_PUTS, STUB_PUTCHAR, STUB_WRITE,
  STUB_STRLEN, STUB_MEMCPY, STUB_MEMMOVE, STUB_MEMSET, STUB_EXIT, STUB_ABORT,
  STUB_STR_FROM_INT, STUB_STR_FROM_UINT, STUB_STR_FROM_BOOL,
  STUB_STR_FROM_CHAR, STUB_STR_EQ, STUB_STR_FREE,
  STUB_COUNT
} Arm64StubId;

static const struct {
  const char *name;
  int needs_malloc;
} RUNTIME_STUBS[STUB_COUNT] = {
    {"malloc", 0},  {"calloc", 1},  {"free", 0},    {"puts", 0},
    {"putchar", 0}, {"write", 0},   {"strlen", 0},  {"memcpy", 0},
    {"memmove", 0}, {"memset", 0},  {"exit", 0},    {"abort", 0},
    {"mettle_string_from_int", 1},  {"mettle_string_from_uint", 1},
    {"mettle_string_from_bool", 1},
    {"mettle_string_from_char", 1},
    {"mettle_string_eq", 0},
    {"mettle_string_free", 0},
};

/* Index of the runtime stub named `name`, or -1. `mettle_heap_zeroed` is the
 * allocator IR_OP_NEW lowers to, and bump memory is already zeroed. */
static int runtime_stub_index(const char *name) {
  if (!name) return -1;
  if (strcmp(name, "mettle_heap_zeroed") == 0) return STUB_MALLOC;
  for (int i = 0; i < STUB_COUNT; i++) {
    if (strcmp(name, RUNTIME_STUBS[i].name) == 0) return i;
  }
  return -1;
}

/* Byte offset of a global in the data image, or -1 if it has none. */
static long data_offset_of(const Arm64Data *d, const char *name) {
  if (!d || !name) return -1;
  for (int i = 0; i < d->count; i++) {
    if (d->names[i] == name || strcmp(d->names[i], name) == 0) {
      return (long)d->offs[i];
    }
  }
  return -1;
}

static const char *module_link_name(const IRModuleSymbol *symbol) {
  return symbol && symbol->link_name && symbol->link_name[0]
             ? symbol->link_name
             : (symbol ? symbol->name : NULL);
}

static int object_add_relocation(Arm64Emit *e, Arm64ObjectContext *object,
                                 size_t offset, BinaryRelocationKind kind,
                                 const char *symbol) {
  if (!object || !symbol ||
      !binary_emitter_add_relocation(object->emitter, object->text_section,
                                     offset, kind, symbol, 0)) {
    arm64_fail(e, "could not relocate a reference to '%s'",
               symbol ? symbol : "(null)");
    return 0;
  }
  return 1;
}

static void emit_imm(Arm64Emit *e, Arm64Reg rd, uint64_t v);
static int label_for(Arm64Emit *e, struct LblMap *m, const char *name);
static int prog_fn_index(const IRProgram *prog, const char *name);
static void emit_lea_local(Arm64Emit *e, Arm64Reg rd, int off);
static void emit_slot_ldr(Arm64Emit *e, Arm64Reg rt, int off);
static void emit_slot_str(Arm64Emit *e, Arm64Reg rt, int off);
static size_t data_reserve(Arm64Data *d, size_t n, size_t align);
static int io_stub_intrinsic(const char *name, int *with_newline,
                             int *is_string);
static const IRModuleSymbol *module_function(const IRProgram *program,
                                             const char *name);

/* rd = &symbol using the ELF small position-independent code model. The two
 * zero-immediate instructions are completed by AAELF64 page/lo12 relocations. */
static void emit_symbol_address(Arm64Emit *e, Arm64ObjectContext *object,
                                Arm64Reg rd, const char *symbol) {
  size_t at = arm64_here(e);
  arm64_emit_word(e, 0x90000000u | (uint32_t)rd); /* adrp rd, symbol */
  if (!object_add_relocation(e, object, at,
                             BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21,
                             symbol)) {
    return;
  }
  at = arm64_here(e);
  arm64_emit_word(e, arm64_add_imm(1, rd, rd, 0, 0));
  object_add_relocation(e, object, at,
                        BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC, symbol);
}

/* rd = &global, whichever output we are producing: a page/lo12 relocation pair
 * in an object, or the constant address of its slot in the self-contained
 * executable's fixed writable segment. */
static void emit_global_address(Arm64Emit *e, const SlotMap *s, Arm64Reg rd,
                                const IRModuleSymbol *global) {
  const char *link_name = module_link_name(global);
  if (s->object) {
    emit_symbol_address(e, s->object, rd, link_name);
    return;
  }
  long off = data_offset_of(s->data, link_name);
  if (off < 0 && global) off = data_offset_of(s->data, global->name);
  if (off < 0) {
    arm64_fail(e, "global '%s' has no slot in the data image",
               link_name ? link_name : "(unnamed)");
    return;
  }
  emit_imm(e, rd, (uint64_t)ELF_DATA_VADDR + (uint64_t)off);
}

static int module_type_size(const MtlcType *type) {
  size_t size = mtlc_type_size(type);
  return size == 1 || size == 2 || size == 4 || size == 8 ? (int)size : 8;
}

static int module_type_signed(const MtlcType *type) {
  return type && (type->kind == MTLC_TYPE_INT8 ||
                  type->kind == MTLC_TYPE_INT16 ||
                  type->kind == MTLC_TYPE_INT32 ||
                  type->kind == MTLC_TYPE_INT64);
}

static void emit_load_sized(Arm64Emit *e, Arm64Reg dest, Arm64Reg address,
                            int size, int sign_extend) {
  switch (size) {
  case 8:
    arm64_emit_word(e, arm64_ldr_imm(1, dest, address, 0));
    break;
  case 4:
    arm64_emit_word(e, arm64_ldr_imm(0, dest, address, 0));
    if (sign_extend) arm64_emit_word(e, arm64_sxtw(dest, dest));
    break;
  case 2:
    arm64_emit_word(e, arm64_ldrh_imm(dest, address, 0));
    if (sign_extend) arm64_emit_word(e, arm64_sxth(dest, dest));
    break;
  case 1:
    arm64_emit_word(e, arm64_ldrb_imm(dest, address, 0));
    if (sign_extend) arm64_emit_word(e, arm64_sxtb(dest, dest));
    break;
  default:
    arm64_fail(e, "no %d-byte scalar load", size);
    break;
  }
}

static void emit_store_sized(Arm64Emit *e, Arm64Reg source,
                             Arm64Reg address, int size) {
  switch (size) {
  case 8: arm64_emit_word(e, arm64_str_imm(1, source, address, 0)); break;
  case 4: arm64_emit_word(e, arm64_str_imm(0, source, address, 0)); break;
  case 2: arm64_emit_word(e, arm64_strh_imm(source, address, 0)); break;
  case 1: arm64_emit_word(e, arm64_strb_imm(source, address, 0)); break;
  default:
    arm64_fail(e, "no %d-byte scalar store", size);
    break;
  }
}

static void emit_f16_widen(Arm64Emit *e, Arm64Reg v) {
  arm64_emit_word(e, arm64_fmov_gp(0, 0, v));
  arm64_emit_word(e, arm64_fcvt_h2s(0, 0));
  arm64_emit_word(e, arm64_fmov_to_gp(0, v, 0));
}

static void emit_f16_narrow(Arm64Emit *e, Arm64Reg v) {
  arm64_emit_word(e, arm64_fmov_gp(0, 0, v));
  arm64_emit_word(e, arm64_fcvt_s2h(0, 0));
  arm64_emit_word(e, arm64_fmov_to_gp(0, v, 0));
}

static void emit_bf16_widen(Arm64Emit *e, Arm64Reg v) {
  arm64_emit_word(e, arm64_lsl_imm(0, v, v, 16));
}

static void emit_bf16_narrow(Arm64Emit *e, Arm64Reg v, Arm64Reg t1,
                             Arm64Reg t2) {
  arm64_emit_word(e, arm64_lsl_imm(0, t2, v, 1));
  arm64_emit_word(e, arm64_movz(0, t1, 0xFF00, 1));
  arm64_emit_word(e, arm64_cmp_reg(0, t2, t1));
  arm64_emit_word(e, arm64_lsl_imm(0, t1, v, 15));
  arm64_emit_word(e, arm64_lsr_imm(0, t1, t1, 31));
  arm64_emit_word(e, arm64_movz(0, t2, 0x7FFF, 0));
  arm64_emit_word(e, arm64_add_reg(0, t1, t1, t2));
  arm64_emit_word(e, arm64_add_reg(0, t1, t1, v));
  arm64_emit_word(e, arm64_lsr_imm(0, t1, t1, 16));
  arm64_emit_word(e, arm64_lsr_imm(0, t2, v, 23));
  arm64_emit_word(e, arm64_lsl_imm(0, t2, t2, 7));
  arm64_emit_word(e, arm64_add_imm(0, t2, t2, 64, 0));
  arm64_emit_word(e, arm64_csel(0, v, t2, t1, ARM64_HI));
}

static int small_float_class(const IRInstruction *in) {
  if (in->alias_class == IR_ALIAS_CLASS_F16) return 16;
  if (in->alias_class == IR_ALIAS_CLASS_BF16) return 8;
  return 0;
}

static int cast_small_float_target(const IRInstruction *in) {
  if (!in->text) return 0;
  if (strcmp(in->text, "float16") == 0) return 16;
  if (strcmp(in->text, "bfloat16") == 0) return 8;
  return 0;
}

/* The base type name inside a declared type text: "int32[4]" and "int32*" both
 * name int32. Every predicate below reads this rather than searching the text,
 * because a monomorphized name carries its type argument: "Pair__float64"
 * contains "float64" and "Some__uint32" contains "uint32", and matching on the
 * substring classified a whole record as the scalar inside its name. */
static const char *type_text_base(const char *t, char *buffer, size_t size) {
  size_t n = 0;
  if (!t || size == 0) {
    if (size > 0) buffer[0] = '\0';
    return buffer;
  }
  while (t[n] && t[n] != '*' && t[n] != '[' && t[n] != ' ' && n + 1 < size) {
    n++;
  }
  memcpy(buffer, t, n);
  buffer[n] = '\0';
  return buffer;
}

static int type_text_base_is(const char *t, const char *name) {
  char base[64];
  return strcmp(type_text_base(t, base, sizeof(base)), name) == 0;
}

/* Byte size of an array element by its type name. */
static int type_elem_size(const char *t) {
  char base[64];
  if (!t) return 8;
  if (strchr(t, '*')) return 8; /* pointer */
  type_text_base(t, base, sizeof(base));
  if (strcmp(base, "int64") == 0 || strcmp(base, "uint64") == 0 ||
      strcmp(base, "float64") == 0) {
    return 8;
  }
  if (strcmp(base, "int32") == 0 || strcmp(base, "uint32") == 0 ||
      strcmp(base, "float32") == 0) {
    return 4;
  }
  if (strcmp(base, "int16") == 0 || strcmp(base, "uint16") == 0) return 2;
  if (strcmp(base, "int8") == 0 || strcmp(base, "uint8") == 0 ||
      strcmp(base, "bool") == 0 || strcmp(base, "char") == 0) {
    return 1;
  }
  return 8;
}

/* True for a type that lives in memory as an object rather than as a value in a
 * register: a struct, an array, or a `string` (a { chars, length } record). The
 * IR names such a local where it means the local's ADDRESS. */
static int type_is_aggregate(const MtlcType *t) {
  if (!t) return 0;
  /* A tagged enum is a record too: a tag word and a payload beside it, wider
   * than a register. Leaving it out here made a value of one travel as its
   * first eight bytes, so a callee read the right tag and a zeroed payload,
   * and a constructor's return arrived empty. The x86 predicate has always
   * counted it. */
  return t->kind == MTLC_TYPE_STRUCT || t->kind == MTLC_TYPE_ARRAY ||
         t->kind == MTLC_TYPE_TAGGED_ENUM || t->kind == MTLC_TYPE_STRING;
}

/* Frame bytes a DECLARE_LOCAL needs from its type text (e.g. "int64[4]"). */
static int local_size_bytes(const char *text) {
  if (!text) return 8;
  const char *lb = strchr(text, '[');
  if (lb) {
    int count = atoi(lb + 1);
    char buf[64];
    size_t n = (size_t)(lb - text);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    int total = count * type_elem_size(buf);
    return total > 0 ? total : 8;
  }
  return 8;
}

/* Resolved type of a DECLARE_LOCAL: the instruction's own value_type, or the
 * program's type registry looked up by the declared type text. Synthesized
 * locals (the ir_agg_ret_N temporaries the frontend makes for aggregate
 * returns) carry only the text. */
static const MtlcType *declared_type(const IRProgram *prog,
                                     const IRInstruction *in) {
  if (in->value_type) return in->value_type;
  if (prog && in->text && !strchr(in->text, '[') && !strchr(in->text, '*')) {
    return ir_program_lookup_type(prog, in->text);
  }
  return NULL;
}

/* Frame bytes a DECLARE_LOCAL needs. The resolved type wins when there is one:
 * an aggregate local IS its object, so it must be allocated at full size, not
 * as one 8-byte word. */
static int declare_local_size(const IRProgram *prog, const IRInstruction *in) {
  const MtlcType *t = declared_type(prog, in);
  if (t) {
    size_t size = mtlc_type_size(t);
    if (size > 0 && size < (size_t)1 << 24) return (int)size;
  }
  return local_size_bytes(in->text);
}

/* Find or allocate `name`'s slot (size rounded up to 8); returns its offset. */
static int slot_alloc(SlotMap *s, const char *name, int size_bytes) {
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == name || strcmp(s->names[i], name) == 0) {
      return s->offs[i];
    }
  }
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 32;
    const char **n = realloc(s->names, (size_t)cap * sizeof(*n));
    int *o = realloc(s->offs, (size_t)cap * sizeof(*o));
    if (n) s->names = n;
    if (o) s->offs = o;
    if (!n || !o) {
      return -1;
    }
    s->cap = cap;
  }
  int sz = size_bytes <= 0 ? 8 : ((size_bytes + 7) & ~7);
  int off = s->frame;
  s->frame += sz;
  s->names[s->count] = name;
  s->offs[s->count] = off;
  s->count++;
  return off;
}

/* Byte offset of a name's slot (default 8-byte scalar if not seen yet). */
static int slot_off(SlotMap *s, const char *name) {
  return slot_alloc(s, name, 8);
}

/* Lookup-only variant: byte offset of a name's slot, or -1 if the name has no
 * slot. Parameters and DECLARE_LOCALs are slot-allocated before the body is
 * lowered, so a hit here means the name is function-local and must shadow any
 * module variable of the same name (matching the x86 backend's locals-first
 * resolution order). */
static int slot_find(const SlotMap *s, const char *name) {
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == name || strcmp(s->names[i], name) == 0) {
      return s->offs[i];
    }
  }
  return -1;
}

/* Does this declared type text name a scalar float? "float32*" and "float32[4]"
 * contain "float" but are a pointer and an array: classifying either as a float
 * hands it to a vector register, while the caller -- which resolves the real
 * type -- passes it in a GP register. The two then disagree about where the
 * argument is. */
static int type_text_is_float_scalar(const char *t) {
  if (!t || strchr(t, '*') != NULL || strchr(t, '[') != NULL) return 0;
  return type_text_base_is(t, "float32") || type_text_base_is(t, "float64");
}

/* 32 or 64 for a scalar float type text; 64 when it does not name one. */
static int type_text_float_bits(const char *t) {
  return type_text_base_is(t, "float32") ? 32 : 64;
}

/* IEEE-754 bit pattern of a FLOAT operand at `bits` (32 or 64). A literal's
 * declared width is nominal -- it takes the width of the expression it lands
 * in -- so the caller passes the width the operation will run at. Building a
 * double pattern and then reading it as a single yields 0, silently. */
static uint64_t ieee_bits_at(const IROperand *op, int bits) {
  if (bits == 32) {
    float f = (float)op->float_value;
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
  }
  double d = op->float_value;
  uint64_t b;
  memcpy(&b, &d, 8);
  return b;
}

/* IEEE-754 bit pattern of a FLOAT operand at its declared width. */
static uint64_t ieee_bits(const IROperand *op) {
  return ieee_bits_at(op, op->float_bits == 32 ? 32 : 64);
}

/* A name -> emit-label-id map, shared use for both branch labels (per function)
 * and function entry labels (whole program). */
typedef struct LblMap {
  const char **names;
  int *ids;
  int count;
  int cap;
} LblMap;

static int label_for(Arm64Emit *e, LblMap *m, const char *name) {
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      return m->ids[i];
    }
  }
  if (m->count == m->cap) {
    int cap = m->cap ? m->cap * 2 : 32;
    const char **n = realloc(m->names, (size_t)cap * sizeof(*n));
    int *ids = realloc(m->ids, (size_t)cap * sizeof(*ids));
    if (n) m->names = n;
    if (ids) m->ids = ids;
    if (!n || !ids) {
      arm64_fail(e, "out of memory growing the label map");
      return 0;
    }
    m->cap = cap;
  }
  m->names[m->count] = name;
  m->ids[m->count] = arm64_new_label(e);
  return m->ids[m->count++];
}

static void emit_imm(Arm64Emit *e, Arm64Reg rd, uint64_t v) {
  arm64_emit_word(e, arm64_movz(1, rd, (uint16_t)(v & 0xFFFF), 0));
  if ((v >> 16) & 0xFFFF)
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 16) & 0xFFFF), 1));
  if ((v >> 32) & 0xFFFF)
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 32) & 0xFFFF), 2));
  if ((v >> 48) & 0xFFFF)
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 48) & 0xFFFF), 3));
}

/* A set of value names known to hold a floating-point value. Needed because the
 * IR does not reliably tag a float on every operand use, yet the AAPCS64 ABI
 * passes/returns floats in v-registers -- so calls, returns, and arguments must
 * know float-ness. */
typedef struct StrSet {
  const char **names;
  /* Parallel to `names`: the float width a name holds (32 or 64), or 0 when the
   * set is not tracking widths. An instruction's own float_bits describes its
   * RESULT, so it is 0 for a float compare and for a float->int cast, and the
   * width has to come from the operand instead. */
  int *bits;
  int count;
  int cap;
} StrSet;

/* Scalar locals narrower than a register, and whether they sign-extend. Every
 * value otherwise lives in an 8-byte slot and is loaded and stored 8 bytes at a
 * time, so `var n: int32 = wide` kept all 64 bits and `int8` arithmetic never
 * wrapped. Floats are deliberately absent: they are bit patterns, and widening
 * one by sign extension would corrupt it. */
typedef struct NarrowMap {
  const char **names;
  signed char *sizes;   /* 1, 2 or 4 */
  signed char *is_signed;
  int count;
  int cap;
} NarrowMap;

/* Byte width `name` must be stored and loaded at, or 0 for a full register. */
static int narrow_size_of(const NarrowMap *m, const char *name, int *is_signed) {
  if (!m || !name) return 0;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      if (is_signed) *is_signed = m->is_signed[i];
      return m->sizes[i];
    }
  }
  return 0;
}

static void narrow_remove(NarrowMap *m, const char *name) {
  if (!name) return;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      m->count--;
      m->names[i] = m->names[m->count];
      m->sizes[i] = m->sizes[m->count];
      m->is_signed[i] = m->is_signed[m->count];
      return;
    }
  }
}

static void narrow_add(NarrowMap *m, const char *name, int size, int sign) {
  if (!name || (size != 1 && size != 2 && size != 4)) return;
  if (narrow_size_of(m, name, NULL)) return;
  if (m->count == m->cap) {
    int cap = m->cap ? m->cap * 2 : 16;
    const char **n = realloc(m->names, (size_t)cap * sizeof(*n));
    signed char *z = realloc(m->sizes, (size_t)cap * sizeof(*z));
    signed char *g = realloc(m->is_signed, (size_t)cap * sizeof(*g));
    if (n) m->names = n;
    if (z) m->sizes = z;
    if (g) m->is_signed = g;
    if (!n || !z || !g) return;
    m->cap = cap;
  }
  m->names[m->count] = name;
  m->sizes[m->count] = (signed char)size;
  m->is_signed[m->count] = (signed char)(sign != 0);
  m->count++;
}

/* Aggregate locals and their byte sizes. */
typedef struct AggMap {
  const char **names;
  int *sizes;
  int count;
  int cap;
} AggMap;

/* Size of the aggregate local `name`, or 0 when it is an ordinary value. */
static int agg_size_of(const AggMap *m, const char *name) {
  if (!m || !name) return 0;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      return m->sizes[i];
    }
  }
  return 0;
}

static void agg_remove(AggMap *m, const char *name) {
  if (!name) return;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      m->count--;
      m->names[i] = m->names[m->count];
      m->sizes[i] = m->sizes[m->count];
      return;
    }
  }
}

static void agg_add(AggMap *m, const char *name, int size) {
  if (!name || size <= 0 || agg_size_of(m, name)) return;
  if (m->count == m->cap) {
    int cap = m->cap ? m->cap * 2 : 16;
    const char **n = realloc(m->names, (size_t)cap * sizeof(*n));
    int *z = realloc(m->sizes, (size_t)cap * sizeof(*z));
    if (n) m->names = n;
    if (z) m->sizes = z;
    if (!n || !z) return;
    m->cap = cap;
  }
  m->names[m->count] = name;
  m->sizes[m->count] = size;
  m->count++;
}

static int set_has(const StrSet *s, const char *n) {
  if (!n) return 0;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) return 1;
  }
  return 0;
}
static void set_add_bits(StrSet *s, const char *n, int bits) {
  if (!n) return;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) {
      if (bits && !s->bits[i]) s->bits[i] = bits;
      return;
    }
  }
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 32;
    const char **p = realloc(s->names, (size_t)cap * sizeof(*p));
    int *w = realloc(s->bits, (size_t)cap * sizeof(*w));
    if (p) s->names = p;
    if (w) s->bits = w;
    if (!p || !w) return;
    s->cap = cap;
  }
  s->names[s->count] = n;
  s->bits[s->count] = bits;
  s->count++;
}

static void set_add(StrSet *s, const char *n) { set_add_bits(s, n, 0); }

static void set_remove(StrSet *s, const char *n) {
  if (!n) return;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) {
      s->count--;
      s->names[i] = s->names[s->count];
      s->bits[i] = s->bits[s->count];
      return;
    }
  }
}

/* Force `n`'s membership/width, replacing any earlier scope's entry. */
static void set_rebind(StrSet *s, const char *n, int present, int bits) {
  set_remove(s, n);
  if (present) set_add_bits(s, n, bits);
}

/* Recorded width for `n`, or 0 if unknown. */
static int set_bits(const StrSet *s, const char *n) {
  if (!n) return 0;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) return s->bits[i];
  }
  return 0;
}

/* Width a float operation must run at. The instruction's own float_bits
 * describes its RESULT and is not dependable: it is 0 for a comparison (whose
 * result is a bool), and an expression mixing a float32 value with a literal
 * can leave it at the default double. Decide from the operands instead. A float
 * LITERAL adapts to its context and so votes for nothing; any operand known to
 * be double forces double.
 *
 * Getting this wrong is silent: fmov'ing a float32 bit pattern into a d
 * register reads it as a denormal, and the arithmetic quietly returns ~0. */
static int float_op_bits(const StrSet *fs, const IROperand *a,
                         const IROperand *b, int instruction_bits);

/* Width of a float operand: its own if it carries one, otherwise the width
 * recorded for its name, defaulting to double. */
static int operand_float_bits(const StrSet *fs, const IROperand *op) {
  if (op->float_bits) return op->float_bits;
  if (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) {
    int bits = set_bits(fs, op->name);
    if (bits) return bits;
  }
  return 64;
}

static int operand_is_float(const StrSet *fs, const IROperand *op) {
  if (op->kind == IR_OPERAND_FLOAT || op->float_bits != 0) return 1;
  if ((op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL))
    return set_has(fs, op->name);
  return 0;
}

static int float_op_bits(const StrSet *fs, const IROperand *a,
                         const IROperand *b, int instruction_bits) {
  /* The frontend's width on the instruction is authoritative when present --
   * mixed-width arithmetic PROMOTES (float32 * 100.0 runs in double), and
   * operands of the other width are converted on load. Only when it is absent
   * (comparisons, some casts) do the operands vote; a literal adapts to
   * context and votes for nothing. */
  if (instruction_bits == 32 || instruction_bits == 64) return instruction_bits;
  const IROperand *ops[2] = {a, b};
  int known = 0, all_single = 1;
  for (int i = 0; i < 2; i++) {
    if (!ops[i] || ops[i]->kind == IR_OPERAND_NONE) continue;
    if (ops[i]->kind == IR_OPERAND_FLOAT) continue;
    known = 1;
    if (operand_float_bits(fs, ops[i]) != 32) all_single = 0;
  }
  return known && all_single ? 32 : 64;
}

static int prog_fn_index(const IRProgram *prog, const char *name) {
  if (!prog || !name) return -1;
  for (size_t i = 0; i < prog->function_count; i++) {
    if (strcmp(prog->functions[i]->name, name) == 0) return (int)i;
  }
  return -1;
}

/* Populate `fs` with every value name that holds a float in `fn` (params,
 * float locals, float-producing instructions). `retf` (callee returns-float
 * flags) lets a call result be recognized as float. */
static void build_float_set(const IRFunction *fn, const IRProgram *prog,
                            const int *retf, StrSet *fs) {
  /* Module-level float variables count too. Their names carry no float_bits on
   * the operand, so without this a global `const RATE = 1.5` reads as an
   * integer and its bit pattern gets converted instead of reinterpreted. */
  for (size_t i = 0; prog && i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &prog->module_symbols[i];
    if (symbol->kind == IR_MODSYM_VARIABLE && mtlc_type_is_float(symbol->type)) {
      set_add_bits(fs, symbol->name,
                   symbol->type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64);
    }
  }
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (fn->parameter_types &&
        type_text_is_float_scalar(fn->parameter_types[i])) {
      set_add_bits(fs, fn->parameter_names[i],
                   type_text_float_bits(fn->parameter_types[i]));
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        type_text_is_float_scalar(in->text)) {
      set_add_bits(fs, in->dest.name, type_text_float_bits(in->text));
    }
  }
  for (int pass = 0; pass < 4; pass++) {
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL)
        continue;
      switch (in->op) {
      case IR_OP_LOAD:
        /* A float load's width is the width it loaded; the instruction's
         * float_bits is often unset. */
        if (in->is_float) {
          int loaded = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value
                                                      : 8;
          set_add_bits(fs, in->dest.name,
                       in->float_bits ? in->float_bits
                                      : (loaded == 4 ? 32 : 64));
        }
        break;
      case IR_OP_BINARY:
      case IR_OP_UNARY:
        if (in->is_float) {
          set_add_bits(fs, in->dest.name,
                       in->float_bits
                           ? in->float_bits
                           : float_op_bits(fs, &in->lhs, &in->rhs, 0));
        }
        break;
      case IR_OP_CAST:
        if (in->dest.float_bits != 0)
          set_add_bits(fs, in->dest.name, in->dest.float_bits);
        break;
      case IR_OP_ASSIGN:
        if (operand_is_float(fs, &in->lhs))
          set_add_bits(fs, in->dest.name, operand_float_bits(fs, &in->lhs));
        break;
      case IR_OP_CALL:
      case IR_OP_CALL_INDIRECT: {
        const IRModuleSymbol *callee = module_function(prog, in->text);
        int ci = retf ? prog_fn_index(prog, in->text) : -1;
        if ((callee && mtlc_type_is_float(callee->return_type)) ||
            (ci >= 0 && retf[ci])) {
          /* Record the RETURN width too: a float32-returning call whose result
           * is later cast to int would otherwise be decoded as a double. */
          set_add_bits(fs, in->dest.name,
                       callee && callee->return_type
                           ? (callee->return_type->kind == MTLC_TYPE_FLOAT32
                                  ? 32
                                  : 64)
                           : 0);
        }
        break;
      }
      default:
        break;
      }
    }
  }
}

static int cmp_cond(const char *op);

static int type_is_unsigned(const MtlcType *t) {
  if (!t) return 0;
  switch (t->kind) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_FUNCTION_POINTER:
  case MTLC_TYPE_STRING:
    return 1;
  default:
    return 0;
  }
}

/* Does this type name spell an unsigned integer or a pointer? Parameter and
 * DECLARE_LOCAL types reach the backend as text, not as an MtlcType. "uint" also
 * covers "uint8[16]"; a '*' anywhere makes it a pointer. */
static int type_text_is_unsigned(const char *t) {
  if (!t) return 0;
  if (strchr(t, '*') != NULL) return 1;
  return type_text_base_is(t, "uint8") || type_text_base_is(t, "uint16") ||
         type_text_base_is(t, "uint32") || type_text_base_is(t, "uint64");
}

/* Populate `us` with every value name in `fn` that holds an unsigned integer or
 * a pointer. Comparisons and shifts of those need the unsigned condition codes:
 * on a 64-bit compare, ARM64_LT on 0x8000000000000000 answers the reverse of
 * what uint64 ordering says. Mirrors build_float_set, including its fixpoint
 * over assignment chains. */
static void build_unsigned_set(const IRFunction *fn, StrSet *us) {
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (fn->parameter_types && type_text_is_unsigned(fn->parameter_types[i])) {
      set_add(us, fn->parameter_names[i]);
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        (type_text_is_unsigned(in->text) || type_is_unsigned(in->value_type))) {
      set_add(us, in->dest.name);
    }
  }
  for (int pass = 0; pass < 4; pass++) {
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL)
        continue;
      switch (in->op) {
      case IR_OP_LOAD:
      case IR_OP_UNARY:
        if (in->is_unsigned) set_add(us, in->dest.name);
        break;
      case IR_OP_BINARY:
        /* A comparison yields 0/1 whatever its operands were, so it must not
         * mark its own destination unsigned-by-inheritance; every other binary
         * result carries its operands' signedness. */
        if ((in->is_unsigned || type_is_unsigned(in->value_type)) &&
            in->text && cmp_cond(in->text) < 0) {
          set_add(us, in->dest.name);
        }
        break;
      case IR_OP_ADDRESS_OF:
        set_add(us, in->dest.name);
        break;
      case IR_OP_CAST:
        if (type_is_unsigned(in->value_type)) set_add(us, in->dest.name);
        break;
      case IR_OP_ASSIGN:
        if ((in->lhs.kind == IR_OPERAND_TEMP ||
             in->lhs.kind == IR_OPERAND_SYMBOL) &&
            set_has(us, in->lhs.name)) {
          set_add(us, in->dest.name);
        }
        break;
      default:
        break;
      }
    }
  }
}

static int fn_returns_float(const IRFunction *fn, const IRProgram *prog,
                            const int *retf) {
  StrSet fs = {0};
  build_float_set(fn, prog, retf, &fs);
  int rf = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_RETURN && in->lhs.kind != IR_OPERAND_NONE &&
        operand_is_float(&fs, &in->lhs)) {
      rf = 1;
      break;
    }
  }
  free(fs.names);
  free(fs.bits);
  return rf;
}

/* Materialize the address of a NUL-terminated string literal into `dest`.
 * Object emission pools the bytes into .rodata behind a local symbol resolved
 * by page/lo12 relocations; the legacy self-contained path embeds the bytes
 * in the text stream (branched over) at a known virtual address. */
static void emit_string_literal_address(Arm64Emit *e, Arm64ObjectContext *object,
                                        const char *str, Arm64Reg dest) {
  if (!str) str = "";
  if (object) {
    char symbol[64];
    size_t offset = 0;
    snprintf(symbol, sizeof(symbol), ".Lmtlc.str.%u", object->string_id++);
    if (!binary_emitter_append_bytes(object->emitter, object->rodata_section,
                                     str, strlen(str) + 1, &offset) ||
        !binary_emitter_define_symbol(object->emitter, symbol,
                                      BINARY_SYMBOL_LOCAL,
                                      object->rodata_section, offset,
                                      strlen(str) + 1)) {
      arm64_fail(e, "could not place a string literal in .rodata");
      return;
    }
    emit_symbol_address(e, object, dest, symbol);
  } else {
    int past = arm64_new_label(e);
    arm64_emit_b(e, past);
    size_t soff = arm64_here(e);
    arm64_emit_bytes(e, str, strlen(str) + 1);
    arm64_bind_label(e, past);
    emit_imm(e, dest, (uint64_t)ELF_BASE + ELF_HDRS + soff);
  }
}

static const IRModuleSymbol *module_function(const IRProgram *program,
                                             const char *name);

/* Emit a bl to an external (or not-yet-seen) symbol through a CALL26
 * relocation, declaring the symbol on first use. Object emission only. */
static int emit_external_call(Arm64Emit *e, Arm64ObjectContext *object,
                              const IRProgram *prog, const char *name) {
  const IRModuleSymbol *callee = module_function(prog, name);
  const char *link_name = callee ? module_link_name(callee) : name;
  if (!binary_emitter_find_symbol(object->emitter, link_name) &&
      !binary_emitter_declare_external(object->emitter, link_name)) {
    arm64_fail(e, "could not declare external symbol '%s'", link_name);
    return 0;
  }
  size_t call_offset = arm64_here(e);
  arm64_emit_word(e, arm64_bl(0));
  return object_add_relocation(e, object, call_offset,
                               BINARY_RELOCATION_ARM64_CALL26, link_name);
}

/* Call one of the owned runtime entry points used by higher level
 * operations from. The self-contained executable emits its own freestanding
 * stub and branches to it; an object leaves a CALL26 relocation for the linker
 * to resolve against the full owned runtime. The names match in both paths
 * for the functions they stand in for. */
static int emit_runtime_call(Arm64Emit *e, Arm64ObjectContext *object,
                             const IRProgram *prog, LblMap *fns, int stub) {
  if (object) {
    return emit_external_call(e, object, prog, RUNTIME_STUBS[stub].name);
  }
  arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[stub].name));
  return 1;
}

/* rd = &function. An object defers it to page/lo12 relocations against the
 * function's symbol, exactly as for a global; the self-contained executable has
 * no relocations, so it patches the absolute address into a movz/movk quartet
 * once the label lands. Either way a function's address becomes a value, which
 * a PC-relative branch displacement cannot express. */
static void emit_function_address(Arm64Emit *e, SlotMap *s, Arm64Reg rd,
                                  const char *name) {
  if (s->object) {
    const IRModuleSymbol *symbol = module_function(s->program, name);
    emit_symbol_address(e, s->object, rd,
                        symbol ? module_link_name(symbol) : name);
    return;
  }
  arm64_emit_label_address(e, rd, label_for(e, s->fns, name));
}

/* Materialize a string literal used as a VALUE. A Mettle `string` is a
 * { chars, length } record, so the value is that record's address: the IR reads
 * `*"lit" [8]` to get the chars field back out. Handing back the characters'
 * address instead made that load read the first eight characters and use them
 * as a pointer. (cstr("lit") is different, and is folded at its call site to
 * the characters' address, which is what a `cstring` is.) */
static void emit_string_value(Arm64Emit *e, SlotMap *s, Arm64Reg dest,
                              const char *str, size_t length) {
  if (!str) {
    str = "";
    length = 0;
  }
  if (s->object) {
    Arm64ObjectContext *object = s->object;
    char chars_symbol[64], record_symbol[64];
    size_t chars_offset = 0, record_offset = 0;
    uint64_t encoded_length = (uint64_t)length;
    BinarySection *rodata = NULL;
    snprintf(chars_symbol, sizeof(chars_symbol), ".Lmtlc.str.%u",
             object->string_id++);
    snprintf(record_symbol, sizeof(record_symbol), ".Lmtlc.str.%u",
             object->string_id++);
    if (!binary_emitter_append_bytes(object->emitter, object->rodata_section,
                                     str, length + 1, &chars_offset) ||
        !binary_emitter_define_symbol(object->emitter, chars_symbol,
                                      BINARY_SYMBOL_LOCAL,
                                      object->rodata_section, chars_offset,
                                      length + 1) ||
        !binary_emitter_align_section(object->emitter, object->rodata_section, 8,
                                      0) ||
        !binary_emitter_append_zeros(object->emitter, object->rodata_section, 16,
                                     &record_offset) ||
        !binary_emitter_define_symbol(object->emitter, record_symbol,
                                      BINARY_SYMBOL_LOCAL,
                                      object->rodata_section, record_offset,
                                      16) ||
        !binary_emitter_add_relocation(object->emitter, object->rodata_section,
                                       record_offset, BINARY_RELOCATION_ADDR64,
                                       chars_symbol, 0)) {
      arm64_fail(e, "could not place a string literal in .rodata");
      return;
    }
    rodata = binary_emitter_get_section(object->emitter, object->rodata_section);
    if (!rodata || !rodata->data || record_offset + 16 > rodata->size) {
      arm64_fail(e, "could not write a string literal's length");
      return;
    }
    memcpy(rodata->data + record_offset + 8, &encoded_length, 8);
    emit_symbol_address(e, object, dest, record_symbol);
    return;
  }
  if (!s->data) {
    arm64_fail(e, "no data image to hold a string literal");
    return;
  }
  size_t chars = data_reserve(s->data, length + 1, 1);
  if (chars == (size_t)-1) {
    arm64_fail(e, "out of memory storing a string literal");
    return;
  }
  memcpy(s->data->bytes + chars, str, length + 1);
  size_t record = data_reserve(s->data, 16, 8);
  if (record == (size_t)-1) {
    arm64_fail(e, "out of memory storing a string literal's record");
    return;
  }
  uint64_t address = (uint64_t)ELF_DATA_VADDR + (uint64_t)chars;
  uint64_t encoded_length = (uint64_t)length;
  memcpy(s->data->bytes + record, &address, 8);
  memcpy(s->data->bytes + record + 8, &encoded_length, 8);
  emit_imm(e, dest, (uint64_t)ELF_DATA_VADDR + (uint64_t)record);
}

/* Load an IR value operand (temp/local/int/float/string) into `dest` as raw
 * 64-bit bits (a float operand yields its IEEE pattern; a string literal
 * yields its address). */
static Arm64Reg load_into(Arm64Emit *e, SlotMap *s, const IROperand *op,
                          Arm64Reg dest) {
  switch (op->kind) {
  case IR_OPERAND_INT:
    emit_imm(e, dest, (uint64_t)op->int_value);
    return dest;
  case IR_OPERAND_FLOAT:
    emit_imm(e, dest, ieee_bits(op));
    return dest;
  case IR_OPERAND_STRING:
    emit_string_value(e, s, dest, op->name, ir_operand_string_length(op));
    return dest;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    int off = slot_find(s, op->name);
    const IRModuleSymbol *global =
        off < 0 && op->kind == IR_OPERAND_SYMBOL ? module_variable(s, op->name)
                                                 : NULL;
    if (global) {
      if (type_is_aggregate(global->type)) {
        /* An aggregate global named as a value means its address, exactly as
         * an aggregate local does; "loading" it read its first word. */
        emit_global_address(e, s, dest, global);
        return dest;
      }
      emit_global_address(e, s, ARM64_X16, global);
      emit_load_sized(e, dest, ARM64_X16, module_type_size(global->type),
                      module_type_signed(global->type));
      return dest;
    }
    if (off < 0 && op->kind == IR_OPERAND_SYMBOL &&
        prog_fn_index(s->program, op->name) >= 0) {
      emit_function_address(e, s, dest, op->name); /* a function as a value */
      return dest;
    }
    if (off < 0) off = slot_off(s, op->name);
    if (off < 0) {
      arm64_fail(e, "no stack slot for '%s'", op->name ? op->name : "(unnamed)");
      return dest;
    }
    if (op->kind == IR_OPERAND_SYMBOL &&
        agg_size_of(s->aggregates, op->name) > 8) {
      /* A wide aggregate local named as a value means its ADDRESS: the IR
       * computes `@s + 8` for s.length. A <=8-byte struct is instead its
       * packed bytes (that is how it travels as a call argument), and its
       * fields are reached through an explicit &. */
      emit_lea_local(e, dest, off);
      return dest;
    }
    int is_signed = 0;
    int width = narrow_size_of(s->narrow, op->name, &is_signed);
    if (width) {
      /* Address the slot through a scratch so the access width's own offset
       * encoding (strb's 4095, strh's 8190) cannot overflow on a big frame. */
      Arm64Reg base = dest == ARM64_X17 ? ARM64_X16 : ARM64_X17;
      emit_lea_local(e, base, off);
      emit_load_sized(e, dest, base, width, is_signed);
      return dest;
    }
    emit_slot_ldr(e, dest, off);
    return dest;
  }
  default:
    arm64_fail(e, "operand kind %d cannot be loaded", (int)op->kind);
    return dest;
  }
}

static void store_dest(Arm64Emit *e, SlotMap *s, const IROperand *dst,
                       Arm64Reg src) {
  int off = dst && dst->name ? slot_find(s, dst->name) : -1;
  const IRModuleSymbol *global =
      off < 0 && dst && dst->kind == IR_OPERAND_SYMBOL
          ? module_variable(s, dst->name)
          : NULL;
  if (global) {
    emit_global_address(e, s, ARM64_X16, global);
    emit_store_sized(e, src, ARM64_X16, module_type_size(global->type));
    return;
  }
  if (off < 0) off = slot_off(s, dst->name);
  if (off < 0) {
    arm64_fail(e, "no stack slot for destination '%s'",
               dst && dst->name ? dst->name : "(unnamed)");
    return;
  }
  int width = narrow_size_of(s->narrow, dst->name, NULL);
  if (width) {
    /* Keep only the declared width, so `var n: int32 = wide` truncates and
     * int8 arithmetic wraps the way two's complement says it must. */
    Arm64Reg base = src == ARM64_X17 ? ARM64_X16 : ARM64_X17;
    emit_lea_local(e, base, off);
    emit_store_sized(e, src, base, width);
    return;
  }
  emit_slot_str(e, src, off);
}

/* rd = sp + off (address of a local's slot), valid for any frame size up to
 * 16MB: two add-immediates (the second shifted by 12) reach 24 bits without
 * borrowing a scratch register. */
static void emit_lea_local(Arm64Emit *e, Arm64Reg rd, int off) {
  if (off <= 4095) {
    arm64_emit_word(e, arm64_add_imm(1, rd, ARM64_SP, (uint32_t)off, 0));
    return;
  }
  if (off > 0xFFFFFF) {
    arm64_fail(e, "frame offset %d exceeds the 16MB lea reach", off);
    return;
  }
  arm64_emit_word(e, arm64_add_imm(1, rd, ARM64_SP, (uint32_t)off & 0xFFF, 0));
  arm64_emit_word(e, arm64_add_imm(1, rd, rd, (uint32_t)off >> 12, 1));
}

/* 8-byte slot load/store at ANY offset. The scaled imm12 form reaches 32760;
 * past that the offset silently truncated into the field before -- the code
 * encoded, ran, and read the wrong slot. */
static void emit_slot_ldr(Arm64Emit *e, Arm64Reg rt, int off) {
  if (off >= 0 && off <= 32760) {
    arm64_emit_word(e, arm64_ldr_imm(1, rt, ARM64_SP, off));
    return;
  }
  Arm64Reg base = rt == ARM64_X16 ? ARM64_X17 : ARM64_X16;
  emit_lea_local(e, base, off);
  arm64_emit_word(e, arm64_ldr_imm(1, rt, base, 0));
}

static void emit_slot_str(Arm64Emit *e, Arm64Reg rt, int off) {
  if (off >= 0 && off <= 32760) {
    arm64_emit_word(e, arm64_str_imm(1, rt, ARM64_SP, off));
    return;
  }
  Arm64Reg base = rt == ARM64_X16 ? ARM64_X17 : ARM64_X16;
  emit_lea_local(e, base, off);
  arm64_emit_word(e, arm64_str_imm(1, rt, base, 0));
}

/* FP-register variant for parameter homing. */
static void emit_slot_str_fp(Arm64Emit *e, int is_double, int ft, int off) {
  int limit = is_double ? 32760 : 16380;
  if (off >= 0 && off <= limit) {
    arm64_emit_word(e, arm64_str_fp(is_double, ft, ARM64_SP, off));
    return;
  }
  emit_lea_local(e, ARM64_X16, off);
  arm64_emit_word(e, arm64_str_fp(is_double, ft, ARM64_X16, 0));
}

/* Condition code for a signed integer comparison, or -1 if `op` is not a
 * comparison at all (callers use that as the "is this a compare?" test). */
static int cmp_cond(const char *op) {
  if (strcmp(op, "==") == 0) return ARM64_EQ;
  if (strcmp(op, "!=") == 0) return ARM64_NE;
  if (strcmp(op, "<") == 0) return ARM64_LT;
  if (strcmp(op, "<=") == 0) return ARM64_LE;
  if (strcmp(op, ">") == 0) return ARM64_GT;
  if (strcmp(op, ">=") == 0) return ARM64_GE;
  return -1;
}

/* Unsigned twin: LO/LS/HI/HS read the carry flag instead of N-vs-V, so a value
 * with the high bit set orders above a small one instead of below it. */
static int cmp_cond_unsigned(const char *op) {
  if (strcmp(op, "<") == 0) return ARM64_CC;  /* LO */
  if (strcmp(op, "<=") == 0) return ARM64_LS;
  if (strcmp(op, ">") == 0) return ARM64_HI;
  if (strcmp(op, ">=") == 0) return ARM64_CS; /* HS */
  return cmp_cond(op);                        /* ==/!= are sign-agnostic */
}

/* Condition code to read FCMP's flags. An unordered compare (either operand
 * NaN) sets N=0,Z=0,C=1,V=1, where LT and LE both come out TRUE -- so `<` and
 * `<=` must use MI and LS, which are false there. GT/GE/EQ are already false
 * and NE already true under those flags, which is what IEEE-754 requires. */
static int cmp_cond_float(const char *op) {
  if (strcmp(op, "<") == 0) return ARM64_MI;
  if (strcmp(op, "<=") == 0) return ARM64_LS;
  return cmp_cond(op);
}

/* True when this binary's integer operands must be ordered as unsigned. `us`
 * holds the names the function's unsigned/pointer values live under. */
static int binary_is_unsigned(const StrSet *us, const IRInstruction *in) {
  if (in->is_unsigned || type_is_unsigned(in->value_type)) return 1;
  const IROperand *ops[2] = {&in->lhs, &in->rhs};
  for (int k = 0; k < 2; k++) {
    if ((ops[k]->kind == IR_OPERAND_TEMP ||
         ops[k]->kind == IR_OPERAND_SYMBOL) &&
        set_has(us, ops[k]->name)) {
      return 1;
    }
  }
  return 0;
}

/* Put one operand of a float binary into FP register `fd` at width `d`. A float
 * operand carries an IEEE bit pattern, so it moves across as bits; an integer
 * operand (`x + 1`, or a mixed-type expression the frontend left unconverted)
 * has to be CONVERTED, since reinterpreting its bits as a double is nonsense. */
static void load_float_operand(Arm64Emit *e, SlotMap *s, const StrSet *fs,
                               const IROperand *op, int d, int fd,
                               Arm64Reg scratch) {
  if (op->kind == IR_OPERAND_FLOAT) {
    /* Build the constant at the width the operation runs at, not at whatever
     * width the literal nominally carries. */
    emit_imm(e, scratch, ieee_bits_at(op, d ? 64 : 32));
    arm64_emit_word(e, arm64_fmov_gp(d, fd, scratch));
    return;
  }
  Arm64Reg bits = load_into(e, s, op, scratch);
  if (operand_is_float(fs, op)) {
    /* Move the bits in at the operand's OWN width, then convert if the
     * operation runs at the other one. fmov'ing a 32-bit pattern into a d
     * register would read it as a denormal, not widen it. */
    int src_d = operand_float_bits(fs, op) != 32;
    arm64_emit_word(e, arm64_fmov_gp(src_d, fd, bits));
    if (src_d != d) {
      arm64_emit_word(e, arm64_fcvt(d, fd, fd));
    }
  } else {
    arm64_emit_word(e, arm64_scvtf(d, fd, bits));
  }
}

static void lower_binary(Arm64Emit *e, SlotMap *s, const StrSet *fs,
                         const StrSet *us, const IRInstruction *in) {
  const char *op = in->text;

  /* Floating-point binary: get both operands into d0/d1, operate, store the
   * result bits. A comparison yields a 0/1 integer via fcmp + cset. */
  if (in->is_float) {
    int fcc = cmp_cond(op) < 0 ? -1 : cmp_cond_float(op);
    int d = float_op_bits(fs, &in->lhs, &in->rhs, in->float_bits) != 32;
    load_float_operand(e, s, fs, &in->lhs, d, 0, R_LHS);
    load_float_operand(e, s, fs, &in->rhs, d, 1, R_RHS);
    if (fcc >= 0) {
      arm64_emit_word(e, arm64_fcmp(d, 0, 1));
      arm64_emit_word(e, arm64_cset(1, R_RES, (Arm64Cond)fcc));
    } else if (strcmp(op, "+") == 0) {
      arm64_emit_word(e, arm64_fadd(d, 0, 0, 1));
    } else if (strcmp(op, "-") == 0) {
      arm64_emit_word(e, arm64_fsub(d, 0, 0, 1));
    } else if (strcmp(op, "*") == 0) {
      arm64_emit_word(e, arm64_fmul(d, 0, 0, 1));
    } else if (strcmp(op, "/") == 0) {
      arm64_emit_word(e, arm64_fdiv(d, 0, 0, 1));
    } else {
      arm64_fail(e, "float binary operator '%s' has no AArch64 lowering", op);
      return;
    }
    if (fcc < 0) {
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
    }
    store_dest(e, s, &in->dest, R_RES);
    return;
  }

  int uns = binary_is_unsigned(us, in);
  Arm64Reg a = load_into(e, s, &in->lhs, R_LHS);

  /* Strength-reduce `x <op> C` through the shared rule table before spending
   * a mul or a divide. The kinds this backend does not handle fall through to
   * the general instruction below. Shifting a canonically-extended value is
   * exact: a multiply agrees with a shift mod 2^64, and an unsigned home
   * holds its zero-extended value, so lsr and the mask read it directly. */
  if (in->rhs.kind == IR_OPERAND_INT && op[0] != '\0' && op[1] == '\0') {
    CgStrengthRewrite rw;
    if (cg_strength_classify(op[0], in->rhs.int_value, uns, &rw)) {
      switch (rw.kind) {
      case CG_SR_MUL_SHL:
        arm64_emit_word(e, arm64_lsl_imm(1, R_RES, a, rw.shift));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_MUL_SHL_ADD:
        arm64_emit_word(e, arm64_lsl_imm(1, R_AUX, a, rw.shift));
        arm64_emit_word(e, arm64_add_reg(1, R_RES, R_AUX, a));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_MUL_SHL_SUB:
        arm64_emit_word(e, arm64_lsl_imm(1, R_AUX, a, rw.shift));
        arm64_emit_word(e, arm64_sub_reg(1, R_RES, R_AUX, a));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_UDIV_SHR:
        arm64_emit_word(e, arm64_lsr_imm(1, R_RES, a, rw.shift));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_UREM_AND:
        emit_imm(e, R_AUX, (uint64_t)rw.mask);
        arm64_emit_word(e, arm64_and_reg(1, R_RES, a, R_AUX));
        store_dest(e, s, &in->dest, R_RES);
        return;
      default:
        break; /* signed pow2 fixups and magic divides keep sdiv/udiv */
      }
    }
  }

  Arm64Reg b = load_into(e, s, &in->rhs, R_RHS);
  int cc = cmp_cond(op) < 0 ? -1 : (uns ? cmp_cond_unsigned(op) : cmp_cond(op));
  if (cc >= 0) {
    arm64_emit_word(e, arm64_cmp_reg(1, a, b));
    arm64_emit_word(e, arm64_cset(1, R_RES, (Arm64Cond)cc));
  } else if (strcmp(op, "+") == 0) {
    arm64_emit_word(e, arm64_add_reg(1, R_RES, a, b));
  } else if (strcmp(op, "-") == 0) {
    arm64_emit_word(e, arm64_sub_reg(1, R_RES, a, b));
  } else if (strcmp(op, "*") == 0) {
    arm64_emit_word(e, arm64_mul(1, R_RES, a, b));
  } else if (strcmp(op, "/") == 0) {
    arm64_emit_word(e, uns ? arm64_udiv(1, R_RES, a, b)
                           : arm64_sdiv(1, R_RES, a, b));
  } else if (strcmp(op, "%") == 0) {
    arm64_emit_word(e, uns ? arm64_udiv(1, R_AUX, a, b)
                           : arm64_sdiv(1, R_AUX, a, b));
    arm64_emit_word(e, arm64_msub(1, R_RES, R_AUX, b, a));
  } else if (strcmp(op, "&") == 0) {
    arm64_emit_word(e, arm64_and_reg(1, R_RES, a, b));
  } else if (strcmp(op, "|") == 0) {
    arm64_emit_word(e, arm64_orr_reg(1, R_RES, a, b));
  } else if (strcmp(op, "^") == 0) {
    arm64_emit_word(e, arm64_eor_reg(1, R_RES, a, b));
  } else if (strcmp(op, "<<") == 0) {
    arm64_emit_word(e, arm64_lslv(1, R_RES, a, b));
  } else if (strcmp(op, ">>") == 0) {
    arm64_emit_word(e, uns ? arm64_lsrv(1, R_RES, a, b)
                           : arm64_asrv(1, R_RES, a, b));
  } else {
    arm64_fail(e, "binary operator '%s' has no AArch64 lowering", op);
    return;
  }
  store_dest(e, s, &in->dest, R_RES);
}

static void lower_unary(Arm64Emit *e, SlotMap *s, const StrSet *fs,
                        const IRInstruction *in) {
  const char *op = in->text;
  if (in->is_float || operand_is_float(fs, &in->lhs)) {
    int d = float_op_bits(fs, &in->lhs, NULL, in->float_bits) != 32;
    if (strcmp(op, "-") == 0) {
      load_float_operand(e, s, fs, &in->lhs, d, 0, R_LHS);
      arm64_emit_word(e, arm64_fneg(d, 0, 0));
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
      store_dest(e, s, &in->dest, R_RES);
      return;
    }
    if (strcmp(op, "!") == 0) {
      /* !x on a float: compare against +0.0 (which -0.0 also equals). */
      load_float_operand(e, s, fs, &in->lhs, d, 0, R_LHS);
      emit_imm(e, R_LHS, 0);
      arm64_emit_word(e, arm64_fmov_gp(d, 1, R_LHS));
      arm64_emit_word(e, arm64_fcmp(d, 0, 1));
      arm64_emit_word(e, arm64_cset(1, R_RES, ARM64_EQ));
      store_dest(e, s, &in->dest, R_RES);
      return;
    }
    arm64_fail(e, "float unary operator '%s' has no AArch64 lowering", op);
    return;
  }
  Arm64Reg a = load_into(e, s, &in->lhs, R_LHS);
  if (strcmp(op, "-") == 0) {
    arm64_emit_word(e, arm64_neg(1, R_RES, a));
  } else if (strcmp(op, "~") == 0) {
    arm64_emit_word(e, arm64_mvn(1, R_RES, a));
  } else if (strcmp(op, "!") == 0) {
    arm64_emit_word(e, arm64_cmp_imm(1, a, 0, 0));
    arm64_emit_word(e, arm64_cset(1, R_RES, ARM64_EQ));
  } else {
    arm64_fail(e, "unary operator '%s' has no AArch64 lowering", op);
    return;
  }
  store_dest(e, s, &in->dest, R_RES);
}

static const IRModuleSymbol *module_function(const IRProgram *program,
                                             const char *name) {
  if (!program || !name) return NULL;
  const IRModuleSymbol *symbol = ir_program_lookup_symbol(program, name);
  return symbol && symbol->kind == IR_MODSYM_FUNCTION ? symbol : NULL;
}

/* Declared type of argument `index`, from the callee's signature when the call
 * names one and from the instruction's own argument_types otherwise -- an
 * indirect call has no name, so that array is the only signature it carries. */
static const MtlcType *call_arg_type(const IRProgram *program,
                                     const IRInstruction *call, size_t index) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && index < callee->param_count && callee->param_types &&
      callee->param_types[index]) {
    return callee->param_types[index];
  }
  if (call->argument_types && index < call->argument_count &&
      call->argument_types[index]) {
    return call->argument_types[index];
  }
  return NULL;
}

static int call_arg_is_float(const IRProgram *program,
                             const IRInstruction *call, size_t index,
                             const StrSet *floats) {
  const MtlcType *type = call_arg_type(program, call, index);
  if (type) {
    return mtlc_type_is_float(type);
  }
  return operand_is_float(floats, &call->arguments[index]);
}

static int call_arg_float_bits(const IRProgram *program,
                               const IRInstruction *call, size_t index) {
  const MtlcType *type = call_arg_type(program, call, index);
  if (type) {
    return type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
  }
  return call->arguments[index].float_bits == 32 ? 32 : 64;
}

static int call_returns_float(const IRProgram *program,
                              const IRInstruction *call, const int *retf) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && callee->return_type) {
    return mtlc_type_is_float(callee->return_type);
  }
  int index = retf ? prog_fn_index(program, call->text) : -1;
  return index >= 0 && retf[index];
}

static int call_return_float_bits(const IRProgram *program,
                                  const IRInstruction *call) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && callee->return_type) {
    return callee->return_type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
  }
  return call->dest.float_bits == 32 ? 32 : 64;
}

static int max_outgoing_stack(Arm64Emit *e, const IRFunction *fn,
                              const IRProgram *program,
                              const StrSet *floats) {
  int maximum = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *call = &fn->instructions[i];
    int indirect = call->op == IR_OP_CALL_INDIRECT;
    if ((call->op != IR_OP_CALL && !indirect) || call->argument_count == 0 ||
        (!indirect && (!call->text || strcmp(call->text, "cstr") == 0))) {
      continue;
    }
    if (call->argument_count > (size_t)INT32_MAX) {
      arm64_fail(e, "call to '%s' has an implausible argument count",
                 indirect ? "(indirect)" : call->text);
      return 0;
    }
    int count = (int)call->argument_count;
    int *is_float = malloc((size_t)count * sizeof(*is_float));
    Arm64ArgLocation *locations =
        malloc((size_t)count * sizeof(*locations));
    if (!is_float || !locations) {
      free(is_float);
      free(locations);
      arm64_fail(e, "out of memory laying out the arguments of '%s'",
                 indirect ? "(indirect)" : call->text);
      return 0;
    }
    for (int k = 0; k < count; k++) {
      is_float[k] = call_arg_is_float(program, call, (size_t)k, floats);
    }
    int bytes = 0;
    if (!arm64_compute_arg_layout(is_float, count, locations, &bytes)) {
      arm64_fail(e, "cannot lay out the %d arguments of '%s' under AAPCS64",
                 count, indirect ? "(indirect)" : call->text);
    }
    if (bytes > maximum) maximum = bytes;
    free(is_float);
    free(locations);
    if (e->error) return 0;
  }
  return (maximum + 15) & ~15;
}

/* Does this operand need a frame slot? Temps always do; a named symbol does
 * unless it is a module global (which lives in .data) or a function (whose name
 * denotes its code address, not storage). */
static int operand_needs_slot(const SlotMap *slots, const IRProgram *prog,
                              const IROperand *op) {
  if (op->kind == IR_OPERAND_TEMP) return 1;
  if (op->kind != IR_OPERAND_SYMBOL || !op->name) return 0;
  if (module_variable(slots, op->name)) return 0;
  return prog_fn_index(prog, op->name) < 0;
}

/* Narrow integer locals and parameters. Floats stay out: a float32 is a bit
 * pattern, and sign-extending it would corrupt the value. */
static void build_narrow_map(const IRFunction *fn, const IRProgram *prog,
                             NarrowMap *nm) {
  const IRModuleSymbol *self = module_function(prog, fn->name);
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (!self || i >= self->param_count || !self->param_types) break;
    const MtlcType *t = self->param_types[i];
    if (!t || mtlc_type_is_float(t) || type_is_aggregate(t)) continue;
    size_t size = mtlc_type_size(t);
    if (size == 1 || size == 2 || size == 4) {
      narrow_add(nm, fn->parameter_names[i], (int)size, module_type_signed(t));
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op != IR_OP_DECLARE_LOCAL || !in->dest.name) continue;
    const MtlcType *t = declared_type(prog, in);
    if (!t || mtlc_type_is_float(t) || type_is_aggregate(t)) continue;
    size_t size = mtlc_type_size(t);
    if (size == 1 || size == 2 || size == 4) {
      narrow_add(nm, in->dest.name, (int)size, module_type_signed(t));
    }
  }
}

/* Aggregate LOCALS in `fn`: names whose slot holds the object itself, so naming
 * one can only mean its address. A `string` local is a { chars, length } record
 * and `*@msg [8]` reads chars out of it; loading the slot instead would read
 * the first eight characters and use them as a pointer.
 *
 * Parameters are deliberately absent. An aggregate is passed by reference here,
 * so a parameter's slot holds a POINTER to the caller's object and naming it
 * must keep loading that pointer. */
static void build_aggregate_set(const IRFunction *fn, const IRProgram *prog,
                                AggMap *ag) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        type_is_aggregate(declared_type(prog, in))) {
      agg_add(ag, in->dest.name, declare_local_size(prog, in));
    }
  }
}

/* Address of a load's or store's memory operand. A `string` local is a
 * { chars, length } record and the IR names it bare where it means "&msg": the
 * slot IS the record, so `*@msg [8]` reads its chars field. Loading the slot
 * instead read the first eight characters and used them as a pointer.
 *
 * Only this position gets the rule. Elsewhere -- a call argument, a return --
 * the frontend has already chosen how the aggregate travels (a small struct
 * goes packed in a register), and overriding that corrupts it. */
static Arm64Reg address_operand(Arm64Emit *e, SlotMap *s, const AggMap *ag,
                                const IROperand *op, Arm64Reg dest) {
  if (op->kind == IR_OPERAND_SYMBOL && agg_size_of(ag, op->name) > 0) {
    int off = slot_find(s, op->name);
    if (off >= 0) {
      emit_lea_local(e, dest, off);
      return dest;
    }
  }
  return load_into(e, s, op, dest);
}

/* Past this many bytes a record is copied by a loop rather than unrolled: two
 * instructions per eight bytes stops being a good trade, and the scaled
 * ldr/str offset field runs out at 32760 regardless. */
#define ARM64_COPY_UNROLL_MAX 128

/* Copy `size` bytes from [src] to [dst], widest access first. Both are
 * addresses; offsets stay naturally aligned for each access width. */
static void emit_fixed_copy(Arm64Emit *e, Arm64Reg dst, Arm64Reg src,
                            int size) {
  int off = 0;
  if (size > ARM64_COPY_UNROLL_MAX) {
    /* Walk the 8-byte bulk through private cursors so the caller's dst/src
     * survive the copy. X13-X15 are live only inside the freestanding stubs,
     * which never copy a record. */
    int words = size / 8;
    int loop = arm64_new_label(e);
    arm64_emit_word(e, arm64_mov_reg(1, ARM64_X13, dst));
    arm64_emit_word(e, arm64_mov_reg(1, ARM64_X14, src));
    emit_imm(e, ARM64_X15, (uint64_t)words);
    arm64_bind_label(e, loop);
    arm64_emit_word(e, arm64_ldr_imm(1, R_AUX, ARM64_X14, 0));
    arm64_emit_word(e, arm64_str_imm(1, R_AUX, ARM64_X13, 0));
    arm64_emit_word(e, arm64_add_imm(1, ARM64_X14, ARM64_X14, 8, 0));
    arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X13, 8, 0));
    arm64_emit_word(e, arm64_sub_imm(1, ARM64_X15, ARM64_X15, 1, 0));
    arm64_emit_cbnz(e, 1, ARM64_X15, loop);
    /* The cursors now point at the tail, so it copies from offset zero. */
    dst = ARM64_X13;
    src = ARM64_X14;
    size -= words * 8;
  }
  while (size - off >= 8) {
    arm64_emit_word(e, arm64_ldr_imm(1, R_AUX, src, off));
    arm64_emit_word(e, arm64_str_imm(1, R_AUX, dst, off));
    off += 8;
  }
  if (size - off >= 4) {
    arm64_emit_word(e, arm64_ldr_imm(0, R_AUX, src, off));
    arm64_emit_word(e, arm64_str_imm(0, R_AUX, dst, off));
    off += 4;
  }
  if (size - off >= 2) {
    arm64_emit_word(e, arm64_ldrh_imm(R_AUX, src, off));
    arm64_emit_word(e, arm64_strh_imm(R_AUX, dst, off));
    off += 2;
  }
  if (size - off >= 1) {
    arm64_emit_word(e, arm64_ldrb_imm(R_AUX, src, off));
    arm64_emit_word(e, arm64_strb_imm(R_AUX, dst, off));
  }
}

/* Does this argument denote an aggregate wider than a register? Those travel
 * as their ADDRESS (the self-contained backend's internal convention; the
 * callee, also compiled here, reads fields through it). Two exceptions: a
 * STRING literal already loads as a record address, and the hand-written io
 * print stubs read x0 as the chars pointer, i.e. the record's first word. */
static int arg_is_wide_aggregate(const IRProgram *prog, SlotMap *s,
                                 const IRInstruction *call,
                                 const IROperand *arg) {
  if (arg->kind != IR_OPERAND_SYMBOL) return 0;
  if (call->text && io_stub_intrinsic(call->text, NULL, NULL)) return 0;
  if (agg_size_of(s->aggregates, arg->name) > 8) return 1;
  const IRModuleSymbol *global = module_variable(s, arg->name);
  (void)prog;
  return global && type_is_aggregate(global->type) &&
         mtlc_type_size(global->type) > 8;
}

/* Materialize the ADDRESS an aggregate operand lives at. */
static void aggregate_address(Arm64Emit *e, SlotMap *s, const IROperand *op,
                              Arm64Reg dest) {
  const IRModuleSymbol *global =
      slot_find(s, op->name) < 0 ? module_variable(s, op->name) : NULL;
  if (global) {
    emit_global_address(e, s, dest, global);
  } else {
    emit_lea_local(e, dest, slot_off(s, op->name));
  }
}

/* Byte size of `name`'s aggregate return, or 0 for a scalar/float/small one.
 * A composite wider than a register is returned through a caller-provided
 * buffer whose address travels in x8 (the AAPCS64 indirect-result register):
 * returning it packed would need multiple registers, and returning the callee
 * frame's address would dangle. */
static int fn_aggregate_return_size(const IRProgram *prog, const char *name) {
  const IRModuleSymbol *symbol = module_function(prog, name);
  if (!symbol || !symbol->return_type ||
      !type_is_aggregate(symbol->return_type)) {
    return 0;
  }
  size_t size = mtlc_type_size(symbol->return_type);
  return size > 8 && size <= INT_MAX ? (int)size : 0;
}

/* Slot names for the sret plumbing (interned literals, so slot_alloc's
 * pointer-equality fast path also works). */
static const char SRET_SAVE_SLOT[] = "..sret.save";
static const char SRET_BUFFER_SLOT[] = "..sret.buf";
static const char CONCAT_A_SLOT[] = "..concat.a";
static const char CONCAT_B_SLOT[] = "..concat.b";
static const char CONCAT_DST_SLOT[] = "..concat.dst";

/* Is this instruction the string `+`? The result type says so; the operands
 * are records. */
static int binary_is_string_concat(const IRInstruction *in) {
  return in->op == IR_OP_BINARY && in->text && strcmp(in->text, "+") == 0 &&
         in->value_type && in->value_type->kind == MTLC_TYPE_STRING;
}

/* One GP call argument. Wide aggregates travel as their address (load_into
 * already says so for locals; globals and literals resolve the same way). The
 * hand-written io print stubs predate the record convention and read x0 as the
 * CHARS pointer, so a string argument to them is dereferenced once. */
static void load_call_argument(Arm64Emit *e, SlotMap *slots,
                               const IRProgram *prog,
                               const IRInstruction *call, const IROperand *arg,
                               Arm64Reg dest) {
  int stub_is_string = 0;
  int is_stub =
      call->text && io_stub_intrinsic(call->text, NULL, &stub_is_string);
  /* A TEMP argument to a string stub is a record address too (a concat or
   * "{expr}" conversion result), so it takes the same single dereference. */
  if (is_stub &&
      (arg->kind == IR_OPERAND_STRING ||
       (stub_is_string && arg->kind == IR_OPERAND_TEMP) ||
       (arg->kind == IR_OPERAND_SYMBOL &&
        agg_size_of(slots->aggregates, arg->name) > 8))) {
    load_into(e, slots, arg, dest); /* record address */
    arm64_emit_word(e, arm64_ldr_imm(1, dest, dest, 0)); /* .chars */
    return;
  }
  if (arg->kind == IR_OPERAND_SYMBOL &&
      arg_is_wide_aggregate(prog, slots, call, arg)) {
    aggregate_address(e, slots, arg, dest);
    return;
  }
  load_into(e, slots, arg, dest);
}

/* Place a call's arguments where AAPCS64 wants them. GP and FP registers are
 * independent banks; overflow values go to the frame's reserved outgoing-call
 * area at [sp,#stack_offset]. Shared by direct and indirect calls, and needed
 * for real 11-argument C ABIs like cuLaunchKernel, not just synthetic tests. */
static int emit_call_arguments(Arm64Emit *e, SlotMap *slots,
                               const IRProgram *prog, const StrSet *fs,
                               const IRInstruction *in) {
  if (in->argument_count == 0) return 1;
  const char *who = in->text ? in->text : "(indirect)";
  int count = (int)in->argument_count;
  int *is_float = malloc((size_t)count * sizeof(*is_float));
  Arm64ArgLocation *locations = malloc((size_t)count * sizeof(*locations));
  if (!is_float || !locations) {
    free(is_float);
    free(locations);
    arm64_fail(e, "out of memory laying out the arguments of '%s'", who);
    return 0;
  }
  for (int k = 0; k < count; k++) {
    is_float[k] = call_arg_is_float(prog, in, (size_t)k, fs);
  }
  if (!arm64_compute_arg_layout(is_float, count, locations, NULL)) {
    arm64_fail(e, "cannot lay out the %d arguments of '%s' under AAPCS64",
               count, who);
  }
  for (int k = 0; k < count && !e->error; k++) {
    const IROperand *arg = &in->arguments[k];
    Arm64ArgLocation location = locations[k];
    if (location.kind == ARM64_ARG_IN_VEC_REGISTER) {
      /* The parameter is float. An argument that is not (a literal `3` for a
       * float64 parameter) must be converted, not bit-reinterpreted. */
      int is_double = call_arg_float_bits(prog, in, (size_t)k) != 32;
      load_float_operand(e, slots, fs, arg, is_double, (int)location.reg,
                         R_LHS);
    } else if (location.kind == ARM64_ARG_IN_GP_REGISTER) {
      load_call_argument(e, slots, prog, in, arg, location.reg);
    } else {
      load_call_argument(e, slots, prog, in, arg, R_LHS);
      arm64_emit_word(
          e, arm64_str_imm(1, R_LHS, ARM64_SP, location.stack_offset));
    }
  }
  free(is_float);
  free(locations);
  return !e->error;
}

/* Lower one function body. `fns` maps callee names to entry labels so IR_OP_CALL
 * can resolve a cross-function bl (and so a function's address can be taken);
 * `prog`/`retf` drive the float ABI (which callees return floats); `object` and
 * `data` select where globals live, and exactly one of them is set. All may be
 * NULL for the single-function path. */
typedef struct {
  Arm64Emit *e;
  const IRFunction *fn;
  LblMap *fns;
  const IRProgram *prog;
  const int *retf;
  Arm64ObjectContext *object;
  SlotMap *slots;
  LblMap *labels;
  StrSet *fs;
  StrSet *us;
  AggMap *ag;
  NarrowMap *nm;
  StrSet *wide_params;
  int frame;
  int sret_size;
} Arm64Scope;

static int encode_control_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_NOP:
    break;
  case IR_OP_DECLARE_LOCAL: {
    /* The IR reuses local names across block scopes with different types
     * (`ref` can be float64 in one block and uint32 in the next). The maps
     * were seeded with each name's FIRST declaration; from here on, this
     * declaration is the truth for this name. */
    const char *name = in->dest.name;
    if (name) {
      const MtlcType *t = declared_type(prog, in);
      int is_scalar_float =
          t ? mtlc_type_is_float(t) : type_text_is_float_scalar(in->text);
      int fbits = t ? (t->kind == MTLC_TYPE_FLOAT32 ? 32 : 64)
                    : type_text_float_bits(in->text);
      set_rebind(fs, name, is_scalar_float, is_scalar_float ? fbits : 0);
      set_rebind(us, name,
                 type_text_is_unsigned(in->text) || type_is_unsigned(t), 0);
      narrow_remove(nm, name);
      if (t && !mtlc_type_is_float(t) && !type_is_aggregate(t)) {
        size_t size = mtlc_type_size(t);
        if (size == 1 || size == 2 || size == 4) {
          narrow_add(nm, name, (int)size, module_type_signed(t));
        }
      }
      agg_remove(ag, name);
      if (type_is_aggregate(t)) {
        agg_add(ag, name, declare_local_size(prog, in));
      }
    }
    break;
  }
  case IR_OP_LABEL:
    arm64_bind_label(e, label_for(e, labels, in->text));
    break;
  case IR_OP_JUMP:
    arm64_emit_b(e, label_for(e, labels, in->text));
    break;
  case IR_OP_BRANCH_ZERO:
    arm64_emit_cbz(e, 1, load_into(e, slots, &in->lhs, R_LHS),
                   label_for(e, labels, in->text));
    break;
  case IR_OP_BRANCH_EQ: {
    Arm64Reg a = load_into(e, slots, &in->lhs, R_LHS);
    Arm64Reg b = load_into(e, slots, &in->rhs, R_RHS);
    arm64_emit_word(e, arm64_cmp_reg(1, a, b));
    arm64_emit_bcond(e, ARM64_EQ, label_for(e, labels, in->text));
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_assign_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_ASSIGN: {
    /* An assignment whose destination is a WIDE aggregate local is a
     * whole-object copy: the local IS the object. The source's record
     * address comes from wherever it lives -- another aggregate local, an
     * aggregate global, a string literal's record, or (for a temp) a
     * pointer value such as an sret result. Everything else, including a
     * packed <=8-byte struct, is a one-word move. */
    int bytes = in->dest.kind == IR_OPERAND_SYMBOL
                    ? agg_size_of(ag, in->dest.name)
                    : 0;
    int source = in->lhs.kind == IR_OPERAND_SYMBOL
                     ? agg_size_of(ag, in->lhs.name)
                     : 0;
    if (bytes > 8) {
      if (source > 0) {
        emit_lea_local(e, R_LHS, slot_off(slots, in->lhs.name));
      } else {
        /* STRING literals load as a record address; a temp holds one. */
        load_into(e, slots, &in->lhs, R_LHS);
      }
      emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
      emit_fixed_copy(e, R_RHS, R_LHS,
                      source > 0 && source < bytes ? source : bytes);
      break;
    }
    if (bytes > 0 && source > 0) {
      emit_lea_local(e, R_LHS, slot_off(slots, in->lhs.name));
      emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
      emit_fixed_copy(e, R_RHS, R_LHS, bytes < source ? bytes : source);
      break;
    }
    if (in->lhs.kind == IR_OPERAND_FLOAT ||
        operand_is_float(fs, &in->lhs)) {
      /* A float assignment stores at the DESTINATION's width. The source may
       * have been computed at the other one (a global float32 updated with a
       * literal runs in double), and the low half of a double is not a
       * float. */
      int dst_bits = 0;
      const IRModuleSymbol *dst_global =
          in->dest.kind == IR_OPERAND_SYMBOL &&
                  slot_find(slots, in->dest.name) < 0
              ? module_variable(slots, in->dest.name)
              : NULL;
      if (dst_global && mtlc_type_is_float(dst_global->type)) {
        dst_bits = dst_global->type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
      } else if (in->dest.kind == IR_OPERAND_SYMBOL ||
                 in->dest.kind == IR_OPERAND_TEMP) {
        dst_bits = set_bits(fs, in->dest.name);
      }
      if (dst_bits) {
        int d = dst_bits != 32;
        load_float_operand(e, slots, fs, &in->lhs, d, 0, R_LHS);
        arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
        store_dest(e, slots, &in->dest, R_RES);
        break;
      }
    }
    store_dest(e, slots, &in->dest, load_into(e, slots, &in->lhs, R_LHS));
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_cast_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_CAST: {
    int srcf = in->is_float;
    int dstf = in->dest.float_bits != 0;
    if (srcf && !dstf) { /* float -> int (truncating) */
      int d = operand_float_bits(fs, &in->lhs) != 32;
      /* A uint64 target reaches past 2^63, where the signed truncation
       * saturates; fcvtzu covers the whole range. */
      int to_u64 = in->text && strcmp(in->text, "uint64") == 0;
      arm64_emit_word(e, arm64_fmov_gp(d, 0, load_into(e, slots, &in->lhs,
                                                       R_LHS)));
      arm64_emit_word(e, to_u64 ? arm64_fcvtzu(d, R_RES, 0)
                                : arm64_fcvtzs(d, R_RES, 0));
      store_dest(e, slots, &in->dest, R_RES);
    } else if (!srcf && dstf) { /* int -> float */
      int d = in->dest.float_bits != 32;
      /* is_unsigned says the source is an unsigned integer, so bit 63 is part
       * of the magnitude and not a sign. */
      Arm64Reg src = load_into(e, slots, &in->lhs, R_LHS);
      arm64_emit_word(e, in->is_unsigned ? arm64_ucvtf(d, 0, src)
                                         : arm64_scvtf(d, 0, src));
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
      store_dest(e, slots, &in->dest, R_RES);
    } else if (srcf && dstf && cast_small_float_target(in) != 0) {
      int sd = operand_float_bits(fs, &in->lhs) != 32;
      load_float_operand(e, slots, fs, &in->lhs, 0, 0, R_RHS);
      (void)sd;
      arm64_emit_word(e, arm64_fmov_to_gp(0, R_RHS, 0));
      if (cast_small_float_target(in) == 16) {
        emit_f16_narrow(e, R_RHS);
        emit_f16_widen(e, R_RHS);
      } else {
        emit_bf16_narrow(e, R_RHS, R_RES, R_AUX);
        emit_bf16_widen(e, R_RHS);
      }
      store_dest(e, slots, &in->dest, R_RHS);
    } else if (srcf && dstf) { /* float -> float */
      int sd = operand_float_bits(fs, &in->lhs) != 32;
      int dd = in->dest.float_bits != 32;
      arm64_emit_word(e, arm64_fmov_gp(sd, 0, load_into(e, slots, &in->lhs,
                                                        R_LHS)));
      if (sd != dd) {
        arm64_emit_word(e, arm64_fcvt(dd, 1, 0));
        arm64_emit_word(e, arm64_fmov_to_gp(dd, R_RES, 1));
      } else {
        arm64_emit_word(e, arm64_fmov_to_gp(sd, R_RES, 0));
      }
      store_dest(e, slots, &in->dest, R_RES);
    } else {
      /* int -> int: re-express the value as the TARGET integer type. A bit
       * copy was wrong for every narrowing cast used as a value rather than
       * stored into a typed local: `(int8)511` answered 511, because only
       * the store to a declared-narrow slot ever truncated. The value in the
       * register is already canonical for its own type, so extending its low
       * `tw` bytes by the target's signedness is the whole conversion. */
      Arm64Reg src = load_into(e, slots, &in->lhs, R_LHS);
      const MtlcType *tt =
          prog && in->text && !strchr(in->text, '[') && !strchr(in->text, '*')
              ? ir_program_lookup_type(prog, in->text)
              : NULL;
      int tw = tt ? (int)mtlc_type_size(tt) : type_elem_size(in->text);
      int tu = tt ? type_is_unsigned(tt) : type_text_is_unsigned(in->text);
      if (tt && type_is_aggregate(tt)) {
        tw = 8;
      }
      if (in->text && strchr(in->text, '*')) {
        tw = 8;
      }
      if (tw == 1 || tw == 2 || tw == 4) {
        arm64_emit_word(
            e, tw == 1 ? (tu ? arm64_uxtb(R_RES, src) : arm64_sxtb(R_RES, src))
               : tw == 2
                   ? (tu ? arm64_uxth(R_RES, src) : arm64_sxth(R_RES, src))
                   : (tu ? arm64_mov_reg(0, R_RES, src)
                         : arm64_sxtw(R_RES, src)));
        store_dest(e, slots, &in->dest, R_RES);
      } else {
        store_dest(e, slots, &in->dest, src);
      }
    }
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_memory_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_ADDRESS_OF: {
    const IRModuleSymbol *global =
        slot_find(slots, in->lhs.name) < 0
            ? module_variable(slots, in->lhs.name)
            : NULL;
    if (global) {
      emit_global_address(e, slots, R_RES, global);
    } else if (slot_find(slots, in->lhs.name) < 0 &&
               prog_fn_index(prog, in->lhs.name) >= 0) {
      emit_function_address(e, slots, R_RES, in->lhs.name);
    } else if (set_has(wide_params, in->lhs.name)) {
      emit_slot_ldr(e, R_RES, slot_off(slots, in->lhs.name));
    } else {
      emit_lea_local(e, R_RES, slot_off(slots, in->lhs.name));
    }
    store_dest(e, slots, &in->dest, R_RES);
    break;
  }
  case IR_OP_LOAD: {
    int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
    Arm64Reg addr = address_operand(e, slots, ag, &in->lhs, R_LHS);
    if (size > 8) {
      /* A wide load produces an aggregate VALUE, which travels as its
       * address. Into an aggregate local: copy the bytes. Into a temp: the
       * loaded address IS the value. */
      int dest_agg = in->dest.kind == IR_OPERAND_SYMBOL
                         ? agg_size_of(ag, in->dest.name)
                         : 0;
      if (dest_agg > 8) {
        emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
        emit_fixed_copy(e, R_RHS, addr, size < dest_agg ? size : dest_agg);
      } else {
        store_dest(e, slots, &in->dest, addr);
      }
      break;
    }
    emit_load_sized(e, R_RES, addr, size,
                    !in->is_unsigned && !in->is_float);
    if (size == 2 && in->is_float && small_float_class(in) == 16) {
      emit_f16_widen(e, R_RES);
    } else if (size == 2 && in->is_float && small_float_class(in) == 8) {
      emit_bf16_widen(e, R_RES);
    }
    store_dest(e, slots, &in->dest, R_RES);
    break;
  }
  case IR_OP_STORE: {
    int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
    Arm64Reg addr = address_operand(e, slots, ag, &in->dest, R_LHS);
    if (size > 8) {
      /* Wide store: copy the source record's bytes through the pointer. */
      Arm64Reg src;
      if (in->lhs.kind == IR_OPERAND_SYMBOL &&
          agg_size_of(ag, in->lhs.name) > 0) {
        emit_lea_local(e, R_RHS, slot_off(slots, in->lhs.name));
        src = R_RHS;
      } else {
        src = load_into(e, slots, &in->lhs, R_RHS); /* record address */
      }
      emit_fixed_copy(e, addr, src, size);
      break;
    }
    if (size == 2 && in->is_float && small_float_class(in) != 0) {
      load_float_operand(e, slots, fs, &in->lhs, 0, 0, R_RHS);
      arm64_emit_word(e, arm64_fmov_to_gp(0, R_RHS, 0));
      if (small_float_class(in) == 16) {
        emit_f16_narrow(e, R_RHS);
      } else {
        emit_bf16_narrow(e, R_RHS, R_RES, R_AUX);
      }
      emit_store_sized(e, R_RHS, addr, 2);
      break;
    }
    if ((size == 4 || size == 8) && operand_is_float(fs, &in->lhs)) {
      /* A float store's size names the STORAGE width; convert the value to
       * it first, or a double's low half (or a single's bits zero-padded)
       * lands in memory. */
      int d = size == 8;
      load_float_operand(e, slots, fs, &in->lhs, d, 0, R_RHS);
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RHS, 0));
      emit_store_sized(e, R_RHS, addr, size);
      break;
    }
    Arm64Reg val = load_into(e, slots, &in->lhs, R_RHS);
    emit_store_sized(e, val, addr, size);
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_arithmetic_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_BINARY:
    if (binary_is_string_concat(in)) {
      /* result = a + b over { chars, length } records. One block holds the
       * new record and its characters (record first, chars at +16); bump
       * memory is fresh-zero, so the NUL after the copied bytes is free. */
      if (!object && !fns) {
        arm64_fail(e, "string concatenation in '%s' needs the runtime "
                      "kernel, which this path does not provide",
                   fn->name);
        break;
      }
      int off_a = slot_off(slots, CONCAT_A_SLOT);
      int off_b = slot_off(slots, CONCAT_B_SLOT);
      int off_dst = slot_off(slots, CONCAT_DST_SLOT);
      /* Record addresses of both operands, parked across the calls. */
      if (in->lhs.kind == IR_OPERAND_SYMBOL &&
          agg_size_of(ag, in->lhs.name) > 0) {
        emit_lea_local(e, R_LHS, slot_off(slots, in->lhs.name));
      } else {
        load_into(e, slots, &in->lhs, R_LHS);
      }
      emit_slot_str(e, R_LHS, off_a);
      if (in->rhs.kind == IR_OPERAND_SYMBOL &&
          agg_size_of(ag, in->rhs.name) > 0) {
        emit_lea_local(e, R_LHS, slot_off(slots, in->rhs.name));
      } else {
        load_into(e, slots, &in->rhs, R_LHS);
      }
      emit_slot_str(e, R_LHS, off_b);
      /* malloc(16 + len(a) + len(b) + 1) */
      emit_slot_ldr(e, R_LHS, off_a);
      arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, R_LHS, 8));
      emit_slot_ldr(e, R_RHS, off_b);
      arm64_emit_word(e, arm64_ldr_imm(1, R_RHS, R_RHS, 8));
      arm64_emit_word(e, arm64_add_reg(1, ARM64_X0, R_LHS, R_RHS));
      arm64_emit_word(e, arm64_add_imm(1, ARM64_X0, ARM64_X0, 17, 0));
      emit_runtime_call(e, object, prog, fns, STUB_MALLOC);
      emit_slot_str(e, ARM64_X0, off_dst);
      /* record: chars = block+16; length = len(a) + len(b) */
      arm64_emit_word(e, arm64_add_imm(1, R_LHS, ARM64_X0, 16, 0));
      arm64_emit_word(e, arm64_str_imm(1, R_LHS, ARM64_X0, 0));
      emit_slot_ldr(e, R_LHS, off_a);
      arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, R_LHS, 8));
      emit_slot_ldr(e, R_RHS, off_b);
      arm64_emit_word(e, arm64_ldr_imm(1, R_RHS, R_RHS, 8));
      arm64_emit_word(e, arm64_add_reg(1, R_LHS, R_LHS, R_RHS));
      arm64_emit_word(e, arm64_str_imm(1, R_LHS, ARM64_X0, 8));
      /* memcpy(block+16, a.chars, a.len) */
      emit_slot_ldr(e, ARM64_X0, off_dst);
      arm64_emit_word(e, arm64_add_imm(1, ARM64_X0, ARM64_X0, 16, 0));
      emit_slot_ldr(e, R_LHS, off_a);
      arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X1, R_LHS, 0));
      arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X2, R_LHS, 8));
      emit_runtime_call(e, object, prog, fns, STUB_MEMCPY);
      /* memcpy(block+16+a.len, b.chars, b.len) */
      emit_slot_ldr(e, ARM64_X0, off_dst);
      arm64_emit_word(e, arm64_add_imm(1, ARM64_X0, ARM64_X0, 16, 0));
      emit_slot_ldr(e, R_LHS, off_a);
      arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, R_LHS, 8));
      arm64_emit_word(e, arm64_add_reg(1, ARM64_X0, ARM64_X0, R_LHS));
      emit_slot_ldr(e, R_LHS, off_b);
      arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X1, R_LHS, 0));
      arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X2, R_LHS, 8));
      emit_runtime_call(e, object, prog, fns, STUB_MEMCPY);
      /* Terminate. The freestanding allocator hands back fresh-zero memory,
       * so this is redundant there, but the general allocator need not. */
      emit_slot_ldr(e, ARM64_X0, off_dst);
      arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, ARM64_X0, 8));
      arm64_emit_word(e, arm64_add_imm(1, ARM64_X0, ARM64_X0, 16, 0));
      arm64_emit_word(e, arm64_add_reg(1, ARM64_X0, ARM64_X0, R_LHS));
      arm64_emit_word(e, arm64_strb_imm(ARM64_XZR, ARM64_X0, 0));
      /* The block's start IS the result record. */
      emit_slot_ldr(e, R_RES, off_dst);
      if (in->dest.kind == IR_OPERAND_SYMBOL &&
          agg_size_of(ag, in->dest.name) > 8) {
        emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
        emit_fixed_copy(e, R_RHS, R_RES, 16);
      } else {
        store_dest(e, slots, &in->dest, R_RES);
      }
      break;
    }
    lower_binary(e, slots, fs, us, in);
    break;
  case IR_OP_UNARY:
    lower_unary(e, slots, fs, in);
    break;
  default:
    return 0;
  }
  return 1;
}

static int encode_call_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_CALL: {
    if (!fns || !in->text) {
      arm64_fail(e, in->text
                        ? "'%s' calls '%s', but this path lowers one function"
                          " in isolation"
                        : "'%s' contains a call with no callee name%s",
                 fn->name, in->text ? in->text : "");
      break;
    }
    /* cstr("literal"): embed the bytes in the loaded segment (branched over),
     * and materialize their virtual address into dest -- no actual call. */
    if (strcmp(in->text, "cstr") == 0 && in->argument_count >= 1 &&
        in->arguments[0].kind == IR_OPERAND_STRING &&
        in->arguments[0].name) {
      const char *str = in->arguments[0].name;
      if (object) {
        char symbol[64];
        size_t offset = 0;
        snprintf(symbol, sizeof(symbol), ".Lmtlc.str.%u",
                 object->string_id++);
        if (!binary_emitter_append_bytes(object->emitter,
                                         object->rodata_section, str,
                                         strlen(str) + 1, &offset) ||
            !binary_emitter_define_symbol(
                object->emitter, symbol, BINARY_SYMBOL_LOCAL,
                object->rodata_section, offset, strlen(str) + 1)) {
          arm64_fail(e, "could not place a cstr literal in .rodata");
          break;
        }
        emit_symbol_address(e, object, R_RES, symbol);
      } else {
        int past = arm64_new_label(e);
        arm64_emit_b(e, past);
        size_t soff = arm64_here(e);
        arm64_emit_bytes(e, str, strlen(str) + 1);
        arm64_bind_label(e, past);
        emit_imm(e, R_RES, (uint64_t)ELF_BASE + ELF_HDRS + soff);
      }
      if (in->dest.kind == IR_OPERAND_TEMP ||
          in->dest.kind == IR_OPERAND_SYMBOL) {
        store_dest(e, slots, &in->dest, R_RES);
      }
      break;
    }
    /* Runtime traps in a relocatable object lower to owned puts and exit.
     * The compact self contained image exits through its local syscall. */
    if (strcmp(in->text, "mettle_crash_trap_ex") == 0 ||
        strcmp(in->text, "mettle_crash_trap") == 0) {
      size_t msg_index =
          strcmp(in->text, "mettle_crash_trap_ex") == 0 ? 1u : 0u;
      /* x0 = the message as a CSTRING. A literal goes via the chars-address
       * emitter, not load_into, whose STRING case yields a record address. */
      if (in->argument_count > msg_index &&
          in->arguments[msg_index].kind == IR_OPERAND_STRING) {
        emit_string_literal_address(e, object,
                                    in->arguments[msg_index].name, ARM64_X0);
      } else if (in->argument_count > msg_index) {
        load_into(e, slots, &in->arguments[msg_index], ARM64_X0);
      } else {
        emit_string_literal_address(e, object, "Fatal error", ARM64_X0);
      }
      if (object) {
        if (!emit_external_call(e, object, prog, "puts")) break;
        emit_imm(e, ARM64_X0, 1);
        if (!emit_external_call(e, object, prog, "exit")) break;
      } else {
        /* Match the x86 observable: the message on stdout, then exit(1).
         * The puts stub is emitted whenever a trap is reachable. */
        arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[STUB_PUTS].name));
        emit_imm(e, ARM64_X0, 1);
        arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_EXIT, 0));
        arm64_emit_word(e, ARM64_SVC0);
      }
      break;
    }
    if (strcmp(in->text, IR_SYSCALL_CALL_NAME) == 0) {
      size_t syscall_arguments =
          in->argument_count > 0 ? in->argument_count - 1 : 0;
      if (in->argument_count == 0 ||
          syscall_arguments > MTLC_SYSCALL_MAX_ARGUMENTS_SVC) {
        arm64_fail(e,
                   "'%s' makes a system call with %llu arguments; this target "
                   "passes at most %d",
                   fn->name, (unsigned long long)syscall_arguments,
                   MTLC_SYSCALL_MAX_ARGUMENTS_SVC);
        break;
      }
      for (size_t k = 0; k < syscall_arguments; k++) {
        load_into(e, slots, &in->arguments[k + 1], (Arm64Reg)(ARM64_X0 + k));
      }
      load_into(e, slots, &in->arguments[0], ARM64_X8);
      arm64_emit_word(e, ARM64_SVC0);
      if (in->dest.kind == IR_OPERAND_TEMP ||
          in->dest.kind == IR_OPERAND_SYMBOL) {
        store_dest(e, slots, &in->dest, ARM64_X0);
      }
      break;
    }

    if (!emit_call_arguments(e, slots, prog, fs, in)) break;

    int callee_sret = fn_aggregate_return_size(prog, in->text);
    if (callee_sret) {
      emit_lea_local(e, ARM64_X8, slot_off(slots, SRET_BUFFER_SLOT));
    }
    int stub = object ? -1 : runtime_stub_index(in->text);
    if (prog_fn_index(prog, in->text) >= 0) {
      arm64_emit_bl(e, label_for(e, fns, in->text));
    } else if (object) {
      if (!emit_external_call(e, object, prog, in->text)) {
        break;
      }
    } else if (stub >= 0) {
      /* Resolved to a stub this backend writes itself. Branch to the
       * canonical name so aliases (mettle_heap_zeroed) share one body. */
      arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[stub].name));
    } else {
      arm64_fail(e,
                 "'%s' calls '%s', which is neither in this program, a "
                 "runtime stub, nor an external symbol (the self-contained "
                 "ELF has no linker)",
                 fn->name, in->text);
      break;
    }
    if (in->dest.kind == IR_OPERAND_TEMP ||
        in->dest.kind == IR_OPERAND_SYMBOL) {
      int dest_agg = in->dest.kind == IR_OPERAND_SYMBOL
                         ? agg_size_of(ag, in->dest.name)
                         : 0;
      int resf = call_returns_float(prog, in, retf) ||
                 set_has(fs, in->dest.name);
      if (callee_sret && dest_agg > 8) {
        /* Copy the returned record straight from the sret buffer. */
        emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
        arm64_emit_mov(e, 1, R_LHS, ARM64_X0);
        emit_fixed_copy(e, R_RHS, R_LHS,
                        callee_sret < dest_agg ? callee_sret : dest_agg);
      } else if (resf) { /* float result arrives in d0/s0 */
        int d = call_return_float_bits(prog, in) != 32;
        arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
        store_dest(e, slots, &in->dest, R_RES);
      } else {
        store_dest(e, slots, &in->dest, ARM64_X0);
      }
    }
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_runtime_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_CALL_INDIRECT: {
    /* Call through a function pointer in `lhs`. Arguments go first: loading a
     * global operand borrows x16, so the callee address must land there only
     * once every argument is already in place. x16 is the AAPCS64 IP0 scratch,
     * which is exactly what an indirect branch target may use. */
    if (!emit_call_arguments(e, slots, prog, fs, in)) break;
    load_into(e, slots, &in->lhs, ARM64_X16);
    arm64_emit_word(e, arm64_blr(ARM64_X16));
    if (in->dest.kind == IR_OPERAND_TEMP ||
        in->dest.kind == IR_OPERAND_SYMBOL) {
      if (call_returns_float(prog, in, retf) || set_has(fs, in->dest.name)) {
        int d = call_return_float_bits(prog, in) != 32;
        arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
        store_dest(e, slots, &in->dest, R_RES);
      } else {
        store_dest(e, slots, &in->dest, ARM64_X0);
      }
    }
    break;
  }
  case IR_OP_NEW: {
    /* Heap allocation of `rhs` bytes, zeroed. The object path calls owned
     * calloc(1, size); the self contained path branches to the bump
     * allocator, whose memory is already zero. */
    Arm64Reg size_reg = object ? ARM64_X1 : ARM64_X0;
    if (in->rhs.kind == IR_OPERAND_NONE ||
        (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value <= 0)) {
      emit_imm(e, size_reg, 8);
    } else if (in->rhs.kind == IR_OPERAND_INT) {
      emit_imm(e, size_reg, (uint64_t)in->rhs.int_value);
    } else {
      load_into(e, slots, &in->rhs, size_reg);
    }
    if (object) {
      emit_imm(e, ARM64_X0, 1);
      if (!emit_external_call(e, object, prog, "calloc")) break;
    } else if (fns) {
      arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[STUB_MALLOC].name));
    } else {
      arm64_fail(e, "'%s' allocates, but this path lowers one function in "
                    "isolation", fn->name);
      break;
    }
    if (in->dest.kind == IR_OPERAND_TEMP ||
        in->dest.kind == IR_OPERAND_SYMBOL) {
      store_dest(e, slots, &in->dest, ARM64_X0);
    }
    break;
  }
  case IR_OP_RETURN:
    if (sret_size && in->lhs.kind != IR_OPERAND_NONE) {
      /* Copy the returned record into the caller's buffer and hand the
       * buffer back in x0. */
      if (in->lhs.kind == IR_OPERAND_SYMBOL &&
          agg_size_of(ag, in->lhs.name) > 0) {
        emit_lea_local(e, R_LHS, slot_off(slots, in->lhs.name));
      } else {
        load_into(e, slots, &in->lhs, R_LHS); /* record address */
      }
      emit_slot_ldr(e, R_RHS, slot_off(slots, SRET_SAVE_SLOT));
      emit_fixed_copy(e, R_RHS, R_LHS, sret_size);
      arm64_emit_mov(e, 1, ARM64_X0, R_RHS);
      arm64_emit_epilogue(e, frame, NULL, 0);
      break;
    }
    if (in->lhs.kind != IR_OPERAND_NONE) {
      if (operand_is_float(fs, &in->lhs)) { /* float result goes in d0/s0 */
        const IRModuleSymbol *current = module_function(prog, fn->name);
        int d = current && current->return_type
                    ? current->return_type->kind != MTLC_TYPE_FLOAT32
                    : operand_float_bits(fs, &in->lhs) != 32;
        load_float_operand(e, slots, fs, &in->lhs, d, 0, R_LHS);
      } else {
        arm64_emit_mov(e, 1, ARM64_X0, load_into(e, slots, &in->lhs, R_LHS));
      }
    }
    arm64_emit_epilogue(e, frame, NULL, 0);
    break;
  default:
    return 0;
  }
  return 1;
}

static void encode_instruction(const Arm64Scope *scope,
                               const IRInstruction *in) {
  if (encode_control_instruction(scope, in) ||
      encode_assign_instruction(scope, in) ||
      encode_cast_instruction(scope, in) ||
      encode_memory_instruction(scope, in) ||
      encode_arithmetic_instruction(scope, in) ||
      encode_call_instruction(scope, in) ||
      encode_runtime_instruction(scope, in)) {
    return;
  }
  arm64_fail(scope->e, "'%s' uses %s, which has no AArch64 lowering",
             scope->fn->name, ir_opcode_name(in->op));
}

static int encode_function(Arm64Emit *e, const IRFunction *fn, LblMap *fns,
                           const IRProgram *prog, const int *retf,
                           Arm64ObjectContext *object, Arm64Data *data) {
  SlotMap slots = {0};
  LblMap labels = {0};
  StrSet fs = {0};
  StrSet us = {0};
  AggMap ag = {0};
  NarrowMap nm = {0};
  build_float_set(fn, prog, retf, &fs);
  build_unsigned_set(fn, &us);
  build_aggregate_set(fn, prog, &ag);
  build_narrow_map(fn, prog, &nm);
  slots.object = object;
  slots.program = prog;
  slots.data = data;
  slots.fns = fns;
  slots.aggregates = &ag;
  slots.narrow = &nm;
  slots.frame = max_outgoing_stack(e, fn, prog, &fs);

  /* Slot allocation order: parameters first (so x0../v0.. home to known
   * offsets), then declared locals at their real sizes (arrays!), then any
   * remaining temps/symbols at 8 bytes. */
  for (size_t i = 0; i < fn->parameter_count; i++) {
    slot_alloc(&slots, fn->parameter_names[i], 8);
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      slot_alloc(&slots, in->dest.name, declare_local_size(prog, in));
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    const IROperand *ops[3] = {&in->dest, &in->lhs, &in->rhs};
    for (int k = 0; k < 3; k++) {
      if (operand_needs_slot(&slots, prog, ops[k])) {
        slot_off(&slots, ops[k]->name);
      }
    }
    for (size_t k = 0; k < in->argument_count; k++) {
      if (operand_needs_slot(&slots, prog, &in->arguments[k])) {
        slot_off(&slots, in->arguments[k].name);
      }
    }
  }

  /* Parameters that are WIDE aggregates arrive as a pointer to the caller's
   * object (the by-reference convention). The IR still writes `&@param` for
   * field access as if the object lived in this frame, so ADDRESS_OF on one of
   * these names must produce the POINTER IN the slot, not the slot's address. */
  StrSet wide_params = {0};
  {
    const IRModuleSymbol *self = module_function(prog, fn->name);
    for (size_t i = 0; self && i < fn->parameter_count; i++) {
      if (i < self->param_count && self->param_types &&
          type_is_aggregate(self->param_types[i]) &&
          mtlc_type_size(self->param_types[i]) > 8) {
        set_add(&wide_params, fn->parameter_names[i]);
      }
    }
  }

  int sret_size = fns ? fn_aggregate_return_size(prog, fn->name) : 0;
  if (sret_size) {
    slot_alloc(&slots, SRET_SAVE_SLOT, 8);
  }
  /* One buffer, sized for the widest record any call in this function returns:
   * the callee writes its result through x8 into it. */
  int sret_buffer = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_CALL) {
      int ret = fn_aggregate_return_size(prog, in->text);
      if (ret > sret_buffer) {
        sret_buffer = ret;
      }
    }
  }
  if (sret_buffer) {
    slot_alloc(&slots, SRET_BUFFER_SLOT, sret_buffer);
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    if (binary_is_string_concat(&fn->instructions[i])) {
      slot_alloc(&slots, CONCAT_A_SLOT, 8);
      slot_alloc(&slots, CONCAT_B_SLOT, 8);
      slot_alloc(&slots, CONCAT_DST_SLOT, 8);
      break;
    }
  }

  int frame = (slots.frame + 15) & ~15;
  if (e->error || !arm64_emit_prologue(e, frame, NULL, 0)) {
    goto done;
  }
  if (sret_size) {
    /* x8 carries the caller's result buffer; park it, calls clobber it. */
    emit_slot_str(e, ARM64_X8, slot_off(&slots, SRET_SAVE_SLOT));
  }
  /* Home incoming parameters using the same AAPCS64 classifier as calls.
   * Stack arguments begin at caller SP, which is [x29,#16] after our saved
   * FP/LR pair and remains stable regardless of the local frame size. */
  if (fn->parameter_count > 0) {
    int count = (int)fn->parameter_count;
    int *is_float = malloc((size_t)count * sizeof(*is_float));
    Arm64ArgLocation *locations =
        malloc((size_t)count * sizeof(*locations));
    if (!is_float || !locations) {
      free(is_float);
      free(locations);
      arm64_fail(e, "out of memory homing the parameters of '%s'", fn->name);
      goto done;
    }
    for (int i = 0; i < count; i++) {
      const char *type = fn->parameter_types ? fn->parameter_types[i] : NULL;
      is_float[i] = type_text_is_float_scalar(type);
    }
    if (!arm64_compute_arg_layout(is_float, count, locations, NULL)) {
      arm64_fail(e, "cannot lay out the %d parameters of '%s' under AAPCS64",
                 count, fn->name);
    }
    for (int i = 0; i < count && !e->error; i++) {
      int off = slot_off(&slots, fn->parameter_names[i]);
      Arm64ArgLocation location = locations[i];
      if (location.kind == ARM64_ARG_IN_VEC_REGISTER) {
        const char *type =
            fn->parameter_types ? fn->parameter_types[i] : NULL;
        int is_double = type_text_float_bits(type) == 64;
        emit_slot_str_fp(e, is_double, (int)location.reg, off);
      } else if (location.kind == ARM64_ARG_IN_GP_REGISTER) {
        emit_slot_str(e, location.reg, off);
      } else {
        arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, ARM64_X29,
                                         16 + location.stack_offset));
        emit_slot_str(e, R_LHS, off);
      }
    }
    free(is_float);
    free(locations);
    if (e->error) goto done;
  }

  {
    Arm64Scope scope;
    scope.e = e;
    scope.fn = fn;
    scope.fns = fns;
    scope.prog = prog;
    scope.retf = retf;
    scope.object = object;
    scope.slots = &slots;
    scope.labels = &labels;
    scope.fs = &fs;
    scope.us = &us;
    scope.ag = &ag;
    scope.nm = &nm;
    scope.wide_params = &wide_params;
    scope.frame = frame;
    scope.sret_size = sret_size;
    for (size_t i = 0; i < fn->instruction_count && !e->error; i++) {
      encode_instruction(&scope, &fn->instructions[i]);
    }
  }

done:
  free(slots.names);
  free(slots.offs);
  free(labels.names);
  free(labels.ids);
  free(fs.names);
  free(fs.bits);
  free(us.names);
  free(ag.names);
  free(ag.sizes);
  free(nm.names);
  free(nm.sizes);
  free(nm.is_signed);
  free(wide_params.names);
  free(wide_params.bits);
  return e->error ? 0 : 1;
}

int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn) {
  return encode_function(e, fn, NULL, NULL, NULL, NULL, NULL);
}

/* I/O intrinsics we provide as hand-written AArch64 stubs (a direct write(2)
 * syscall) instead of compiling the std/io body, which bottoms out in
 * OS-specific externs/strings. `with_newline` distinguishes the println form.
 * Values print through "{expr}" interpolation, whose conversions are runtime
 * stubs of their own. cstr is handled inline, not via a stub. */
static int io_stub_intrinsic(const char *name, int *with_newline,
                             int *is_string) {
  if (!name) {
    return 0;
  }
  struct {
    const char *n;
    int nl, str;
  } table[] = {{"println", 1, 1}, {"print", 0, 1}};
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(name, table[i].n) == 0) {
      if (with_newline) *with_newline = table[i].nl;
      if (is_string) *is_string = table[i].str;
      return 1;
    }
  }
  return 0;
}

/* True for any std/io function the backend handles specially (a printer stub or
 * the inline cstr), so reachability treats it as a leaf. */
static int io_leaf(const char *name) {
  return io_stub_intrinsic(name, NULL, NULL) ||
         (name && strcmp(name, "cstr") == 0);
}

/* Emit a leaf that writes the NUL-terminated cstring in x0 to stdout (then a
 * newline if with_newline): strlen, then write(1, ptr, len). `zero_result`
 * returns 0 instead of whatever the write syscall left in x0, which is what
 * puts(3) promises. */
static void emit_str_print(Arm64Emit *e, int with_newline, int zero_result) {
  int l_scan = arm64_new_label(e);
  int l_write = arm64_new_label(e);
  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X9, ARM64_X0));   /* walker */
  arm64_emit_word(e, arm64_movz(1, ARM64_X10, 0, 0));         /* len */
  arm64_bind_label(e, l_scan);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_cbz(e, 0, ARM64_X11, l_write);                   /* NUL -> done */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_scan);
  arm64_bind_label(e, l_write);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X1, ARM64_X0));   /* buf = ptr */
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X2, ARM64_X10));  /* len */
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));          /* fd=stdout */
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
  arm64_emit_word(e, ARM64_SVC0);
  if (with_newline) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, 10, 0));      /* '\n' */
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_SP, 0));
    arm64_emit_word(e, arm64_mov_sp(ARM64_X1, ARM64_SP));     /* buf = sp */
    arm64_emit_word(e, arm64_movz(1, ARM64_X2, 1, 0));        /* len=1 */
    arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));
    arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
    arm64_emit_word(e, ARM64_SVC0);
  }
  if (zero_result) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  }
  arm64_emit_epilogue(e, 16, NULL, 0);
}

/* ---- compact owned runtime stubs --------------------------------------- *
 *
 * The self contained executable has no external runtime or linker, so malloc
 * or puts has nothing to resolve to. These hand-written leaves stand in for the
 * small set of runtime entry points that reachable Mettle code needs, built on
 * raw AArch64 Linux syscalls. They are emitted only when a program calls them.
 *
 * The allocator is a bump pointer over anonymous mmap: free() is a no-op and
 * memory is never reused, which is why malloc can also serve calloc and `new`
 * (a fresh mmap page is already zero). That is the right trade for a target
 * whose job is to run a program to completion and exit. */

/* x0 = malloc(x0): bump the cursor kept in the writable segment, mapping a
 * fresh arena when the current one cannot satisfy the request. */
static void emit_stub_malloc(Arm64Emit *e) {
  int l_grow = arm64_new_label(e);
  int l_fits = arm64_new_label(e);
  int l_oom = arm64_new_label(e);

  arm64_emit_prologue(e, 0, NULL, 0);
  /* x9 = (size + 15) & ~15 -- keep every block 16-byte aligned. */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X0, 15, 0));
  arm64_emit_word(e, arm64_movn(1, ARM64_X10, 15, 0)); /* ~15 */
  arm64_emit_word(e, arm64_and_reg(1, ARM64_X9, ARM64_X9, ARM64_X10));

  emit_imm(e, ARM64_X11, (uint64_t)ELF_DATA_VADDR);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X12, ARM64_X11, HEAP_CURSOR_OFF));
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X13, ARM64_X11, HEAP_END_OFF));
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X14, ARM64_X12, ARM64_X9));
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X14, ARM64_X13));
  arm64_emit_bcond(e, ARM64_LS, l_fits); /* unsigned <=: room in this arena */

  arm64_bind_label(e, l_grow);
  /* mmap(NULL, arena + request, PROT_READ|PROT_WRITE,
   *      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
  emit_imm(e, ARM64_X15, HEAP_ARENA_BYTES);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X15, ARM64_X15, ARM64_X9));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  arm64_emit_mov(e, 1, ARM64_X1, ARM64_X15);
  arm64_emit_word(e, arm64_movz(1, ARM64_X2, 3, 0));    /* RW */
  arm64_emit_word(e, arm64_movz(1, ARM64_X3, 0x22, 0)); /* PRIVATE|ANON */
  arm64_emit_word(e, arm64_movn(1, ARM64_X4, 0, 0));    /* fd = -1 */
  arm64_emit_word(e, arm64_movz(1, ARM64_X5, 0, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_MMAP, 0));
  arm64_emit_word(e, ARM64_SVC0);
  /* mmap reports failure as a small negative errno. Linux preserves x1..x30
   * across a syscall, so x9/x15 still hold the request and arena sizes. */
  arm64_emit_word(e, arm64_cmp_imm(1, ARM64_X0, 0, 0));
  arm64_emit_bcond(e, ARM64_LT, l_oom);
  arm64_emit_mov(e, 1, ARM64_X12, ARM64_X0);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X13, ARM64_X0, ARM64_X15));
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X14, ARM64_X12, ARM64_X9));
  emit_imm(e, ARM64_X11, (uint64_t)ELF_DATA_VADDR);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X11, HEAP_END_OFF));

  arm64_bind_label(e, l_fits);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X14, ARM64_X11, HEAP_CURSOR_OFF));
  arm64_emit_mov(e, 1, ARM64_X0, ARM64_X12);
  arm64_emit_epilogue(e, 0, NULL, 0);

  arm64_bind_label(e, l_oom);
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* x0 = calloc(x0 count, x1 size). Bump-allocated memory comes straight from a
 * fresh anonymous mapping, so it is already zero. */
static void emit_stub_calloc(Arm64Emit *e, int malloc_label) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_mul(1, ARM64_X0, ARM64_X0, ARM64_X1));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* free(x0): nothing to do -- the bump allocator never reuses a block. */
static void emit_stub_free(Arm64Emit *e) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* x0 = putchar(x0): write the low byte to stdout and hand it back. */
static void emit_stub_putchar(Arm64Emit *e) {
  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_strb_imm(ARM64_X0, ARM64_SP, 0));
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_mov(e, 1, ARM64_X1, ARM64_SP);
  arm64_emit_word(e, arm64_movz(1, ARM64_X2, 1, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0)); /* fd = stdout */
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
  arm64_emit_word(e, ARM64_SVC0);
  arm64_emit_mov(e, 1, ARM64_X0, ARM64_X9);
  arm64_emit_epilogue(e, 16, NULL, 0);
}

/* x0 = write(x0 fd, x1 buf, x2 count). */
static void emit_stub_write(Arm64Emit *e) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
  arm64_emit_word(e, ARM64_SVC0);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* x0 = strlen(x0). */
static void emit_stub_strlen(Arm64Emit *e) {
  int l_scan = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_word(e, arm64_movz(1, ARM64_X10, 0, 0));
  arm64_bind_label(e, l_scan);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_cbz(e, 0, ARM64_X11, l_done);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_scan);
  arm64_bind_label(e, l_done);
  arm64_emit_mov(e, 1, ARM64_X0, ARM64_X10);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* x0 = memcpy(x0 dst, x1 src, x2 n), byte at a time -- the encoder has no
 * register-offset addressing, so both ends walk forward. */
static void emit_stub_memcpy(Arm64Emit *e) {
  int l_loop = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_cbz(e, 1, ARM64_X2, l_done);
  arm64_bind_label(e, l_loop);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X1, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X1, ARM64_X1, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_loop);
  arm64_bind_label(e, l_done);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* x0 = memmove(x0 dst, x1 src, x2 n): copy backwards when the destination
 * overlaps above the source, forwards otherwise. */
static void emit_stub_memmove(Arm64Emit *e) {
  int l_fwd = arm64_new_label(e);
  int l_bloop = arm64_new_label(e);
  int l_floop = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_cbz(e, 1, ARM64_X2, l_done);
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X0, ARM64_X1));
  arm64_emit_bcond(e, ARM64_LS, l_fwd); /* dst <= src: forward is safe */
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X9, ARM64_X0, ARM64_X2));
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X10, ARM64_X1, ARM64_X2));
  arm64_bind_label(e, l_bloop);
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_bloop);
  arm64_emit_b(e, l_done);
  arm64_bind_label(e, l_fwd);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_bind_label(e, l_floop);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X1, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X1, ARM64_X1, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_floop);
  arm64_bind_label(e, l_done);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* x0 = memset(x0 dst, x1 byte, x2 n). */
static void emit_stub_memset(Arm64Emit *e) {
  int l_loop = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_cbz(e, 1, ARM64_X2, l_done);
  arm64_bind_label(e, l_loop);
  arm64_emit_word(e, arm64_strb_imm(ARM64_X1, ARM64_X9, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_loop);
  arm64_bind_label(e, l_done);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* x0 = mettle_string_from_int/uint(x0): a fresh heap string record of the
 * value's decimal digits, so "{n}" interpolation works on the self-contained
 * target. Block layout matches the concat kernel: the record at the block's
 * start, characters after it. 48 bytes holds record(16) + sign + 20 digits
 * with room to spare, and bump memory is fresh-zero, so the byte after the
 * last digit is already the cosmetic NUL. Digits go in back-to-front ending
 * at block+46, the same loop emit_int_print uses. */
static void emit_string_from_value(Arm64Emit *e, int malloc_label,
                                   int is_signed) {
  int l_pos = arm64_new_label(e);
  int l_loop = arm64_new_label(e);
  int l_sign = arm64_new_label(e);
  int l_done = arm64_new_label(e);

  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X0, ARM64_SP, 0));      /* value */
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 48, 0));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_SP, 0));      /* value */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X0, 46, 0)); /* ptr */
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 0, 0));               /* neg=0 */
  if (is_signed) {
    arm64_emit_word(e, arm64_cmp_imm(1, ARM64_X9, 0, 0));
    arm64_emit_bcond(e, ARM64_GE, l_pos);
    arm64_emit_word(e, arm64_movz(1, ARM64_X12, 1, 0));             /* neg=1 */
    arm64_emit_word(e, arm64_neg(1, ARM64_X9, ARM64_X9));
  }
  arm64_bind_label(e, l_pos);
  arm64_emit_cbnz(e, 1, ARM64_X9, l_loop);
  arm64_emit_word(e, arm64_movz(1, ARM64_X11, 48, 0));              /* '0' */
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_sign);
  arm64_bind_label(e, l_loop);
  arm64_emit_cbz(e, 1, ARM64_X9, l_sign);
  arm64_emit_word(e, arm64_movz(1, ARM64_X13, 10, 0));
  arm64_emit_word(e, arm64_udiv(1, ARM64_X14, ARM64_X9, ARM64_X13));
  arm64_emit_word(e, arm64_msub(1, ARM64_X15, ARM64_X14, ARM64_X13, ARM64_X9));
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X9, ARM64_X14));        /* /=10 */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X15, ARM64_X15, 48, 0));/* +'0' */
  arm64_emit_word(e, arm64_strb_imm(ARM64_X15, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_loop);
  arm64_bind_label(e, l_sign);
  arm64_emit_cbz(e, 1, ARM64_X12, l_done);
  arm64_emit_word(e, arm64_movz(1, ARM64_X11, 45, 0));              /* '-' */
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_bind_label(e, l_done);
  /* chars = ptr+1; length = (block+46) - ptr; the record is the block. */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X10, 1, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X0, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X11, ARM64_X0, 46, 0));
  arm64_emit_word(e, arm64_sub_reg(1, ARM64_X11, ARM64_X11, ARM64_X10));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X11, ARM64_X0, 8));
  arm64_emit_epilogue(e, 16, NULL, 0);
}

/* x0 = mettle_string_from_char(x0): the low byte as a one-character record. */
static void emit_string_from_char(Arm64Emit *e, int malloc_label) {
  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X0, ARM64_SP, 0));      /* value */
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 32, 0));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_SP, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X9, ARM64_X0, 16));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X0, 16, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X0, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 1, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X12, ARM64_X0, 8));
  arm64_emit_epilogue(e, 16, NULL, 0);
}

/* x0 = mettle_string_from_bool(x0): "true" or "false" as a heap record. */
/* x0 = mettle_string_eq(x0 = &a, x1 = &b). A string value travels as the
 * address of its { chars, length } record, so both arguments are pointers to
 * one. Answers 1 when the lengths match and every byte does. */
static void emit_string_eq(Arm64Emit *e) {
  int l_loop = arm64_new_label(e);
  int l_equal = arm64_new_label(e);
  int l_differ = arm64_new_label(e);

  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_X0, 8));
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X10, ARM64_X1, 8));
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X9, ARM64_X10));
  arm64_emit_bcond(e, ARM64_NE, l_differ);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X11, ARM64_X0, 0));
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X12, ARM64_X1, 0));
  arm64_bind_label(e, l_loop);
  arm64_emit_cbz(e, 1, ARM64_X9, l_equal);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X13, ARM64_X11, 0));
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X14, ARM64_X12, 0));
  arm64_emit_word(e, arm64_cmp_reg(0, ARM64_X13, ARM64_X14));
  arm64_emit_bcond(e, ARM64_NE, l_differ);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X11, ARM64_X11, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X12, ARM64_X12, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_b(e, l_loop);
  arm64_bind_label(e, l_equal);
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));
  arm64_emit_epilogue(e, 0, NULL, 0);
  arm64_bind_label(e, l_differ);
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_string_from_bool(Arm64Emit *e, int malloc_label) {
  int l_true = arm64_new_label(e);
  int l_store = arm64_new_label(e);
  static const char k_false[] = "false";
  static const char k_true[] = "true";

  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X0, ARM64_SP, 0));      /* value */
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 32, 0));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_SP, 0));
  arm64_emit_cbnz(e, 1, ARM64_X9, l_true);
  for (int i = 0; i < 5; i++) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, (uint16_t)k_false[i], 0));
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X0, 16 + i));
  }
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 5, 0));
  arm64_emit_b(e, l_store);
  arm64_bind_label(e, l_true);
  for (int i = 0; i < 4; i++) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, (uint16_t)k_true[i], 0));
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X0, 16 + i));
  }
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 4, 0));
  arm64_bind_label(e, l_store);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X0, 16, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X0, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X12, ARM64_X0, 8));
  arm64_emit_epilogue(e, 16, NULL, 0);
}

/* exit(x0) / abort(): leave through the exit syscall, which never returns. */
static void emit_stub_exit(Arm64Emit *e, int fixed_status) {
  if (fixed_status >= 0) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X0, (uint16_t)fixed_status, 0));
  }
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_EXIT, 0));
  arm64_emit_word(e, ARM64_SVC0);
}

static void emit_runtime_stub(Arm64Emit *e, int id, int malloc_label) {
  switch (id) {
  case STUB_MALLOC: emit_stub_malloc(e); break;
  case STUB_CALLOC: emit_stub_calloc(e, malloc_label); break;
  case STUB_FREE: emit_stub_free(e); break;
  case STUB_PUTS: emit_str_print(e, 1, 1); break;
  case STUB_PUTCHAR: emit_stub_putchar(e); break;
  case STUB_WRITE: emit_stub_write(e); break;
  case STUB_STRLEN: emit_stub_strlen(e); break;
  case STUB_MEMCPY: emit_stub_memcpy(e); break;
  case STUB_MEMMOVE: emit_stub_memmove(e); break;
  case STUB_MEMSET: emit_stub_memset(e); break;
  case STUB_EXIT: emit_stub_exit(e, -1); break;
  case STUB_ABORT: emit_stub_exit(e, 134); break;
  case STUB_STR_FROM_INT: emit_string_from_value(e, malloc_label, 1); break;
  case STUB_STR_FROM_UINT: emit_string_from_value(e, malloc_label, 0); break;
  case STUB_STR_FROM_BOOL: emit_string_from_bool(e, malloc_label); break;
  case STUB_STR_FROM_CHAR: emit_string_from_char(e, malloc_label); break;
  case STUB_STR_EQ: emit_string_eq(e); break;
  /* The self-contained image allocates by bumping, so releasing a string is
   * the same nothing that releasing anything else is here. The call still has
   * to resolve: lowering emits it wherever it can see a string end. */
  case STUB_STR_FREE: emit_stub_free(e); break;
  default: arm64_fail(e, "internal: no runtime stub %d", id); break;
  }
}

/* ---- writable data image for the self-contained executable --------------- */

static void data_free(Arm64Data *d) {
  free(d->bytes);
  free(d->names);
  free(d->offs);
  free(d->fixup_fns);
  free(d->fixup_offs);
  memset(d, 0, sizeof(*d));
}

/* Grow the image by `n` zero bytes at `align`, returning the new region's
 * offset, or (size_t)-1 on allocation failure. */
static size_t data_reserve(Arm64Data *d, size_t n, size_t align) {
  if (align < 1) align = 1;
  size_t off = (d->len + align - 1) & ~(align - 1);
  size_t need = off + n;
  if (need > d->cap) {
    size_t cap = d->cap ? d->cap : 256;
    while (cap < need) cap *= 2;
    unsigned char *grown = realloc(d->bytes, cap);
    if (!grown) return (size_t)-1;
    d->bytes = grown;
    d->cap = cap;
  }
  memset(d->bytes + d->len, 0, need - d->len);
  d->len = need;
  return off;
}

static int data_add_fn_fixup(Arm64Data *d, const char *fn, size_t off) {
  if (d->fixup_count == d->fixup_cap) {
    int cap = d->fixup_cap ? d->fixup_cap * 2 : 8;
    const char **f = realloc(d->fixup_fns, (size_t)cap * sizeof(*f));
    size_t *o = realloc(d->fixup_offs, (size_t)cap * sizeof(*o));
    if (f) d->fixup_fns = f;
    if (o) d->fixup_offs = o;
    if (!f || !o) return 0;
    d->fixup_cap = cap;
  }
  d->fixup_fns[d->fixup_count] = fn;
  d->fixup_offs[d->fixup_count] = off;
  d->fixup_count++;
  return 1;
}

static int data_name(Arm64Data *d, const char *name, size_t off) {
  if (d->count == d->name_cap) {
    int cap = d->name_cap ? d->name_cap * 2 : 16;
    const char **n = realloc(d->names, (size_t)cap * sizeof(*n));
    size_t *o = realloc(d->offs, (size_t)cap * sizeof(*o));
    if (n) d->names = n;
    if (o) d->offs = o;
    if (!n || !o) return 0;
    d->name_cap = cap;
  }
  d->names[d->count] = name;
  d->offs[d->count] = off;
  d->count++;
  return 1;
}

/* Lay out and initialize every module global. Two passes: reserve all the slots
 * first, so an initializer that points at another global (or at string
 * characters appended during the fill) can be resolved to its final absolute
 * address -- this path has no relocations, only constants. */
static int build_data_image(Arm64Data *d, const IRProgram *prog, Arm64Emit *e) {
  if (data_reserve(d, DATA_RESERVED, 8) == (size_t)-1) {
    arm64_fail(e, "out of memory building the data image");
    return 0;
  }
  for (size_t i = 0; i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *s = &prog->module_symbols[i];
    if (s->kind != IR_MODSYM_VARIABLE || s->is_extern) continue;
    if (!s->type) {
      arm64_fail(e, "global '%s' has no type", s->name);
      return 0;
    }
    if (s->has_unfoldable_initializer) {
      arm64_fail(e,
                 "global '%s' needs its initializer evaluated at startup, "
                 "which the self-contained AArch64 executable cannot do",
                 s->name);
      return 0;
    }
    /* A string value is a { chars, length } record, like everywhere else. */
    size_t size = s->type->kind == MTLC_TYPE_STRING ? 16
                                                    : mtlc_type_size(s->type);
    size_t align = s->type->alignment ? s->type->alignment : 8;
    if (size == 0) {
      arm64_fail(e, "global '%s' has zero size", s->name);
      return 0;
    }
    size_t off = data_reserve(d, size, align);
    if (off == (size_t)-1 ||
        !data_name(d, module_link_name(s), off) ||
        (module_link_name(s) != s->name && !data_name(d, s->name, off))) {
      arm64_fail(e, "out of memory reserving global '%s'", s->name);
      return 0;
    }
  }

  for (size_t i = 0; i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *s = &prog->module_symbols[i];
    if (s->kind != IR_MODSYM_VARIABLE || s->is_extern) continue;
    long base = data_offset_of(d, module_link_name(s));
    if (base < 0) continue;
    unsigned char *slot = d->bytes + base;

    if (s->type->kind == MTLC_TYPE_STRING) {
      if (!s->has_initializer || !s->init_string) continue;
      size_t length = s->init_string_length;
      size_t chars = data_reserve(d, length + 1, 1);
      if (chars == (size_t)-1) {
        arm64_fail(e, "out of memory storing the text of global '%s'", s->name);
        return 0;
      }
      memcpy(d->bytes + chars, s->init_string, length + 1);
      slot = d->bytes + base; /* data_reserve may have moved the buffer */
      uint64_t address = (uint64_t)ELF_DATA_VADDR + (uint64_t)chars;
      uint64_t encoded_length = (uint64_t)length;
      memcpy(slot, &address, 8);
      memcpy(slot + 8, &encoded_length, 8);
      continue;
    }

    if (s->type->kind == MTLC_TYPE_STRUCT || s->type->kind == MTLC_TYPE_ARRAY) {
      if (!s->init_bytes || s->init_bytes_size == 0) continue; /* zeroed */
      size_t size = mtlc_type_size(s->type);
      size_t n = s->init_bytes_size < size ? s->init_bytes_size : size;
      memcpy(slot, s->init_bytes, n);
      for (size_t r = 0; r < s->init_reloc_count; r++) {
        const IRInitReloc *reloc = &s->init_relocs[r];
        uint64_t address = 0;
        if (reloc->symbol) {
          long target = data_offset_of(d, reloc->symbol);
          if (target < 0) {
            arm64_fail(e,
                       "global '%s' points at '%s', which the self-contained "
                       "AArch64 executable cannot address",
                       s->name, reloc->symbol);
            return 0;
          }
          address = (uint64_t)ELF_DATA_VADDR + (uint64_t)target;
        } else {
          size_t length = reloc->string ? reloc->string_length : 0;
          size_t chars = data_reserve(d, length + 1, 1);
          if (chars == (size_t)-1) {
            arm64_fail(e, "out of memory storing a string in global '%s'",
                       s->name);
            return 0;
          }
          memcpy(d->bytes + chars, reloc->string ? reloc->string : "",
                 length + 1);
          address = (uint64_t)ELF_DATA_VADDR + (uint64_t)chars;
          if (reloc->string_wants_record) {
            size_t record = data_reserve(d, 16, 8);
            if (record == (size_t)-1) {
              arm64_fail(e, "out of memory storing a string record in '%s'",
                         s->name);
              return 0;
            }
            uint64_t encoded_length = (uint64_t)length;
            memcpy(d->bytes + record, &address, 8);
            memcpy(d->bytes + record + 8, &encoded_length, 8);
            address = (uint64_t)ELF_DATA_VADDR + (uint64_t)record;
          }
        }
        if (reloc->offset + 8 > size) continue;
        memcpy(d->bytes + base + reloc->offset, &address, 8);
      }
      continue;
    }

    if (s->init_symbol_ref ||
        (s->init_reloc_count > 0 && s->init_relocs[0].symbol)) {
      const char *target =
          s->init_symbol_ref ? s->init_symbol_ref : s->init_relocs[0].symbol;
      long toff = data_offset_of(d, target);
      if (toff >= 0) {
        uint64_t address = (uint64_t)ELF_DATA_VADDR + (uint64_t)toff;
        memcpy(slot, &address, 8);
      } else if (prog && ir_program_lookup_symbol(prog, target) &&
                 ir_program_lookup_symbol(prog, target)->kind ==
                     IR_MODSYM_FUNCTION) {
        if (!data_add_fn_fixup(d, target, (size_t)base)) {
          arm64_fail(e, "out of memory noting '%s' points at code", s->name);
          return 0;
        }
      } else {
        arm64_fail(e, "global '%s' points at '%s', which the self-contained "
                      "AArch64 executable cannot address",
                   s->name, target);
        return 0;
      }
      continue;
    }
    if (!s->has_initializer) continue;
    size_t size = mtlc_type_size(s->type);
    if (size == 0 || size > 8) {
      arm64_fail(e, "global '%s' has an unsupported scalar size %u", s->name,
                 (unsigned)size);
      return 0;
    }
    unsigned char bytes[8] = {0};
    if (s->init_is_float) {
      double value = 0.0;
      memcpy(&value, &s->init_bits, 8);
      if (s->type->kind == MTLC_TYPE_FLOAT32) {
        float narrowed = (float)value;
        memcpy(bytes, &narrowed, 4);
      } else {
        memcpy(bytes, &value, 8);
      }
    } else {
      uint64_t bits = (uint64_t)s->init_bits;
      memcpy(bytes, &bits, size);
    }
    memcpy(slot, bytes, size);
  }
  return 1;
}

/* Index of the function named `name`, or -1. */
static int find_fn(const IRProgram *prog, const char *name) {
  for (size_t i = 0; i < prog->function_count; i++) {
    if (strcmp(prog->functions[i]->name, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

int arm64_ir_encode_program(Arm64Emit *e, const IRProgram *prog,
                            const char *entry, unsigned char **data_out,
                            size_t *data_len_out) {
  LblMap fns = {0};
  Arm64Data data = {0};
  /* Where this code will be loaded, so a label offset can become a function's
   * run-time address (see arm64_emit_label_address). */
  e->code_vaddr = (uint64_t)ELF_BASE + ELF_HDRS;
  if (data_out) *data_out = NULL;
  if (data_len_out) *data_len_out = 0;
  if (!entry) {
    entry = "main";
  }
  size_t n = prog->function_count;
  char *reach = calloc(n ? n : 1, 1);
  int *queue = malloc((n ? n : 1) * sizeof(int));
  int *retf = calloc(n ? n : 1, sizeof(int));
  if (!reach || !queue || !retf) {
    free(reach);
    free(queue);
    free(retf);
    arm64_fail(e, "out of memory walking the call graph from '%s'", entry);
    return 0;
  }
  if (!build_data_image(&data, prog, e)) {
    free(reach);
    free(queue);
    free(retf);
    data_free(&data);
    return 0;
  }

  /* Which functions return a float (fixpoint, since a function can return the
   * result of another float-returning call). Drives the v-register ABI. */
  for (size_t iter = 0; iter <= n; iter++) {
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
      if (!retf[i] && fn_returns_float(prog->functions[i], prog, retf)) {
        retf[i] = 1;
        changed = 1;
      }
    }
    if (!changed) break;
  }

  /* Reachability from `entry` over the call graph, treating I/O intrinsics as
   * leaves (their bodies are replaced by stubs, so the std/io internals they
   * would call are not pulled in). Only reachable functions are emitted. */
  int qh = 0, qt = 0;
  int start = find_fn(prog, entry);
  if (start < 0) {
    free(reach);
    free(queue);
    free(retf);
    free(fns.names);
    free(fns.ids);
    data_free(&data);
    arm64_fail(e, "no entry function '%s' in this program", entry);
    return 0;
  }
  /* Mark on enqueue, not on dequeue, so `queue` cannot hold more than one entry
   * per function -- it is sized for exactly that. */
  reach[start] = 1;
  queue[qt++] = start;
  while (qh < qt) {
    int fi = queue[qh++];
    const IRFunction *f = prog->functions[fi];
    if (io_leaf(f->name)) {
      continue; /* leaf: do not follow into the stdlib body */
    }
    for (size_t k = 0; k < f->instruction_count; k++) {
      const IRInstruction *in = &f->instructions[k];
      /* A callee by name, plus any function named as a VALUE: nothing calls a
       * function pointer's target by name, but its address is taken. */
      const char *referenced[5] = {
          in->op == IR_OP_CALL ? in->text : NULL,
          in->dest.kind == IR_OPERAND_SYMBOL ? in->dest.name : NULL,
          in->lhs.kind == IR_OPERAND_SYMBOL ? in->lhs.name : NULL,
          in->rhs.kind == IR_OPERAND_SYMBOL ? in->rhs.name : NULL, NULL};
      for (int o = 0; o < 4; o++) {
        if (!referenced[o]) continue;
        int ci = find_fn(prog, referenced[o]);
        if (ci >= 0 && !reach[ci]) {
          reach[ci] = 1;
          queue[qt++] = ci;
        }
      }
      for (size_t a = 0; a < in->argument_count; a++) {
        if (in->arguments[a].kind != IR_OPERAND_SYMBOL) continue;
        int ci = find_fn(prog, in->arguments[a].name);
        if (ci >= 0 && !reach[ci]) {
          reach[ci] = 1;
          queue[qt++] = ci;
        }
      }
    }
  }

  /* Which compact runtime stubs the reachable code needs. Computed from the same test
   * the call sites use, so every branch a body emits finds a bound label. */
  int stub_used[STUB_COUNT] = {0};
  /* cstr("literal") is folded at the call site and needs no body. cstr(value)
   * cannot be, so it becomes a real bl and its body must be emitted. */
  int cstr_needs_body = 0;
  for (size_t i = 0; i < n; i++) {
    if (!reach[i]) continue;
    const IRFunction *f = prog->functions[i];
    if (io_leaf(f->name)) continue;
    for (size_t k = 0; k < f->instruction_count; k++) {
      const IRInstruction *in = &f->instructions[k];
      if (in->op == IR_OP_NEW) {
        stub_used[STUB_MALLOC] = 1;
      } else if (binary_is_string_concat(in)) {
        stub_used[STUB_MALLOC] = 1;
        stub_used[STUB_MEMCPY] = 1;
      } else if (in->op == IR_OP_CALL && in->text &&
                 (strcmp(in->text, "mettle_crash_trap") == 0 ||
                  strcmp(in->text, "mettle_crash_trap_ex") == 0)) {
        stub_used[STUB_PUTS] = 1; /* the trap prints its message */
      } else if (in->op == IR_OP_CALL && in->text &&
                 find_fn(prog, in->text) < 0) {
        int id = runtime_stub_index(in->text);
        if (id >= 0) stub_used[id] = 1;
      } else if (in->op == IR_OP_CALL && in->text &&
                 strcmp(in->text, "cstr") == 0 &&
                 (in->argument_count == 0 ||
                  in->arguments[0].kind != IR_OPERAND_STRING)) {
        cstr_needs_body = 1;
      }
    }
  }
  for (int i = 0; i < STUB_COUNT; i++) {
    if (stub_used[i] && RUNTIME_STUBS[i].needs_malloc) stub_used[STUB_MALLOC] = 1;
  }

  /* _start: patch the globals that hold a function's address (unknown while
   * the data image was built), pass argc/argv per the Linux process-entry
   * contract (argc at [sp], argv at sp+8), call the entry, then exit(x0). */
  for (int i = 0; i < data.fixup_count; i++) {
    arm64_emit_label_address(e, ARM64_X9,
                             label_for(e, &fns, data.fixup_fns[i]));
    emit_imm(e, ARM64_X10,
             (uint64_t)ELF_DATA_VADDR + (uint64_t)data.fixup_offs[i]);
    arm64_emit_word(e, arm64_str_imm(1, ARM64_X9, ARM64_X10, 0));
    /* The named function must be emitted even if nothing calls it by name. */
    int fi = find_fn(prog, data.fixup_fns[i]);
    if (fi >= 0 && !reach[fi]) {
      reach[fi] = 1;
      queue[qt++] = fi;
    }
  }
  while (qh < qt) {
    int fi = queue[qh++];
    const IRFunction *f = prog->functions[fi];
    for (size_t k = 0; k < f->instruction_count; k++) {
      const IRInstruction *in = &f->instructions[k];
      if (in->op == IR_OP_CALL && in->text) {
        int ci = find_fn(prog, in->text);
        if (ci >= 0 && !reach[ci]) {
          reach[ci] = 1;
          queue[qt++] = ci;
        }
      }
    }
  }
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X0, ARM64_SP, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X1, ARM64_SP, 8, 0));
  arm64_emit_bl(e, label_for(e, &fns, entry));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_EXIT, 0));
  arm64_emit_word(e, ARM64_SVC0);

  for (size_t i = 0; i < n && !e->error; i++) {
    if (!reach[i]) {
      continue;
    }
    const IRFunction *fn = prog->functions[i];
    /* cstr is folded at call sites that pass a literal, so it is usually never
     * the target of a bl and needs no body at all. */
    if (strcmp(fn->name, "cstr") == 0 && !cstr_needs_body) {
      continue;
    }
    arm64_bind_label(e, label_for(e, &fns, fn->name));
    int with_newline = 0;
    if (io_stub_intrinsic(fn->name, &with_newline, NULL)) {
      emit_str_print(e, with_newline, 0);
    } else if (!encode_function(e, fn, &fns, prog, retf, NULL, &data)) {
      break;
    }
  }

  /* Runtime stub bodies last: their labels were created by the calls above, and
   * calloc branches to malloc, so malloc's label must exist before either body
   * is emitted. */
  int malloc_label = stub_used[STUB_MALLOC]
                         ? label_for(e, &fns, RUNTIME_STUBS[STUB_MALLOC].name)
                         : -1;
  for (int i = 0; i < STUB_COUNT && !e->error; i++) {
    if (!stub_used[i]) continue;
    arm64_bind_label(e, label_for(e, &fns, RUNTIME_STUBS[i].name));
    emit_runtime_stub(e, i, malloc_label);
  }

  free(reach);
  free(queue);
  free(retf);
  free(fns.names);
  free(fns.ids);
  if (e->error) {
    data_free(&data);
    return 0;
  }
  /* Hand the image to the caller to place in the executable's writable
   * segment; it owns the buffer from here. */
  if (data_out && data_len_out) {
    *data_out = data.bytes;
    *data_len_out = data.len;
    data.bytes = NULL;
  }
  data_free(&data);
  return 1;
}

static void arm64_object_error(char *error, size_t capacity,
                               const char *message) {
  if (error && capacity > 0) {
    snprintf(error, capacity, "%s", message ? message : "unknown error");
  }
}

/* Emit a global struct or array. With a folded initializer image it goes to
 * .data followed by one ADDR64 relocation per pointer-sized hole (a function's
 * address, another global's address, or a string element, whose characters and
 * { chars, length } record are parked in .rodata first). Without one it is
 * plain zeroed .bss. Mirrors the x86-64 path in globals.c. */
static int arm64_object_emit_aggregate(Arm64ObjectContext *object,
                                       const IRModuleSymbol *symbol,
                                       const char *link_name,
                                       size_t data_section,
                                       size_t bss_section) {
  BinaryEmitter *emitter = object->emitter;
  size_t size = mtlc_type_size(symbol->type);
  size_t alignment = symbol->type->alignment ? symbol->type->alignment : 8;
  size_t offset = 0;

  if (size == 0) return 0;

  if (!symbol->init_bytes || symbol->init_bytes_size == 0) {
    if (!binary_emitter_align_section(emitter, bss_section, alignment, 0) ||
        !binary_emitter_append_zeros(emitter, bss_section, size, &offset)) {
      return 0;
    }
    return binary_emitter_define_symbol(emitter, link_name,
                                        BINARY_SYMBOL_GLOBAL, bss_section,
                                        offset, size);
  }

  if (!binary_emitter_align_section(emitter, data_section, alignment, 0) ||
      !binary_emitter_append_bytes(emitter, data_section, symbol->init_bytes,
                                   symbol->init_bytes_size, &offset)) {
    return 0;
  }

  for (size_t i = 0; i < symbol->init_reloc_count; i++) {
    const IRInitReloc *reloc = &symbol->init_relocs[i];
    const char *target = reloc->symbol;
    char chars_symbol[64];
    char record_symbol[64];

    if (!target) {
      size_t chars_offset = 0;
      size_t length = reloc->string ? reloc->string_length : 0;
      snprintf(chars_symbol, sizeof(chars_symbol), ".Lmtlc.gstr.%u",
               object->string_id++);
      if (!binary_emitter_append_bytes(emitter, object->rodata_section,
                                       reloc->string ? reloc->string : "",
                                       length + 1, &chars_offset) ||
          !binary_emitter_define_symbol(emitter, chars_symbol,
                                        BINARY_SYMBOL_LOCAL,
                                        object->rodata_section, chars_offset,
                                        length + 1)) {
        return 0;
      }
      target = chars_symbol;

      if (reloc->string_wants_record) {
        /* A `string` slot points at a { chars, length } record, not at the
         * characters, which is what a string value is everywhere else. */
        size_t record_offset = 0;
        uint64_t encoded_length = (uint64_t)length;
        BinarySection *rodata = NULL;
        snprintf(record_symbol, sizeof(record_symbol), ".Lmtlc.gstr.%u",
                 object->string_id++);
        if (!binary_emitter_align_section(emitter, object->rodata_section, 8,
                                          0) ||
            !binary_emitter_append_zeros(emitter, object->rodata_section, 16,
                                         &record_offset) ||
            !binary_emitter_define_symbol(emitter, record_symbol,
                                          BINARY_SYMBOL_LOCAL,
                                          object->rodata_section,
                                          record_offset, 16) ||
            !binary_emitter_add_relocation(emitter, object->rodata_section,
                                           record_offset,
                                           BINARY_RELOCATION_ADDR64,
                                           chars_symbol, 0)) {
          return 0;
        }
        rodata = binary_emitter_get_section(emitter, object->rodata_section);
        if (!rodata || !rodata->data ||
            record_offset + 16 > rodata->size) {
          return 0;
        }
        memcpy(rodata->data + record_offset + 8, &encoded_length, 8);
        target = record_symbol;
      }
    } else {
      const IRModuleSymbol *referenced =
          object->program ? ir_program_lookup_symbol(object->program, target)
                          : NULL;
      if (referenced) {
        target = module_link_name(referenced);
      }
    }

    if (!binary_emitter_add_relocation(emitter, data_section,
                                       offset + reloc->offset,
                                       BINARY_RELOCATION_ADDR64, target, 0)) {
      return 0;
    }
  }

  return binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                      data_section, offset,
                                      symbol->init_bytes_size);
}

static int arm64_object_emit_global(Arm64ObjectContext *object,
                                    const IRModuleSymbol *symbol,
                                    size_t data_section,
                                    size_t bss_section) {
  BinaryEmitter *emitter = object->emitter;
  const char *link_name = module_link_name(symbol);
  if (!link_name || !link_name[0]) return 0;
  if (symbol->is_extern) {
    return binary_emitter_declare_external(emitter, link_name);
  }
  if (!symbol->type || symbol->has_unfoldable_initializer) return 0;

  /* Mettle strings are a {chars,length} pair in host memory. */
  if (symbol->type->kind == MTLC_TYPE_STRING) {
    size_t value_offset = 0;
    if (!binary_emitter_align_section(emitter, data_section, 8, 0) ||
        !binary_emitter_append_zeros(emitter, data_section, 16,
                                     &value_offset)) {
      return 0;
    }
    if (symbol->has_initializer && symbol->init_string) {
      char chars_symbol[64];
      size_t chars_offset = 0;
      size_t length = symbol->init_string_length;
      snprintf(chars_symbol, sizeof(chars_symbol), ".Lmtlc.gstr.%u",
               object->string_id++);
      if (!binary_emitter_append_bytes(
              emitter, object->rodata_section, symbol->init_string,
              length + 1, &chars_offset) ||
          !binary_emitter_define_symbol(
              emitter, chars_symbol, BINARY_SYMBOL_LOCAL,
              object->rodata_section, chars_offset, length + 1)) {
        return 0;
      }
      BinarySection *data =
          binary_emitter_get_section(emitter, data_section);
      uint64_t encoded_length = (uint64_t)length;
      if (!data || value_offset + 16 > data->size) return 0;
      memcpy(data->data + value_offset + 8, &encoded_length, 8);
      if (!binary_emitter_add_relocation(
              emitter, data_section, value_offset, BINARY_RELOCATION_ADDR64,
              chars_symbol, 0)) {
        return 0;
      }
    }
    return binary_emitter_define_symbol(
        emitter, link_name, BINARY_SYMBOL_GLOBAL, data_section, value_offset,
        16);
  }

  if (symbol->type->kind == MTLC_TYPE_STRUCT ||
      symbol->type->kind == MTLC_TYPE_ARRAY) {
    return arm64_object_emit_aggregate(object, symbol, link_name, data_section,
                                       bss_section);
  }

  size_t size = mtlc_type_size(symbol->type);
  if (size == 0 || size > 8) return 0;
  size_t alignment = symbol->type->alignment ? symbol->type->alignment : size;
  size_t section = symbol->has_initializer ? data_section : bss_section;
  size_t offset = 0;
  if (!binary_emitter_align_section(emitter, section, alignment, 0)) return 0;
  if (symbol->has_initializer) {
    unsigned char bytes[8] = {0};
    if (symbol->init_is_float) {
      double value = 0.0;
      memcpy(&value, &symbol->init_bits, 8);
      if (symbol->type->kind == MTLC_TYPE_FLOAT32) {
        float narrowed = (float)value;
        memcpy(bytes, &narrowed, 4);
      } else if (symbol->type->kind == MTLC_TYPE_FLOAT64) {
        memcpy(bytes, &value, 8);
      } else {
        return 0;
      }
    } else {
      uint64_t bits = (uint64_t)symbol->init_bits;
      memcpy(bytes, &bits, size);
    }
    if (!binary_emitter_append_bytes(emitter, section, bytes, size, &offset))
      return 0;
  } else if (!binary_emitter_append_zeros(emitter, section, size, &offset)) {
    return 0;
  }
  return binary_emitter_define_symbol(emitter, link_name,
                                      BINARY_SYMBOL_GLOBAL, section, offset,
                                      size);
}

int arm64_ir_write_object(const IRProgram *prog, const char *path, char *error,
                          size_t error_capacity) {
  BinaryEmitter *emitter = NULL;
  Arm64Emit code;
  LblMap functions = {0};
  int *returns_float = NULL;
  int success = 0;
  int code_initialized = 0;

  if (!prog || !path || !path[0]) {
    arm64_object_error(error, error_capacity, "invalid AArch64 object input");
    return 0;
  }
  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_ELF_ARM64);
  if (!emitter) {
    arm64_object_error(error, error_capacity,
                       "out of memory creating AArch64 object emitter");
    return 0;
  }

  Arm64ObjectContext object = {0};
  object.emitter = emitter;
  object.program = prog;
  object.text_section = binary_emitter_get_or_create_section(
      emitter, ".text", BINARY_SECTION_TEXT, 0, 4);
  object.rodata_section = binary_emitter_get_or_create_section(
      emitter, ".rodata", BINARY_SECTION_RDATA, 0, 8);
  size_t data_section = binary_emitter_get_or_create_section(
      emitter, ".data", BINARY_SECTION_DATA, 0, 8);
  size_t bss_section = binary_emitter_get_or_create_section(
      emitter, ".bss", BINARY_SECTION_BSS, 0, 8);
  if (object.text_section == (size_t)-1 ||
      object.rodata_section == (size_t)-1 || data_section == (size_t)-1 ||
      bss_section == (size_t)-1) {
    arm64_object_error(error, error_capacity,
                       binary_emitter_get_error(emitter));
    goto cleanup;
  }

  /* Undefined symbols must exist before relocations reference them; globals
   * are laid out before code so address materialization is uniformly symbolic. */
  for (size_t i = 0; i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &prog->module_symbols[i];
    if (symbol->kind == IR_MODSYM_VARIABLE) {
      if (!arm64_object_emit_global(&object, symbol, data_section,
                                    bss_section)) {
        arm64_object_error(error, error_capacity,
                           binary_emitter_get_error(emitter)
                               ? binary_emitter_get_error(emitter)
                               : "unsupported AArch64 global variable");
        goto cleanup;
      }
    } else if (symbol->kind == IR_MODSYM_FUNCTION && symbol->is_extern) {
      if (!binary_emitter_declare_external(emitter,
                                           module_link_name(symbol))) {
        arm64_object_error(error, error_capacity,
                           binary_emitter_get_error(emitter));
        goto cleanup;
      }
    }
  }

  returns_float = calloc(prog->function_count ? prog->function_count : 1,
                         sizeof(*returns_float));
  if (!returns_float) {
    arm64_object_error(error, error_capacity,
                       "out of memory classifying AArch64 functions");
    goto cleanup;
  }
  for (size_t iteration = 0; iteration <= prog->function_count; iteration++) {
    int changed = 0;
    for (size_t i = 0; i < prog->function_count; i++) {
      if (!returns_float[i] &&
          fn_returns_float(prog->functions[i], prog, returns_float)) {
        returns_float[i] = 1;
        changed = 1;
      }
    }
    if (!changed) break;
  }

  arm64_emit_init(&code);
  code_initialized = 1;
  if (!binary_emitter_define_symbol(emitter, "$x", BINARY_SYMBOL_LOCAL,
                                    object.text_section, 0, 0)) {
    arm64_object_error(error, error_capacity,
                       binary_emitter_get_error(emitter));
    goto cleanup;
  }

  for (size_t i = 0; i < prog->function_count && !code.error; i++) {
    const IRFunction *function = prog->functions[i];
    const IRModuleSymbol *symbol =
        module_function(prog, function ? function->name : NULL);
    if (!function || function->is_kernel ||
        (symbol && (symbol->is_extern || !symbol->has_body)) ||
        strcmp(function->name, "cstr") == 0) {
      continue;
    }
    size_t start = arm64_here(&code);
    arm64_bind_label(&code,
                     label_for(&code, &functions, function->name));
    if (!encode_function(&code, function, &functions, prog, returns_float,
                         &object, NULL)) {
      break;
    }
    const char *link_name = symbol ? module_link_name(symbol) : function->name;
    if (!binary_emitter_define_symbol(
            emitter, link_name, BINARY_SYMBOL_GLOBAL, object.text_section,
            start, arm64_here(&code) - start)) {
      arm64_fail(&code, "could not define the symbol for '%s'", link_name);
      break;
    }
  }
  if (code.error || !arm64_emit_finalize(&code)) {
    /* The lowering's own reason is the specific one; the emitter's error is a
     * fallback for failures that never reached arm64_fail. */
    arm64_object_error(error, error_capacity,
                       code.reason[0] ? code.reason
                       : binary_emitter_get_error(emitter)
                           ? binary_emitter_get_error(emitter)
                           : "AArch64 IR lowering failed");
    goto cleanup;
  }
  if (!binary_emitter_append_bytes(emitter, object.text_section,
                                   code.code.data, code.code.len, NULL) ||
      !binary_emitter_write_object_file(emitter, path)) {
    arm64_object_error(error, error_capacity,
                       binary_emitter_get_error(emitter));
    goto cleanup;
  }
  success = 1;

cleanup:
  if (code_initialized) arm64_emit_free(&code);
  free(functions.names);
  free(functions.ids);
  free(returns_float);
  binary_emitter_destroy(emitter);
  return success;
}

/* ---- minimal static AArch64 ELF executable ------------------------------ */

static void w16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void w32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

int arm64_write_elf(const char *path, const unsigned char *code, size_t len,
                    const unsigned char *data, size_t data_len) {
  unsigned char h[ELF_HDRS];
  memset(h, 0, sizeof(h));
  uint64_t text_total = ELF_HDRS + len;
  /* The writable segment loads at a fixed address (see ELF_DATA_VADDR), so the
   * text must not grow into it. A program that big belongs on the object path,
   * where the linker places everything. */
  if (text_total >= ELF_TEXT_LIMIT) {
    return 0;
  }
  /* p_offset and p_vaddr must agree modulo the page size. ELF_DATA_VADDR is
   * page-aligned, so a page-aligned file offset satisfies that; the extra page
   * keeps the two segments off a shared page. */
  uint64_t data_off = ((text_total + 0xFFFu) & ~(uint64_t)0xFFF) + 0x1000u;
  uint64_t data_size = data_len ? data_len : 8;

  h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
  h[4] = 2; h[5] = 1; h[6] = 1;
  w16(h + 16, 2); w16(h + 18, 183); w32(h + 20, 1);
  w64(h + 24, ELF_BASE + ELF_HDRS); w64(h + 32, 64); w64(h + 40, 0);
  w32(h + 48, 0); w16(h + 52, 64); w16(h + 54, 56); w16(h + 56, 2);
  unsigned char *ph = h + 64;
  w32(ph + 0, 1); w32(ph + 4, 5); w64(ph + 8, 0); /* PT_LOAD, R+X */
  w64(ph + 16, ELF_BASE); w64(ph + 24, ELF_BASE);
  w64(ph + 32, text_total); w64(ph + 40, text_total); w64(ph + 48, 0x1000);
  ph += 56;
  w32(ph + 0, 1); w32(ph + 4, 6); w64(ph + 8, data_off); /* PT_LOAD, R+W */
  w64(ph + 16, ELF_DATA_VADDR); w64(ph + 24, ELF_DATA_VADDR);
  w64(ph + 32, data_size); w64(ph + 40, data_size); w64(ph + 48, 0x1000);

  FILE *f = fopen(path, "wb");
  if (!f) {
    return 0;
  }
  static const unsigned char zeros[512] = {0};
  int ok = fwrite(h, 1, ELF_HDRS, f) == ELF_HDRS &&
           fwrite(code, 1, len, f) == len;
  for (uint64_t at = text_total; ok && at < data_off;) {
    size_t chunk = (size_t)(data_off - at);
    if (chunk > sizeof(zeros)) chunk = sizeof(zeros);
    ok = fwrite(zeros, 1, chunk, f) == chunk;
    at += chunk;
  }
  if (ok && data_len) {
    ok = fwrite(data, 1, data_len, f) == data_len;
  } else if (ok) {
    ok = fwrite(zeros, 1, 8, f) == 8;
  }
  fclose(f);
  return ok;
}
