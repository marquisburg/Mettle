#ifndef IR_H
#define IR_H

/* libmtlc backend IR - deliberately frontend-free.
 *
 * This header defines the backend's own intermediate representation and must not
 * depend on any frontend's AST or type system. The AST->IR lowering pass (a
 * frontend concern) lives behind ir_lowering.h, which DOES see the frontend
 * types; everything below the lowering boundary (optimizer, codegen, linker)
 * operates on this IR alone. */
#include "../simd_attr.h"
#include "../source_location.h"
#include "mtlc/intrinsic.h"
#include "mtlc/tensor.h"
#include "mtlc/type.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define IR_PROFILE_ID_NONE UINT32_MAX

/* `@simd` loop markers. A marker is an IR_OP_NOP whose `text` is
 * "@@simd:B:<id>:<mode>" (emitted just before a vectorization-requested loop)
 * or "@@simd:E:<id>:0" (just after it); <mode> is a SimdAttr. NOP is skipped by
 * every recognizer and is a no-op in every backend, so the marker never
 * perturbs vectorization or codegen. The release-stage contract verifier
 * (ir_optimize_simd_contract.c) pairs B/E by nesting, checks whether a SIMD
 * intrinsic landed between them, enforces the contract, and clears the
 * markers. Emitted by ir_lowering.c. */
#define IR_SIMD_MARKER_PREFIX "@@simd:"
/* `@unroll(n)` loop marker: an IR_OP_NOP carrying "@@unroll:<factor>" placed
 * immediately before the loop's header label. Consumed (cleared) by the
 * annotated-unroll pass; transparent to every backend like the SIMD markers. */
#define IR_UNROLL_MARKER_PREFIX "@@unroll:"

#define IR_SYSCALL_CALL_NAME "__mtl_syscall"

enum {
  IR_REWRITE_ROLE_NONE = 0,
  IR_REWRITE_ROLE_FROM = 1,
  IR_REWRITE_ROLE_TO = 2,
  IR_REWRITE_ROLE_WHERE = 3
};
#define IR_REWRITE_FROM_PREFIX "__rewrite_from__"
#define IR_REWRITE_TO_PREFIX "__rewrite_to__"
#define IR_REWRITE_WHERE_PREFIX "__rewrite_where__"

typedef enum {
  IR_OPERAND_NONE,
  IR_OPERAND_TEMP,
  IR_OPERAND_SYMBOL,
  IR_OPERAND_INT,
  IR_OPERAND_FLOAT,
  IR_OPERAND_STRING,
  IR_OPERAND_LABEL
} IROperandKind;

typedef struct {
  IROperandKind kind;
  char *name;
  /* For IR_OPERAND_STRING this is the operand's BYTE LENGTH, which is not
   * strlen(name): `\\0` is a legal escape, so a literal may carry an
   * interior NUL. `name` is still NUL-terminated, so an operand built by a pass
   * that has only a C string leaves this zero and readers fall back to strlen. */
  long long int_value;
  double float_value;
  /* IEEE-754 width of a floating operand: 32 or 64. 0 means "not a float /
   * unspecified" and callers must treat it as the default double width (64).
   * Carried so backends never have to re-derive single vs double precision
   * from scattered symbol/type lookups. */
  int float_bits;
} IROperand;

typedef enum {
  IR_OP_NOP,
  IR_OP_LABEL,
  IR_OP_JUMP,
  IR_OP_BRANCH_ZERO,
  IR_OP_BRANCH_EQ,
  IR_OP_DECLARE_LOCAL,
  /* Device storage/view. dest receives a pointer of value_type. A positive rhs
   * is a static element count in WORKGROUP or PRIVATE; rhs zero is an unbounded
   * typed view of the launch-provided WORKGROUP arena. All zero-extent views in
   * one kernel alias the same arena. */
  IR_OP_ADDRESS_SPACE_ALLOC,
  /* Collective workgroup execution + memory barrier. memory_regions is a mask
   * of MtlcMemoryRegion and memory_order is explicit. */
  IR_OP_BARRIER,
  /* Per-work-item global -> workgroup staged copy. arguments[0] is the
   * destination pointer and arguments[1] the source pointer. The fixed element
   * count/type define the byte span. COMMIT closes the current per-work-item
   * copy group; WAIT bounds the number of newer committed groups that may
   * remain pending. Backends without native async copies replay synchronously. */
  IR_OP_ASYNC_COPY,
  IR_OP_ASYNC_COMMIT,
  IR_OP_ASYNC_WAIT,
  /* Collective rank-1..5 rectangular tensor movement. The descriptor and raw
   * memory operands are complete portable semantics; an optional prepared
   * view is acceleration metadata, never a backend encoding in shared IR. */
  IR_OP_TENSOR_TRANSFER,
  /* Cooperative whole-tile D=A*B+C. arguments are A/B/C/D pointers and
   * tensor_mma carries shape, element formats, layouts, leading dimensions,
   * and collective scope without exposing a backend fragment ABI. */
  IR_OP_TENSOR_MMA,
  /* Cooperative bounded matrix region. The ordinary tensor-MMA operand bundle
   * is followed by unsigned row origin, column origin, and problem M/N/K.
   * Every in-bounds result in the descriptor-sized region is computed exactly;
   * M/N/K edges are semantic work, never implicit truncation or padding. */
  IR_OP_TENSOR_MATMUL,
  /* Cooperative in-place alpha/bias/activation over one logical tensor tile.
   * The descriptor and ordinary memory/scalar operands are complete portable
   * semantics; fragment mappings remain backend-private. */
  IR_OP_TENSOR_EPILOGUE,
  /* Commit a verified loop-carried tensor accumulator to its D tile. A
   * replaying backend treats this marker as a no-op; a residency-aware backend
   * delays the intermediate D stores until this point. */
  IR_OP_TENSOR_COMMIT,
  IR_OP_ASSIGN,
  IR_OP_ADDRESS_OF,
  IR_OP_LOAD,
  IR_OP_STORE,
  IR_OP_BINARY,
  IR_OP_UNARY,
  /* Fibonacci-style rotate: dest=next, lhs=a, rhs=b => next=a+b; a=b; b=next */
  IR_OP_ROTATE_ADD,
  IR_OP_CALL,
  IR_OP_CALL_INDIRECT,
  /* Target-neutral asynchronous GPU launch. lhs is the runtime kernel handle.
   * arguments[0..7] are grid xyz, block xyz, dynamic-shared bytes, and stream;
   * arguments[8..] are kernel arguments. argument_types is parallel to
   * arguments and is required for the kernel-argument suffix so host-runtime
   * lowering can marshal exact ABI widths without frontend knowledge. */
  IR_OP_GPU_LAUNCH,
  IR_OP_NEW,
  IR_OP_RETURN,
  IR_OP_INLINE_ASM,
  IR_OP_CAST,
  /* Vectorized idiom: count word starts in a byte buffer. Produced only by
   * ir_vectorize_simple_loops_pass when it recognizes the exact
   * "while (i<len){c=buf[i]; if(ws(c)) in_word=0; else {if(!in_word)count++;
   * in_word=1;} i++}" shape. Semantics: dest receives the number of maximal
   * non-whitespace runs in lhs[0..rhs-1] (whitespace = 0x20/0x09/0x0A/0x0D),
   * added to dest's prior value (the scalar code initializes count=0, so the
   * pass only matches when that holds). lhs = buffer base symbol, rhs = length
   * symbol/operand, dest = count symbol. Codegen lowers this to an SSE2
   * 16-bytes/iteration scan plus a scalar tail. */
  IR_OP_COUNT_WORD_STARTS,
  /* Inline memory copy: dest = dst pointer, lhs = src pointer, rhs = byte count
   * (INT). Produced by ir_memcpy_inline_pass for constant-size memcpy calls. */
  IR_OP_MEMCPY_INLINE,
  /* Horizontal sum of int32 array into int64 accumulator. dest = sum symbol
   * (added to prior value), lhs = base pointer, rhs = element count. */
  IR_OP_SIMD_SUM_I32,
  /* Horizontal sum of a uint8 array into an int64 accumulator. dest = sum
   * symbol (added to prior value), lhs = base pointer, rhs = element count.
   * Bytes are summed as unsigned (vpsadbw), matching (int64)(uint8)load. */
  IR_OP_SIMD_SUM_U8,
  /* In-place element-wise map of a uint8 buffer: for each byte b, apply a chain
   * of constant byte operations (mod 256). lhs = base pointer, rhs = element
   * count, dest = NONE. arguments hold the chain as (op_code INT, const INT)
   * pairs in application order; op_code is an IRByteMapOp. */
  IR_OP_SIMD_BYTE_MAP,
  /* Constant/invariant fill (the memset/frame-clear class): store one value
   * into every element of a buffer. Address modes, selected by arguments[1]
   * (INT):
   *   mode 0: lhs = base pointer, rhs = element BOUND; elements filled =
   *           bound - start, first element at base + (offset+start)*size.
   *           arguments[3] = start (the iv's entry value; INT 0 when the iv
   *           provably starts at zero), arguments[4] = invariant index
   *           offset (INT 0, a symbol, or a temp materialized just before
   *           this op). 32-bit index math, like the loops it replaces.
   *   mode 1: lhs = begin pointer symbol, rhs = end pointer symbol (byte
   *           length = end - begin; the element tail may overshoot `end` by
   *           up to size-1 bytes exactly as the scalar loop did)
   *   mode 2: byte-offset walk `*(base + i) <- v; i += size` with 64-bit
   *           locals: lhs = base (an int64 local), rhs = byte bound,
   *           arguments[3] = start byte offset (iv entry value). Same tail
   *           semantics as mode 1.
   * arguments[0] = element size in bytes (1/2/4/8), arguments[2] = the fill
   * value (INT immediate -- float fills carry their raw bit pattern -- or an
   * invariant SYMBOL). dest = NONE. */
  IR_OP_SIMD_FILL,
  /* Fixed 32x32 int32 matrix multiply. dest = c, lhs = a, rhs = b (pointers). */
  /* Reserved for an explicit 32x32 int32 SIMD matmul API. Do not introduce
   * this from ordinary source by function name or benchmark-shaped matching. */
  IR_OP_SIMD_MATMUL_N32,
  /* In-place signed int32 insertion sort. dest = base pointer, rhs = len. */
  IR_OP_SIMD_INSERTION_SORT_I32,
  /* Signed int32 dot product into int64. dest = sum/result, lhs = a, rhs = b,
   * arguments[0] = element count. */
  IR_OP_SIMD_DOT_I32,
  /* Signed int8 x int8 -> int32 dot product (the quantized GEMM/GEMV inner
   * loop). dest = int32 sum, lhs = a (int8*), rhs = b (int8*), arguments[0] =
   * element count. AVX2 vpmaddwd kernel. */
  IR_OP_SIMD_DOT_I8,
  /* SLP-vectorized group of K parallel int32 multiply-accumulate reductions
   * (K in {4,8}). For lane j in 0..K-1:
   *     out[out_off + j] += sum_{k=0..count-1} a[a_off + k] * b[b_off + k*bstr + j]
   * i.e. one shared scalar a[k] broadcast against K contiguous b lanes, K
   * independent accumulators stored to K contiguous outputs. Matched from the
   * instruction-level parallelism of K isomorphic accumulator chains (broadcast
   * scalar x contiguous loads) -- NOT from matmul's shape or names.
   * dest=out base ptr, lhs=a base ptr, rhs=b base ptr; arguments:
   * [0]=K, [1]=count, [2]=a_off, [3]=b_off, [4]=b_stride, [5]=out_off. */
  IR_OP_SIMD_SLP_MAC_I32,
  /* int8 x int8 -> int32 variant of SLP_MAC: the quantized GEMM tile. Same
   * operand/argument layout, but a and b are int8 arrays (byte loads, widened to
   * int32) while c (out) is int32. Same AVX2 broadcast-MAC kernel with int8
   * widening. */
  IR_OP_SIMD_SLP_MAC_I8,
  /* dst[i] = src[i]*mul+add; dest += sum of outputs. lhs=src, rhs=dst,
   * arguments[0]=len, [1]=mul, [2]=add (int32). */
  IR_OP_SIMD_SCALE_I32,
  /* dst[i] = clamp(src[i], lo, hi); dest += sum of outputs. lhs=src, rhs=dst,
   * arguments[0]=len, [1]=lo, [2]=hi (int32). */
  IR_OP_SIMD_CLAMP_I32,
  /* dst[i] = src[n-1-i]; dest += sum of outputs. lhs=src, rhs=dst,
   * arguments[0]=len. */
  IR_OP_SIMD_REVERSE_COPY_I32,
  /* Lower-bound index search over sorted int32 array:
   * dest=lo index result, lhs=arr, rhs=n, arguments[0]=key(int32).
   * dest is IN/OUT: codegen seeds the running lo from dest's prior value,
   * so the recognizer must prove the source loop initializes it to 0. */
  IR_OP_LOWER_BOUND_I32,
  /* Inclusive int32 prefix sum: dst[i]=sum(src[0..i]) in int32, dest holds
   * int64 running sum. lhs=src, rhs=dst, arguments[0]=len. */
  IR_OP_PREFIX_SUM_I32,
  /* Min/max scan over arr[1..n-1] updating dest=minv and arguments[0]=maxv;
   * caller initializes both from arr[0]. lhs=arr, rhs=n. */
  IR_OP_SIMD_MINMAX_I32,
  /* Horizontal sum of a float64/float32 array into the dest float accumulator
   * (added to dest's prior value). lhs = base pointer, rhs = element count. */
  IR_OP_SIMD_SUM_F64,
  IR_OP_SIMD_SUM_F32,
  /* Float64/float32 dot product into the dest float accumulator (added to
   * dest's prior value). lhs = a, rhs = b, arguments[0] = element count. */
  IR_OP_SIMD_DOT_F64,
  IR_OP_SIMD_DOT_F32,
  /* Float affine memory map:
   * rhs[i] = arguments[1] * lhs[i] + arguments[2] * rhs[i] + arguments[3].
   * lhs = src, rhs = dst, arguments[0] = element count. */
  IR_OP_SIMD_AFFINE_MAP_F64,
  IR_OP_SIMD_AFFINE_MAP_F32,
  /* In-place a[i] = exp(a[i]) over a float32 array (vectorized libm exp).
   * dest = array base, arguments[0] = element count. */
  IR_OP_SIMD_EXP_F32,
  /* SwiGLU gate: out[i] = silu(g[i]) * u[i] = (g[i] / (1 + exp(-g[i]))) * u[i],
   * over float32 arrays (in-place when out aliases g). dest = out base,
   * lhs = g base, rhs = u base, arguments[0] = element count. When rhs is the
   * sentinel "" the multiply is dropped (plain SiLU: out[i] = silu(g[i])). */
  IR_OP_SIMD_SILU_F32,
  /* Counted-loop reduction where each iteration adds (int64)trunc(CHAIN) to the
   * dest accumulator, with CHAIN a straight-line float64 expression in the loop
   * counter: x0 = (float64)i, then a sequence of {x*=k, x+=k, x-=k, x=k-x, x/=k}
   * steps. dest = int64 accumulator symbol; arguments[0] = trip count (a
   * compile-time INT constant); the remaining arguments are alternating
   * (op-code INT, constant FLOAT64) pairs describing the chain, applied to
   * (float64)i in order. Emitted only by ir_simd_i2f_reduce_pass after it proves
   * every per-element value fits int32 and the integer sum stays < 2^53, so an
   * AVX2 f64-lane kernel is bit-identical to the scalar loop. Direct-object
   * backend only. */
  IR_OP_SIMD_I2F_REDUCE_F64,
  /* General auto-vectorized counted unit-stride loop over a straight-line
   * float DAG. Emitted by ir_auto_vectorize_pass for loops the per-shape
   * recognizers above did not claim. The element width is carried in
   * instruction->float_bits (64 = f64x4 lanes / 8-byte elements, 32 = f32x8
   * lanes / 4-byte elements); both stride 32 bytes per vector iteration. The
   * body DAG is serialized into arguments[]:
   *   header (7 INT): [0] reduce_op (0 = element-wise map, 1 = '+' reduction)
   *                   [1] n_arrays  [2] n_nodes  [3] root_node
   *                   [4] n_consts  [5] n_scalars
   *                   [6] max_live (peak simultaneous live ymm)
   *   then n_arrays SYMBOL array-base operands (index k),
   *   then n_scalars SYMBOL loop-invariant scalar operands (read once at loop
   *   entry and broadcast -- a runtime coefficient like saxpy's `a`),
   *   then n_nodes nodes, each 3 INT operands (tag, op0, op1):
   *       tag 0=LOAD(op0=array idx) 1=IOTA 2=CONST(op0=const idx)
   *           3=ADD 4=SUB 5=MUL 6=DIV (op0,op1 = earlier node indices)
   *           7=SCALAR(op0=scalar idx)
   *           8=AND 9=OR 10=XOR (int lanes only; op0,op1 = node indices)
   *           11=SHL (int lanes only; op0 = node index, op1 = literal count),
   *   then n_consts FLOAT64 operands (the kernel narrows them to f32 when
   *   float_bits==32). The int form serializes consts as INT operands instead.
   * dest = reduction accumulator symbol (reduce_op==1) or stored array base
   * (reduce_op==0); lhs = trip count (SYMBOL or INT). Direct-object backend
   * only. The kernel replays the DAG over the packed lanes with stack-hoisted
   * constants + a scalar remainder; element-wise maps are bit-identical to the
   * scalar loop, '+' reductions reassociate like the sum/dot kernels. */
  IR_OP_SIMD_VLOOP_F64,
  /* Integer twin of IR_OP_SIMD_VLOOP_F64: int32/uint32 lanes (i32x8, 4-byte
   * elements, 32 bytes per vector iteration), same arguments[] serialization.
   * Body ops are + - * & | ^ and << by a literal count -- every one congruent
   * mod 2^32 -- so maps AND '+' reductions are BIT-EXACT against the scalar
   * loop (integer wraparound is associative; no float reassociation caveat).
   * Division, %, and >> are never emitted (not congruent / trapping). Emitted
   * by ir_auto_vectorize_int_pass. Direct-object backend only. */
  IR_OP_SIMD_VLOOP_I32,
  /* Vectorized SKIP-AHEAD for early-exit search loops (find / memchr /
   * mismatch). Replaces ONLY the loop counter's zero-init: dest(iv) = the
   * exact first index in [0, n) where the loop's exit predicate holds, else
   * n. The original scalar loop is left fully intact and re-runs from that
   * index, so it executes at most one hit iteration (+ the <lane tail) and
   * every exit path / side effect replays natively -- the recognizer only
   * has to prove the SKIPPED iterations are pure (load, compare, increment).
   * lhs = trip bound n (SYMBOL, TEMP, or INT); rhs = array base `a`.
   *   arguments[0] = predicate (INT: 0 == , 1 != , 2 < , 3 > , 4 <= , 5 >= ;
   *                  6 = first byte outside ASCII identifier continuation)
   *   arguments[1] = element kind (INT: 0 = 4-byte int32, 1 = 1-byte u8)
   *   arguments[2] = rhs kind (INT: 0 = literal, 1 = invariant scalar symbol,
   *                  2 = second array `b[i]`)
   *   arguments[3] = the rhs operand (INT literal or SYMBOL; reserved for 6)
   *   arguments[4] = first index for predicate 6 (INT or SYMBOL)
   * Predicates 0 through 5 walk an align-to-32 scalar head, then aligned
   * 32-byte blocks with vpcmpeq/vpcmpgt + movemask + bsf. Predicate 6 uses
   * SSE4.2 range scans and leaves short or page-crossing tails to the scalar
   * loop. Both forms avoid reading a page the scalar loop would not touch.
   * Two-array reads on `b` are bound-limited instead. Bit-exact: the returned
   * index is the scalar loop's first-exit index. Direct-object backend only. */
  IR_OP_SIMD_FIND,
  /* Outer-loop lane vectorization of a reduction over an outer-IV-INVARIANT
   * inner counted loop carrying one float64 accumulator (a serial recurrence,
   * e.g. a divide chain). The outer loop `while(p<P){ inner; total += iacc; p++ }`
   * has identical independent iterations; this runs 4 of them in lockstep f64x4
   * lanes to hide the inner recurrence's latency (genuinely running all the
   * inner work, 4-wide), then accumulates the (lane-identical) result into total
   * with exact scalar adds. Serialized into arguments[]; see
   * ir_outer_vectorize_pass. dest = total accumulator; lhs = outer trip count P;
   * rhs = inner trip count N. Direct-object backend only. */
  IR_OP_SIMD_OUTER_LANE_F64,
  /* Vectorized linear-congruential recurrence reduction. Replaces a counted
   * loop `state = state*A + C; sum += (int64)(state & MASK); i++` whose state is
   * a uint32 carried serially -- normally unvectorizable -- by advancing 8 lanes
   * in lockstep via the closed form state_{k+8} = A^8*state_k + (sum_{j<8}A^j)*C
   * (all mod 2^32, exact under vpmulld), masking + widening each lane to int64,
   * and accumulating. A scalar remainder finishes iters % 8. Bit-exact vs the
   * scalar loop. dest = sum accumulator symbol; lhs = trip count (SYMBOL/INT);
   * rhs = state symbol (its value at loop entry is the seed); arguments[0]=A,
   * [1]=C, [2]=MASK (all INT, compile-time). Direct-object backend only. */
  IR_OP_SIMD_LCG_U32,
  /* Software prefetch hint (prefetcht0): lhs = a TEMP holding the fully
   * computed byte address. Advisory only -- never faults, no destination, a
   * no-op in the IR interpreter. Emitted by ir_optimize_prefetch.c for
   * indirect (gather) accesses whose future address is computable early. */
  IR_OP_PREFETCH,
  /* Branchless select (conditional move): dest = (lhs != 0) ? rhs :
   * arguments[0]. lhs is the condition, rhs the then-value, arguments[0] the
   * else-value (each a temp/symbol/int). Emitted by ir_optimize_if_convert.c
   * to replace a data-dependent register-only if/else diamond, lowered to a
   * cmov so an unpredictable branch becomes straight-line code. */
  IR_OP_SELECT,
  /* `--safe`: one memory access that has not yet been proved in bounds.
   *
   * Short-lived by design. Lowering emits these, ir_safety_resolve_program()
   * runs immediately afterwards to delete the ones it can prove and rewrite
   * the rest into ordinary compares, branches and calls, and nothing past that
   * point ever sees the opcode. The optimizer, the interpreter and every code
   * generator are therefore untouched by it.
   *
   * Operands live in `arguments` rather than lhs/rhs so that passes which walk
   * the argument vector generically already treat them as inputs:
   *   arguments[0]  base pointer the access derives from (carries provenance)
   *   arguments[1]  signed byte offset applied to that base
   *   arguments[2]  bytes touched, always a constant
   *   arguments[3]  byte extent of the object when it is known statically,
   *                 or IR_SAFETY_EXTENT_UNKNOWN when only the runtime can say
   *   arguments[4]  IR_SAFETY_ACCESS_READ or IR_SAFETY_ACCESS_WRITE
   * `text` is a short source-level spelling of the access for the message. */
  IR_OP_SAFETY_CHECK
} IROpcode;

/* arguments[3] of IR_OP_SAFETY_CHECK when the object's size is not a compile
 * time constant, so the check has to ask the runtime which allocation the base
 * pointer belongs to. */
#define IR_SAFETY_EXTENT_UNKNOWN (-1)

/* arguments[4] of IR_OP_SAFETY_CHECK. Mirrors MettleSafetyAccessKind in
 * src/runtime/safety.h, which is the value passed through to the runtime. */
#define IR_SAFETY_ACCESS_READ 0
#define IR_SAFETY_ACCESS_WRITE 1

/* Fixed positions inside IR_OP_SAFETY_CHECK's argument vector. */
#define IR_SAFETY_ARG_BASE 0u
#define IR_SAFETY_ARG_OFFSET 1u
#define IR_SAFETY_ARG_SIZE 2u
#define IR_SAFETY_ARG_EXTENT 3u
#define IR_SAFETY_ARG_ACCESS 4u
#define IR_SAFETY_ARG_COUNT 5u

#define IR_GPU_LAUNCH_CONTROL_ARGS 8u

/* Chain operation codes for IR_OP_SIMD_BYTE_MAP arguments. Each step applies
 * `b = b <op> k` in uint8 (mod 256) arithmetic. The numeric values are part of
 * the IR contract between the recognizer and the backend kernel. */
typedef enum {
  IR_BYTE_MAP_ADD = 0,
  IR_BYTE_MAP_SUB = 1,
  IR_BYTE_MAP_MUL = 2,
  IR_BYTE_MAP_XOR = 3,
  IR_BYTE_MAP_AND = 4,
  IR_BYTE_MAP_OR = 5
} IRByteMapOp;

typedef enum {
  IR_TENSOR_RESIDENCY_NONE = 0,
  IR_TENSOR_RESIDENCY_START = 1,
  IR_TENSOR_RESIDENCY_UPDATE = 2,
  IR_TENSOR_RESIDENCY_COMMIT = 3
} IRTensorResidencyRole;

typedef enum {
  IR_TENSOR_RESIDENCY_SCOPE_NONE = 0,
  IR_TENSOR_RESIDENCY_SCOPE_LOOP = 1,
  IR_TENSOR_RESIDENCY_SCOPE_PIPELINE = 2
} IRTensorResidencyScope;

/* The three tensor descriptors, held out of line.
 *
 * Together they are 296 bytes, and only the handful of tensor opcodes carry any
 * of them -- yet an inline copy made every IRInstruction in the program 576
 * bytes. That is nine cache lines to touch per instruction in every pass that
 * walks the array, and one full 576-byte store per append. Moving them behind a
 * pointer takes IRInstruction to 288.
 *
 * Ownership follows the conventions the rest of IRInstruction already uses:
 *
 *   - A shallow struct copy is a BORROW. It shares this block and must never be
 *     destroyed. That is what ptx_emitter and the tensor optimizer do with their
 *     local `IRInstruction single = *in;` working copies.
 *   - append / insert / clone / snapshot COPY IN, and deep-copy this block via
 *     ir_instruction_tensor_copy.
 *   - the vector-append and insert helpers that MOVE clear the source pointer.
 *   - destroy releases it through ir_instruction_tensor_clear.
 *
 * `heap_owned` is what makes a borrow safe to modify: a caller that needs to
 * change a descriptor on a working copy points it at a stack block (via
 * ir_instruction_tensor_borrow) instead of writing through the shared one, and
 * clear() then knows not to free it.
 *
 * Those rules are what a shallow struct copy cannot enforce for itself, so
 * building with -DMETTLE_IR_TENSOR_DEBUG checks them: every owned block carries a
 * magic word that clear() invalidates before freeing, and both clear() and the
 * read accessor abort on a block whose magic is wrong. A copy site that was
 * missed then shows up as a double free or a read-after-free with a message,
 * instead of as quiet corruption. Combined with -DMETTLE_ALLOC_POISON, which
 * stamps freed memory, the check is deterministic rather than probabilistic. */
typedef struct IRTensorAux {
  int heap_owned; /* 0 for a stack or otherwise non-owned block */
#ifdef METTLE_IR_TENSOR_DEBUG
  unsigned magic;
#endif
  MtlcTensorTransferDesc transfer;
  MtlcTensorMmaDesc mma;
  MtlcTensorEpilogueDesc epilogue;
} IRTensorAux;

typedef struct {
  IROpcode op;
  /* Semantic identity for a target-neutral intrinsic call. `text` is retained
   * only for dumps/legacy source aliases; code generators must use this enum. */
  MtlcIntrinsic intrinsic;
  /* Explicit memory contract for memory-bearing intrinsics. DEFAULT values
   * exist only for compatibility; device backends normalize legacy atomics to
   * global/relaxed/device and otherwise reject invalid combinations. */
  MtlcAddressSpace address_space;
  MtlcMemoryOrder memory_order;
  /* Compare-exchange has a distinct failure/read order. It is RELAXED for
   * non-CAS atomics and must never be Release or AcqRel for CAS. */
  MtlcMemoryOrder failure_memory_order;
  MtlcMemoryScope memory_scope;
  unsigned memory_regions;
  uint32_t async_copy_element_count;
  uint32_t async_copy_transaction_bytes;
  uint32_t async_copy_pending_groups;
  MtlcAsyncCache async_copy_cache;
  /* Set only when the shared optimizer promoted an ordinary typed
   * global-load/workgroup-store pair. This is provenance for dumps,
   * diagnostics, and backend comments; it never changes copy semantics. */
  int async_copy_generated;
  /* Tensor descriptors, out of line; NULL on the overwhelming majority of
   * instructions. Read through IR_TENSOR_TRANSFER / _MMA / _EPILOGUE, which
   * substitute an all-zero block when it is absent. See IRTensorAux. */
  IRTensorAux *tensor;
  int tensor_transfer_has_prepared_view;
  /* Number of sequential D=A*B+C tiles packed into arguments. Zero and one
   * both mean the legacy single tile. For a chain, each tile contributes one
   * complete operand bundle; C[i] must be D[i-1] and every D must name the
   * same output tile. This is a semantic composition, not a fragment ABI. */
  uint32_t tensor_mma_count;
  /* A nonzero group connects an initial MMA, one or more updates, and one commit.
   * Scope records whether neutral legality proved a loop-carried region or an
   * asynchronously staged straight-line pipeline. Ordinary MMA semantics
   * remain replayable; this metadata never exposes a backend fragment ABI. */
  uint32_t tensor_residency_id;
  IRTensorResidencyRole tensor_residency_role;
  IRTensorResidencyScope tensor_residency_scope;
  SourceLocation location;
  IROperand dest;
  IROperand lhs;
  IROperand rhs;
  char *text;
  IROperand *arguments;
  /* Parallel type metadata for arguments. Entries may be NULL for ordinary
   * calls and launch controls; launch kernel arguments must carry a type.
   * The pointer array is owned, the MtlcType descriptors are borrowed. */
  MtlcType **argument_types;
  size_t argument_count;
  int is_float;
  /* Width of the floating result when is_float is set: 32 or 64. 0 means
   * unspecified and is treated as 64 (double) for backward compatibility with
   * code paths that only ever produced float64. */
  int float_bits;
  /* Set on an IR_OP_LOAD whose loaded scalar is an UNSIGNED integer (uint8/16/32),
   * recorded from the pointee type at lowering time. A 32-bit load zero-extends
   * into the 64-bit register on x86-64, but the backend otherwise sign-extends a
   * 4-byte load into a temp (it cannot recover the load's signedness from the
   * untyped destination temp). Honoring this flag keeps an unsigned value's high
   * bits clean, so the 64-bit ops the fallback emits (compare, divide, (int64)
   * widening) see the true value instead of a sign-extended one. */
  int is_unsigned;
  /* Set on a LOAD or STORE whose pointee type is `volatile`. The access is
   * observable in itself: it is never removed, never merged with another
   * access, never hoisted out of a loop and never served from a register. */
  int is_volatile;
  /* This instruction allocates heap memory at runtime even though its opcode
   * doesn't say so (today: string '+' concatenation, which codegen lowers to a
   * heap-allocating kernel). Set by ir_lowering, consumed by the `@noalloc`
   * contract checker. IR_OP_NEW and allocator calls are recognized by opcode/
   * name and don't need it. */
  int allocates;
  /* Opaque origin token: NULL for instructions the optimizer synthesized,
   * non-NULL for instructions lowered from source. The IR core and optimizer use
   * it only as a "was this synthesized?" flag (set/copy/NULL-test); the frontend
   * that produced it may cast it back to its own node type. Codegen no longer
   * reads it (types are baked into value_type). Kept as void* so the backend IR
   * carries no frontend AST dependency. */
  void *ast_ref;
  /* Backend-owned result/subject type of this instruction, baked at lowering so
   * the code generators never re-derive it from the frontend AST/TypeChecker.
   * For IR_OP_BINARY it is the expression's inferred result type; for IR_OP_CAST
   * the target type; for IR_OP_DECLARE_LOCAL the local's type. NULL when not
   * applicable or synthesized by the optimizer. */
  MtlcType *value_type;
  /* Which class of value a LOAD or STORE moves, for the whole-program alias
   * analysis (ir_optimize_alias.c). Deliberately NOT value_type: that field
   * feeds ABI classification and the GPU emitters, and a load's POINTEE type
   * is not the "result type" they read it as. 0 = unrecorded. */
  unsigned char alias_class;
  /* Which `comptime for` iteration generated this instruction, already
   * formatted the way a diagnostic says it ("iteration 1 (field `kind`)").
   * NULL for anything the programmer wrote directly.
   *
   * Diagnostics carry this on the error reporter's note frames, which exist
   * only while checking. The interpreter walks IR long after that, so `trace`
   * could show an expansion's values but never say which iteration produced
   * them. Borrowed from the type checker's expansion table, which outlives
   * every consumer of the IR. */
  const char *expansion_note;
  const char *effect_signature;
} IRInstruction;

/* Compatibility mapping used at frontend/public-call boundaries. It is the
 * only place legacy source spellings become semantic GPU operations. */
MtlcIntrinsic ir_intrinsic_from_name(const char *name);
/* Source spelling of a GPU-only opcode, for the diagnostic a CPU backend
 * raises when one reaches it. NULL when the opcode is not GPU-only. */
const char *ir_gpu_only_construct_name(IROpcode op);
const char *ir_intrinsic_name(MtlcIntrinsic intrinsic);
int ir_intrinsic_arity(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_atomic(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_compare_exchange(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_atomic_load(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_atomic_store(MtlcIntrinsic intrinsic);
MtlcTypeKind ir_intrinsic_atomic_value_kind(MtlcIntrinsic intrinsic);
MtlcTypeKind ir_intrinsic_atomic_result_kind(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_subgroup(MtlcIntrinsic intrinsic);
MtlcTypeKind ir_intrinsic_subgroup_result_kind(MtlcIntrinsic intrinsic);
int ir_tensor_mma_desc_valid(const MtlcTensorMmaDesc *desc);
int ir_tensor_epilogue_desc_valid(const MtlcTensorEpilogueDesc *desc);
int ir_tensor_transfer_desc_valid(const MtlcTensorTransferDesc *desc);
size_t ir_tensor_transfer_element_bytes(MtlcTensorElement element);
size_t ir_tensor_transfer_tile_elements(const MtlcTensorTransferDesc *desc);
size_t ir_tensor_transfer_operand_count(const MtlcTensorTransferDesc *desc,
                                        int has_prepared_view);
size_t ir_tensor_mma_operand_count(const MtlcTensorMmaDesc *desc);
size_t ir_tensor_matmul_operand_count(const MtlcTensorMmaDesc *desc);
size_t ir_tensor_epilogue_operand_count(
    const MtlcTensorEpilogueDesc *desc);
unsigned ir_tensor_mma_runtime_stride_mask(const MtlcTensorMmaDesc *desc);
size_t ir_tensor_mma_instruction_count(const IRInstruction *instruction);
int ir_tensor_mma_desc_equal(const MtlcTensorMmaDesc *lhs,
                             const MtlcTensorMmaDesc *rhs);
int ir_operand_same(const IROperand *lhs, const IROperand *rhs);
MtlcTypeKind ir_tensor_element_storage_kind(MtlcTensorElement element);

typedef struct {
  const char *label;
  IRInstruction *instructions;
  size_t instruction_count;
  size_t first_instruction;
  size_t *successors;
  size_t successor_count;
  size_t *predecessors;
  size_t predecessor_count;
} IRBasicBlock;

/* Value classes the alias analysis distinguishes. Two accesses of different
 * classes touch different memory unless the program is punning, which the
 * analysis checks for directly. Kept here so lowering and the optimizer agree
 * on the numbering. */
typedef enum {
  IR_ALIAS_CLASS_NONE = 0,
  IR_ALIAS_CLASS_POINTER,
  IR_ALIAS_CLASS_I8,
  IR_ALIAS_CLASS_I16,
  IR_ALIAS_CLASS_I32,
  IR_ALIAS_CLASS_I64,
  IR_ALIAS_CLASS_F32,
  IR_ALIAS_CLASS_F64,
  IR_ALIAS_CLASS_F16,
  IR_ALIAS_CLASS_BF16,
  IR_ALIAS_CLASS_COUNT
} IRAliasClassId;

typedef struct {
  char *name;
  uint32_t profile_id;
  char **parameter_names;
  char **parameter_types;
  size_t parameter_count;
  /* Return type NAME (e.g. "int32", "void"), mirroring parameter_types'
   * by-name representation. Resolve with the module type registry
   * (ir_program_lookup_type), same as a parameter type. NULL for none. */
  char *return_type_name;
  /* Declaration site, for debug info / --annotate-asm / --explain records that
   * used to read the origin AST node's location. */
  SourceLocation location;
  IRInstruction *instructions;
  size_t instruction_count;
  size_t instruction_capacity;
  IRBasicBlock *blocks;
  size_t block_count;
  size_t entry_block;
  int cfg_valid;
  // Function-decorator flags propagated from the AST (see ast.h):
  int is_inline;          // `@inline`  : force inline past the heuristic gate
  int is_inline_contract; // `@inline!` : every call inlines or compile error
  int is_noinline;        // `@noinline`: never inline this function
  int is_pure;            // `@pure`    : side-effect-free; enables call LICM
  /* Inferred, not declared: no instruction in this function (or in anything it
   * can reach through direct calls) writes observable state -- no store, no
   * allocation, no global write, no indirect or unknown call. Loads are
   * allowed, so unlike `@pure` the call may still fault and is never
   * speculated: LICM hoists it only under a clone of the loop's entry test.
   * Computed by the pure-call LICM pass; meaningless before it runs. */
  int is_readonly_inferred;
  int is_noalloc;         // `@noalloc` : proven allocation-free or error
  int is_test;            // `@test`    : compile-time unit test (mettle test)
  /* `@swappable`: may be replaced in a running process at a `quiesce` point.
   * The swap redirects the call, so the call has to survive: lowering also
   * sets is_noinline, because a body copied into ten callers has ten places
   * to redirect and no boundary to name. Opting in is what buys that, so a
   * function without the decorator pays nothing. */
  int is_swappable;
  /* `@naked`: emitted with no prologue, no epilogue and no frame. The body is
   * inline assembly only. */
  int is_naked;
  /* `@interrupt`: reached by the CPU on an interrupt or exception. The entry
   * saves every general-purpose register, calls the body, restores them and
   * returns with iret. */
  int is_interrupt;
  int is_rule;
  const char **effects_with;
  size_t effects_with_count;
  const char **effects_forbids;
  size_t effects_forbids_count;
  const char **effects_requires;
  size_t effects_requires_count;
  const char **effects_provides;
  size_t effects_provides_count;
  /* Set the moment a volatile load or store is appended, and never cleared.
   * The optimizer driver uses it to decide whether a function is worth
   * auditing for dropped, duplicated or reordered volatile accesses. */
  int has_volatile_access;
  int is_kernel;          // GPU entry point; ordinary functions are not entries
  int rewrite_role;
  /* `export fn`: visible outside this compilation. Two things follow. Its
   * object symbol stays global where an internal function's is local, and it
   * is reached under the platform's C ABI rather than Mettle's own internal
   * convention, because whatever calls it may not be Mettle. */
  int is_exported;
  /* `kernel(block = ...)`: the launch block shape this kernel requires.
   * All zero when undeclared. PTX emits .reqntid so the driver rejects a
   * mismatched launch instead of running it with a garbage lane mapping;
   * SPIR-V emits the LocalSize execution mode. */
  int kernel_block[3];
  int kernel_threads_per_item;
} IRFunction;

typedef struct {
  char *name;
  char *filename;
  uint64_t line;
} IRProfileEntry;

/* One debugger variable registration site (--debug-hooks): the name and
 * type are embedded in binary tables and referenced by index, because a
 * string-literal call argument's ABI differs between the MIR and fallback
 * backends (flat cstring vs string-struct pointer). */
typedef struct {
  char *name;
  char *type_name;
} IRDebugLocalEntry;

/* One named-type entry in the module type registry (backend-owned). Populated at
 * lowering; lets codegen resolve a type-name string (from an instruction's text
 * or a function's parameter_types) to an MtlcType without the frontend
 * TypeChecker's get_type_by_name. */
typedef struct {
  char *name;      /* owned */
  MtlcType *type;  /* borrowed (frontend adapter's process-lifetime arena) */
} IRTypeEntry;

typedef enum {
  IR_MODSYM_FUNCTION,
  IR_MODSYM_VARIABLE,
  IR_MODSYM_CONSTANT
} IRModuleSymbolKind;

/* One link-time address inside an aggregate global's initializer image. A
 * pointer, function pointer, or string element has no value until the linker
 * places what it refers to, so the image leaves a pointer-sized hole here and
 * the backend emits a relocation. Exactly one of `symbol` and `string` is set:
 * `symbol` names another module symbol, `string` carries literal bytes the
 * backend parks in .rdata and points at. */
typedef struct {
  size_t offset;
  char *symbol; /* owned */
  char *string; /* owned */
  size_t string_length;
  /* A `string` value is a pointer to a { chars, length } record, so the slot
   * points at a record the backend builds next to the characters; a `cstring`
   * points straight at the characters. Only read when `string` is set. */
  int string_wants_record;
} IRInitReloc;

/* One module-level symbol (global var, function, or folded constant) the code
 * generators emit or reference. Populated at lowering from the frontend symbol
 * table + AST so codegen needs neither. All MtlcType* are borrowed; strings are
 * owned. */
typedef struct {
  char *name;               /* owned */
  MtlcType *type;           /* borrowed; the symbol's type */
  IRModuleSymbolKind kind;
  int is_extern;
  int has_body;             /* functions: defined (has an IR body) vs declared */
  int is_kernel;            /* functions: GPU entry point */
  char *link_name;          /* owned; object-file linkage name, or NULL = name */
  char *effect_clause;
  long long const_value;    /* IR_MODSYM_CONSTANT: folded integer value */
  /* Global-variable initializer, evaluated to a constant at lowering. */
  int has_initializer;
  int init_is_float;
  long long init_bits;      /* numeric initializer (float carries bit pattern) */
  char *init_string;        /* owned; string-literal initializer bytes, or NULL */
  size_t init_string_length;
  /* Set when the initializer is the address of another module symbol
   * (`var p: int32* = &g_x;`, `var f: fn() -> int32 = &handler;`). The value is
   * not known until link time, so the backend reserves a pointer-sized slot and
   * emits a relocation against this name rather than a constant. */
  char *init_symbol_ref;    /* owned; referenced symbol name, or NULL */
  /* Aggregate initializer (`var t: int32[4] = [1, 2, 3, 4];`), folded to the
   * laid-out bytes of the value at type-check time. `init_bytes` is exactly the
   * type's size; `init_relocs` finishes the pointer-sized holes in it. NULL
   * when the global is an aggregate with no initializer -- that is plain .bss
   * zero-fill and needs no image. */
  unsigned char *init_bytes; /* owned */
  size_t init_bytes_size;
  IRInitReloc *init_relocs;  /* owned */
  size_t init_reloc_count;
  /* Set when the source had an initializer expression but it could not be
   * folded to a compile-time constant (or a string global's initializer wasn't
   * a string literal) -- the direct-object backend requires a constant global
   * initializer and should report this as an error rather than silently
   * zero-initializing. */
  int has_unfoldable_initializer;
  /* The source declared this global `const`. A pass may then read the
   * folded initializer as the symbol's only value; without it a global's
   * initializer says what the value STARTS as, not what it is. */
  int is_immutable;
  /* The source declared this global `export`. Something outside this
   * compilation can write it, so scanning THIS program for writes proves
   * nothing about its value and its initializer may not be folded into the
   * reads. */
  int is_exported;
  /* The source declared this global `volatile`. Reading or writing it is
   * observable in itself, so no pass may drop, merge or move one of its
   * accesses and no backend may keep its value in a register. A global's
   * access is a plain symbol operand rather than a load, so the mark travels
   * through the symbol. */
  int is_volatile;
  /* Function signature (IR_MODSYM_FUNCTION), for call ABI classification. */
  MtlcType *return_type;    /* borrowed */
  MtlcType **param_types;   /* owned array of borrowed ptrs, or NULL */
  size_t param_count;
  /* Opaque per-symbol cache for a backend consumer (the code generator builds a
   * frontend-shaped view here on first lookup). Owned by whoever sets it; the IR
   * frees it with a plain free() on destroy. */
  void *codegen_view;
} IRModuleSymbol;

/* Does this inline-assembly text bind `{name}`? An asm block reaches a global
 * through such a binding and may store to it, and nothing else in the IR
 * records that: the block is one opaque instruction carrying its own text. Any
 * analysis asking "is this global ever written" has to read it. */
int ir_inline_asm_binds_symbol(const char *assembly_text, const char *name);

typedef struct {
  IRFunction **functions;
  size_t function_count;
  size_t function_capacity;
  IRProfileEntry *profile_entries;
  size_t profile_entry_count;
  size_t profile_entry_capacity;
  IRDebugLocalEntry *debug_local_entries;
  size_t debug_local_entry_count;
  size_t debug_local_entry_capacity;
  /* Backend-owned type registry (name -> MtlcType), populated at lowering.
   * Replaces the frontend TypeChecker's get_type_by_name for the backend. */
  IRTypeEntry *type_registry;
  size_t type_registry_count;
  size_t type_registry_capacity;
  /* Backend-synthesized descriptors (currently launch parameter arrays).
   * Frontend-registered types are borrowed; these entries are owned here. */
  MtlcType **owned_types;
  size_t owned_type_count;
  size_t owned_type_capacity;
  /* Backend-owned module symbol table (globals/functions/externs + folded
   * constants), populated at lowering. Replaces codegen's frontend SymbolTable
   * lookups and its walk of the AST declaration list. */
  IRModuleSymbol *module_symbols;
  size_t module_symbol_count;
  size_t module_symbol_capacity;
  /* Whether main() takes (argc, argv), baked from the main function signature. */
  int main_wants_argc_argv;
  /* Every global whose address is taken by any function in the module, so a
   * pointer of unknown provenance reaching one function may alias a global
   * another function pointed at. Borrowed interned IR names; built on first
   * use, and only the array is owned. */
  const char **alias_globals;
  size_t alias_global_count;
  int alias_globals_computed;
  /* Set once ir_program_eliminate_dead_functions has run. The binary backend
   * treats a missing IR body as an internal error; this flag tells it a missing
   * body means "eliminated as unreachable", which is expected, not a bug. */
  int dead_functions_eliminated;
} IRProgram;

/* Reachability-ordered device call graph. `order` is postorder (callees before
 * callers), contains kernels plus ordinary helpers reachable from a kernel,
 * and never contains unrelated host functions. */
typedef struct {
  size_t *order;
  size_t count;
  unsigned char *reachable;
  size_t function_count;
} IRGpuCallGraph;

IROperand ir_operand_none(void);
IROperand ir_operand_temp(const char *name);
IROperand ir_operand_symbol(const char *name);
IROperand ir_operand_int(long long value);
/* Defaults float_bits to 64 (double) for backward compatibility. */
IROperand ir_operand_float(double value);
/* Like ir_operand_float but tags the IEEE-754 width (32 or 64). Any other
 * value is normalized to 64. */
IROperand ir_operand_float_sized(double value, int float_bits);
IROperand ir_operand_string(const char *value);
IROperand ir_operand_string_n(const char *value, size_t length);
char *ir_copy_literal_bytes(const char *value, size_t length);
/* The byte length an IR_OPERAND_STRING stands for, falling back to strlen for
 * an operand whose producer did not measure one. */
size_t ir_operand_string_length(const IROperand *operand);
IROperand ir_operand_label(const char *name);
IROperand ir_operand_copy(const IROperand *operand);
void ir_operand_destroy(IROperand *operand);

IRFunction *ir_function_create(const char *name);
/* The name a source type carries into the IR. `char` is a byte with a
 * printing convention, and everything below this line -- the optimizer's width
 * tables, the verifier, the backends' load and store widths, the debug type
 * codes -- wants the byte. Keeping the distinction in the frontend means the
 * new type kind costs no entry in twenty scattered string tables, each of
 * which would quietly stop recognizing a loop the day it was missed. */
const char *ir_backend_type_name(const char *source_name);

int ir_function_set_parameters(IRFunction *function, const char **parameter_names,
                               const char **parameter_types,
                               size_t parameter_count);
void ir_function_destroy(IRFunction *function);
int ir_function_append_instruction(IRFunction *function,
                                   const IRInstruction *instruction);
int ir_function_insert_instruction(IRFunction *function, size_t index,
                                   const IRInstruction *instruction);
enum {
  IR_EFFECT_CLAUSE_WITH = 0,
  IR_EFFECT_CLAUSE_FORBIDS = 1,
  IR_EFFECT_CLAUSE_REQUIRES = 2,
  IR_EFFECT_CLAUSE_PROVIDES = 3
};
int ir_function_set_effects(IRFunction *function, int clause,
                            const char *const *names, size_t count);
int ir_program_register_scalar_pointer_types(IRProgram *program);
/* Opaque handle over the optimizer's integer value-range analysis, for the
 * backend's narrow-home canonicalization: when the 64-bit arithmetic result
 * of the BINARY/ASSIGN at `at` provably fits `bits` (signed or unsigned), the
 * re-extension after it is redundant. Create per function; queries are
 * individually cost-capped and the tables build lazily on the first one. */
void *ir_value_range_oracle_create(const IRFunction *function);
void ir_value_range_oracle_destroy(void *oracle);
int ir_value_range_result_is_narrow(void *oracle, size_t at, int bits,
                                    int is_unsigned);
void ir_function_clear_cfg(IRFunction *function);
int ir_function_rebuild_cfg(IRFunction *function);
const IRBasicBlock *ir_function_blocks(IRFunction *function,
                                       size_t *block_count);

int ir_program_global_address_taken(IRProgram *program, const char *name);
IRProgram *ir_program_create(void);
void ir_program_destroy(IRProgram *program);
int ir_program_add_function(IRProgram *program, IRFunction *function);

/* Module type registry. register copies `name`; `type` is borrowed (owned by the
 * caller's arena) and must outlive the program. Re-registering a name updates it.
 * lookup returns NULL when absent. */
int ir_program_register_type(IRProgram *program, const char *name,
                             MtlcType *type);
MtlcType *ir_program_lookup_type(const IRProgram *program, const char *name);
int ir_program_drop_rules(IRProgram *program);

/* Module symbol table. add copies the proto (deep-copying owned strings and the
 * param_types array; MtlcType* stay borrowed) and returns the stored entry, or
 * NULL on OOM. lookup returns NULL when absent. */
IRModuleSymbol *ir_program_add_symbol(IRProgram *program,
                                      const IRModuleSymbol *proto);
const IRModuleSymbol *ir_program_lookup_symbol(const IRProgram *program,
                                               const char *name);

/* ir_lower_program and ir_lowering_set_explain are the AST->IR lowering entry
 * points. They reference frontend types (ASTNode/TypeChecker/SymbolTable) and so
 * live in the frontend-facing header ir_lowering.h, not here. */
int ir_program_dump(IRProgram *program, FILE *output);
/* Human-readable mnemonic for an opcode (e.g. "simd_dot_i8"), used by dumps and
 * the `--simd-report` diagnostics. */
const char *ir_opcode_name(IROpcode op);
/* ---- tensor descriptor block ------------------------------------------------
 *
 * Reads go through the three macros, which yield an lvalue so that both
 * `IR_TENSOR_MMA(in).k` and `&IR_TENSOR_MMA(in)` read the same as the inline
 * fields they replace. An instruction with no block reads as all zeroes, which is
 * what the zero-initialised inline fields used to give. */
const IRTensorAux *ir_instruction_tensor_ro(const IRInstruction *instruction);

#define IR_TENSOR_TRANSFER(in) (ir_instruction_tensor_ro(in)->transfer)
#define IR_TENSOR_MMA(in) (ir_instruction_tensor_ro(in)->mma)
#define IR_TENSOR_EPILOGUE(in) (ir_instruction_tensor_ro(in)->epilogue)

/* Give `dst` its own copy of `src`'s block. Used by every path that copies an
 * instruction rather than moving or borrowing it. Returns 0 only on OOM. */
int ir_instruction_tensor_copy(IRInstruction *dst, const IRInstruction *src);

/* Release `dst`'s block if it owns one, and detach it either way. */
void ir_instruction_tensor_clear(IRInstruction *instruction);

/* Seed a caller-provided (typically stack) block from `src` and attach it to
 * `dst`, so a working copy can carry a modified descriptor without writing
 * through the block its borrow shares. The block is marked not-owned, so `dst`
 * can still go through the normal destroy path. */
void ir_instruction_tensor_borrow(IRInstruction *dst, IRTensorAux *block,
                                  const IRInstruction *src);

/* Attach a zeroed caller-provided block to a freshly built instruction, for the
 * lowering and builder paths that fill in one descriptor and then append. */
#define ir_instruction_tensor_attach(dst, block) \
  ir_instruction_tensor_borrow((dst), (block), NULL)

int ir_instruction_dump(const IRInstruction *instruction,
                        char *buffer, size_t capacity);

/* --native-heap: retarget the allocation surface onto std/alloc's Mettle
 * allocator at the IR level, so the rewritten calls flow through the normal,
 * fully-optimized call path on every backend (MIR and legacy) instead of a
 * fragile backend-injected call. Rewrites, in every function:
 *   - IR_OP_NEW          -> IR_OP_CALL "mettle_heap_zeroed"(size)
 *   - call "malloc"      -> call "mettle_heap_alloc"
 *   - call "calloc"      -> call "mettle_heap_calloc"
 *   - call "realloc"     -> call "mettle_heap_realloc"
 *   - call "free"        -> call "mettle_heap_free"
 * Returns 1 on success, 0 on allocation failure. */
int ir_program_route_to_native_heap(IRProgram *program);

/* Lower semantic GPU launches to the stable host-runtime provider ABI
 * `mtlc_gpu_launch_checked(handle, grid3, block3, shared, stream, params,
 * nargs)`. Idempotent: a program with no IR_OP_GPU_LAUNCH is unchanged.
 * The provider owns host GPU runtime/API policy; this pass owns only scalar
 * argument marshalling. Run before host optimization/codegen, never for device
 * modules. */
int ir_program_lower_gpu_launches(IRProgram *program);
int ir_program_build_gpu_call_graph(const IRProgram *program,
                                    IRGpuCallGraph *graph, char **error);
void ir_gpu_call_graph_destroy(IRGpuCallGraph *graph);

/* Executable-build dead code elimination: drops every function unreachable
 * from `main`. A function is considered referenced when any instruction of a
 * live function names it in `text` (direct calls), a SYMBOL operand
 * (function-pointer uses), or a STRING operand (dispatch-by-name). Programs
 * without a `main` (library objects) are left untouched. GPU entry points root
 * unconditionally, because the driver launches them against the emitted module
 * rather than through any instruction here. `export fn` roots only when
 * `keep_exports` is set: an executable image publishes no symbol table for
 * anything outside to call through, so unless a foreign object is joining the
 * link, an exported helper nothing in the program reaches is as dead as an
 * internal one. Run it once before optimization to keep the pipeline off bodies
 * that will not ship, and again after inlining so fully-inlined helpers are
 * swept too. Returns 1 on success (including no-op), 0 on allocation failure. */
size_t ir_function_drop_dead_nops(IRFunction *function);
const IRInstruction *ir_function_find_declaration(const IRFunction *function,
                                                 const char *symbol_name,
                                                 int symbols_only);
int ir_program_eliminate_dead_functions(IRProgram *program, int keep_exports);
int ir_program_drop_rewrite_rules(IRProgram *program);

/* True when a module symbol's folded initializer image is nothing but zero
 * bytes and carries no relocations, so reserving the space costs the object
 * file nothing. `var wm: Fact[100];` is written as a zero image now that an
 * uninitialized aggregate starts zeroed, and writing those bytes out was a
 * quarter of some binaries. */
int ir_init_image_is_all_zero(const IRModuleSymbol *symbol);

#endif // IR_H
