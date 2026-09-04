# Examples

Runnable programs for the two halves of the repo. Most are written in the Mettle
reference frontend and double as the runtime benchmark suite; [`calc/`](calc/)
is a second, non-Mettle frontend that drives the libmtlc backend directly through
its public API.

## A second frontend for libmtlc

[`calc/`](calc/) is a tiny C-like language whose entire compiler is one file. It
uses only the public headers in [`include/mtlc/`](../include/mtlc/) and links
only the library, lowering its own parse into libmtlc IR and compiling it to a
native executable. See [`calc/README.md`](calc/README.md) and
[docs/embedding.md](../docs/embedding.md).

## Bindings and graphics

[`raylib/`](raylib/) binds raylib 5.5 with nothing but `extern fn` declarations
and draws a rotating, lit sphere at ~8,600 fps. See [`raylib/README.md`](raylib/README.md).

[`video_player/`](video_player/) plays Motion JPEG video with PCM audio from an
AVI file. The container parser, the baseline JPEG decoder, the inverse DCT, the
chroma upsampler, and the playback clock are all Mettle; Win32 supplies the
window, the blit, and the wave device. See [`video_player/README.md`](video_player/README.md).

## Execution models

[`job_system/`](job_system/) is a job queue on `std/thread` with a swap point
the program names. It is the shape [Runtime model](../docs/runtime-model.md)
describes: the compiler ships no scheduler, and the program states what its
own scheduler must satisfy. A job may not allocate (`@noalloc`), a queue slot
is a declared type the compiler proves in range, running a job `requires
Worker` and only `worker_main` `provides` it, so a job reached from `main` is
`error[F0002]` before any rule runs, and three `@rule`s hold on every build:
jobs are `@noalloc`, jobs are reached from the worker and never from `main`,
and the phase machine's `step` decides every `Phase`. The build
fails when any of that stops being true.

[`engine/`](engine/) is that queue with a frame around it, and it is where the
whole execution-model surface lands in one file. The frame is a `const` of
`std/schedule`, so the dispatcher for each thread and the `quiesce` at every
phase boundary are generated from the order the program wrote and printed by
`mettle expand` as ordinary Mettle. Each phase carries an effect, so a call
reaching across a phase is `error[F0002]` with the chain landing on the
schedule's own row. One global is written from two phases, and the lock that
orders them is an effect both writers require rather than a convention, which
`--report-effects` prints as a line of its own. The blend declares
`where cycles < 1200` and the compiler costs its longest path. The drain
declines to declare one, because it takes a spin lock and a spin lock has no
bound, which is `error[D0002]` rather than a deadline nobody proved. See
[`engine/README.md`](engine/README.md).

[`desk/`](desk/) is a four-channel mixing desk, and it is where the surface is
carried by a program that does something. The frame is three rows of
`std/schedule` with `joins: true`, so the two threads meet at every phase
boundary and the scheduled run prints exactly what the one-thread offline
bounce prints. The audio path is fixed point over declared types, so
`--check-overflow` deletes nineteen checks and `--explain` names the type that
earned each one. Two blocks declare `where cycles < N` and are proven against
the target's cost model; `board.mettle` beside them is that description with
four numbers changed, and the same source is refused on it. The channel shaper
is a function pointer whose type carries `with Dsp`, which is how the mix stage
forbids `alloc` across an indirect call. Seven `@rule`s run: three over the
checked program, one over what it became in codegen, and three over a run
recorded with `--record-trace`, where the rule that gaps under `mettle test`
proves something over the recording. See [`desk/README.md`](desk/README.md).

## A machine described as data

[`machine/`](machine/) describes an eight-register machine with six
instructions, writes a Fibonacci program for it, and runs it. The registers
are a global array, each instruction's meaning is a function, and the encoding
is a string; the compiler reads the description, refuses one that cannot be
decoded back, assembles the program into that machine's bytes and runs it by
calling the semantics functions in the compile-time interpreter.
`mettle machine` prints the machine and `mettle emulate` runs it. See
[`machine/README.md`](machine/README.md).

## Benchmark examples

Each directory below contains `*.mettle`, `*.c`, `*.rs`, and `build.bat`. They are wired into [`docs/benchmarks/harness.json`](../docs/benchmarks/harness.json) and run via [`tools/benchmark/run-benchmarks.ps1`](../tools/benchmark/run-benchmarks.ps1). Every benchmark entry carries a `suite` number; benchmarks without one default to Suite 1.

### Suite 1 (original)

| Directory | Description |
|-----------|-------------|
| [`fib/`](fib/) | Iterative Fibonacci; 10M× fib(35) |
| [`word_count/`](word_count/) | Whitespace word counting on a synthetic buffer |
| [`grep/`](grep/) | Line grep with uint64 pattern matching |
| [`sum_squares/`](sum_squares/) | Sum of squares 1..n |
| [`collatz/`](collatz/) | Collatz step counting |
| [`byte_hash/`](byte_hash/) | djb2 byte hash |
| [`prime_count/`](prime_count/) | Trial-division prime counting |
| [`matrix_mul/`](matrix_mul/) | 32×32 matrix multiply |
| [`sort_insertion/`](sort_insertion/) | Insertion sort |

The full Suite 1 roster (including microbenchmarks like `saxpy`, `memcpy_bench`, `dot_product`, etc.) is listed in [`docs/benchmarks/harness.json`](../docs/benchmarks/harness.json).

### Suite 2 (data structures and codecs)

| Directory | Description |
|-----------|-------------|
| [`quicksort/`](quicksort/) | Recursive quicksort (Lomuto partition) over 2048 int32 values |
| [`crc32/`](crc32/) | CRC-32 (bit-by-bit) checksum over a 256 KB buffer |
| [`base64_encode/`](base64_encode/) | Base64 encoding of a 256 KB buffer |
| [`linked_list_sum/`](linked_list_sum/) | Pointer-chasing sum over a shuffled 65536-node singly-linked list |
| [`matvec/`](matvec/) | float64 512×512 matrix-vector multiply |
| [`heapsort/`](heapsort/) | In-place binary-heap sort over 2048 int32 values |
| [`merge_sort/`](merge_sort/) | Recursive top-down merge sort over 2048 int32 values |
| [`radix_sort/`](radix_sort/) | LSD radix sort (4× 8-bit digit passes) over 4096 uint32 values |
| [`rle_encode/`](rle_encode/) | Run-length encoding of a 256 KB buffer |
| [`bst_insert/`](bst_insert/) | Binary-search-tree build + recursive in-order traversal, 4096 nodes |

### Suite 3 (applications)

Small complete programs rather than kernels: each one generates its own input,
transforms it, and verifies the result, so no single loop owns the measurement.
The Mettle and C sources are structural mirrors, neither side hand tuned, and a
pair is accepted only when both print the same checksum. The rules the suite is
written to are in [`docs/benchmarks/README.md`](../docs/benchmarks/README.md).

| Directory | Description |
|-----------|-------------|
| [`json_parse/`](json_parse/) | Recursive-descent JSON parser over a generated 330 KB document, into a node arena |
| [`interp_ast/`](interp_ast/) | Lexer, precedence-climbing parser, and tree-walking interpreter for a 165 KB program |
| [`word_freq/`](word_freq/) | Word counting through an open-addressing hash map that grows and rehashes, then a top-16 select |
| [`huffman/`](huffman/) | Huffman codec: histogram, min-heap tree build, code assignment, bit-pack, decode, verify |
| [`lz77/`](lz77/) | LZ77 hash-chain match finder over a 32 KB window, with decompression and verification |
| [`astar_grid/`](astar_grid/) | A* pathfinding with a binary-heap open set over a generated 192×192 cave |
| [`regex_match/`](regex_match/) | Backtracking regex engine: five compiled patterns over 4000 log lines |
| [`physics_grid/`](physics_grid/) | Particle simulation: uniform-grid bucketing, neighbour collisions, integration |

Shared timing helpers live in [`bench_time.h`](bench_time.h) (C) and [`bench_time.rs`](bench_time.rs) (Rust). Mettle programs import `std/bench`.

Build one example manually:

```bat
examples\fib\build.bat
examples\fib\fib.exe
```

Run the full Mettle-vs-C suite (all three suites):

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

Run a single suite:

```powershell
.\tools\benchmark\run-benchmarks.ps1 -Suite 1
.\tools\benchmark\run-benchmarks.ps1 -Suite 2
.\tools\benchmark\run-benchmarks.ps1 -Suite 3
```

## Mettle vs Rust demo

[`mettle_vs_rust/`](mettle_vs_rust/): a single workload in Mettle and Rust with a script that compares **compile time**, **binary size**, and **runtime** side by side. Run `examples\mettle_vs_rust\build.bat`.

## Other examples

| Directory | Description |
|-----------|-------------|
| [`grep/`](grep/) | Also the reference string-search benchmark |
| [`hexdump/`](hexdump/) | Hex dump utility |
| [`ui_demo/`](ui_demo/) | Win32 UI demo (`std/ui`); see [ui_demo/README.md](ui_demo/README.md) |
| [`tracy_demo/`](tracy_demo/) | Tracy profiler demo (`std/tracy`); see [tracy_demo/README.md](tracy_demo/README.md) |
| [`gpu_vadd/`](gpu_vadd/) | GPU offload demo: a `kernel` compiled to PTX (or SPIR-V with `--emit-spirv`) and launched with `dispatch` (`std/gpu`); see [docs/gpu.md](../docs/gpu.md) |
| [`gpu_inference/`](gpu_inference/) | A transformer feed-forward block on the GPU: rmsnorm, warp-per-row matvec, SwiGLU, softmax; checked against a CPU reference and timed against a captured launch graph |
| [`guessing-game/`](guessing-game/) | Simple interactive game |
| [`direct_object_smoke/`](direct_object_smoke/) | Direct object backend smoke test |

## Regenerating compile stress fixtures

```powershell
python tests/gen_parse_stress_test.py
python tests/gen_profiler_test.py
```

See [`docs/benchmarks/README.md`](../docs/benchmarks/README.md) for compile-only benchmark details.
