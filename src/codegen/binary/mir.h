#ifndef CODEGEN_BINARY_MIR_H
#define CODEGEN_BINARY_MIR_H

/* Machine-IR (MIR) for the direct-object backend.
 *
 * MIR is a flat list of near-machine instructions over VIRTUAL registers. It
 * sits between the IR and the byte encoder so a real linear-scan allocator can
 * keep short-lived temps in registers instead of round-tripping every value
 * through a stack home.
 *
 * Pipeline: IR --mir_lower--> MIR(vregs) --mir_regalloc--> MIR(physregs)
 *           --mir_encode--> bytes in BinaryFunctionContext.code.
 *
 * MIR opcodes map 1:1 onto the existing binary_emit_* encoders; this header
 * defines only the data model and construction helpers. Lowering, allocation,
 * and encoding live in mir_lower.c / mir_regalloc.c / mir_encode.c. */

#include "codegen/binary/internal.h"

#include <stddef.h>
#include <stdint.h>

/* A virtual register id. Physical registers are modeled separately (see
 * MirOperand): a vreg is an allocation unit the allocator assigns to a physreg
 * or a spill slot. MIR_VREG_NONE marks "no register". */
typedef int MirVregId;
#define MIR_VREG_NONE (-1)

typedef enum {
  MIR_RC_GP = 0,  /* general-purpose integer/pointer */
  MIR_RC_XMM = 1, /* scalar float/double in an XMM lane */
  MIR_RC_VEC = 2  /* packed SIMD vector in a YMM register (auto-vectorizer) */
} MirRegClass;

/* One virtual register: its class, byte width (1/2/4/8), and, filled in by the
 * allocator, either an assigned physical register or a spill-slot rbp offset. */
typedef struct {
  MirRegClass rclass;
  int width; /* 1,2,4,8 (GP); 4,8 (XMM); per-lane byte width for VEC */
  int lanes; /* VEC only: number of packed lanes (e.g. 4 for f64x4 in YMM) */
  /* Allocation result (set by mir_regalloc). */
  int assigned;      /* 1 once allocated */
  int in_register;   /* 1 = physical register, 0 = spilled to stack */
  int phys;          /* BinaryGpRegister or BinaryXmmRegister when in_register */
  int spill_offset;  /* positive rbp-relative offset when spilled (mem = [rbp-off]) */
  /* Liveness, computed by the allocator: first def and last use as MIR indices.
   * MIR_LIVE_NONE until seen. */
  int live_start;
  int live_end;
  /* Set by the allocator when the interval spans a MIR_CALL: the value must be
   * kept in a register the callee preserves (or spilled), since a call clobbers
   * the caller-saved registers. */
  int crosses_call;
  /* Set when this value's live interval spans a loop back-edge (extended across
   * [label, branch] during liveness): it is reused on every iteration, so the
   * spill heuristic prefers to evict a NON-loop-carried value instead -- evicting
   * a loop-carried base pointer / accumulator / induction var reloads it each
   * iteration, the opposite of what we want. */
  int loop_carried;
  /* Two-address coalescing hint: a source vreg of this value's defining op that
   * dies exactly at the def, so this value can reuse its register and the
   * encoder elides the `mov dst, a` copy. -1 (MIR_VREG_NONE) when absent. */
  int coalesce_hint;
  /* Set when this value's address is taken (IR_OP_ADDRESS_OF): it must be
   * memory-resident, the allocator never assigns it a register, so every use
   * loads and every def stores through its stack home, and a store through an
   * aliasing pointer is visible to a later by-name read. */
  int address_taken;
  /* Byte size of this value's stack home when address_taken. 0 means the
   * default single 8-byte slot (scalars and DIRECT small aggregates). An
   * INDIRECT struct local sets this to its struct size rounded up to 8 so the
   * home covers the whole aggregate (field stores reach past the first 8
   * bytes). Always a multiple of 8. */
  int home_bytes;
  /* Declared scalar width (1/2/4) of an address-taken GP local, with its
   * signedness. 0 means a full 8-byte value. An aliasing pointer writes only
   * the declared bytes, so the upper bytes of the 8-byte home are undefined
   * and a by-name load must extend from this width. Loading the whole slot
   * read stack residue: `rt_unpack(x, &neg, ...)` stored 4 bytes, the 64-bit
   * read of `neg` picked up garbage above them, and every positive double
   * printed with a minus sign whenever the residue was nonzero. */
  int home_width;
  int home_signed;
  /* `--safe`: this local is described to the safety runtime, whose map resolves
   * an address to its owning object at METTLE_SAFETY_GRANULE resolution. Two
   * described objects sharing one of those units cannot both be covered -- the
   * runtime marks the unit contested and neither is ever accused -- and a small
   * overrun lands in exactly the unit an object shares with its neighbour. So
   * the home gets a unit to itself: start aligned to the granule and size
   * padded up to it. Same rule the fallback frame layout applies in abi.c. */
  int home_granule;
  /* Set for values defined by the prologue (parameters and the hidden
   * indirect-return pointer): they are ALL simultaneously live from function
   * entry, each arriving in its own incoming ABI register. Two such values must
   * therefore interfere even when each is used at only a single (shared)
   * instruction index -- their point-like [i,i] intervals would otherwise be
   * judged disjoint by the strict-overlap test and wrongly share a register
   * (the parallel param-homing move would then clobber one with the other). */
  int entry_live;
  /* Set with crosses_call when EVERY call the range spans preserves RAX (see
   * MirInst::preserves_rax). Such a value may take RAX in addition to the
   * callee-saved pool.
   *
   * This is what makes a checked inner loop keep its values in registers. A
   * `--safe` check is a comparison and a call that does not happen, but the
   * allocator has to assume it does, so everything live across it needs a
   * register the call would not clobber -- and there are only seven. A nested
   * loop carrying two pointers, two counters, two resolved spans and two
   * indices has spent them all before it reaches the value it just loaded,
   * which then goes to the stack on every iteration. One more register is the
   * difference (`transpose`). */
  int crosses_preserving_only;
  /* The same question for the volatile XMM lanes, asked of preserves_xmm. */
  int crosses_xmm_preserving_only;
} MirVreg;
#define MIR_LIVE_NONE (-1)

typedef enum {
  MIR_OPK_NONE = 0,
  MIR_OPK_VREG,      /* a virtual register (reg.vreg) */
  MIR_OPK_PHYS,      /* a fixed physical reg (reg.phys, reg.rclass) */
  MIR_OPK_IMM,       /* integer immediate (imm) */
  MIR_OPK_FIMM,      /* float immediate: raw IEEE-754 bits in `imm`; the width
                        (4 or 8) comes from the consuming instruction. Encoded by
                        staging the bits through a GP reg + movd/movq. */
  MIR_OPK_MEM,       /* [base + index*scale + disp]; base/index are vreg ids or
                        MIR_VREG_NONE, may also be RBP-relative via phys_base */
  MIR_OPK_LABEL,     /* a branch/jump target label name (sym, borrowed) */
  MIR_OPK_SYMBOL,    /* a global/extern/function symbol name (sym, borrowed) */
  MIR_OPK_STACKHOME  /* rbp-relative home of an existing named symbol: [rbp-disp].
                        Used to read/write params/locals that already own a home
                        in BinaryFunctionContext; never allocated or spilled. */
} MirOperandKind;

/* A memory address: [base + index*scale + disp]. base/index are vreg ids, or
 * MIR_VREG_NONE when absent. When phys_base_valid, the base is the fixed
 * physical register phys_base instead of a vreg (e.g. RBP for homes/spills). */
typedef struct {
  MirVregId base;
  MirVregId index;
  int scale;          /* 1,2,4,8 (0/1 when no index) */
  int disp;
  int phys_base_valid;
  int phys_base;      /* BinaryGpRegister when phys_base_valid */
} MirMem;

typedef struct {
  MirOperandKind kind;
  MirVregId vreg;       /* VREG */
  int phys;             /* PHYS: BinaryGpRegister/BinaryXmmRegister */
  MirRegClass rclass;   /* PHYS: which bank */
  long long imm;        /* IMM */
  MirMem mem;           /* MEM */
  const char *sym;      /* LABEL/SYMBOL/STACKHOME name (borrowed, interned) */
  int disp;             /* STACKHOME: rbp-relative offset (mem = [rbp-disp]) */
} MirOperand;

/* MIR opcodes. Each maps onto one or a tiny fixed sequence of binary_emit_*
 * calls in mir_encode.c. Operand roles are documented per-op where non-obvious;
 * the common shape is dst = op(a, b). */
typedef enum {
  MIR_NOP = 0,

  /* data movement */
  MIR_MOV,        /* dst <- a (reg/imm/mem load/mem store depending on kinds) */
  MIR_LEA,        /* dst(reg) <- address of a(mem) */
  MIR_LEA_LOCAL,  /* dst(reg) <- address of the spill home of local vreg a. The
                     local is forced memory-resident (address_taken) so its
                     stack slot is its canonical storage; this leas that slot. */
  MIR_LEA_GLOBAL, /* dst(reg) <- address of global symbol a.sym (RIP-relative).
                     The global stays cached, but is flushed/reloaded around
                     pointer memory ops since the alias can read/write it. */
  MIR_LEA_FUNC,   /* dst(reg) <- address of function symbol a.sym (RIP-relative).
                     Used to initialize function-pointer values without falling
                     back to the stack-home backend. */
  MIR_LEA_CSTR,   /* dst(reg) <- address of the string literal a.sym (RIP-relative
                     lea into a .rdata cstring). Carries no vreg source, so the
                     allocator ignores it. Used to pass a string-literal call
                     argument. */
  MIR_LEA_STRLIT, /* dst(reg) <- address of the string literal a.sym's
                     {chars,length} record in .rdata (the fat `string` value, as
                     opposed to MIR_LEA_CSTR's bare character bytes). Used as
                     the copy source when a literal is passed to a `string`
                     (INDIRECT aggregate) parameter or assigned to a string
                     local. */
  MIR_POPCNT,     /* dst <- popcount(a), 64-bit. */
  MIR_HEAP_NEW,   /* Win64 zeroed heap allocation (IR_OP_NEW): byte size
                     marshalled into R8 by a preceding MIR_MOV; the encoder
                     emits the self-contained GetProcessHeap +
                     HeapAlloc(HEAP_ZERO_MEMORY) sequence (own rsp bubble, so
                     it composes with the pre-reserved outgoing area); result
                     lands in RAX. A call barrier for the allocator, exactly
                     like MIR_CALL. SysV lowers IR_OP_NEW to a plain MIR_CALL
                     of calloc instead. */
  MIR_MOVZX,      /* dst <- zero-extend a (width from a.mem/src width to dst) */
  MIR_MOVSX,      /* dst <- sign-extend a */
  MIR_LOAD_GLOBAL,/* dst <- value of global scalar a(SYMBOL); width/is_unsigned
                     give the load size and signedness. Emitted once at entry to
                     cache a global in a register (leaf fns only). */
  MIR_STORE_GLOBAL,/* global scalar a(SYMBOL) <- b(value vreg); width gives the
                      store size. Emitted before each return to write a
                      register-promoted global back to memory (leaf fns only). */

  /* integer ALU: dst = dst OP a   (two-address; lowering pre-copies into dst) */
  MIR_ADD,
  MIR_SUB,
  MIR_AND,
  MIR_OR,
  MIR_XOR,
  MIR_IMUL,       /* dst = dst * a (or dst = a * imm via b when IMM present) */
  MIR_NEG,        /* dst = -dst */
  MIR_NOT,        /* dst = ~dst */
  MIR_SHL,        /* dst <<= a (a is IMM or CL/phys) */
  MIR_SHR,        /* logical >> */
  MIR_SAR,        /* arithmetic >> */

  /* signed/unsigned divide: uses RDX:RAX, result in RAX(quot)/RDX(rem).
   * Modeled with fixed-phys constraints; lowering sets a=divisor vreg. */
  MIR_CQO,        /* sign-extend RAX into RDX */
  MIR_XOR_RDX,    /* zero RDX (unsigned divide) */
  MIR_IDIV,       /* signed divide RDX:RAX / a */
  MIR_DIV,        /* unsigned divide */
  MIR_MULHI,      /* dst = high 64 bits of (a * b); is_unsigned picks mul vs
                     imul. b is the magic IMM (or a reg). Uses RAX:RDX like a
                     divide; emitted by constant-divisor strength reduction. */

  /* compares + materialization */
  MIR_CMP,        /* flags = a - b */
  MIR_TEST,       /* flags = a & b */
  MIR_SETCC,      /* dst(8-bit) <- cc (imm carries x86 setcc opcode) */
  MIR_CMOVCC,     /* dst <- a if cc (imm carries cmov opcode) */

  /* control flow */
  MIR_JMP,        /* -> label */
  MIR_JCC,        /* test a; cc -> label (cc carries jcc opcode) */
  MIR_CMPBR,      /* cmp a,b; cc -> label (fused compare-and-branch) */
  MIR_JMP_TABLE,  /* jump through aux's label table, indexed by a */
  MIR_LABEL,      /* defines label (sym) at this point */
  MIR_PREFETCH,   /* prefetcht0 [a]: a is a MEM operand (usually [vreg+0]).
                     A read-only use of the address register(s); no def, and
                     the access never faults. */
  MIR_CMOV,       /* dst = (a != 0) ? b : dst. dst is PRE-LOADED with the
                     else-value by a preceding MIR_MOV, so its live range
                     starts there and overlaps a/b at this point (forcing
                     distinct registers). Encodes to `test a,a; cmovnz dst,b`.
                     b must be a register (cmov has no immediate source). */

  /* calls / return (Stage 3 for full ABI; declared now for completeness) */
  MIR_CALL,       /* call sym; clobbers volatiles */
  MIR_CALL_INDIRECT, /* call a(reg); clobbers volatiles */
  /* `memcpy`/`memset` performed inline as a string operation instead of called.
   * Operands arrive in the ordinary call-argument registers, marshalled by the
   * same preceding MIR_MOVs a call would use, and RAX is left holding the
   * destination so the return value moves out exactly as a call's would.
   *
   * These exist because the fallback emitter has always inlined them, and a
   * plain call reaches the runtime's byte-at-a-time definition instead -- so a
   * function moving to the register allocator would otherwise get ~8x slower at
   * copying, which is how this was found. RSI and RDI are nonvolatile and are
   * saved and restored around the sequence, so a value the allocator parked in
   * either survives. */
  MIR_REP_MOVSB,  /* memcpy(arg0, arg1, arg2) -> rax */
  MIR_REP_STOSB,  /* memset(arg0, arg1, arg2) -> rax */
  MIR_SYSCALL,
  MIR_STORE_OUTARG,/* store outgoing stack call argument a to [rsp + b.imm].
                      Used for the 5th+ GP argument (beyond the ABI's argument
                      registers); the prologue reserves the outgoing region. The
                      encoder adds outgoing_indirect_bytes (the struct-arg copy
                      region sits below the shadow/stack-arg area). */
  MIR_LEA_OUTARG, /* dst <- lea [rsp + a.imm]: address of a slot in the outgoing
                     INDIRECT struct-argument copy region (at the bottom of the
                     frame, rsp-relative). Used to pass a struct by value. */
  MIR_TRAP,       /* terminal runtime trap: puts(a.sym)+exit(1). a.sym is the
                     abort message. Reached only on a cold guard-fail path and
                     never returns, so it needs no vreg operands and the
                     allocator treats it as a non-call (its volatile clobbers
                     never reach the normal path). */
  MIR_INLINE_ASM,
  MIR_RET,        /* function return (epilogue emitted separately) */

  /* float scalar (Stage 3) */
  MIR_FADD,
  MIR_FSUB,
  MIR_FMUL,
  MIR_FDIV,
  /* Bitwise XOR of the whole register, in the float domain. Its only caller is
   * negation, which flips the sign bit: `0 - x` gets every float right except
   * zero, where IEEE 754 asks for -0.0 and the subtract yields +0.0, and it
   * cannot flip the sign of a NaN at all. */
  MIR_FXOR,
  /* f64x2 lane ops for the pair vectorizer (vreg width 16). Three-address:
   * dst is written whole, a/b are read. FDUP broadcasts a's low lane to both
   * lanes (vmovddup); FEXTHI copies a's high lane into dst's low lane
   * (vunpckhpd dst,a,a), so a scalar consumer can read lane 1. */
  MIR_FDUP,
  MIR_FEXTHI,
  MIR_CVTSI2F,    /* xmm dst <- int a */
  MIR_CVTF2SI,    /* gp dst  <- float a (truncating) */
  MIR_CVTF2F,     /* float width convert (sd<->ss) */
  MIR_UCOMIS,     /* float compare -> flags */
  MIR_FSETCC,     /* dst <- (a CMP b) as 0/1: ucomis a,b; setcc; movzx (cc set) */
  MIR_FCMPBR,     /* ucomis a,b; jcc -> label (fused float compare-and-branch) */
  MIR_MOVD_TO_XMM,
  MIR_MOVD_TO_GP,
  MIR_CVTPH2PS,
  MIR_CVTPS2PH,

  /* packed SIMD (auto-vectorizer; see VECTORIZER_DESIGN.md). width = per-lane
   * bytes, lane count from the vreg. */
  MIR_VADD,        /* vaddpd/vaddps */
  MIR_VSUB,
  MIR_VMUL,
  MIR_VDIV,
  MIR_VCVTSI2F,    /* vcvtdq2pd: int32 lanes -> f64 lanes */
  MIR_VCVTF2SI,    /* vcvttpd2dq: f64 lanes -> int32 lanes (truncating) */
  MIR_VLOAD,       /* vmovupd dst <- [mem] */
  MIR_VSTORE,      /* vmovupd [mem] <- a */
  MIR_VBROADCAST,  /* vbroadcastsd: scalar/imm -> all lanes */
  MIR_VIOTA,       /* lane i <- base + i (induction vector) */
  MIR_VHREDUCE,    /* horizontal add/min/max of all lanes -> scalar xmm */

  /* SLP multiply-accumulate kernel, emitted inline inside an otherwise
   * register-allocated function (so the surrounding outer loops keep MIR-quality
   * codegen instead of dropping the whole function to the spill-everything
   * fallback). Call-like: the lowering marshals a_ptr->RCX, b_ptr->RDX,
   * out_ptr->R8, count->R9 with preceding MIR_MOVs (exactly like call args), and
   * this op emits the pure inner loop. dst.imm = K (4 or 8); a.imm = row stride
   * in BYTES (baked as an imm32 b-advance). Clobbers RAX/RCX/RDX/R8/R9/R10/R11 +
   * xmm0..3, so the allocator treats it like a call (no live value crosses it in
   * a volatile register). */
  MIR_SIMD_SLP_MAC,

  /* Inline element-counted memset/fill kernel (IR_OP_SIMD_FILL), run in place so
   * the surrounding function keeps register-allocated codegen instead of
   * dropping to the spill-everything fallback. Call-like: the lowering marshals
   * base->RCX, element_count->R8, value->RAX with preceding MIR_MOVs, and this op
   * emits the splat-build + 16-byte-store loop + scalar tail. dst.imm = element
   * size in BYTES (1/2/4/8). Covers only the no-offset, no-live-iv-writeback
   * subset (the frame-clear / `a[i] = c` shape); other fill forms stay in the
   * fallback. Clobbers RAX/RCX/RDX/R8/R9 + xmm0, so the allocator treats it like
   * a call. */
  MIR_SIMD_FILL,

  /* Inline float32 affine-map kernel (IR_OP_SIMD_AFFINE_MAP_F32, the
   * `dst[i] = a*src[i] + b*dst[i] + c` / float-copy class) run in place so the
   * function keeps register-allocated codegen. Call-like: the lowering marshals
   * src->RCX, dst->RDX, count->R8 with preceding MIR_MOVs; this op materializes
   * the (compile-time) coefficient broadcasts and emits the AVX2 loop + scalar
   * tail + vzeroupper. dst.imm/a.imm/b.imm carry the a/b/c float bits; cc carries
   * b_is_one|b_is_zero<<1|c_is_zero<<2. Clobbers RAX/RCX/RDX/R8/R9/R10 + ymm0-5. */
  MIR_SIMD_AFFINE_MAP_F32,

  /* Inline float64 affine-map kernel (IR_OP_SIMD_AFFINE_MAP_F64, the saxpy
   * `dst[i] = a*src[i] + b*dst[i] + c` class) run in place so the function keeps
   * register-allocated codegen instead of dropping to the spill-everything
   * fallback. Identical shape to the F32 variant but 4-wide f64 (vfmadd231pd):
   * lowering marshals src->RCX, dst->RDX, count->R8; this op materializes the
   * 64-bit coefficient broadcasts and emits the AVX2 loop + scalar tail +
   * vzeroupper. dst.imm/a.imm/b.imm carry the a/b/c double bits; cc carries
   * b_is_one|b_is_zero<<1|c_is_zero<<2. Clobbers RAX/RCX/RDX/R8/R9/R11 + ymm0-5. */
  MIR_SIMD_AFFINE_MAP_F64,

  /* Inline float32 SiLU/SwiGLU gate (IR_OP_SIMD_SILU_F32) run in place. Call-like:
   * the lowering marshals g/out->RCX, u->RDX (SwiGLU only), count->R8; this op
   * emits the AVX2 exp-poly SiLU loop. dst.imm = has_mul (1 = SwiGLU `* u[i]`).
   * Clobbers RAX/RCX/RDX/R8/R9/R10/R11 + ymm0-7 and reserves a scratch frame. */
  MIR_SIMD_SILU_F32,

  /* Inline general auto-vectorized loop kernel (IR_OP_SIMD_VLOOP_F64 maps) run
   * in place so the function keeps register-allocated codegen. The DAG is too
   * large for MirOperands, so the op borrows a pointer to the source
   * IRInstruction in `aux`; the lowering marshals the <=3 distinct base pointers
   * into RCX/RDX/R8/R9 and the element count into the next arg register (the
   * kernel moves it to R10 -- R10/R11 are MIR scratch and unsafe to marshal
   * into). Call-like: clobbers the caller-saved set + ymm0-5. Maps only. */
  MIR_SIMD_VLOOP,

  /* Generic inline legacy kernel. The five opcodes above each hard-wire one
   * kernel: a MIR opcode, a name, an encoder case, and four enumerations in the
   * allocator. This one covers the rest of the family with no per-kernel MIR
   * surface at all -- a kernel joins by adding a row to the table in
   * mir_kernel.c and nothing else.
   *
   * The difference is how operands reach the kernel. Those five marshal into
   * fixed physical registers, so each needs the lowering to know exactly which
   * register the kernel reads which operand from. This one stages every
   * TEMP/SYMBOL operand of the source IR instruction into a frame slot and
   * publishes the slot addresses on the context; the kernel's own
   * emit_operand_load / emit_destination_store calls then hit those slots
   * instead of the fallback's named stack homes, wherever in its body they
   * happen to be and into whatever register it happens to want. Kernels with
   * several outputs, conditional operand loads, or an operand read twice into
   * different registers all work without the bridge knowing anything about
   * them.
   *
   * `aux` borrows a MirKernelAux (owned by the MirFunction) naming the source
   * instruction, the table row, and the staging vregs; the surrounding MIR_MOVs
   * fill the slots before and read them back after. Call-like: treated as a
   * clobber barrier by the allocator, plus whatever callee-saved registers the
   * row declares. */
  MIR_IR_KERNEL,

  MIR_OPCODE_COUNT
} MirOpcode;

/* True for the inline-kernel opcodes: ops that run a whole vector kernel in
 * place, clobbering the caller-saved set the way a call does. The allocator and
 * the annotator ask this rather than each listing the set, so a new kernel
 * opcode is one edit here instead of five. */
int mir_op_is_inline_kernel(MirOpcode op);

/* The volatile XMM lanes MIR allocates scalar floats from. Shared because a
 * preserving call has to save exactly the set the allocator hands out; the two
 * drifting apart would lose a float across a check. XMM4/XMM5 are left as
 * encoder scratch. */
#define MIR_XMM_POOL_COUNT 4
extern const BinaryXmmRegister MIR_XMM_POOL[MIR_XMM_POOL_COUNT];

/* One MIR instruction. dst/a/b are the general operand slots; mem is the address
 * for load/store/lea; width is the operation width in bytes; cc holds an x86
 * condition opcode for SETCC/CMOVCC/JCC. ir_index records the source IR
 * instruction (for debug line markers); -1 if synthetic. */
typedef struct {
  MirOpcode op;
  MirOperand dst;
  MirOperand a;
  MirOperand b;
  int width;       /* 1/2/4/8 */
  int is_float;
  int is_unsigned; /* affects shifts, divides, compares, extensions */
  unsigned char cc;/* x86 condition opcode for SETCC/CMOVCC/JCC */
  int ir_index;    /* source IR index, or -1 */
  const void *aux; /* MIR_SIMD_VLOOP: borrowed const IRInstruction* (the DAG) */
  /* MIR_CALL: the encoder saves and restores RAX around this call, so a value
   * may live in RAX across it. Set for the `--safe` check, which is entered
   * only when the comparison in front of it fails -- on a correct program,
   * never. See MirVreg::crosses_preserving_only. */
  int preserves_rax;
  /* MIR_CALL: likewise for the volatile XMM lanes (MIR_XMM_POOL). Set for the
   * `--safe` check and for the span resolution the check compares against.
   * Span is a real call that really runs, but the compiler hoists it in front
   * of the loop, so paying eight instructions there buys the loop body its
   * float accumulator -- otherwise the accumulator is reloaded and restored
   * once per element. See MirVreg::crosses_xmm_preserving_only. */
  int preserves_xmm;
} MirInst;

/* Upper bound on the operands one inline kernel stages through frame slots.
 * Only TEMP/SYMBOL operands need a slot (an immediate or string literal is
 * materialized by the kernel itself), and the widest kernel in the table reads
 * six. A kernel instruction with more defers to the fallback. */
#define MIR_KERNEL_MAX_SLOTS 8

/* The bridge payload for one MIR_IR_KERNEL: which kernel to run, over which IR
 * instruction, and where its operands were staged.
 *
 * `slot_vreg` holds one address-taken vreg per DISTINCT staged value -- keyed by
 * value, not by operand, because two operand positions can name the same value
 * (a kernel that accumulates into its own dest reads and writes one variable).
 * Two slots for one value would each be filled with it and each read back, and
 * whichever reload ran last would win -- dropping the kernel's result whenever
 * the stale copy came second. `operand_slot[i]` maps operand[i] onto its slot.
 *
 * The MIR lowering fills the slots before the kernel and reads them back after;
 * the encoder turns each slot into a frame address for the kernel's own operand
 * loads and stores. */
typedef struct {
  const IRInstruction *ir; /* borrowed: the IR outlives this codegen */
  int kernel_index;        /* row in the mir_kernel.c table */
  int slot_count;
  MirVregId slot_vreg[MIR_KERNEL_MAX_SLOTS];
  int operand_count;
  const IROperand *operand[MIR_KERNEL_MAX_SLOTS]; /* borrowed, address-matched */
  int operand_slot[MIR_KERNEL_MAX_SLOTS];
} MirKernelAux;

/* A pooled (loop-invariant) float constant: its IEEE bits at `width`, and the
 * vreg materialized once near the loop that first uses it. */
typedef struct {
  uint64_t bits;
  int width;
  MirVregId vreg;
} MirFConst;

/* A pooled (loop-invariant) 64-bit integer constant: its raw value and the GP
 * vreg materialized once near the loop that first uses it. Used to hoist the
 * div/mod magic-multiply constant out of a loop so it is not re-materialized
 * with a 10-byte movabs every iteration. */
typedef struct {
  int64_t value;
  MirVregId vreg;
} MirIConst;

/* An incoming parameter: which vreg it lives in, its ABI argument index, and
 * how it must be extended from the (possibly narrow) incoming register to the
 * 64-bit value MIR computes with. */
typedef struct {
  MirVregId vreg;
  int arg_index; /* positional index among all parameters */
  int width;     /* 1/2/4/8 */
  int is_signed; /* sign-extend (1) vs zero-extend (0) into 64 bits */
  int is_float;  /* arrives in an XMM register (float32/float64) */
  int sysv_eightbytes;
  int sysv_in_memory;
  int sysv_sse[2];
  int sysv_size;
  int sysv_direct_sse;
  MirVregId sysv_storage;
} MirParam;

/* Upper bound on parameters a MIR function can take. The first few arrive in
 * ABI registers; the rest are homed from the caller's stack frame. 16 covers
 * essentially all real signatures while keeping the fixed per-function param
 * arrays small. */
/* A call with more arguments than this drops the whole enclosing function off
 * the MIR backend and onto the baseline emitter, which reserves no outgoing
 * argument area in its prologue and overwrites the caller's own locals. That
 * made a 17-argument call silently corrupt a caller's double.
 *
 * Everything keyed off this constant is a fixed-size array that scales with
 * it, so raising it is cheap; the ceiling exists to bound those arrays, not
 * because the lowering has a 16-argument assumption. */
#define MIR_MAX_PARAMS 32

typedef struct {
  MirVreg *vregs;
  size_t vreg_count;
  size_t vreg_capacity;

  MirInst *insns;
  size_t insn_count;
  size_t insn_capacity;

  /* Owned label-name strings synthesized by layout passes (e.g. cold-block
   * sinking). The encoder strdups label names into its tables, so these only
   * need to outlive mir_encode; freed in mir_function_destroy. */
  char **owned_syms;
  size_t owned_sym_count;
  size_t owned_sym_capacity;

  /* Owned side payloads pointed at by MirInst.aux (currently the MirKernelAux
   * of each MIR_IR_KERNEL). They must outlive the encoder, which reads them
   * after allocation has filled in the staging vregs' frame offsets; freed in
   * mir_function_destroy. */
  void **owned_aux;
  size_t owned_aux_count;
  size_t owned_aux_capacity;

  /* Borrowed: the function context owning stack homes, ABI, fixup tables, and
   * the output code buffer; and the code generator (for type queries, fixup
   * resolution, and error reporting). Not owned by the MIR function. */
  BinaryFunctionContext *context;
  CodeGenerator *generator;

  /* Incoming parameters (GP only in Stage 2), consumed by the encoder prologue
   * to move ABI arg registers into the param vregs with correct extension. */
  MirParam params[MIR_MAX_PARAMS];
  size_t param_count;

  /* INDIRECT struct return (Win64: hidden out-pointer in RCX, SysV: RDI). When
   * set, the prologue homes that register into indirect_return_vreg (shifting
   * every user parameter up one ABI slot), and each RETURN copies the struct
   * into [indirect_return_vreg] and leaves the pointer in RAX. */
  int returns_indirect;
  int indirect_return_size;       /* struct size in bytes (>8, INDIRECT) */
  MirVregId indirect_return_vreg; /* holds the hidden out-pointer */

  /* A sub-64-bit integer return type: its byte width (1/2/4) and signedness.
   * RETURN canonicalizes the value to 64 bits (sign/zero-extend) before `mov
   * rax` so callers using the full register read no garbage. 0 = not narrow. */
  int scalar_return_width;
  int scalar_return_signed;

  /* A float return type: declared width in bits (32/64). RETURN converts the
   * value to this width before placing it in XMM0, a float64-tracked temp
   * returned from a float32 function must cvtsd2ss, not pass through raw. */
  int float_return_bits;

  /* Divmod fusion: when `x / d` and `x % d` appear together, one div produces
   * both quotient (RAX) and remainder (RDX). Lowering the first of the pair
   * captures both and records the sibling's IR dest name -> the vreg holding the
   * result it needs, so the sibling lowers to a plain move (no second div). */
  struct {
    const char *name; /* sibling IR dest temp/symbol name (borrowed) */
    MirVregId vreg;   /* vreg already holding its quotient/remainder */
  } divmod_precomp[16];
  size_t divmod_precomp_count;

  /* Loop-invariant float constants materialized once near their first hot loop. */
  MirFConst *fconsts;
  size_t fconst_count;
  size_t fconst_capacity;

  /* Loop-invariant 64-bit integer constants (div/mod magic numbers) materialized
   * once near their first hot loop instead of re-emitted per iter. */
  MirIConst *iconsts;
  size_t iconst_count;
  size_t iconst_capacity;

  /* Bytes of spill area the allocator appended below the existing frame; the
   * encoder grows the prologue allocation by this much. */
  int spill_bytes;

  /* Frame slot where a RAX-preserving call parks RAX, in the same form as a
   * vreg's spill_offset. Zero when the function has no such call. A slot rather
   * than a push: the call's stack arguments and shadow space are addressed off
   * rsp, so moving rsp between them and the call would misplace both. */
  int preserve_slot;
  /* Far end of the block the same call parks the volatile XMM lanes in, one
   * eight-byte slot per MIR_XMM_POOL entry (MIR floats are scalars). Lane i
   * lives at frame_base - preserve_xmm_slot + i*8. */
  int preserve_xmm_slot;

  /* Max bytes of outgoing stack-argument space any call in this function needs
   * (for calls with more GP arguments than the ABI has argument registers).
   * Reserved once at the bottom of the frame, above the shadow space, so calls
   * write stack args at a fixed rsp offset without adjusting rsp in-body. */
  int outgoing_stack_bytes;

  /* Max bytes any single call needs for copying INDIRECT (by-value) struct
   * arguments. The Win64/SysV ABI passes such a struct as a pointer to a
   * caller-made copy; this region (at the very bottom of the frame, below the
   * shadow space) holds those copies. 16-aligned. */
  int outgoing_indirect_bytes;

  /* Set by the encoder when it emits an inline vector kernel (MIR_SIMD_SLP_MAC).
   * Such a kernel leaves the YMM upper halves dirty; the epilogue emits one
   * vzeroupper before returning so a caller using legacy SSE pays no AVX->SSE
   * transition penalty. Doing it once per function (not per kernel invocation)
   * keeps tiled inner loops cheap. */
  int used_inline_vector;

  /* Set during lowering when this function passes a float argument to a call in
   * an XMM register (XMM0..XMM3). Those registers are then removed from the
   * float allocation pool for the whole function (only the callee-saved XMM8..15
   * remain), exactly as the GP integer arg registers are never allocatable: it
   * guarantees no allocated value ever sits in an outgoing XMM argument register,
   * so the sequence of arg-homing moves before a call can never clobber a
   * not-yet-consumed argument source (the parallel-move hazard for 2+ float
   * args). Single-float-arg calls are safe regardless, but the exclusion is
   * applied uniformly for simplicity. */
  int has_xmm_arg_call;

  /* --annotate-asm: index of the IR instruction currently being lowered. The
   * mir_emit chokepoint stamps it onto every MirInst whose ir_index is still
   * unset (-1), so each emitted op can be traced back to its source line. Inert
   * unless the annotator is enabled. */
  int cur_ir_index;

  const IRFunction *ir_function;

  int reserve_rbx;

  size_t incoming_arg_slots;

  int returns_sysv_registers;
  int sysv_return_eightbytes;
  int sysv_return_sse[2];
  int sysv_return_size;

  int has_error;

  /* Label name -> instruction index, built on demand by mir_label_index.
     Open addressed, power-of-two capacity, 0 = empty and any other value is
     index+1. Only indices are stored: every probe re-reads the instruction it
     names and compares the label there, so a stale entry fails to match and
     falls back to a rescan rather than answering wrongly. That is what lets
     the cache survive passes that rewrite the stream without telling it. */
  size_t *label_slots;
  size_t label_slot_capacity;
  size_t label_slot_insns;
} MirFunction;

/* ---- construction ------------------------------------------------------- */

#define MIR_PARAM_SLOTS (2 * MIR_MAX_PARAMS + 1)

int mir_param_layout(const MirFunction *fn, const BinaryAbi *abi,
                     BinaryArgLocation *locs, size_t *first_slot,
                     size_t *count_out);

void mir_function_init(MirFunction *fn, BinaryFunctionContext *context);
void mir_function_destroy(MirFunction *fn);

/* Create a fresh virtual register; returns its id or MIR_VREG_NONE on OOM
 * (which also sets fn->has_error). */
MirVregId mir_new_vreg(MirFunction *fn, MirRegClass rclass, int width);

/* Append an instruction; returns 0 on OOM (and sets fn->has_error). */
int mir_emit(MirFunction *fn, const MirInst *inst);

/* Take ownership of `block` (freed by mir_function_destroy) and return it, or
 * NULL on OOM (which also sets fn->has_error; `block` is freed). */
void *mir_function_own_aux(MirFunction *fn, void *block);

#define MIR_MAX_JUMP_TABLES 64

typedef struct {
  char **labels;
  size_t count;
} MirJumpTable;

/* ---- inline kernel table (mir_kernel.c) --------------------------------- */

/* One legacy vector kernel the MIR backend can run in place. `emit` is the
 * fallback emitter, called unchanged -- the operand bridge makes its own
 * operand loads and stores resolve to the staging slots. */
typedef struct {
  IROpcode ir_op;
  const char *name;
  int (*emit)(CodeGenerator *generator, BinaryFunctionContext *context,
              const IRInstruction *instruction);
  /* Callee-saved GP registers the kernel writes without preserving, as a
   * 1u<<BinaryGpRegister mask. The allocator keeps live values out of them
   * across the kernel; the caller-saved set is already handled by treating the
   * kernel as a call barrier, so only the exceptions are listed. */
  unsigned gp_clobbers;
} MirIrKernel;

#define MIR_ASM_MAX_BINDS 16

typedef struct {
  const IRInstruction *ir;
  int count;
  const char *names[MIR_ASM_MAX_BINDS];
  MirVregId vregs[MIR_ASM_MAX_BINDS];
} MirAsmAux;

/* Row for `op`, or NULL if no kernel handles it. */
const MirIrKernel *mir_ir_kernel_for_op(IROpcode op);

/* Row `index`, or NULL if out of range. Indices are stable for a build. */
const MirIrKernel *mir_ir_kernel_at(int index);

/* Index of `op`'s row, or -1. */
int mir_ir_kernel_index_for_op(IROpcode op);

/* Operand builders. */
MirOperand mir_op_none(void);
MirOperand mir_op_vreg(MirVregId v);
MirOperand mir_op_phys(int phys, MirRegClass rclass);
MirOperand mir_op_imm(long long value);
MirOperand mir_op_fimm(uint64_t ieee_bits);
MirOperand mir_op_label(const char *name);
MirOperand mir_op_symbol(const char *name);
MirOperand mir_op_mem_vreg(MirVregId base, MirVregId index, int scale, int disp);

/* Debug dump of a MIR function to a FILE (used under METTLE_MIR_DUMP). */
void mir_function_dump(const MirFunction *fn, FILE *out);

/* Human-readable mnemonic for a MIR opcode (e.g. "mov", "simd_fill"). Used by
 * the dump and by the --annotate-asm codegen annotator. */
const char *mir_opcode_name(MirOpcode op);

/* ---- passes ------------------------------------------------------------- */

/* Assign every vreg a physical register or spill slot via linear scan. Returns
 * 0 on failure (sets fn->has_error). Defined in mir_regalloc.c. */
int mir_regalloc(MirFunction *fn);

/* The two XMM registers the encoder stages values through. They must be
 * volatile and must not be argument registers, or a value staged for one
 * argument lands in the register an earlier one was marshalled into.
 *
 * MS-x64 carries float arguments in xmm0-3, so xmm4/xmm5 are free. SysV
 * carries EIGHT, xmm0-7, so those two are arguments there and the scratch pair
 * moves up to xmm8/xmm9, which neither convention passes anything in. The
 * allocator drops whichever pair is live from its non-volatile float pool. */
static inline BinaryXmmRegister mir_xmm_scratch_a(void) {
  return code_generator_binary_active_abi()->counts_classes_separately
             ? BINARY_XMM8
             : BINARY_XMM4;
}

static inline BinaryXmmRegister mir_xmm_scratch_b(void) {
  return code_generator_binary_active_abi()->counts_classes_separately
             ? BINARY_XMM9
             : BINARY_XMM5;
}

static inline int mir_xmm_is_encoder_scratch(BinaryXmmRegister reg) {
  return reg == mir_xmm_scratch_a() || reg == mir_xmm_scratch_b();
}

static inline int mir_fsetcc_unordered_cc(unsigned char cc) {
  if (cc == 0x94) {
    return 0x9B;
  }
  if (cc == 0x95) {
    return 0x9A;
  }
  return -1;
}

/* Encode an allocated MIR function into fn->context->code, emitting prologue and
 * epilogue and populating the context's label/relocation tables. Returns 0 on
 * failure. Defined in mir_encode.c. */
int mir_encode(MirFunction *fn);

/* ---- driver hooks (defined in mir_lower.c) ------------------------------ */

int mir_rewrite_string_concat_calls(IRFunction *ir_function);

/* Lower + allocate + encode a function into context->code (full
 * prologue..epilogue, fixups resolved). Returns 0 on failure. */
int code_generator_binary_emit_function_via_mir(
    CodeGenerator *generator,
    IRFunction *ir_function, BinaryFunctionContext *context);

#endif /* CODEGEN_BINARY_MIR_H */
