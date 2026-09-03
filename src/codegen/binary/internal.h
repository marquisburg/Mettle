#ifndef CODEGEN_BINARY_INTERNAL_H
#define CODEGEN_BINARY_INTERNAL_H

#include "codegen/code_generator_internal.h"

#include <stddef.h>
#include <stdint.h>

/* Codegen stage timing, the counterpart of METTLE_TIME_IR_PASSES.
 * Codegen is the large majority of compile time on a big function, and until
 * this there was no way to see inside it. Enabled by METTLE_TIME_CODEGEN.
 * Raw clock() ticks: clock()'s units do not reliably match CLOCKS_PER_SEC
 * across the toolchains this builds with, and a number in the wrong units is
 * worse than none. */
/* Label name -> the earliest instruction index defining it. Both the frame
 * planner and the emitter ask where a branch's target label is; answering by
 * scanning is quadratic in a function that is mostly branches. */
typedef struct {
  const char **names;
  size_t *indices;
  size_t capacity;
} BinaryLabelIndex;

int binary_label_index_build(const IRFunction *function,
                             BinaryLabelIndex *index);
/* SIZE_MAX when the name defines no label. */
size_t binary_label_index_find(const BinaryLabelIndex *index, const char *name);
void binary_label_index_destroy(BinaryLabelIndex *index);

int cg_time_enabled(void);
double cg_time_begin(void);
void cg_time_end(const char *name, double started);
void cg_time_report(void);

#define BINARY_TEXT_SECTION_ALIGNMENT 16
#define BINARY_FUNCTION_STACK_SLOT_SIZE 8

/* Resolution of the --safe runtime's address-to-object map, mirroring
 * METTLE_SAFETY_GRANULE in src/runtime/safety.h. A local the compiler
 * describes to that map is aligned and padded to this so it cannot share a
 * unit with the local beside it; see the comment at its use in abi.c. */
#define BINARY_SAFETY_GRANULE 16
#define BINARY_WIN64_REGISTER_ARG_COUNT 4
#define BINARY_WIN64_SHADOW_SPACE_SIZE 32
#define BINARY_STACK_PAGE_SIZE 4096

typedef enum {
  BINARY_GP_RAX = 0,
  BINARY_GP_RCX = 1,
  BINARY_GP_RDX = 2,
  BINARY_GP_RBX = 3,
  BINARY_GP_RSP = 4,
  BINARY_GP_RBP = 5,
  BINARY_GP_RSI = 6,
  BINARY_GP_RDI = 7,
  BINARY_GP_R11 = 11,
  BINARY_GP_R8 = 8,
  BINARY_GP_R9 = 9,
  BINARY_GP_R10 = 10,
  BINARY_GP_R12 = 12,
  BINARY_GP_R13 = 13,
  BINARY_GP_R14 = 14,
  BINARY_GP_R15 = 15,
} BinaryGpRegister;

/* Scratch for IR store values. Address temps that survive past a fused memory
 * op must be spilled by the peephole liveness checks before stores are emitted. */
#define BINARY_GP_STORE_VALUE BINARY_GP_RCX

typedef enum {
  BINARY_XMM0 = 0,
  BINARY_XMM1 = 1,
  BINARY_XMM2 = 2,
  BINARY_XMM3 = 3,
  BINARY_XMM4 = 4,
  BINARY_XMM5 = 5,
  BINARY_XMM6 = 6,
  BINARY_XMM7 = 7,
  BINARY_XMM8 = 8,
  BINARY_XMM9 = 9,
  BINARY_XMM10 = 10,
  BINARY_XMM11 = 11,
  BINARY_XMM12 = 12,
  BINARY_XMM13 = 13,
  BINARY_XMM14 = 14,
  BINARY_XMM15 = 15,
} BinaryXmmRegister;

/* System V AMD64 aggregate classification.
 *
 * MS-x64 asks one question of an aggregate: does it fit in a register (1, 2, 4
 * or 8 bytes) or not. SysV asks a different one. It cuts the aggregate into
 * 8-byte chunks and gives each chunk its own class, so a 12-byte struct of
 * integers travels in a register PAIR where MS-x64 would pass a pointer, and a
 * {double,double} travels in two XMM registers. Anything over 16 bytes is
 * MEMORY, which the caller copies onto the stack by value rather than passing
 * by reference.
 *
 * Only the two-eightbyte window matters here: the classes array is sized for
 * the 16-byte limit above which everything is MEMORY. */
typedef enum {
  BINARY_EIGHTBYTE_NONE = 0,
  BINARY_EIGHTBYTE_INTEGER,
  BINARY_EIGHTBYTE_SSE,
} BinaryEightbyteClass;

typedef struct {
  /* MEMORY class: the caller copies the bytes into the outgoing stack area.
   * When set, eightbyte_count is 0. */
  int in_memory;
  size_t size;
  size_t eightbyte_count; /* 0, 1 or 2 */
  BinaryEightbyteClass classes[2];
  /* Filled by the caller, not the classifier: where this argument's first
   * slot sits in the expanded layout array, since an aggregate can consume
   * two entries where a scalar consumes one. */
  size_t first_slot;
} BinarySysvAggregate;

typedef struct {
  unsigned char *data;
  size_t size;
  size_t capacity;
} BinaryCodeBuffer;

typedef struct {
  char *name;
  int offset;
} BinaryNamedSlot;

typedef struct {
  BinaryNamedSlot *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinaryNamedSlotTable;

typedef struct {
  char *name;
  size_t offset;
} BinaryLabelEntry;

typedef struct {
  BinaryLabelEntry *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinaryLabelTable;

typedef struct {
  char *name;
  size_t displacement_offset;
} BinaryLabelFixup;

typedef struct {
  BinaryLabelFixup *items;
  size_t count;
  size_t capacity;
} BinaryLabelFixupTable;

typedef struct {
  char *symbol_name;
  size_t displacement_offset;
} BinaryCallRelocation;

typedef struct {
  BinaryCallRelocation *items;
  size_t count;
  size_t capacity;
} BinaryCallRelocationTable;

/* A symbol reference an inline `asm` block left behind. Unlike a call
 * relocation these carry their own kind and addend, because an asm block can
 * name a symbol as a rip-relative operand, an absolute 8-byte pointer, or the
 * target of a branch, and each needs a different fixup. */
typedef struct {
  char *symbol_name;
  size_t offset;
  int kind;
  int32_t addend;
} BinaryAsmRelocation;

typedef struct {
  BinaryAsmRelocation *items;
  size_t count;
  size_t capacity;
} BinaryAsmRelocationTable;

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} BinaryOffsetTable;

typedef struct {
  const char *name;
  const char *target;
} BinarySymbolAliasEntry;

typedef struct {
  BinarySymbolAliasEntry *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinarySymbolAliasTable;

typedef struct {
  char *name;
  size_t offset;
} BinaryDebugLabelExport;

typedef struct {
  BinaryDebugLabelExport *items;
  size_t count;
  size_t capacity;
} BinaryDebugLabelExportTable;

/* What the IR records about one operand name, gathered so that resolving an
 * operand's type does not have to rescan the function.
 *
 * A DECLARE_LOCAL carries its local's type as text; any instruction may carry a
 * baked value_type for the value it defines. TEMP and SYMBOL are separate
 * namespaces, so a name can have a distinct value_type in each. Each field holds
 * the FIRST instruction to supply it, which is what the scans this replaces
 * returned. */
typedef struct {
  const char *name;         /* borrowed from the IR */
  const char *decl_type;    /* first DECLARE_LOCAL's type text, or NULL */
  MtlcType *symbol_type;    /* first value_type defined into this SYMBOL */
  MtlcType *temp_type;      /* first value_type defined into this TEMP */
} BinaryOperandTypeEntry;

typedef struct {
  BinaryOperandTypeEntry *items;
  size_t count;
  size_t capacity;
  size_t *buckets; /* open addressing over items: slot+1, 0 = empty */
  size_t bucket_count;
  int built; /* built lazily: many functions never need it */
} BinaryOperandTypeIndex;

/* One operand of an inline kernel, staged into a frame slot by the MIR
 * lowering. Matched by ADDRESS, not by name: the kernel is handed the same
 * IRInstruction the bridge scanned, so `&instruction->lhs` and
 * `&instruction->arguments[3]` are the very pointers recorded here, and no
 * name lookup (which would not resolve in an allocated frame anyway) is
 * needed. */
typedef struct {
  const IROperand *operand; /* borrowed */
  int base_register;        /* BinaryGpRegister the slot is addressed off */
  int displacement;         /* byte displacement from base_register */
} BinaryMarshaledOperand;

/* Ceiling on staged operands, mirroring MIR_KERNEL_MAX_SLOTS in mir.h (kept
 * separate so internal.h does not have to include the MIR header). */
#define BINARY_MAX_MARSHALED_OPERANDS 8

typedef struct {
  BinaryCodeBuffer code;
  BinaryNamedSlotTable parameter_slots;
  BinaryNamedSlotTable local_slots;
  BinaryNamedSlotTable temp_slots;
  BinaryNamedSlotTable string_symbols;
  BinaryNamedSlotTable cstring_symbols;
  BinaryNamedSlotTable float64_symbols;
  BinaryNamedSlotTable address_taken_symbols;
  BinaryNamedSlotTable register_symbols;
  /* The subset of register_symbols that are cached GLOBALS rather than
   * register-homed locals: only these get a load at entry and a write-back
   * before each return. Recorded when the promotion picks them, because the
   * name alone cannot answer the question -- a local shadowing a global (a
   * `var exp` next to std/math's `exp`) resolves to the global in the module
   * symbol table, and writing the local back over it corrupts the global or
   * faults storing into .text. */
  BinaryNamedSlotTable register_global_symbols;
  BinarySymbolAliasTable symbol_aliases;
  BinaryLabelTable labels;
  BinaryLabelFixupTable label_fixups;
  BinaryCallRelocationTable call_relocations;
  BinaryAsmRelocationTable asm_relocations;
  BinaryOffsetTable return_fixups;
  /* Up to 8 callee-saved GP regs: RBX, RSI, RDI, R12..R15, and RBP (the last
   * only when the frame pointer is omitted and rbp joins the allocatable pool). */
  BinaryGpRegister saved_registers[8];
  int saved_register_offsets[8];
  size_t saved_register_count;
  /* Callee-saved XMM registers (xmm8..xmm15 on Win64) the MIR allocator used and
   * the prologue must preserve. Stored as 16-byte movdqu slots below the GP
   * saves. xmm8..15 are argument registers on neither Win64 nor SysV, so adding
   * them to the float pool needs no parameter/call-marshalling special-casing. */
  BinaryXmmRegister saved_xmm_registers[8];
  int saved_xmm_offsets[8];
  size_t saved_xmm_count;
  int raw_frame_size;
  int frame_size;
  /* When set, the frame pointer is omitted: rbp is not pushed/established, stack
   * slots are addressed off rsp (which is stable for the whole body), and rbp is
   * freed for register allocation. Gated off when stack traces / debug info /
   * debug hooks need a frame-pointer chain. Since rsp sits frame_size below where
   * rbp would be, an [rbp+d] slot becomes [rsp+frame_size+d]. */
  int omit_frame_pointer;
  /* Set when a loop header inside this function was padded to
   * BINARY_LOOP_ALIGN_BIG. That padding is computed as an offset within the
   * function's own code buffer, so it only lands on a real 32-byte boundary if
   * the function itself starts on one -- otherwise the loop ends up 16 bytes
   * off, which is exactly the placement the alignment was meant to avoid. The
   * .text append reads this and aligns the function entry to match. */
  int wants_wide_loop_alignment;
  /* IEEE-754 width of the function's float return (0/32/64). 0 = not float. */
  int return_float_bits;
  /* Set when the function's return type classifies INDIRECT (struct >8B or
   * non-pow2). The hidden out-pointer lives at [rbp - 8]; IR_OP_RETURN
   * memcpys through it. */
  int returns_indirect;
  /* Byte count of the INDIRECT return struct (0 if not INDIRECT). */
  size_t indirect_return_size;
  /* Set when this function is reached under SysV and hands back an aggregate
   * of 16 bytes or less, which travels in RAX/RDX or XMM0/XMM1 with no hidden
   * out-pointer. Mutually exclusive with returns_indirect. */
  int returns_sysv_registers;
  BinarySysvAggregate sysv_return_class;
  /* FIFO of caller-side return-slot rbp offsets, one per IR_OP_CALL whose
   * callee returns INDIRECT. Populated in the function pre-pass, consumed
   * in instruction order by emit_call. */
  int *indirect_return_slot_offsets;
  size_t indirect_return_slot_count;
  size_t indirect_return_slot_capacity;
  size_t indirect_return_slot_cursor;
  /* Storage for an aggregate parameter that SysV delivers in registers. The
   * home slot holds one word, so a two-eightbyte struct is rebuilt here and
   * the home is pointed at it, which is the shape the rest of the backend
   * already expects from an INDIRECT parameter. One rbp offset per parameter;
   * 0 means the parameter did not arrive that way. */
  int *incoming_aggregate_offsets;
  size_t incoming_aggregate_count;
  /* Side-table: which IR temps currently hold a POINTER to an indirect-
   * returned struct, with the byte size of that struct. Names are interned IR
   * strings (borrowed). */
  char **indirect_temp_names;
  size_t *indirect_temp_sizes;
  size_t indirect_temp_count;
  size_t indirect_temp_capacity;
  IRFunction *ir_function;
  const char *function_name;
  BinaryOperandTypeIndex operand_types;
  char *runtime_end_label;
  BinaryDebugLabelExportTable debug_export_labels;
  /* Live only while the MIR encoder is running one inline kernel (see
   * MIR_IR_KERNEL): the kernel's operands and the frame slots the surrounding
   * MIR staged them into. code_generator_binary_emit_operand_load and
   * ..._emit_destination_store consult this first, so a kernel written against
   * the fallback's named stack homes reads and writes the right storage inside
   * an allocated frame without knowing it. Zero at every other moment, which is
   * what makes the fallback path pay nothing for this. */
  BinaryMarshaledOperand marshaled_operands[BINARY_MAX_MARSHALED_OPERANDS];
  size_t marshaled_operand_count;
} BinaryFunctionContext;

typedef struct {
  char *name;
  uint64_t bits;
  long long int_value;
  double float_value;
  int is_float;
  int can_inline_load;
} BinaryGlobalConstEntry;

typedef struct {
  BinaryGlobalConstEntry *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinaryGlobalConstTable;
typedef struct {
  long long int_value;
  double float_value;
  int is_float;
} BinaryNumericConstant;
/* Name -> IRFunction index for the binary backend.
 *
 * code_generator_find_ir_function_binary used to linear-scan every IR function
 * (strcmp each), and it is called once per emitted function plus once per call
 * and addr-of instruction. That is O(functions^2) and dominated codegen on
 * large programs. We cache an open-addressing hash table keyed on the current
 * ir_program pointer + function_count, rebuilding only when those change. */
typedef struct {
  const char *name; /* borrowed from the IRFunction; not owned */
  IRFunction *function;
} BinaryIRFunctionSlot;

typedef struct {
  BinaryIRFunctionSlot *slots;
  size_t slot_count; /* power of two */
  const IRProgram *program;
  size_t function_count;
} BinaryIRFunctionIndex;



extern const BinaryGpRegister BINARY_WIN64_INT_PARAM_REGISTERS[];
extern const BinaryXmmRegister BINARY_WIN64_FLOAT_PARAM_REGISTERS[];

/* --- Calling-convention descriptor -------------------------------------- *
 *
 * The binary backend supports two x86-64 conventions: Microsoft x64 (used with
 * COFF/Windows) and System V AMD64 (used with ELF/Linux). They differ in more
 * than register names:
 *   - MS-x64 passes the first 4 args in registers using ONE positional slot
 *     index shared by int and float classes, reserves 32 bytes of "shadow
 *     space" the callee owns, and returns INDIRECT-struct out-pointers in RCX.
 *   - SysV passes up to 6 integer args (RDI,RSI,RDX,RCX,R8,R9) and up to 8
 *     float args (XMM0..7) using SEPARATE per-class counters, has no shadow
 *     space (but a 128-byte red zone), and returns the out-pointer in RDI.
 *
 * BinaryAbi captures the differences; code consults the active descriptor
 * instead of the BINARY_WIN64_* macros so a single backend serves both. */
typedef struct {
  const BinaryGpRegister *int_param_registers;
  size_t int_param_count;
  const BinaryXmmRegister *float_param_registers;
  size_t float_param_count;
  /* Bytes the caller reserves below the return address that the callee owns
   * (MS-x64 = 32, SysV = 0). */
  int shadow_space_size;
  /* Register carrying the hidden out-pointer for INDIRECT struct returns. */
  BinaryGpRegister indirect_return_register;
  /* When nonzero, int and float arguments consume independent register
   * sequences (SysV). When zero, a single positional slot indexes both
   * sequences (MS-x64). */
  int counts_classes_separately;
} BinaryAbi;

/* The active descriptor for the current build, selected from the code
 * generator's target object format. Defined in abi_spec.c. */
const BinaryAbi *code_generator_binary_active_abi(void);
void code_generator_binary_select_abi(BinaryTargetFormat format);
void code_generator_binary_describe_abi(const BinaryGpRegister *int_regs,
                                        size_t int_count,
                                        const BinaryXmmRegister *float_regs,
                                        size_t float_count, int shadow_space,
                                        BinaryGpRegister indirect_return,
                                        int separate_classes);


/* Classifies an aggregate under SysV. Returns 0 when `type` is not an
 * aggregate the classifier handles, in which case *out is left zeroed and the
 * caller should treat the type as an ordinary scalar. */
int code_generator_binary_classify_sysv_aggregate(MtlcType *type,
                                                  BinarySysvAggregate *out);

/* Is this function reached from outside the compilation, and therefore under
 * the platform's C ABI rather than Mettle's internal convention? True for
 * `extern` declarations, `export fn` definitions, and main. Both sides of a
 * call ask this of the CALLEE, so caller and callee always agree. */
int code_generator_binary_function_is_abi_public(CodeGenerator *generator,
                                                 const char *name);

/* Where a single argument or parameter is passed under the active ABI. */
typedef enum {
  BINARY_ARG_IN_GP_REGISTER,
  BINARY_ARG_IN_XMM_REGISTER,
  BINARY_ARG_ON_STACK,
} BinaryArgLocationKind;

typedef struct {
  BinaryArgLocationKind kind;
  BinaryGpRegister gp_register;  /* valid when kind == GP */
  BinaryXmmRegister xmm_register; /* valid when kind == XMM */
  /* Byte offset of this argument's home, relative to the start of the
   * outgoing stack-argument region (i.e. above shadow space). Valid when
   * kind == ON_STACK. */
  int stack_offset;
} BinaryArgLocation;

/* Computes, for an argument sequence, where each argument lands under the
 * active ABI. is_float[i] marks float/double args (passed in XMM). The hidden
 * INDIRECT-return out-pointer, if present, must be modeled by the caller as a
 * leading integer argument (it is for both conventions). Returns the total
 * bytes of stack-argument space needed (above shadow space), or -1 on error.
 * locations_out must have room for `count` entries. */
int code_generator_binary_compute_arg_layout(const BinaryAbi *abi,
                                              const int *is_float, size_t count,
                                              BinaryArgLocation *locations_out,
                                              int *stack_bytes_out);

/* As above, with two additions SysV aggregates need.
 *
 * force_stack[i], when nonzero, keeps slot i out of the register pools and
 * gives it a stack home even when registers remain. That is the MEMORY class,
 * which travels on the stack by value.
 *
 * stack_slots[i] is how many 8-byte slots slot i occupies once it is on the
 * stack, so a MEMORY aggregate reserves its whole width rather than one word.
 * Pass 0 or NULL to mean one slot.
 *
 * Either array may be NULL, in which case this behaves exactly like the plain
 * form above. */
int code_generator_binary_compute_arg_layout_ex(const BinaryAbi *abi,
                                                const int *is_float,
                                                const int *force_stack,
                                                const size_t *stack_slots,
                                                size_t count,
                                                BinaryArgLocation *locations_out,
                                                int *stack_bytes_out);

extern BinaryGlobalConstTable g_binary_global_consts;
extern BinaryIRFunctionIndex g_binary_ir_function_index;

BinaryLabelEntry *binary_label_table_get(BinaryLabelTable *table, const char *name);
size_t *code_generator_binary_build_loop_weights( const IRFunction *function);
const IRInstruction *code_generator_binary_find_temp_producer_before( const IRFunction *function, size_t before, const char *name);
MtlcType *code_generator_binary_get_operand_type(CodeGenerator *generator, const IROperand *operand);
MtlcType *code_generator_binary_get_operand_type_in_context( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand);
MtlcType *code_generator_binary_get_resolved_type(CodeGenerator *generator, const char *type_name, int allow_void);
IRFunction *code_generator_find_ir_function_binary(CodeGenerator *generator, const char *name);
int binary_align_up_int(int value, int alignment, int *result_out);
int binary_call_relocation_table_add(BinaryCallRelocationTable *table, const char *symbol_name, size_t displacement_offset);
void binary_call_relocation_table_destroy( BinaryCallRelocationTable *table);
int binary_code_buffer_append_bytes(BinaryCodeBuffer *buffer, const void *data, size_t size);
int binary_code_buffer_append_u32(BinaryCodeBuffer *buffer, uint32_t value);
int binary_code_buffer_append_u64(BinaryCodeBuffer *buffer, uint64_t value);
int binary_code_buffer_append_u8(BinaryCodeBuffer *buffer, unsigned char value);
void binary_code_buffer_destroy(BinaryCodeBuffer *buffer);
int binary_code_buffer_reserve(BinaryCodeBuffer *buffer, size_t minimum_capacity);
int binary_emit_add_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_add_rsp_imm32(BinaryCodeBuffer *buffer, uint32_t immediate);
int binary_emit_addsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_addss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_alu_reg8_reg8(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_alu_reg_imm32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_alu_reg_reg(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_alu_reg_mem(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister base, int displacement, int width);
int binary_emit_alu_reg_reg32(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_alu_reg_imm_w32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_unary_reg32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg);
int binary_emit_neg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_not_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_alu_rsp_imm32(BinaryCodeBuffer *buffer, unsigned char subopcode, uint32_t immediate);
int binary_emit_and_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_call_placeholder(BinaryCodeBuffer *buffer, size_t *displacement_offset_out);
int binary_emit_call_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_jmp_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_cmovcc_reg_reg(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_cmp_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_cmp_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister lhs, BinaryGpRegister rhs);
int binary_emit_cmp_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister lhs, BinaryGpRegister rhs);
int binary_emit_cmp_reg_imm_w32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_cqo(BinaryCodeBuffer *buffer);
int binary_emit_cvtsd2ss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_cvtsi2sd_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_cvtsi2ss_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_cvtss2sd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_cvttsd2si_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_cvttss2si_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_divsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_divss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_frame_allocation(BinaryCodeBuffer *code, int frame_size);
int binary_emit_idiv_reg(BinaryCodeBuffer *buffer, BinaryGpRegister divisor);
int binary_emit_div_reg(BinaryCodeBuffer *buffer, BinaryGpRegister divisor);
int binary_emit_mul_reg(BinaryCodeBuffer *buffer, BinaryGpRegister src);
int binary_emit_imul_reg(BinaryCodeBuffer *buffer, BinaryGpRegister src);
int binary_emit_imul_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_imul_reg_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, uint32_t immediate);
int binary_emit_imul_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_imul_reg_reg_imm32_w32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, uint32_t immediate);
/* As above, but the caller offers a free scratch register. Two of the
 * shift-and-add expansions need the multiplicand to survive the final add, so
 * without a scratch an in-place `x = x * C` has to fall back to imul and its
 * longer latency. Pass have_scratch = 0 when no register is free. */
int binary_emit_imul_reg_reg_imm32_scratch(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, uint32_t immediate, int have_scratch, BinaryGpRegister scratch);
int binary_emit_imul_reg_reg_imm32_scratch_w32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, uint32_t immediate, int have_scratch, BinaryGpRegister scratch);
int binary_emit_imul_reg_reg_small_imm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, int32_t immediate);
int binary_emit_jcc_placeholder(BinaryCodeBuffer *buffer, unsigned char condition_opcode, size_t *displacement_offset_out);
int binary_emit_je_placeholder(BinaryCodeBuffer *buffer, size_t *displacement_offset_out);
int binary_emit_jmp_placeholder(BinaryCodeBuffer *buffer, size_t *displacement_offset_out);
int binary_emit_lea_reg_base_index_scale_disp( BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
int binary_emit_lea_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_lea32_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_lea32_reg_base_index_scale_disp(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
int binary_emit_syscall(BinaryCodeBuffer *buffer);
int binary_emit_lea_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister lhs, BinaryGpRegister rhs);
int binary_emit_lea_reg_rip_placeholder(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_memory_access(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister reg, BinaryGpRegister base, int displacement);
int binary_emit_memory_access_ex(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, int displacement);
int binary_emit_prefetcht0_mem(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement);
int binary_emit_memory_access_sib(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
/* Forced-REX variants for byte ops: a register operand encoding 4..7 names
 * SPL/BPL/SIL/DIL only under a REX prefix (AH/CH/DH/BH without). */
int binary_emit_memory_access_sib_forced(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
int binary_emit_memory_access_ex_forced(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, int displacement);
int binary_emit_mov_eax_eax(BinaryCodeBuffer *buffer);
/* Loop-top alignment, shared by both x86-64 backends.
 *
 * Aligning a loop header keeps the loop's speed from depending on where its
 * function happened to land. How far to align is a cost/benefit question, and
 * the cost is padding on the fall-through path into the loop while the benefit
 * grows with how many times the loop body is fetched. So the boundary scales
 * with the body: 16 bytes normally, 32 -- the instruction-fetch and uop-cache
 * window -- once the body is big enough that up to 31 bytes of padding is a
 * rounding error next to it.
 *
 * The threshold is "loop body of about 128 bytes or more", where 31 bytes of
 * one-time padding is a quarter of a single fetch of the body. Byte sizes are
 * not known until the loop has been emitted, so each backend counts the
 * instructions it has and converts at its own expansion rate: a MIR
 * instruction is roughly one machine instruction (~4 bytes), while the
 * baseline backend works at IR granularity and expands each IR instruction
 * into several machine ones (~13 bytes).
 *
 * The MAX_PAD caps stop a loop that starts just past a boundary from soaking up
 * an almost-full boundary's worth of NOPs for a marginal gain. */
#define BINARY_LOOP_ALIGN 16u
#define BINARY_LOOP_ALIGN_MAX_PAD 11u
#define BINARY_LOOP_ALIGN_TIGHT_MAX_PAD 15u
#define BINARY_LOOP_TIGHT_MIR_INSTRUCTIONS 20u
#define BINARY_LOOP_ALIGN_BIG 32u
#define BINARY_LOOP_ALIGN_BIG_MAX_PAD 31u
#define BINARY_LOOP_BIG_MIR_INSTRUCTIONS 32u
#define BINARY_LOOP_BIG_IR_INSTRUCTIONS 10u

int binary_emit_align_code(BinaryCodeBuffer *buffer, size_t boundary, size_t max_pad);
int binary_emit_mov_mem_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, int32_t immediate);
int binary_emit_mov_mem_imm_width(BinaryCodeBuffer *buffer, BinaryGpRegister base, int has_index, BinaryGpRegister index, int scale, int displacement, long long value, int width);
int binary_emit_mov_mem_reg(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_rip_reg(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_mem_rip_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_mem_rip_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_mem_rip_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_reg32_rip_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_mov_reg_imm32_zero_extend(BinaryCodeBuffer *buffer, BinaryGpRegister destination, uint32_t immediate);
int binary_emit_mov_reg_imm64(BinaryCodeBuffer *buffer, BinaryGpRegister destination, uint64_t immediate);
int binary_emit_mov_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_mov_reg_mem32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_mov_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_mov_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_mov_reg_rip_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_movd_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_movd_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_movq_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_movq_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_movsx_rax_al(BinaryCodeBuffer *buffer);
int binary_emit_movsx_rax_ax(BinaryCodeBuffer *buffer);
int binary_emit_movsx_reg_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movsx_reg_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movsxd_rax_eax(BinaryCodeBuffer *buffer);
int binary_emit_movsxd_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_reg_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_reg_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_eax_al(BinaryCodeBuffer *buffer);
int binary_emit_movzx_eax_ax(BinaryCodeBuffer *buffer);
int binary_emit_movzx_reg_mem16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movzx_reg_mem8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movsx_reg_mem8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movsx_reg_mem16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movsxd_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movzx_reg_rip_mem16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_movzx_reg_rip_mem8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_mulsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_mulss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_neg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_idiv_wrapping(BinaryCodeBuffer *buffer, BinaryGpRegister divisor);
int binary_emit_not_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_or_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_pop_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_push_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_pxor_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_ret(BinaryCodeBuffer *buffer);
int binary_emit_rex(BinaryCodeBuffer *buffer, int w, int r, int x, int b);
int binary_emit_rip_relative_access_ex( BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, size_t *displacement_offset_out);
int binary_emit_setcc_al(BinaryCodeBuffer *buffer, unsigned char condition_opcode);
int binary_emit_setcc_reg8(BinaryCodeBuffer *buffer, unsigned char condition_opcode, BinaryGpRegister reg);
int binary_emit_shift_reg_cl(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg);
int binary_emit_shift_reg_imm8(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, unsigned char immediate);
int binary_emit_shift_reg_imm8_32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, unsigned char immediate);
int binary_emit_sse_reg_reg(BinaryCodeBuffer *buffer, unsigned char mandatory_prefix, int rex_w, unsigned char opcode1, unsigned char opcode2, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_sub_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_sub_rsp_imm32(BinaryCodeBuffer *buffer, uint32_t immediate);
int binary_emit_subsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_subss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_test_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_test_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_ucomisd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister lhs, BinaryXmmRegister rhs);
int binary_emit_ucomiss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister lhs, BinaryXmmRegister rhs);
int binary_emit_unary_reg(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg);
int binary_emit_xor_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_xor_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
void binary_function_context_destroy(BinaryFunctionContext *context);
int code_generator_binary_emit_unsigned_int_to_float(BinaryFunctionContext *context, int float_bits, BinaryXmmRegister destination, BinaryGpRegister source, BinaryGpRegister work, BinaryGpRegister odd);
int code_generator_binary_emit_float_to_unsigned_int(BinaryFunctionContext *context, int float_bits, BinaryGpRegister destination, BinaryXmmRegister source, BinaryGpRegister work, BinaryXmmRegister scratch);
int binary_function_context_patch_rel32(BinaryFunctionContext *context, size_t displacement_offset, size_t target_offset);
uint64_t binary_global_const_bits(long long int_value, double float_value, int is_float);
int binary_global_const_table_add(const char *name, long long int_value, double float_value, int is_float, int can_inline_load);
int binary_global_const_table_get(const char *name, uint64_t *value_out);
int binary_global_const_table_rebuild(size_t needed_count);
void binary_global_const_table_reset(void);
int binary_immediate_positive_power_of_two_i32(int32_t value, unsigned char *shift_out);
int binary_indirect_temp_add(BinaryFunctionContext *context, const char *name, size_t size);
size_t binary_indirect_temp_get(BinaryFunctionContext *context, const char *name);
int binary_ir_function_index_ensure(const IRProgram *program);
void binary_ir_function_index_insert(BinaryIRFunctionIndex *index, IRFunction *function);
void binary_ir_function_index_reset(void);
int binary_label_fixup_table_add(BinaryLabelFixupTable *table, const char *name, size_t displacement_offset);
void binary_label_fixup_table_destroy(BinaryLabelFixupTable *table);
int binary_label_table_define(BinaryLabelTable *table, const char *name, size_t offset);
void binary_label_table_destroy(BinaryLabelTable *table);
int binary_named_slot_table_add(BinaryNamedSlotTable *table, const char *name, int offset);
void binary_named_slot_table_destroy(BinaryNamedSlotTable *table);
int binary_named_slot_table_get_offset(const BinaryNamedSlotTable *table, const char *name);
int binary_offset_table_add(BinaryOffsetTable *table, size_t offset);
void binary_offset_table_destroy(BinaryOffsetTable *table);
int binary_symbol_alias_table_add(BinarySymbolAliasTable *table, const char *name, const char *target);
void binary_symbol_alias_table_destroy(BinarySymbolAliasTable *table);
const char * binary_symbol_alias_table_get(const BinarySymbolAliasTable *table, const char *name);
int code_generator_binary_address_consumed_by_adjacent_memory( const IRFunction *function, size_t address_index);
int code_generator_binary_chain_producer_supported(const char *op);
int code_generator_binary_collect_global_constants(CodeGenerator *generator);
int code_generator_binary_collect_symbol_aliases( CodeGenerator *generator, BinaryFunctionContext *context, IRFunction *ir_function);
int code_generator_binary_compare_false_jcc(const char *op, unsigned char *opcode_out);
int code_generator_binary_compare_false_jcc_u(const char *op, int is_unsigned, unsigned char *opcode_out);
int code_generator_binary_compare_true_cmov(const char *op, unsigned char *opcode_out);
int code_generator_binary_compare_true_cmov_u(const char *op, int is_unsigned, unsigned char *opcode_out);
int code_generator_binary_context_add_saved_register( BinaryFunctionContext *context, BinaryGpRegister reg);
int code_generator_binary_context_add_saved_xmm_register( BinaryFunctionContext *context, BinaryXmmRegister reg);
int code_generator_binary_declare_external_symbol( CodeGenerator *generator, const char *symbol_name);
int code_generator_binary_emit_address_add_to_rax( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *address);
int code_generator_binary_emit_address_of( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_and_mask(BinaryFunctionContext *context, BinaryGpRegister target_register, unsigned long long mask);
int code_generator_binary_emit_binary(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_call(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_call_argument_load( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand, MtlcType *parameter_type, BinaryGpRegister target_register);
int code_generator_binary_emit_call_indirect( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_cast(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_compare_false_branch( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *compare, const char *target_label);
int code_generator_binary_emit_compare_false_branch_from_rax( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *compare, const IROperand *rhs, const char *target_label);
int code_generator_binary_emit_compare_flags( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *compare);
int code_generator_binary_emit_count_word_starts( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_cstring_literal_address( CodeGenerator *generator, BinaryFunctionContext *context, const char *value, BinaryGpRegister target_register);
int code_generator_binary_emit_destination_store( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *destination, BinaryGpRegister source_register);
int code_generator_binary_emit_float_call_argument( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand, MtlcType *parameter_type, int param_fbits, BinaryXmmRegister xmm_register);
int code_generator_binary_emit_float_operand_to_xmm( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand, BinaryXmmRegister target_register);
int code_generator_binary_emit_float_operand_to_xmm_bits( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand, BinaryXmmRegister target_register, int want_bits);
int code_generator_binary_emit_float_reg_convert( BinaryFunctionContext *context, BinaryGpRegister gp_register, int src_bits, int dst_bits);
int code_generator_binary_emit_global_string_variable( CodeGenerator *generator, const char *link_name, const char *value, size_t value_length);
int code_generator_binary_emit_global_symbol_load( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, MtlcType *type, int declare_external, BinaryGpRegister target_register);
int code_generator_binary_emit_global_symbol_store( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, MtlcType *type, int declare_external, BinaryGpRegister source_register);
int code_generator_binary_emit_indirect_source_address( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand, BinaryGpRegister target_register);
int code_generator_binary_emit_instruction( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_integer_binary_to_rax( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_load(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_load_from_address( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister address_register, int size, BinaryGpRegister target_register);
int code_generator_binary_emit_local_string_store( CodeGenerator *generator, BinaryFunctionContext *context, int offset, BinaryGpRegister source_register);
int code_generator_binary_emit_memcpy_inline( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_new(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_operand_load( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand, BinaryGpRegister target_register);
int code_generator_binary_emit_prologue(CodeGenerator *generator, BinaryFunctionContext *context);
int code_generator_binary_emit_profile_enter(CodeGenerator *generator,
                                             BinaryFunctionContext *context,
                                             uint32_t fn_id);
int code_generator_binary_emit_profile_op(CodeGenerator *generator,
                                          BinaryFunctionContext *context,
                                          uint32_t op_class,
                                          uint64_t amount);
int code_generator_binary_emit_profile_exit(CodeGenerator *generator,
                                            BinaryFunctionContext *context);
int code_generator_binary_emit_promoted_global_loads(CodeGenerator *generator,
                                                     BinaryFunctionContext *context);
int code_generator_binary_emit_promoted_global_stores(CodeGenerator *generator,
                                                      BinaryFunctionContext *context);
int code_generator_binary_emit_profile_tables(CodeGenerator *generator);
int code_generator_binary_emit_dwarf_debug_sections(CodeGenerator *generator);
int code_generator_binary_emit_runtime_debug_tables(CodeGenerator *generator);
int code_generator_binary_emit_crash_startup(CodeGenerator *generator);
int code_generator_binary_emit_elf_runtime_hooks(CodeGenerator *generator);
int code_generator_binary_emit_runtime_location_marker(
    CodeGenerator *generator, BinaryFunctionContext *context,
    size_t source_line, size_t source_column, const char *filename);
int code_generator_binary_record_debug_label_export(
    BinaryFunctionContext *context, const char *name, size_t offset);
int code_generator_binary_export_debug_symbols(
    CodeGenerator *generator, BinaryFunctionContext *context,
    size_t text_section, size_t function_offset, size_t end_offset);
int code_generator_binary_emit_rax_binary_rhs( CodeGenerator *generator, BinaryFunctionContext *context, const char *op, const IROperand *rhs, int lhs_unsigned);
int code_generator_binary_emit_rep_movsb( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister src_addr_reg, BinaryGpRegister dst_addr_reg, size_t size);
int code_generator_binary_emit_rep_movsq( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister src_addr_reg, BinaryGpRegister dst_addr_reg, size_t qword_count);
int code_generator_binary_emit_rotate_add( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_runtime_trap_call( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_scaled_address_to_rax( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *base, const IROperand *index, int scale);
int code_generator_binary_emit_scaled_address_to_rax_disp( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *base, const IROperand *index, int scale, int displacement);
int code_generator_binary_try_match_offset_scaled_address( const IRFunction *function, size_t instruction_index, const IRInstruction **mem_out, const IROperand **base_out, const IROperand **index_out, int *scale_out, int *displacement_out);
int code_generator_binary_try_emit_offset_scaled_address_load( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_offset_scaled_address_store( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_emit_simd_clamp_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_i8( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_exp_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_silu_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_silu_f32_inline(BinaryCodeBuffer *b, int has_mul);
int code_generator_binary_emit_simd_slp_mac_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
/* Pure inner loop of the SLP MAC kernel for the MIR pass-through path. Assumes
 * RCX/RDX/R8 = a/b/out element pointers (offsets already applied), R9 = k count,
 * and RAX = b row stride in bytes (advances b each iteration). K is 4 or 8. */
int code_generator_binary_emit_simd_slp_mac_i32_loop(BinaryCodeBuffer *b, long long K);
int code_generator_binary_emit_simd_slp_mac_i8_loop(BinaryCodeBuffer *b, long long K);
int code_generator_binary_emit_simd_slp_mac_i8( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
/* Emit a single vzeroupper (clears YMM upper halves). Used by the MIR epilogue
 * to guard the AVX->legacy-SSE transition once per function that ran an inline
 * vector kernel. */
int code_generator_binary_emit_vzeroupper(BinaryCodeBuffer *b);
int code_generator_binary_emit_simd_insertion_sort_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_matmul_n32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_reverse_copy_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_lower_bound_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_scale_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_lcg_u32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_u8( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_byte_map( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_fill( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
/* Shared fill primitives (RAX = value in, XMM0 = splat out; loop assumes
 * RCX = base, R8 = element count): used by both the fallback fill lowering and
 * the MIR inline-fill passthrough (MIR_SIMD_FILL). */
int code_generator_binary_emit_simd_fill_splat(BinaryCodeBuffer *b, long long size);
int code_generator_binary_emit_simd_fill_loop_mode0(BinaryCodeBuffer *b, long long size);
int code_generator_binary_emit_simd_fill_loop_bytewalk(BinaryCodeBuffer *b, long long size, int mode);
/* Shared float32 affine-map loop (RCX=src iterated, RDX=dst, R9=src end ptr,
 * ymm4=a, ymm5=b, ymm3=c broadcasts): fallback + MIR passthrough share it. */
int code_generator_binary_emit_simd_affine_map_f32_loop(BinaryCodeBuffer *b, int b_is_one, int b_is_zero, int c_is_zero);
int code_generator_binary_emit_simd_affine_map_f32_inline(BinaryCodeBuffer *b, unsigned a_bits, unsigned b_bits, unsigned c_bits, int b_is_one, int b_is_zero, int c_is_zero);
/* Shared float64 affine-map loop + MIR inline passthrough (coeffs from raw
 * 64-bit IEEE bits; assumes RCX=src, RDX=dst, R8=count marshalled). */
int code_generator_binary_emit_simd_affine_map_f64_loop(BinaryCodeBuffer *b, int b_is_one, int b_is_zero, int c_is_zero);
int code_generator_binary_emit_simd_affine_map_f64_inline(BinaryCodeBuffer *b, unsigned long long a_bits, unsigned long long b_bits, unsigned long long c_bits, int b_is_one, int b_is_zero, int c_is_zero, int a_runtime);
int code_generator_binary_emit_prefix_sum_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_minmax_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_affine_map_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_affine_map_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_i2f_reduce_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_vloop_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction, int operands_marshaled);
int code_generator_binary_emit_simd_vloop_unmarshaled( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
/* Distinct base operands of a vloop in kGp order; shared by the kernel and the
 * MIR passthrough lowering. names/srcs must hold VLOOP_KERNEL_MAX_BASES (4). */
int code_generator_vloop_collect_dist(const IRInstruction *in, int is_reduce, const char *names[4], const IROperand *srcs[4], int *n_out);
int code_generator_binary_emit_simd_find( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_outer_lane_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_store(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_store_to_address( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister address_register, int size, BinaryGpRegister source_register);
int code_generator_binary_emit_string_literal_value_address( CodeGenerator *generator, BinaryFunctionContext *context, const char *value, size_t value_length, BinaryGpRegister target_register);
int code_generator_binary_emit_string_symbol_load( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, const CgSym *symbol, BinaryGpRegister target_register);
int code_generator_binary_emit_struct_destination_address( CodeGenerator *generator, BinaryFunctionContext *context, const char *name, BinaryGpRegister target_register);
int code_generator_binary_emit_symbol_address( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, int declare_external, BinaryGpRegister target_register);
int code_generator_binary_emit_unary(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_extract_positive_power_of_two( long long value, unsigned int *shift_out, unsigned long long *mask_out);
/* Emits signed `rax / divisor` or `rax % divisor` via magic multiply when safe.
 * On success with handled_out=1, RAX holds the quotient/remainder. */
int code_generator_binary_try_emit_signed_const_divmod( BinaryFunctionContext *context, const char *op, long long divisor, int *handled_out);
int code_generator_binary_try_emit_unsigned_const_divmod( BinaryFunctionContext *context, const char *op, unsigned long long divisor, int *handled_out);
int code_generator_binary_function_can_promote_rsi_rdi( CodeGenerator *generator, IRFunction *function, MtlcType *return_type);
int code_generator_binary_function_has_calls(const IRFunction *function);
size_t code_generator_binary_function_symbol_score( const BinaryFunctionContext *context, const IRFunction *function, const char *name, const size_t *loop_weights);
int code_generator_binary_function_temp_use_count( const IRFunction *function, const char *name);
int code_generator_binary_get_access_size(CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *size_operand);
int code_generator_binary_get_local_offset(BinaryFunctionContext *context, const char *name);
int code_generator_binary_get_parameter_offset( BinaryFunctionContext *context, const char *name);
int binary_asm_relocation_table_add(BinaryAsmRelocationTable *table, const char *symbol_name, size_t offset, int kind, int32_t addend);
void binary_asm_relocation_table_destroy(BinaryAsmRelocationTable *table);
int code_generator_binary_assemble_text(CodeGenerator *generator, BinaryFunctionContext *context, const char *text, int bits, int allow_bits_directive, const SourceLocation *location, int *final_bits_out);
int code_generator_binary_emit_inline_asm(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int ir_function_has_inline_asm(const IRFunction *function);
int code_generator_emit_binary_naked_function(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_binary_check_interrupt_signature(CodeGenerator *generator, IRFunction *ir_function);
int code_generator_binary_emit_interrupt_entry(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_binary_emit_interrupt_exit(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_emit_binary_function_x86_16(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_binary_get_symbol_offset(BinaryFunctionContext *context, const char *name);
/* The module symbol a value operand of this name refers to, or NULL when a
 * local/parameter of the function shadows it or the name is a function. */
const CgSym *code_generator_binary_value_symbol(CodeGenerator *generator, BinaryFunctionContext *context, const char *name);
int code_generator_binary_get_temp_offset(BinaryFunctionContext *context, const char *name);
int code_generator_binary_global_is_written(IRProgram *ir_program, const char *name);
int code_generator_binary_gp_register_is_win64_nonvolatile( BinaryGpRegister reg);
int code_generator_binary_immediate_fits_signed_32(long long value);
/* Should `value <op> immediate` be emitted at operand size 32?
 *
 * The 64-bit ALU immediate form sign-extends its imm32, so it cannot express a
 * constant above INT32_MAX -- 0xEDB88320, 0x9E3779B9, 0xFFFFFF00 and most
 * other bit-twiddling constants. Those otherwise cost a scratch register and a
 * separate `mov reg, imm` at every use, inside the loop that uses them.
 * Operand size 32 takes the full unsigned range in its immediate field and
 * zero-extends the result.
 *
 * AND is always safe that way: the immediate's upper half is zero, so the
 * result's upper half is zero at either width. OR and XOR are safe only when
 * the value operand already has a zero upper half -- a uint32, whose register
 * holds the canonical zero-extended form. Returns 0 for every other case,
 * including immediates that already fit a signed imm32. */
int code_generator_binary_bitwise_imm_wants_operand_size_32(
    CodeGenerator *generator, BinaryFunctionContext *context, const char *op,
    const IROperand *value, long long immediate);
int code_generator_binary_instruction_in_backward_loop( const IRFunction *function, size_t instruction_index);
int code_generator_binary_instruction_result_float_bits( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_instruction_result_is_float64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_instruction_temp_use_count( const IRInstruction *instruction, const char *name);
int code_generator_binary_instruction_writes_dest(IROpcode op);
int code_generator_binary_is_compare_operator(const char *op);
int code_generator_binary_is_marked_float64_symbol( const BinaryFunctionContext *context, const char *name);
int code_generator_binary_label_reference_count( const IRFunction *function, const char *label);
int code_generator_binary_load_needs_sign_extend( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *destination, int load_size);
int code_generator_binary_widen_narrow_load(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *load, int size, BinaryGpRegister value_register);
int code_generator_binary_mark_float64_symbol( BinaryFunctionContext *context, const char *name);
int code_generator_binary_mark_float_symbol( BinaryFunctionContext *context, const char *name, int bits);
int code_generator_binary_marked_symbol_float_bits( const BinaryFunctionContext *context, const char *name);
int code_generator_binary_named_type_float_bits(CodeGenerator *generator, const char *type_name);
int code_generator_binary_named_type_is_float64(CodeGenerator *generator, const char *type_name, int allow_void);
int code_generator_binary_operand_float_bits( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand);
int code_generator_binary_operand_is_known_float64( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand);
int code_generator_binary_operand_mentions_symbol( const IROperand *operand, const char *name);
int code_generator_binary_operand_mentions_symbol_or_alias( const BinaryFunctionContext *context, const IROperand *operand, const char *name);
int code_generator_binary_operand_uses_temp(const IROperand *operand, const char *name);
int code_generator_binary_operator_is_commutative(const char *op);
int code_generator_binary_parameter_is_indirect( CodeGenerator *generator, BinaryFunctionContext *context, const char *name);
int code_generator_binary_prepare_function_context( CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int binary_function_local_is_safety_described(const IRFunction *function,
                                              const char *name);
void binary_operand_type_index_destroy(BinaryOperandTypeIndex *ix);
int code_generator_binary_promote_hot_symbols( CodeGenerator *generator, BinaryFunctionContext *context, IRFunction *ir_function);
int code_generator_binary_resolve_fixups(CodeGenerator *generator, BinaryFunctionContext *context, size_t return_offset);
int code_generator_binary_resolved_type_float_bits(MtlcType *type);
int code_generator_binary_resolved_type_is_abi_supported(MtlcType *type, int allow_void);
int code_generator_binary_resolved_type_is_float64(MtlcType *type);
int code_generator_binary_resolved_type_is_signed_integer(MtlcType *type);
int code_generator_binary_resolved_type_is_stack_scalar(MtlcType *type);
int code_generator_binary_resolved_type_is_supported(MtlcType *type, int allow_void);
int code_generator_binary_resolved_type_scalar_size(MtlcType *type);
int code_generator_binary_shift_only_feeds_scaled_addresses( const IRFunction *function, size_t shift_index);
int code_generator_binary_shift_scale(const IRInstruction *instruction, int *scale_out);
int code_generator_binary_symbol_already_promoted( BinaryFunctionContext *context, const char *name);
int code_generator_binary_symbol_assigned_register( CodeGenerator *generator, BinaryFunctionContext *context, const char *name, BinaryGpRegister *register_out);
int code_generator_binary_symbol_is_scalar_accessible( CodeGenerator *generator, const char *name);
int code_generator_binary_symbol_move_width(const CgSym *symbol);
int code_generator_binary_type_scalar_width(MtlcType *type);
int code_generator_binary_emit_temp_stack_load( CodeGenerator *generator, BinaryFunctionContext *context, int stack_offset, BinaryGpRegister target_register, MtlcType *type);
int code_generator_binary_emit_temp_stack_store( CodeGenerator *generator, BinaryFunctionContext *context, int stack_offset, BinaryGpRegister source_register, MtlcType *type);
int code_generator_binary_instruction_compare_width( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_reg_reg_compare( BinaryCodeBuffer *buffer, BinaryGpRegister lhs, BinaryGpRegister rhs, int width);
int code_generator_binary_emit_reg_reg_move( BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, MtlcType *type);
int code_generator_binary_try_emit_reg_multiply_immediate( BinaryFunctionContext *context, BinaryGpRegister target_register, long long immediate, int *handled_out);
int code_generator_binary_emit_symbol_stack_load( CodeGenerator *generator, BinaryFunctionContext *context, MtlcType *type, int stack_offset, BinaryGpRegister target_register);
int code_generator_binary_emit_symbol_stack_store( CodeGenerator *generator, BinaryFunctionContext *context, MtlcType *type, int stack_offset, BinaryGpRegister source_register);
size_t code_generator_binary_symbol_write_count( const IRFunction *function, const char *name);
int code_generator_binary_try_emit_address_add_load( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_address_add_store( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_binary_cast_chain( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_binary_compare_branch_chain( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_binary_expression_chain( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_float_binary_expression_chain( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_float_cast_binary_chain( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_compare_assign_diamond( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_compare_update_pair_diamond( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_compare_branch_zero( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_scaled_address_load( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_emit_scaled_address_store( CodeGenerator *generator, BinaryFunctionContext *context, const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_try_match_scaled_address( const IRFunction *function, size_t instruction_index, const IRInstruction **address_out, const IROperand **base_out, const IROperand **index_out, int *scale_out);
int code_generator_binary_try_match_scaled_temp_address( const IRFunction *function, size_t instruction_index, const IRInstruction *address, const IROperand **base_out, const IROperand **index_out, int *scale_out);
int code_generator_binary_try_skip_scaled_address_shift( const IRFunction *function, size_t instruction_index, size_t *consumed_out);
int code_generator_binary_type_is_abi_supported(CodeGenerator *generator, const char *type_name, int allow_void);
int code_generator_binary_type_is_cstring(MtlcType *type);
int code_generator_binary_type_is_direct_aggregate(MtlcType *type);
int code_generator_binary_type_is_gp_promotable(MtlcType *type);
int code_generator_binary_type_is_string(MtlcType *type);
int code_generator_binary_validate_call(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_validate_indirect_call( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
MtlcType *code_generator_binary_indirect_callee_type( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_validate_signature(CodeGenerator *generator, IRFunction *ir_function);
int code_generator_declare_binary_externs(CodeGenerator *generator);
int code_generator_emit_binary_function(CodeGenerator *generator,
                                        IRFunction *ir_function);
int code_generator_emit_binary_global_variable(CodeGenerator *generator, const IRModuleSymbol *sym);
int code_generator_generate_program_binary_object(CodeGenerator *generator);
int simd_emit_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char opcode, int xmm, int gpr, int displacement);
int simd_emit_prefixed_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char prefix, unsigned char opcode, int xmm, int gpr, int displacement);
int simd_movdqu_mem_xmm_disp(BinaryCodeBuffer *b, int gpr, int displacement, int xmm);
int simd_movdqu_xmm_mem_disp(BinaryCodeBuffer *b, int xmm, int gpr, int displacement);
int wcs_accumulate_xmm0_i32_to_rax(BinaryCodeBuffer *b);
int wcs_add_reg_reg64(BinaryCodeBuffer *b, int dst, int src);
int wcs_addsub_reg_imm8(BinaryCodeBuffer *b, int gpr, int is_sub, unsigned char imm);
int wcs_and_reg_reg(BinaryCodeBuffer *b, int dst, int src);
int wcs_broadcast_i32_to_xmm(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_cmp_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm);
int wcs_cmp_reg_imm8(BinaryCodeBuffer *b, int gpr, unsigned char imm);
int wcs_cmp_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_fold_xmm6_i32_sum_to_rax(BinaryCodeBuffer *b);
int wcs_jcc(BinaryCodeBuffer *b, unsigned char cc, size_t *disp_off);
int wcs_mov_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm);
int wcs_mov_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_movd_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm);
int wcs_movd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_avx_vcvtph2ps_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vcvtps2ph_xmm(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_movdqu_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_movdqu_xmm_rcx(BinaryCodeBuffer *b, int xmm);
int wcs_movzx_reg_byte_rcx(BinaryCodeBuffer *b, int gpr);
int wcs_not_reg(BinaryCodeBuffer *b, int gpr);
int wcs_or_reg_reg(BinaryCodeBuffer *b, int dst, int src);
int wcs_paddd(BinaryCodeBuffer *b, int dst, int src);
int wcs_paddq(BinaryCodeBuffer *b, int dst, int src);
int wcs_patch_here(BinaryCodeBuffer *b, size_t disp_off);
int wcs_patch_to(BinaryCodeBuffer *b, size_t disp_off, size_t target);
int wcs_pmaxsd(BinaryCodeBuffer *b, int dst, int src);
int wcs_pminsd(BinaryCodeBuffer *b, int dst, int src);
int wcs_pmovmskb(BinaryCodeBuffer *b, int gpr, int xmm);
int wcs_pmuldq(BinaryCodeBuffer *b, int dst, int src);
int wcs_pmulld(BinaryCodeBuffer *b, int dst, int src);
int wcs_pmuludq(BinaryCodeBuffer *b, int dst, int src);
int wcs_popcnt(BinaryCodeBuffer *b, int dst, int src);
int wcs_pshufd(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_psrldq_imm(BinaryCodeBuffer *b, int xmm, unsigned char imm);
int wcs_psrlq_imm(BinaryCodeBuffer *b, int xmm, unsigned char imm);
int wcs_shift_reg_imm(BinaryCodeBuffer *b, int gpr, int is_shr, unsigned char imm);
int wcs_sse_66(BinaryCodeBuffer *b, unsigned char op, int dst, int src);
int wcs_sse_66_38(BinaryCodeBuffer *b, unsigned char op, int dst, int src);
int wcs_add_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_sub_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_sub_reg_reg64(BinaryCodeBuffer *b, int dst, int src);
int wcs_test_reg_reg32(BinaryCodeBuffer *b, int gpr);
int wcs_xor_self32(BinaryCodeBuffer *b, int gpr);

#endif /* CODEGEN_BINARY_INTERNAL_H */
