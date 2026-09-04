# GPU offload

libmtlc has two GPU code generators, NVIDIA PTX and SPIR-V (OpenCL),
both emitted from the same IR with no `nvcc`, no `cudart`, and no LLVM. Through
the reference frontend, kernels are written in Mettle, compiled to a `.ptx`
module with `--emit-ptx` (or a `.spv` module with `--emit-spirv`), and, for the
CUDA path, launched from a normal Mettle host program via the
[`std/gpu`](standard-library.md#stdgpu) bindings and the `dispatch` statement.
(A frontend driving libmtlc directly reaches the same generators through
`mtlc_emit`; see [Writing a frontend for libmtlc](embedding.md).)

The model is two-stage and explicit: kernels live in their own file, the
host manages device memory itself, and `dispatch` only performs the launch. This
mirrors how real GPU code manages persistent VRAM.

## Writing a kernel

A kernel file is compiled with `mettle --emit-ptx`. Use the `kernel` keyword for
GPU entry points (it parses like `fn` and is emitted as a PTX `.entry`):

```mettle
kernel vadd(a: float32*, b: float32*, c: float32*, n: int32) {
  var i: int32 = block.x * block_dim.x + thread.x;
  if (i < n) {
    c[i] = a[i] + b[i];
  }
}
```

### Index built-ins

Inside `--emit-ptx` compiles, the GPU thread/block indices are built-in member
expressions that mirror CUDA:

| Mettle        | CUDA          | PTX special register |
|---------------|---------------|----------------------|
| `thread.x`    | `threadIdx.x` | `%tid.x`             |
| `block.x`     | `blockIdx.x`  | `%ctaid.x`           |
| `block_dim.x` | `blockDim.x`  | `%ntid.x`            |
| `grid_dim.x`  | `gridDim.x`   | `%nctaid.x`          |

`.x`, `.y`, and `.z` are all available. The canonical global-thread index is:

```mettle
var i: int32 = block.x * block_dim.x + thread.x;
```

These built-ins are only active during GPU-module compiles, so member access on
an ordinary struct named `block` in a CPU program is unaffected.

### Supported kernel constructs

Kernels use the same syntax as CPU code: arithmetic, comparisons, `if`/`while`,
pointer indexing, casts, and a set of GPU math intrinsics declared as `extern`:
`sqrtf`, `rsqrtf`, `fabsf`, `sinf`, `cosf`, `logf`, `expf` (lowered to PTX
`sqrt.rn` / `rsqrt.approx` / `ex2.approx` etc.), plus `h2f` / `f2h` for fp16
conversion. The PTX backend is validated structurally by round-tripping emitted
PTX through `ptxas`; a CUDA Driver differential suite executes correctness and
sanitizer cases on development hardware and has a stricter native GB10 mode.
See the [GPU architecture and acceptance contract](gpu-architecture.md).

An ordinary function called by a kernel is emitted as a non-entry device helper
in both PTX and SPIR-V. Reachability is transitive and unrelated host functions
are omitted; `kernel` remains the only launch-entry marker. Direct calls with
scalar, pointer, or record parameters/results are supported. Recursion, indirect
calls, external calls, host launches from device code, and calling a kernel as a
normal function are rejected by a shared IR call-graph verifier, so the rule is
the same for every frontend and GPU backend.

### Records

Structs, fixed arrays, and arrays of structs work in device code the way they do
on the host:

```mettle
struct Ray { origin: Vec3; direction: Vec3; depth: int32; }

@noinline fn advance(ray: Ray*, t: float32) -> Vec3 { /* ... */ }

kernel trace(hits: float32*, scene: Params) {
  var ray: Ray;
  var stack: Ray[8];
  var top: int32 = 0;
  /* ... */
  var point: Vec3 = advance(&stack[top], 0.5);
}
```

A record local, a record field, a runtime-indexed array of records, taking a
record's address, passing one to a helper by value or by pointer, returning one,
and assigning one whole are all available. A record is also a kernel parameter
type, which is how a launch passes one settings block in place of eleven
scalars.

Under `-O` the shared optimizer splits a record whose fields are only ever read
and written at constant offsets into separate scalars, so the common case costs
no memory at all. A record it cannot split keeps per-thread storage: PTX uses
`.local` and SPIR-V a Function-storage byte array. An address that leaves the
function, as a call argument or stored into memory, is converted to a generic
address first, so a helper never reads a local address as if it were global.

A record parameter crosses the launch boundary as its own bytes, the same shape
CUDA gives a struct argument, so `--emit-kernel-decls` writes the record
declarations alongside the `extern kernel` lines and the generated file stays
self-contained. Every field has to mean the same thing on both sides, so a
record reaching a kernel holds scalars, pointers, fixed arrays, and nested
records; a string, a closure, or a function pointer is refused at the source.

The current OpenCL 2.0 SPIR-V profile has record locals, record arrays, and
pointers to records. It has no by-value record call ABI, and says so rather than
passing the wrong thing.

### Decorators on device code

[Function decorators](declarations.md#decorators) apply to device
helpers under `-O`, and they buy more here than on the host. A helper left out
of line becomes a PTX `.func` reached by `call.uni`: the call ABI's parameter
space, plus a register allocation that stops at the call boundary, paid once
per work item.

```mettle
@inline fn scale(alpha: float32, x: float32) -> float32 { return alpha * x; }

@noinline fn table_lookup(table: float32*, k: int32) -> float32 { /* ... */ }
```

`@inline` absorbs the helper into every kernel that calls it; `@inline!` fails
the build at any call site that survives, the same contract it carries on the
host; and `@noinline` is how a helper stays a real device call. Recursion and
indirect calls are already rejected in device code, so what the inliner sees is
a plain DAG of direct calls.

A loop-invariant call is hoisted out of a kernel's loop where the compiler
proves the callee writes nothing and cannot fault. That proof is inferred,
so `@pure` changes nothing about the hoist; it is a contract that fails the
build if the helper ever starts writing.

A frontend driving libmtlc directly reaches all four through
`mtlc_fn_set_inline`, `mtlc_fn_set_inline_required`, `mtlc_fn_set_noinline`,
and `mtlc_fn_set_pure`.

### Static and launch-sized workgroup memory, private memory, and barriers

Kernels can allocate statically sized scalar arrays in semantic address spaces:

```mettle
kernel staged(x: float32*, out: float32*, n: int32) {
  workgroup var tile: float32[256];
  private var scratch: int32[4];
  var lane: int32 = thread.x;
  if (lane < 256 && lane < n) { tile[lane] = x[lane]; }
  barrier(workgroup, global, acq_rel);
  if (lane < 256 && lane < n) { out[lane] = tile[lane]; }
}
```

These declarations are kernel-only, require a nonzero static array extent and a
scalar numeric element type, cannot have declaration initializers, and cannot be
rebound (their elements remain mutable). PTX maps them to `.shared` / `.local`;
SPIR-V maps them to real `Workgroup` / `Private` `OpVariable` arrays. They enter
both emitters as one target-neutral address-space allocation IR operation.

A pointer-shaped workgroup binding is an unbounded typed view of the dynamic
arena whose byte size is supplied by the host launch:

```mettle
kernel dynamic_staged(x: float32*, out: float32*, n: int32) {
  workgroup var values: float32*;
  workgroup var metadata: uint32*;
  var lane: int32 = thread.x;
  if (lane < n) {
    values[lane] = x[lane];
    metadata[256 + lane] = (uint32)lane;
  }
  barrier(workgroup, acq_rel);
  if (lane < n) { out[lane] = values[lane]; }
}
```

Every dynamic view in a kernel aliases the same base. Kernels partition it with
ordinary element offsets, must preserve each view's natural alignment, and must
keep accesses within the launch's dynamic byte count; the kernel receives no
implicit bounds value. PTX emits one module-scope external shared array per
kernel. SPIR-V appends one legal `Workgroup` pointer kernel argument, choosing
the most strictly aligned view type, and materializes all views from it. The
OpenCL host adapter must bind the launch byte count to that hidden local-memory
argument. PTX execution is sanitizer-tested; SPIR-V is `spirv-val`-validated,
but that host-binding path has not yet been execution-tested here.

`barrier(regions..., order)` is a uniform workgroup collective. Regions are
`workgroup` and/or `global`; the order is `acquire`, `release`, `acq_rel`, or
`seq_cst`. With no arguments it defaults to workgroup memory and sequential
consistency. Every live work-item in the workgroup must reach the statement.
SPIR-V receives the exact order and memory-class bits. PTX `bar.sync` is a full
CTA barrier and therefore deliberately strengthens acquire/release-only forms.
The legacy `gpu_barrier()` intrinsic remains accepted as a workgroup/seq-cst
compatibility alias.

### Target-neutral asynchronous staging

Mettle exposes asynchronous staging as a memory operation, not as a PTX
spelling:

```mettle
kernel stage(src: uint32*, out: uint32*) {
  workgroup var tile: uint32[128];
  var lane: int32 = thread.x;
  var base: int32 = lane * 4;
  async_copy_workgroup(&tile[base], src + base, 4,
                       transaction: 16, cache: global);
  async_copy_commit();
  async_copy_wait(0);
  barrier(workgroup, acq_rel);
  out[base] = tile[base];
}
```

Each copy moves matching scalar elements from global to workgroup storage for
the issuing work-item. `transaction` is 4, 8, or 16 bytes; the total byte span
must be a transaction multiple, and both addresses have that runtime alignment
precondition. `cache: all|global` is a performance hint. A commit closes the
current group, and `async_copy_wait(n)` waits until at most `n` newer groups
remain. The public pending range is 0..7 and every function exit must have no
outstanding or uncommitted group. The shared verifier checks this over the
whole CFG, including branches and loops.

A wait makes completion visible only to the issuing work-item. Other work-items
must not consume staged data until a uniform workgroup barrier publishes it;
`acq_rel` is the normal handoff. Reading an overlapping destination before its
wait is invalid. PTX 7.0/sm_80 and newer lower eligible operations to
`cp.async.{ca,cg}.shared.global`, `commit_group`, and `wait_group`. Portable
PTX and OpenCL 2.0 SPIR-V replay typed copies synchronously while preserving
commit/wait/barrier ordering. Compiler-owned PTX workgroup allocations are at
least 32-byte aligned, covering 16-byte transactions and WMMA tile loads; raw
global pointers retain their documented runtime precondition.

With `-O`, the shared optimizer can form the same neutral region from ordinary
typed global loads followed by their single-use workgroup stores. It proves
address-space provenance, scalar width, natural transaction alignment,
single-use connectivity, straight-line control, a later acq-rel/seq-cst
workgroup barrier, and absence of intervening memory effects. Commit is placed
before independent scalar work and wait immediately before publication. A
function containing explicit async groups is left alone, so generated staging
never splices into user-managed pending-group state. Source Mettle and public
libmtlc builders use the identical pass; the backend chooses native versus
synchronous realization only after neutral legality succeeds.

### Scoped atomics

Atomics are native, type-directed kernel operations over `uint32*`, `uint64*`,
or matching workgroup arrays. They are not extern calls and carry a target-
neutral address space, memory order, and scope into shared IR:

```mettle
var ticket: uint64 = atomic_fetch_add(queue_head, index, 1,
                                      order: acq_rel, scope: device);
atomic_store(ready, index, 1, order: release, scope: device);
var published: uint32 = atomic_load(ready, index,
                                    order: acquire, scope: device);
var old: uint32 = atomic_compare_exchange(
    state, index, expected, desired,
    success_order: acq_rel, failure_order: acquire, scope: device);
```

The u32/u64 family includes load, store, fetch add/sub/min/max/and/or/xor,
exchange, and compare-exchange. Value-returning operations return the observed
old value; store returns `void`. Element indices remain 64-bit through backend
address generation. Storage provenance is inferred as global or workgroup and
may be stated with `space:`. Global operations default to device scope;
workgroup operations default to workgroup scope. All default to seq-cst. Loads
permit relaxed/acquire/seq-cst, stores relaxed/release/seq-cst, and RMWs all
five C/C++ orders. CAS carries separate success and failure orders; failure may
not be release/acq-rel or stronger than success.

PTX emits scoped ordered `ld`/`st`/`atom` instructions and NVIDIA's required
`fence.sc` sequences for sequential consistency. SPIR-V emits `OpAtomicLoad`,
`OpAtomicStore`, and the matching RMW/CAS operations with exact Scope, ordering,
and memory-class operands. Optional u64 SPIR-V atomics require the OpenCL
`cl_khr_int64_base_atomics` and `cl_khr_int64_extended_atomics` extensions.
The real-device message-passing test uses an actual release store paired with
an acquire load; contended RMW/CAS and workgroup atomics are checked separately.

### Subgroup collectives

Mettle/libmtlc defines a subgroup as an implementation-sized set of work-items,
not as a CUDA warp. The initial portable collective surface is:

| Reference Mettle alias | Contract |
|---|---|
| `subgroup_local_id() -> uint32` | caller's zero-based lane within its subgroup |
| `subgroup_size() -> uint32` | number of lanes in the current subgroup |
| `subgroup_broadcast(value, source_lane)` | u32/f32 value from one uniform, valid source lane, returned to every lane |
| `subgroup_reduce_add(value)` | u32/f32 subgroup sum, returned to every lane |
| `subgroup_reduce_min(value)` / `subgroup_reduce_max(value)` | u32/f32 subgroup extrema, returned to every lane |
| `subgroup_scan_inclusive_add(value)` | u32/f32 prefix sum including the caller's value |
| `subgroup_scan_exclusive_add(value)` | u32/f32 prefix sum preceding the caller, with zero identity |
| `subgroup_shuffle(value, source_lane)` | u32/f32 value from a per-lane source; an inactive or out-of-range source returns the caller's value |
| `subgroup_ballot(predicate, word)` | 32-bit word of the active-lane predicate mask; words outside the implementation mask are zero |
| `subgroup_any(predicate)` / `subgroup_all(predicate)` | subgroup-wide boolean vote over active lanes |

These are native, type-checked Mettle kernel built-ins; no declaration or CUDA
header is required. Another frontend uses the corresponding typed
`MTLC_INTRINSIC_GPU_SUBGROUP_*` identities directly. Every live lane must
execute a collective in uniform control flow.
The broadcast source lane must be identical and in range for the whole subgroup;
shuffle source lanes may vary. Ballot is word-addressed so a backend is never
forced to narrow its implementation-sized mask into one scalar. The current
portable mask contract exposes words 0 through 3 and returns zero for any
larger word index.
Unsigned addition wraps modulo 2^32. Floating-point reduction order is
implementation-defined, so it is not bit-reproducible across backends.

The shared IR call-graph verifier enforces statically visible uniformity before
either GPU backend runs. Its lattice distinguishes values uniform across a
workgroup, values uniform only within a subgroup, and work-item-varying values.
Kernel arguments and group topology are workgroup-uniform; broadcast and
reduction and vote results are subgroup-uniform, while shuffle, scan, and a
ballot selected with a varying word index vary by lane. Local
IDs, memory loads, and opaque call results are conservatively varying. Helper parameter ranks are joined from every
reachable call site. Collectives under control that is insufficiently uniform for their
scope, varying-trip loops, and work-item-varying broadcast lanes are rejected
without putting backend reconvergence rules in the frontend.

PTX maps a subgroup to the 32-lane NVIDIA warp and uses `activemask`,
`shfl.sync`, and `vote.sync`. The reduction guards every tree edge with the active mask, so a
uniform partial final warp need not have a power-of-two lane count. SPIR-V uses
`SubgroupLocalInvocationId`, `SubgroupSize`, the native SPIR-V 1.0 `Groups`
broadcast/add/min/max reduce and scan operations, and the standardized
`SPV_KHR_shader_ballot` / `SPV_KHR_subgroup_vote` operations. An OpenCL consumer
must support the corresponding subgroup extensions. The current OpenCL 2.0
profile cannot represent a varying-source shuffle; it reports that capability
gap rather than miscompiling it as uniform broadcast.

### Cooperative tensor operations

`tensor_mma` is a native Mettle kernel operation for one collective
`D = A * B + C` tile. It is deliberately a whole-tile memory operation: source
code and shared IR describe the arithmetic, element formats, shape, layouts,
leading dimensions, rounding, overflow, sparsity, scaling, and collective
scope. They never expose PTX register fragments, lane-to-fragment mappings, or
an NVIDIA instruction name.

```mettle
kernel gemm_tile(a: uint16*, b: uint16*, c: float32*, d: float32*) {
  tensor_mma(a, b, c, d,
             shape: m16n16k16,
             input_type: f16,
             output_type: f32,
             a_layout: row, b_layout: col,
             c_layout: row, d_layout: row);
}
```

For whole problems, `tensor_matmul` applies that same target-neutral descriptor
to one bounded output region. A typical 2-D launch assigns one subgroup to one
logical region:

```mettle
kernel gemm_region(a: uint16*, b: uint16*, c: float32*, d: float32*,
                   problem_m: uint32, problem_n: uint32, problem_k: uint32,
                   lda: uint32, ldb: uint32, ldc: uint32, ldd: uint32) {
  var row: uint32 = (uint32)block.y * (uint32)16;
  var column: uint32 = (uint32)block.x * (uint32)16;
  tensor_matmul(a, b, c, d, row, column,
                problem_m, problem_n, problem_k,
                shape: m16n16k16,
                input_type: f16, output_type: f32,
                lda: lda, ldb: ldb, ldc: ldc, ldd: ldd);
}

dispatch gemm_region[
  grid: ((n + 15) / 16, (m + 15) / 16, 1),
  block: (32, 1, 1)
](a, b, c, d, m, n, k, lda, ldb, ldc, ldd);
```

The exact region semantics are, for every `row+local_row < problem_m` and
`column+local_column < problem_n`,
`D[r,c] = C[r,c] + sum(q=0..problem_k-1) A[r,q]*B[q,c]` under the descriptor's
numeric rules. Out-of-range output coordinates do nothing; `problem_k == 0`
copies C to D for the active region. The descriptor M/N are the region size and
descriptor K is a preferred exact native chunk, not permission to round the
problem down. Origins and M/N/K are unsigned, collective-uniform runtime
values. Whole matrices require explicit `lda`/`ldb`/`ldc`/`ldd`; runtime
leading dimensions are uniform unsigned values in the descriptor's uint32
range. Pointer slicing remains the backend-neutral way to select a batch, so a
`grid.z` batch does not add a vendor-specific tensor ABI.

`tensor_matmul` has its own shared-IR operation. Its operands are ordinary
matrix pointers, optional neutral metadata/scale/stride values, and the five
unsigned region controls; it does not lower into a frontend loop or expose a
fragment. Shared verification checks exact pointer/control types and collective
uniformity. It is intentionally not eligible for the single-tile chain/loop
optimizer.

The current PTX backend supports exact dense, subgroup-scoped
f16/bf16-to-f32, f64, i8/u8-to-i32-wrap, unscaled E4M3/E5M2-to-f32, and
block-scaled FP8/FP6/FP4-to-f32 regions, plus matching f16 or bf16 structured
2:4 A with f32 accumulation/result. Independent A/B transpose is supported
under either stored layout. For stable-WMMA families, a full M/N interior is
native when the tuple budget permits it: PTX
normalizes a transposed stored layout into the opposite backend-local logical
view, C is
loaded once, all complete runtime-K chunks remain in the accumulator, and D is
stored once. A K remainder then continues cooperatively from that exact-width
D value. Partial M/N regions, K smaller than one native chunk, partial warps,
and tuple-budget refusals use complete cooperative scalar replay with 64-bit
address arithmetic. In that path transpose swaps logical coordinates before
applying the selected stored layout; it never materializes a frontend or
shared-IR transpose. Unscaled FP8 edges use PTX's architectural
packed-FP8-to-f16 conversion followed by f32 widening; complete interiors retain
backend-private direct-MMA accumulators across runtime K.

Scaled regions use the same neutral scale contract as `tensor_mma`:
`scale_A` is row-major `[M, ceil(K/block)]`, `scale_B` is column-major
`[ceil(K/block), N]`, and both scale leading dimensions are explicit. Scale
coordinates stay logical even when A or B is transposed. FP6/FP4 dense storage
is the neutral least-significant-bit-first stream including logical stride
padding. Exact replay gathers values across byte boundaries, decodes
E2M1/E2M3/E3M2 and UE8M0 with backend-local integer arithmetic, decodes UE4M3
through the supported positive E4M3 conversion, applies both block scales, and
continues with f32 FMA. This deliberately avoids the narrower alternate-format
conversion instructions that PTX 8.8 does not qualify for `sm_121a`.

Complete scaled interiors retain native `mxf8f6f4`, MXFP4, or NVFP4
accumulators across runtime K and advance both scale grids per block. Canonical
row-A/column-B packed matrices use exact contiguous fragment loads when the
runtime logical stride is byte aligned (multiple of four values for FP6,
multiple of two for FP4). A uniform origin/stride guard sends every other case
to exact cooperative replay.

Structured 2:4 regions use the existing neutral compressed-A contract. Each
logical four-wide K group owns two stored A values in increasing selected-index
order and one uint8 mask; metadata is compact row-major
`[M,ceil(problem_k/4)]`. A partial final group still stores two values, while
logical positions outside runtime K do not contribute. A transpose changes
only compressed A's stored coordinates; metadata remains in logical row/group
order. PTX rebases complete K16 chunks by eight stored A values and four
metadata bytes, translates those masks to ordered `mma.sp` metadata, and keeps
the accumulator resident across runtime K. M/N/K edges and forced fallback
decode the same masks directly with 64-bit addresses and f32 FMA. Invalid masks
remain outside the source contract; both paths clamp them to the safe `0011`
mask before use as defensive hardening.

TF32 tails, reduced-precision accumulator/result, saturating integer tails,
and other sparse ratios/types remain explicit capability errors. Native
transpose, scaled paths, and bounded structured sparsity are offline-assembler
qualified, not device-numerically qualified. The OpenCL 2.0 SPIR-V profile also
rejects the operation explicitly.

CUDA 12.9 offline assembly accepts the five native-interior fixtures, three
exact-transpose fixtures, two unscaled direct-MMA FP8 fixtures, and four scaled
FP8/FP6/FP4 fixtures plus two structured-2:4 fixtures for `sm_121a` with zero
spills and zero stack. The
stable-WMMA modules peak at 40 registers, unscaled FP8 at 64, and the scaled
fixtures at 48 (mixed FP6), 48 (MXFP4), 56 (NVFP4), and 72 (scaled transposed
FP8); structured 2:4 uses 48 registers natively and 40 under forced replay. The
CPU oracle covers 19x23x21 unscaled, 19x23x67 scaled, and 19x23x19 sparse M/N/K tails,
E4M3/E5M2/E2M1/E2M3/E3M2/UE8M0/UE4M3 decoding, exact LSB-first streams,
independent A/B transpose, compact mask indexing, compressed selected-value
order, mixed layouts and padded strides, integer wrap, K=0, and out-of-range
no-op semantics. Neither result is GPU/GB10 numerical execution or performance
evidence.

The first four arguments are positional base pointers. Arithmetic configuration
is named and compile-time: `shape` (or separate `m`/`n`/`k`), independent
`a_type`/`b_type`/`accumulator_type`/`result_type`, four layouts,
`transpose_a`/`transpose_b`, `math`, `sparsity`, `rounding`, `overflow`, A/B
packing and scale modes/types, scale-table leading dimensions, and `subgroup`
or `workgroup` scope. Each of
`lda`/`ldb`/`ldc`/`ldd` may instead be a scalar integer expression, so one
compiled kernel can operate on padded, sliced, or batched matrices. Runtime
strides and all pointer operands must be uniform at the collective scope;
positive layout-compatible values are runtime preconditions.
`input_type` and `output_type` are conveniences, not a restricted type model.
Sparse/scaled descriptors take matching named `metadata`, `a_scale`, and
`b_scale` pointer operands. The public equivalent is `MtlcTensorMmaDesc` plus
`mtlc_tensor_mma` / `mtlc_tensor_mma_ex`; a frontend that already owns a tiled
operation can emit the same neutral accumulator chain with
`mtlc_tensor_mma_chain`. Dense sub-byte packing is target-neutral: consecutive
logical values occupy each byte from least- to most-significant bits, while all
matrix leading dimensions and pointer offsets remain logical element counts.
Block-scale metadata is canonical rather than vendor-fragment-shaped: A scales
are row-major `M x ceil(K/block)` and B scales are column-major
`ceil(K/block) x N`.

Post-processing is a separate `tensor_epilogue` collective, so nonlinear
activation never changes an MMA chain's exact sequential semantics:

```mettle
kernel finish_tile(d: float32*, bias: float32*, alpha: float32,
                   beta: float32) {
  tensor_epilogue(d, shape: m32n16, element_type: f32,
                  bias_mode: column, bias: bias,
                  alpha: alpha, beta: beta, activation: relu);
}
```

Its exact operation is
`D[row,col] = activation(alpha*D[row,col] + beta*bias[row,col])`.
Omitted scales are one. Bias may be absent, per-row, per-column, or a matrix
with an independent row/column layout and static or uniform runtime leading
dimension. D has the same layout/stride choices; activation is identity, ReLU,
or clamp with uniform `clamp_min`/`clamp_max`. f16/bf16 storage computes in f32;
f32/f64 compute in their own format. Every pointer, scale, bound, and runtime
stride is uniform at subgroup or workgroup scope. ReLU replaces only ordered
negative values with positive zero. Clamp performs ordered lower- then
upper-bound replacement; unordered values survive either activation. An active
bias region must not overlap the destination region.

PTX always has entry-ordered, exit-ordered cooperative logical-memory replay.
It supports generic/global/workgroup storage, arbitrary nonzero M/N, both
layouts, every bias mode, and the four formats above.

For an exactly compatible adjacent MMA/commit and epilogue, PTX may instead
retain the accumulator. A verified loop commit can make the same handoff across
its exit jump only when the epilogue label has that jump as its sole incoming
edge and cannot be reached by fallthrough. An outer guard or any other bypass
forces the complete memory operation at the shared label. Compatibility in all
cases requires the same D pointer,
M/N, result format, layout, static or identical runtime stride, and subgroup
scope. Stable WMMA permits bias-free scale/ReLU/clamp because applying one
scalar operation to every accumulator register needs no fragment-coordinate
assumption; any bias forces replay because stable WMMA's coordinate mapping is
opaque. Direct m16n8 native MMA has an explicit backend-owned coordinate
mapping and supports every bias mode. Straight-line chains, verified staged
pipeline commits, and uniquely reached loop exits are covered. Tuple-budget
refusal, incompatible operands, unsupported mappings, and ambiguous control
flow fall back to the complete memory operation.

CUDA 12.9 `ptxas -arch=sm_121a` accepts the standalone
f16/bf16/f32/f64 matrix with zero spills and 14--21 registers. The resident
suite assembles with zero spills at 40 registers for a two-MMA stable-WMMA
scale/ReLU chain, 58 for a two-MMA FP8 matrix-bias/clamp chain, and 31 for a
staged commit handoff. Both the fused loop exit and guarded-loop replay use 40
registers; the forced memory-replay module peaks at 66 registers.
These are offline structural/resource results, not GB10 execution or
performance evidence. The current OpenCL 2.0 SPIR-V profile rejects the
operation explicitly until it has exact collective ordering and lowering.

Structured sparsity has an equally backend-neutral memory contract. A is the
sparse operand; logical K is divided into consecutive groups of 2, 4, or 8 for
1:2, 2:4, or 4:8. Each group stores exactly half of its values, in increasing
logical-index order, so A's leading dimension counts `K/2` stored elements.
`metadata` is a `uint8*` row-major `[M][K/group]` occupancy matrix: only the low
`group` bits are used and exactly `group/2` bits must be set. This is a Mettle
data format, not PTX sparse metadata; each backend must translate it or reject
the requested profile.

The target-neutral format vocabulary includes f16, bf16, tf32, f32, f64,
FP8 E4M3/E5M2, FP6 E2M3/E3M2, FP4 E2M1, UE8M0/UE4M3 scales, signed/unsigned
i8 and i4, b1, and i32. Until Mettle has first-class low-precision scalars,
f16/bf16 storage is passed as `uint16*`; byte FP8 and packed FP6/FP4/i4/b1
storage use `uint8*`. The descriptor, rather than that carrier spelling,
defines arithmetic meaning.

The PTX backend currently implements the complete stable PTX WMMA family below
as load/MMA/store sequences. The listed shapes are physical instructions. For
byte-addressable f16, bf16, tf32, f64, and i8/u8, a larger logical M/N shape up
to 256x256 may be an exact grid of the listed square physical shape: 16x16 for
K=16 f16/bf16/i8/u8, 16x16 for K=8 tf32, or 8x8 for K=4 f64. PTX computes every
subtile address from the neutral layout and leading dimension, then chooses an
A-outer or B-outer traversal to reuse the fragment that removes more loads.
The logical descriptor, shared IR, and result remain one whole tile.

A capability check either selects an exact physical shape/grid or rejects the
descriptor; it never truncates the logical tile or silently changes precision,
layout, sparsity, scale, rounding, or scope. The native and public-builder
32x32 f16 chain/loop gates retain four accumulator subtiles at a logical tuple
peak of 56, while a budget of 55 produces exact replay. CUDA 12.9 assembles the
f16, bf16, tf32, f64, i8, and u8 grid matrix, including f16-result,
row/column-layout, and runtime-leading-dimension variants, with zero spills;
this is structural evidence, not a GB10 performance or numerical-execution
claim.

| A/B format | Accumulator/result | Shapes | Additional rule |
|---|---|---|---|
| f16 | f16 or f32 | m16n16k16, m8n32k16, m32n8k16; 16x16 logical grids | row/column A and B |
| bf16 | f32 | m16n16k16, m8n32k16, m32n8k16; 16x16 logical grids | PTX 7.0 / sm_80+ |
| tf32 | f32 | m16n16k8; 16x16 logical grids | PTX 7.0 / sm_80+ |
| f64 | f64 | m8n8k4; 8x8 logical grids | default/RN rounding |
| i8 or u8 | i32 | m16n16k16, m8n32k16, m32n8k16; 16x16 logical grids | wrap or finite saturation |
| i4 or u4 | i32 | m8n8k32 | A row-major, B column-major |
| b1 | i32 | m8n8k128 | A row-major, B column-major; XOR/AND popcount |

On architecture-specific Blackwell targets, the same whole-tile operation also
selects native warp-level narrow floating-point MMA. Larger neutral M/N tiles
are backend-owned m16n8 subdivisions; accumulators stay resident across proved
chains and exact runtime-K loops.

| A/B format | Scale contract | Native PTX profile |
|---|---|---|
| FP8 E4M3/E5M2 | unscaled, byte storage | mixed types, K=16 or 32 |
| any FP8 E4M3/E5M2, FP6 E2M3/E3M2, or FP4 E2M1 A/B pair | UE8M0 block32 | `mxf8f6f4` `m16n8k32`, scale vector 1 |
| packed FP4 E2M1 | UE8M0 block32 | MXFP4 `m16n8k64`, scale vector 2 |
| packed FP4 E2M1 | UE4M3 block16 | NVFP4 `m16n8k64`, scale vector 4 |
| structured 2:4 f16 or bf16 | compressed A + canonical uint8 masks | `mma.sp` `m16n8k16`, f32 accumulator/result |

The block-scaled profiles require PTX 8.8 and an architecture- or family-specific
`sm_120a`/`sm_121a` target. Raw `sm_121` is deliberately rejected because it
does not promise architecture-specific instructions.

The sparse f16/bf16 profile requires PTX 7.1 and sm_80+, subgroup scope,
M divisible by 16, N divisible by 8, and K=16. PTX translates each neutral mask
to its lane metadata and uses ordered metadata from PTX 8.5 onward. Invalid
dynamic masks are contract violations; the generated translation nevertheless
clamps them to a defined safe selector before issuing the tensor instruction,
rather than feeding an architecturally undefined metadata encoding to hardware.
For adjacent N subtiles, compressed A and translated metadata are prepared once
per M/K fragment and reused.

All live lanes in the subgroup must execute the operation uniformly with the
same descriptor, pointer values, and runtime leading dimensions. The pointed-to
tiles, leading dimensions, and base addresses must satisfy the selected PTX
WMMA layout and alignment requirements; those are runtime preconditions and
cannot generally be proved from a raw pointer. Generic, global, and workgroup
tile memory are accepted. The shared verifier rejects pointer or runtime-stride
expressions known to vary at the declared subgroup/workgroup scope.

The GB10 family is emitted at PTX 8.8 / `sm_121a` and assembled by CUDA 12.9
`ptxas -arch=sm_121a` in the validation suite. That proves syntax, register
tuples, target capability declarations, and zero-spill register ceilings. A
compute-12.0 development GPU additionally passes poisoned-padding
runtime-stride MMA and a 32x32x64
multi-tile f16/f32 GEMM against CPU oracles. The GEMM's runtime K loop loads C
once, executes one initial and three loop-carried WMMA updates in the resident
accumulator, and stores D once. A K=15 guard case proves that the commit is
bypassed when no complete K tile exists. A separate four-K-tile straight-line
chain has the same one-load/one-store property. Staged-pipeline tests cover
both a double-buffered two-tile schedule and a four-group schedule. The latter
issues eight native 16-byte copies, drains pending groups with waits 3 through
0, retains one accumulator through all four wait/publication handoffs, and
executes four MMAs with one C load and one D store. Distinct per-tile numerical
oracles pass on the same device. Native mixed FP8, mixed E3M2/E2M3 dense FP6,
MXFP4/UE8M0 block32, and NVFP4/UE4M3 block16 each pass direct whole-tile,
resident-chain, and runtime-K residency oracles with independently varied rows,
columns, scale chunks, logical strides, and poisoned padding. The public-builder
  gate additionally emits and assembles all 25 documented `mxf8f6f4` FP8/FP6/FP4
  A/B type pairs for both whole-tile and exact whole-matrix operations. Canonical
  byte-aligned FP6 fragments use an exact three-byte load per four values;
  transpose/noncanonical cases retain a boundary-safe bit gather or exact region
  replay.

A separate earlier source-expressed tail-complete GEMM keeps the resident WMMA path for every full
16x16 output tile and full K prefix, then uses typed f16-to-f32 scalar work for
M, N, and K edges in the same kernel. A 19x23x21 launch simultaneously proves
tensor interior, row/column edges, K-tail publication, runtime padded strides,
and untouched poison padding; a 16x16x7 launch proves the no-tensor scalar path.
That fixture remains useful device evidence for explicit composition. The new
`tensor_matmul` operation now makes native full-K residency plus exact M/N/K
edge selection backend-generated for each bounded region. Generating the 2-D
launch grid, batch pointer slicing, layout transforms, and architecture-tuned
region size for an arbitrary higher-level GEMM is still open.

Structured 2:4 f16/bf16 source and public-builder whole-tile kernels emit the
expected ordered `mma.sp` form for `sm_121a`; bounded `tensor_matmul` regions
now combine that native K16 interior with exact sparse M/N/K replay. CUDA 12.9
`ptxas` accepts whole tiles at 40 registers and bounded regions at 48 registers,
with zero spills and no `tcgen05` dependency. A-fragment coordinates and
the lower-row/upper-row halves of the metadata word have also been checked
against the PTX 8.8 fragment equations and Figure 119. A CPU oracle checks
compressed selected-value order, compact masks, transpose, odd K, K=0, and
no-op bounds. This remains CPU semantic plus offline structural evidence only;
GPU sparse numerics, malformed-input behavior beyond defensive clamping,
performance, and actual GB10 execution remain unqualified gates.

With `-O`, adjacent exact compositions of the form `D=A0*B0+C;
D=A1*B1+D; ...` become one backend-neutral tensor-chain instruction. Fusion is
legal only when descriptor semantics, result/accumulator type, C/D layout,
output pointer, and output leading dimension agree and no observable IR
instruction intervenes. PTX uses an inspectable tuple-pressure model (64
logical registers before sm_90, 96 from sm_90) and annotates its choice as
`mtlc.tensor_chain resident ...` or `replay ...`. A resident chain performs one
C load and one D store; an over-budget chain replays the exact component
operations. Another backend may always replay without changing the frontend or
shared IR.

`--gpu-tensor-tuple-budget=N` overrides that PTX ceiling for variant generation;
`N=0` restores the architecture default. The equivalent public-context policy
is `mtlc_context_set_ptx_tensor_tuple_budget`. This is deliberately backend-only:
the optimized neutral chain/loop/pipeline remains identical, while PTX chooses
resident or replay. It is a hook for measurement-driven tuning, not an
occupancy or performance claim by itself; final registers/spills come from
`ptxas`, and useful thresholds require real workload timing.

`tools/gpu/ptxas_profile.py --arch sm_121a --require-zero-spills -o
resources.json kernel.ptx` turns that offline assembler evidence into stable
JSON: input hashes plus per-function registers, spill traffic, stack, barriers,
shared memory, constant memory, and lexical PTX instruction-class counts.
`--max-registers=N` makes the same command a reproducible resource gate. The
tool deletes its temporary cubin and never creates a CUDA context, queries a
device, loads a module, or launches code; its output is offline evidence, not
runtime evidence.

`tools/gpu/ptxas_select.py` compares two or more such profiles under an explicit
resource model: threads per block, registers/shared memory per execution unit,
warp and block limits, and allocation granularities. It computes register-,
shared-memory-, and warp-limited residency bounds, rejects spills by default,
and emits a per-entry Pareto frontier over occupancy, resource headroom, and
static global-memory instruction count. The default `pareto` policy deliberately
leaves a resident-versus-replay tradeoff unresolved; `--policy=occupancy` or
`--policy=traffic` makes that policy explicit, while `--require-selection`
fails closed on ambiguity. The JSON always records `performance_claim: false`:
static counts and modeled occupancy are inputs to real autotuning, not a
substitute for timed GB10 workloads.

For the checked-in DGX workflow, the explicit 12.x upper-limit model comes from
NVIDIA's [compute-capability resource tables](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/compute-capabilities.html):
64K 32-bit registers, 100KB shared memory, 48 resident warps, 24 resident
blocks, and 32 threads per warp. The workflow deliberately uses allocation unit
1 because that table does not specify the allocation granularity; its result is
therefore an upper bound and the archived JSON records the assumption.

For measured selection, pass `--policy=measured --measurements=timings.json`.
The input follows `tools/gpu/variant_measurements.schema.json` and binds every
candidate to the SHA-256 of its exact resource profile, every entry to a
workload hash and launch shape, and the run to a device UUID, host architecture,
compute capability, integrated/discrete-memory fact, and driver version.
`samples_ns[i]` across candidates is one paired round; collectors must warm up
separately and interleave/randomize candidate order so index-aligned samples do
not encode drift. The selector never collects those samples itself.

The default measured gate requires at least 21 positive pairs, chooses the
lowest median only provisionally, then compares it against every eligible
candidate using a two-sided paired sign test with Bonferroni-corrected
`alpha=0.01` and a median paired speedup of at least 1.01. The thresholds are
recorded and configurable. If either statistical significance or practical
effect is missing, selection remains null; even a passing local selection keeps
`performance_claim: false` because it is evidence for that bound workload and
device, not a general competitive claim.

The same shared optimizer also recognizes an exact linear runtime loop after an
initial tile. It requires one connected MMA update, identical descriptor and
output-stride semantics, a single loop entry/backedge, and a body containing
only register-local address/counter arithmetic. Loads, stores, calls, atomics,
barriers, a second MMA, or any other observable body operation refuse the
transform. The optimizer inserts a neutral `tensor_commit` on only the loop's
exit edge; outer guards that share the original end label bypass that commit.
The shared GPU verifier independently rebuilds the CFG and rechecks the unique
start/update/commit shape before either backend may consume the metadata.

PTX applies the same tuple budget and emits `mtlc.tensor_loop resident ...` or
`replay ...`; replay executes the original load/MMA/store semantics, while the
resident path loads C once and delays D until the commit. A libmtlc frontend
gets this result from ordinary builder control flow and tensor descriptors, with no
Mettle-frontend marker exists. `--dump-ir` exposes the neutral residency roles,
and `--explain` reports the legality decision plus the actual PTX/SPIR-V backend
boundary.

The optimizer also recognizes a finite straight-line staged composition:
two or more connected tensor MMAs, with every pair separated by neutral async
completion followed by an acq-rel/seq-cst workgroup publication barrier. It
marks the maximal proved group with pipeline scope and inserts one neutral
commit after the last MMA. Shared verification requires the start, every
update, and commit in one basic block; an ordered WAIT-to-BARRIER handoff at
every transition; no intervening observation of D; identical
descriptor/output/stride connectivity; and only pure scalar work around each
handoff. PTX reports
`mtlc.tensor_pipeline resident ...` or `replay ...`; the resident form carries
the accumulator tuple through wait and barrier. The public builder test creates
the complete copy/tensor pipeline without frontend-private metadata and
receives the same optimization.

This accepts an arbitrary explicit N-stage sequence within the asynchronous
group contract; it does not hard-code double buffering. The compiler does not
yet generate the tile loop or buffer rotation, unroll an arbitrary MMA
schedule, or model occupancy from assembler feedback. Native
FP8/FP6/MXFP4/NVFP4 selection is implemented, but broader generated scheduling
remains compiler work, not implied by this legality foundation.

Native mixed FP8, dense mixed FP6, scaled MXFP4/NVFP4, and the canonical
structured-2:4 f16/bf16 profile are lowered by PTX without exposing fragments
to the frontend. Other sparse ratios and unsupported type/transpose/shape
combinations remain represented but are rejected with a capability diagnostic.
The current
SPIR-V OpenCL 2.0 profile likewise rejects tensor MMA because it has no enabled
cooperative-matrix capability; adding a newer SPIR-V device profile is separate
backend work, not a frontend redesign.

### Multidimensional tensor transfers

`tensor_transfer_workgroup` moves one complete rank-1 through rank-5 rectangular
tile between global and workgroup storage. Its source contract contains only
logical extents, byte strides, tile extents, per-dimension element strides,
signed coordinates, zero-fill/discard bounds, element format, direction, and
workgroup scope. Dimension 0 is contiguous and fastest-changing. No CUDA tensor
map, async proxy, transaction barrier, warp, or bank layout appears in source or
shared IR.

An optional `view: uint8*` is an opaque provider acceleration token. The raw
pointer plus logical descriptor remain authoritative: no view or a null view
selects cooperative replay, and a target that cannot preserve the descriptor
must replay or reject. The PTX backend additionally falls back at runtime when
the view is not 64-byte aligned or the workgroup address is not 16-byte aligned.
Valid neutral operations with a
dimension-0 element stride, a sub-16-byte inner TMA box, an encoded box above
256, a stride above 8, or incompatible global geometry never enter the native
path.

For the current NVIDIA provider, a non-null view points to an immutable
`CUtensorMap` in global memory whose address, rank, dimensions, and strides
exactly match the raw operand and descriptor. It uses tiled encoding, no
interleave, no shared-memory swizzle, `boxDim[d] = tile_extent[d] *
element_stride[d]` (with dimension-0 stride equal to one), and the corresponding
element strides. PTX acquires a host-copied map through the tensor-map proxy.
Global-to-workgroup TMA initializes a compiler-owned transaction barrier,
publishes that initialization to the async proxy, issues the tensor copy,
arrives with the expected byte count, waits for phase completion, publishes the
tile, and invalidates the barrier. Workgroup-to-global makes every producer
proxy-fence its own shared writes before the workgroup rendezvous, then the
elected thread issues and drains one bulk group.

Portable PTX replays both directions cooperatively with rank-aware coordinate
unravelling and exact out-of-bounds behavior. The OpenCL 2.0 SPIR-V profile
currently rejects the operation explicitly because its exact cooperative
lowering is not implemented. Native TMA PTX assembles for `sm_121a` with exact
ordering and zero-spill gates, but device execution is quarantined and remains
unqualified; see [the GPU validation tiers](../tests/gpu/README.md). Offline
assembly is not a hardware-safety or correctness claim.

### AI correctness kernels

The real-device suite also expresses numerically stable f32 row softmax and
affine layer normalization directly in Mettle. One subgroup owns each row while
each lane loops over a strided slice, so runtime column counts may exceed the
subgroup width and need not be multiples of it. Both kernels accept independent
padded row strides and use 64-bit address arithmetic. Along with the
multi-output/multi-K-tile and tail-complete tensor/scalar f16/f32 GEMMs above,
they pass CPU numerical oracles on the compute-12.0 development GPU.

These are correctness/expressiveness baselines plus three exact accumulator
residency forms: straight-line chains, a single-update runtime K loop, and
explicit N-stage asynchronous pipelines. They are not claims of general fusion or peak
performance. Bounded-region M/N/K tail selection is now backend-generated for
the documented `tensor_matmul` families; automatic launch-grid/batch tiling,
pipeline rotation, multi-exit and opaque-fragment biased epilogue residency,
attention, general quantized matmul
generation, low-precision scalar types, measurement collection/autotuning, and
GB10 benchmark evidence remain required.

A worked example of the whole path -- kernels, generated declarations, a
checked result, and a captured launch graph -- is
[`examples/gpu_inference/`](../examples/gpu_inference/), which runs one
transformer feed-forward block on the GPU.

## Which GPU, and what it gets built for

`mettle --gpu-info` reports what the machine has and which target `--emit-ptx`
would pick, without compiling anything:

```
$ mettle --gpu-info
Mettle GPU target report
  Driver            CUDA 12.9 (cuda-driver)
  Devices           1
  [0] NVIDIA GeForce RTX 5060 Ti
      compute capability   12.0  ->  sm_120a
      multiprocessors      36
      warp size            32
      max threads / block  1024
      shared mem / block   48 KiB
      global memory        15.9 GiB
  Assembler         ptxas 12.9 (sm_120a supported)
  Default target    sm_120a, PTX ISA 8.8
```

The answer comes from the CUDA driver itself, loaded on demand: the compiler
links no CUDA library and starts normally on a machine with no NVIDIA driver at
all. Detection settles two things `--emit-ptx` would otherwise have to guess:

- The `.target`. The compute capability the driver reports, taking the
  architecture-specific `sm_NNa` form from compute capability 9.0 onward when
  the installed `ptxas` confirms that form exists.
- The `.version`. A PTX ISA above what the local driver understands fails
  inside `cuModuleLoadData` at run time with nothing but a status code to
  explain it, so the emitted ISA is capped at what this driver can load.

Nothing is detected when no driver answers: the GB10 profile
(`sm_121a`, PTX 8.8) stays the default, which is what makes building on a
laptop for a DGX Spark work. `--gpu-arch=native` asks for the local card by
name and fails rather than falling back, which is what a build that is meant
for this machine wants. Exit status is 0 when a device was found and 1 when
none was, so `mettle --gpu-info` also works as a check in a script.

## Launching from the host

The host is a normal Mettle program. Import `std/gpu`, set up device buffers
explicitly, then launch with `dispatch`:

```mettle
import "std/io";
import "std/mem";
import "std/gpu";

extern kernel(block = 256) vadd(a: float32*, b: float32*, c: float32*, n: int32);

fn main() -> int32 {
  if (gpu_open("kernels.ptx") == 0) { return 1; }

  var n: int32 = 1 << 20;
  var bytes: int64 = (int64)n * 4;
  var ha: float32* = malloc(bytes);
  var hb: float32* = malloc(bytes);
  var hc: float32* = malloc(bytes);
  var i: int32 = 0;
  while (i < n) { ha[i] = (float32)i; hb[i] = (float32)(2 * i); i = i + 1; }

  var da: int64 = gpu_malloc(bytes);
  var db: int64 = gpu_malloc(bytes);
  var dc: int64 = gpu_malloc(bytes);
  gpu_to_device(da, (uint8*)ha, bytes);
  gpu_to_device(db, (uint8*)hb, bytes);

  dispatch vadd[work: n](da, db, dc, n);

  gpu_to_host((uint8*)hc, dc, bytes);
  gpu_free(da); gpu_free(db); gpu_free(dc);
  return 0;
}
```

### Declarations the host cannot get wrong

`dispatch` checks a launch against a host-side `extern kernel` declaration, so
a hand-written declaration is one more thing to keep in step with the kernel.
`--emit-kernel-decls` writes them instead:

```bash
mettle --emit-ptx kernels.mettle -o kernels.ptx \
  --emit-kernel-decls=kernel_decls.mettle
```

```mettle
extern kernel(block = 256) vadd(a: float32*, b: float32*, c: float32*, n: int32);
```

The host imports that file rather than restating it:

```mettle
import "std/gpu";
import "kernel_decls";
```

Every launch is now checked against the kernels as they were actually
compiled, block shape included. Change a kernel's signature, re-emit, and the
host stops compiling until it agrees -- which is the whole point. With no
`=path`, the declarations land beside the PTX as `<output>.mettle`.

### When something goes wrong

The two failures that dominate GPU bring-up are a module that will not compile
and a launch the driver refuses, and both used to arrive as a bare zero or a
bare status code. They now say what happened.

A module that fails to JIT prints the driver's own log, line numbers included:

```
mettle: the GPU module would not load: CUDA_ERROR_INVALID_PTX (a PTX JIT compilation failed)
--- CUDA PTX JIT log ---
ptxas application ptx input, line 5; error   : Not a name of any known instruction: 'bogus'
ptxas fatal   : Ptx assembly aborted due to errors
------------------------
```

A launch the driver rejects names the kernel and the shape it was given:

```
mettle: GPU launch of scale failed with grid 0x1x1, block 256x1x1, 0 shared bytes
  driver status: CUDA_ERROR_INVALID_VALUE (invalid argument)
```

`gpu_module_ex` returns the status instead of reporting it, for a caller that
wants to decide what a failure means; `gpu_jit_log` hands back the last log;
`gpu_error_name` and `gpu_error_text` turn any `CUresult` into words.

### Asking the device about itself

The same facts `--gpu-info` prints at build time are available at run time, so
a server can size its launches to the card it actually landed on:

```mettle
var buffer: uint8[128];
println("{gpu_device_name(0, &buffer[0], 128)}: {gpu_sm_count(0)} SMs");

var blocks: int32 = gpu_sm_count(0) * 4;
dispatch resident[blocks, 256](state, n);
```

`gpu_device_count`, `gpu_sm_count`, `gpu_warp_size`,
`gpu_max_threads_per_block`, `gpu_max_shared_memory_per_block`,
`gpu_compute_capability` (as `major * 10 + minor`), `gpu_total_memory`,
`gpu_is_integrated`, and `gpu_driver_version` cover the rest. `gpu_init_on`
and `gpu_open_on` take a device ordinal for a process that owns more than one
card.

### The `dispatch` statement

```
dispatch KERNEL[grid, block](arg0, arg1, ...);

dispatch KERNEL[
  grid: (grid_x, grid_y, grid_z),
  block: (block_x, block_y, block_z),
  shared: dynamic_shared_bytes,
  stream: stream_handle
](arg0, arg1, ...);
```

- `KERNEL` is either the name of a host-side `extern kernel` declaration, in
  which case the arguments and the block shape are checked at compile time and
  the handle is resolved by name, or a raw handle (the `int64` returned by
  `gpu_func`), in which case nothing is checked.
- The compact form supplies one integer grid and block dimension; the other
  axes are one and dynamic shared bytes/stream are zero.
- The named form requires exactly three integer dimensions in both `grid` and
  `block`. `shared` and `stream` are optional, default to zero, and named
  controls may be reordered. Every statically known dimension must be positive;
  shared bytes are an integer and a stream is an integer or pointer handle.
- The arguments are passed by value. Device pointers are `int64` handles; scalars
  (`int32`, `float32`, ...) are forwarded with their natural width.

`dispatch` becomes a typed, target-neutral launch operation in IR. Only host
lowering marshals the argument cells and calls the stable
`mtlc_gpu_launch_checked` runtime-provider ABI. The bundled CUDA provider maps
that ABI to `cuLaunchKernel`; another frontend uses `mtlc_gpu_launch` to build
the same operation without depending on Mettle syntax. A failed enqueue is not
silently discarded: the checked statement contract names the kernel, the grid
and block it was given, and the driver's status, then terminates. Use
`gpu_launch_3d` directly when code needs to inspect and recover from the
returned status. Allocation and copies remain explicit.

For explicit concurrency, `std/gpu` also exposes nonblocking streams, events,
asynchronous copies, stream-ordered allocation/free, managed memory, and
`gpu_launch_3d` / `gpu_launch_on`. Those are runtime foundations; they do not
yet constitute a hardware-validated scheduler or graph implementation.

## Building

```bash
# 0. see what this machine has, and what step 1 will target
mettle --gpu-info

# 1. compile optimized kernels for the local GPU (auto-detected via the
#    driver; falls back to the DGX Spark GB10 profile when no GPU is visible)
mettle -O --emit-ptx kernels.mettle -o kernels.ptx

# The same, but refuse to build at all if this machine has no GPU:
mettle -O --emit-ptx --gpu-arch=native kernels.mettle -o kernels.ptx

# Or pin a profile explicitly, for example DGX Spark GB10:
mettle -O --emit-ptx --gpu-arch=gb10 kernels.mettle -o kernels.ptx

# A forward-compatible development baseline is also available:
mettle -O --emit-ptx --gpu-arch=portable kernels.mettle -o kernels.ptx

# 2. build the host, linking the CUDA driver import stub (build-time only)
mettle --build host.mettle -o host.exe \
  --link-arg "<CUDA>/lib/x64/cuda.lib"        # Windows x86-64
```

The host links `nvcuda`, the OS driver, as an explicit vendor API. There is no
bundled CUDA DLL. At run time the driver JITs
the PTX to SASS for the installed GPU.

### Reporting and check flags

| Flag | Effect |
| --- | --- |
| `--gpu-info` | Reports the local GPUs, the driver, `ptxas`, and the target `--emit-ptx` would pick. Takes no input file |
| `--emit-kernel-decls[=F]` | With `--emit-ptx`, also writes each kernel's host-side `extern kernel` declaration, so an importing host cannot drift from the module it launches |
| `--report-launches` | Lists every dispatch site with the grid and block the compiler can fold, and the kernel each one names |
| `--report-occupancy` | With `--emit-ptx`, runs `ptxas -v` on the emitted module and prints each kernel's registers per thread and the occupancy ceiling they imply |
| `--sms=N` | SM count used for the whole-card fill thresholds in that report. Defaults to asking the local driver, and the thresholds are omitted when no driver answers |
| `--gpu-checks` | Emits the trap for each kernel-side `gpu_assert`. Without it those assertions cost nothing |

`--report-occupancy` needs `ptxas` on `PATH`, since it reads that tool's own
register and spill accounting.

GPU `-O` is intentionally backend-agnostic. It runs shared scalar/CFG
transformations only over kernel-reachable device functions and retains
address spaces, memory order/scope, barriers, subgroup collectives, and tensor
descriptors as semantic IR. It may combine an exact straight-line tensor
accumulation into a neutral chain or license the exact verified loop-carried
residency region described above; backend selection remains separate. The x86
SIMD/idiom pipeline and ML optimizer are not allowed to shape PTX or SPIR-V
modules.

On DGX Spark, build and run the Mettle compiler natively. Ordinary compile and
`--build` commands emit and link AArch64 ELF64 objects. AAPCS64 external calls,
global and string addresses, owned runtime linkage, and the eleven argument
`cuLaunchKernel` call shape are CI-gated on native Arm. `--emit-arm64` remains a
separate self-contained smoke executable for bring-up, while
`mtlc_emit(MTLC_ARCH_ARM64)` is the relocatable cross-host product.

The repository CI uses a hardware-free CUDA-shaped provider to validate the
complete host call ABI. Real driver loading, PTX JIT, launch, and result checks
remain part of the DGX acceptance suite because generic Arm runners do not have
GB10 hardware or `libcuda`.

Build the compiler itself for the Spark CPU with
`make DGX_SPARK=1`; with GCC 15 or LLVM 21, prefer
`make DGX_SPARK=1 DGX_SPARK_CFLAGS=-mcpu=gb10`.

Before treating a build as a Spark release, run the strict native gate:

```bash
bash tests/gpu/run_hardware_tests.sh --require-gb10 --sanitizer
```

It compiles optimized kernels, executes CPU-oracle comparisons through the
CUDA Driver, then runs compute-sanitizer memcheck and racecheck. Strict mode
refuses to pass unless the harness itself is AArch64 and CUDA reports an
integrated compute-12.1 device. The exact cases and non-claims are documented
in [`tests/gpu/README.md`](../tests/gpu/README.md).

The strict release gate intentionally excludes experimental TMA. Its separate
three-way opt-in is documented with the recovery requirements in that
validation guide; do not enable it on a workstation.

## SPIR-V (OpenCL) target

The same kernels compile to SPIR-V with `--emit-spirv`, targeting the
OpenCL 2.0 execution environment (Physical64 addressing, the `Kernel`
capability, the OpenCL memory model). This is the flavor that fits Mettle's
kernel ABI unchanged: kernels take raw typed pointers and do pointer arithmetic
plus loads/stores, which is the OpenCL/CUDA model, not the Vulkan
descriptor-buffer model.

```bash
mettle --emit-spirv kernels.mettle -o kernels.spv
```

The output is a binary SPIR-V module (one `OpEntryPoint ... Kernel` per kernel)
that an OpenCL runtime consumes with `clCreateProgramWithIL`. The same source
constructs as the PTX path are supported: arithmetic, comparisons, `if`/`while`
(including `&&`/`||` and nesting), pointer indexing, casts, the `gpu_*` index
built-ins (mapped to the OpenCL work-item built-ins, so `thread` reads
`LocalInvocationId`, `block` reads `WorkgroupId`, `block_dim` reads
`WorkgroupSize`, and `grid_dim` reads `NumWorkgroups`), static workgroup/private
arrays, launch-sized workgroup views, explicit `barrier(...)` (an
`OpControlBarrier`), the f32 math intrinsics
(an `OpExtInst` from `OpenCL.std`),
`h2f`/`f2h`, the unsigned u32/u64 atomic load/store/RMW/CAS family, and the portable subgroup subset above. SPIR-V
subgroup modules require the OpenCL `cl_khr_subgroups` device extension.
Modules using u64 atomics declare `Int64Atomics` and require both
`cl_khr_int64_base_atomics` and `cl_khr_int64_extended_atomics`; u32-only
modules remain valid against the mandatory OpenCL 2.0 capability set.
The current OpenCL 2.0 profile does not enable a cooperative-matrix extension,
so it rejects `tensor_mma` explicitly rather than scalarizing it invisibly.

libmtlc frontends can make pointer address spaces and atomic order/scope
explicit with `mtlc_type_pointer_in`, `mtlc_intrinsic_memory`, and
`mtlc_atomic_compare_exchange`. PTX preserves
global/workgroup/generic spaces and scoped C/C++ atomic ordering (including the
specified sequentially-consistent fence sequence); SPIR-V emits the matching
OpenCL storage class, Scope, ordering, and memory-class bits. Mettle source now
exposes static workgroup/private storage, launch-sized workgroup views,
configurable workgroup barriers, and native type-directed atomics without
extern declarations. The operations are load, store, fetch
add/sub/min/max/and/or/xor, exchange, and compare-exchange for uint32/uint64;
named options select address space, order, failure order, and scope.

Control flow maps directly onto SPIR-V basic blocks (`OpBranch` /
`OpBranchConditional`), exactly as the PTX path maps it onto `bra`. SPIR-V's
structured-control-flow rules (`OpSelectionMerge`/`OpLoopMerge`) are mandated
only by the `Shader` capability, so `Kernel` (OpenCL) modules may branch freely,
which `spirv-val --target-env opencl2.0` confirms.

## Notes and limits

- Without `--gpu-arch`, the CLI targets the local GPU: it asks the CUDA
  driver for the device's compute capability, falling back to `nvidia-smi`
  when the driver library is out of reach, and selects the matching
  `sm_NN` target, using the architecture-specific `a` variant from `sm_90`
  onward so the full tensor instruction surface is available. When no GPU or
  driver is visible (headless or cross-compiling hosts), the default is
  `.version 8.8` / `.target sm_121a` for GB10. `--gpu-arch` always wins:
  `portable` emits PTX 6.4 / `compute_75`; raw `sm_NN` and `compute_NN`
  targets plus `--ptx-version=M.m` are available for integration.
- `dispatch` is a checked enqueue: provider failure terminates the process.
  Status-returning `gpu_launch_3d` remains available when recovery is required.
  Provider-neutral launch attributes beyond geometry, dynamic shared bytes, and
  stream are not represented yet.
- Kernels and host code live in separate files (the kernel file is compiled
  with `--emit-ptx`; the host with `--build`).
- A record kernel parameter is passed by value. The OpenCL 2.0 SPIR-V profile
  rejects by-value record parameters and results; pass a pointer there.

See `examples/gpu_vadd/` for the complete x86-64 CUDA host example and
`tests/gpu/compute_kernels.mettle` for the assembler/validator kernel matrix.
There is not yet a production LLM kernel library in this repository.
