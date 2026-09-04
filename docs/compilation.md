# Compilation

The `mettle` driver: what it produces, the options that change that, and the
subcommands that do something other than compile.

## The short version

```bash
mettle --build hello.mettle
```

That compiles and links an executable next to the source. Drop `--build` and
you get an object file instead. Add `--release` when you want it optimized.

The compiler takes one entry file. It resolves imports transitively and hands
the backend one program, so you never compile imported files separately.

## Input and output

| Option | Effect |
|--------|--------|
| `-i <file>` | The entry file. A positional path works too. |
| `-o <file>` | Output path. Defaults to `output.obj` or `output.o`, or the executable path with `--build`. |
| `-I <dir>` | Add an import search directory. Repeatable. |
| `--stdlib <dir>` | Set the stdlib root. |
| `--prelude` | Auto-import `std/prelude`. |

## What to emit

| Option | Product |
|--------|---------|
| `--emit-obj` | A native object. This is the default. |
| `--build` | An executable: PE on Windows, ELF on Linux. |
| `--emit-arm64` | A self-contained AArch64 Linux executable. |
| `--emit-arm64-obj` | An AArch64 relocatable object. The default on an ARM host. |
| `--emit-ptx` | Declared kernels as NVIDIA PTX. |
| `--emit-spirv` | Declared kernels as OpenCL SPIR-V. |
| `--emit-flat <file>` | A raw image with no container, laid out at `--image-base`. |

[Linker and build pipelines](linker-build-pipelines.md) covers which linker
runs for each combination. [GPU offload](gpu.md) covers the device targets.

## Which machine

| Option | Effect |
|--------|--------|
| `--target <triple>` | Compile for another machine. |
| `--image-base <addr>` | Where the linked image loads, replacing the format's default. |

The triples are `x86_64-windows`, `x86_64-linux`, `x86_64-none`,
`aarch64-linux`, `aarch64-none`, `i386-none`, `i686-none`, and `i8086-none`. A
trailing vendor or environment is accepted and ignored, so
`x86_64-unknown-linux-gnu` selects `x86_64-linux`. The 16- and 32-bit targets
emit a flat image only.

`--target` emits an object for the named machine; it does not link one.
`--build` runs this machine's linker against this machine's runtime, so it is
refused for a foreign or freestanding target, with a message saying which
machine has to do the linking.

[Bare metal](bare-metal.md) covers cross-compilation, flat images, and 16-bit
code generation.

## Optimization

| Option | Effect |
|--------|--------|
| `-O`, `--optimize` | Turn the optimizer on. |
| `-r`, `--release` | Optimize for size. Implies `-O`, strips comments, drops unreachable functions. |
| `--safe` | Check every memory access not proven in bounds. Kept under `--release`. |
| `--native-heap` | Route `new` and the `malloc` family through `std/alloc`. |

Three passes go further, and each pays for itself in a different currency.

`--pgo` interprets `main()` at compile time and feeds the measured call
frequencies to the optimizer, so a hot callee bypasses the inliner's size
budget. No instrumented build and no training run. It implies `-O`. See
[Profile-guided optimization](pgo.md).

`--verify` executes each function's before-and-after IR on generated inputs
after every pass and compares. A diverging pass is reported with a
counterexample, quarantined for that function, and the build continues from
the validated IR. It implies `-O`. See
[Translation validation](translation-validation.md).

`--ml-opt` runs a learned optimizer after the classical passes. Every rewrite
it applies is re-executed through the validation interpreter and discarded on
divergence. `--ml-opt-speculative` also applies its unproven proposals, which
stand only when the validator can execute the function and finds no
divergence. See [ML-driven IR optimization](ml-opt.md).

## Reports

| Option | Report |
|--------|--------|
| `--explain` | Every optimization decision, with the reason whenever the optimizer declined. |
| `--explain=SELECTOR` | One slice: `missed`, `fixable`, `proven`, `loops`, `calls`, a function name, or a decision code. |
| `--explain-json` | Also write `<stem>.explain.json`. Implies `--explain`. |
| `--simd-report` | What each `@simd` loop became. |
| `--annotate-asm` | The emitted assembly with the codegen decision behind each instruction, a cost model, recovered loops, a register-lifetime map, and an instruction mix. Implies `-O`. |
| `--asm-syntax=S` | `intel`, `att`, or `both` for `--annotate-asm`. Default `both`. |
| `--annotate-lines=A-B` | A focused codegen report for those source lines. |
| `--annotate-fn=NAME` | Restrict the annotate reports to one function. |
| `--annotate-hot[=N]` | The top N codegen hotspots. Default 8. |
| `--dump-ast` | Write the parsed AST to a `.ast` sidecar. |
| `--dump-ir` | Write the optimized IR to a `.ir` sidecar. |
| `--report-rules` | The verdict and interpreter steps of every `@rule`; see [Rules](rules.md). |
| `--rule-budget=N` | Fail the build when the rules spend more than N interpreter steps together. |
| `--report-proofs` | Every conversion into a [declared type](types.md) the prover settled: the type, the expression, the site, whether it was proven, what it cost, and the route that settled it. |
| `--proof-budget=N` | Fail the build when the declared-type prover spends more than N steps. |
| `--report-effects` | What each function performs and needs, which globals more than one thread writes and what orders them, and what the [effect](effects.md) pass spent getting there. |
| `--report-twins` | What each `reference` twin was checked against, and on how many generated input sets. See [Translation validation](translation-validation.md). |
| `--fix` | Apply the replacement lines that failing `@rule`s proposed, and say which rule asked for each. See [Rules](rules.md). |
| `--effect-budget=N` | Fail the build when the effect pass spends more than N steps. |
| `--report-target` | The target in effect, printed as a `TargetDesc` in Mettle; `mettle target <triple>` prints a built-in one and `--target desc.mettle` builds for a described one. See [Bare metal](bare-metal.md). |
| `--check-proofs` | Trap at run time when a value the compiler proved to be a [declared type](types.md) is not one. Survives `--release`, and costs nothing in a program with no declared types. |
| `--check-effects` | Trap at run time when an [effect](effects.md) the compiler proved absent is performed, or one it proved provided is not. Survives `--release`, and costs nothing in a program that declares no effect. |
| `--report-deadlines` | Each `where cycles < N` deadline, its longest path block by block, and whether it was proven or measured. |
| `--check-deadlines` | Count what a path actually costs while the program runs and trap past the longest path the compiler proved. Costs nothing in a program with no deadline. |
| `--record-trace` | Write what the run did to `METTLE_TRACE` (default `mettle-trace.txt`), for `mettle check-trace` to hold the [trace rules](rules.md) to. |
| `--check-overflow` | Trap at run time when a signed `+`, `-` or `*` leaves its type. A [declared range](types.md) that bounds the operands deletes the check, and `--explain` reports which one did. |
| `--check-tasks` | Trap at run time when a pointer handed to a task lies in the stack of the thread that spawned it, re-checking M0121 without consulting the [borrow analysis](borrow-checker.md). Costs nothing in a program that spawns no task. |

`--explain` needs `-O` or `--release`, because there are no decisions to
report without the optimizer. A repeat run leads with what changed since the
last build, regressions first. It reports the build it is on and does not
change it: the same source with and without `--explain` gets the same
optimizations, and a report that said otherwise would be a report about a
program nobody built.

Four of its sections are not optimizer decisions. **types proven** lists every
conversion into a declared type, the route that proved it, and the pass that
consumed it. **effects held** lists what each function performs and needs.
**rules run** lists every `@rule`, its verdict and its cost. **beliefs** lists
everything the build took on trust and did not check: every `extern` with a
`with` clause, which is read as written, and every `extern` without one, which
is taken to allocate. A build that rests on nothing says so. Those beliefs are
the only claims in a build the compiler did not establish itself, and
`mettle test`, `--check-effects` and `--check-proofs` are where they get
checked.

[The `--explain-json` schema](explain-json.md) documents the machine-readable
form.

## Asking why

A diagnostic explains a fact that did not hold. `mettle why` explains one that
did, with the same chain and the same range the refusal would have printed:

```bash
mettle why app.mettle main Audit
mettle why app.mettle 15 Percent
```

The first walks the call graph to the line that performs the effect. The
second names the range the compiler knew for the expression and the route that
settled the predicate. A subject that starts with a digit is a site and the
third argument is a declared type; otherwise it is a function and the third
argument is an effect.

## Debugging

| Option | Effect |
|--------|--------|
| `-d`, `--debug` | Debug output and symbols. |
| `-g`, `--debug-symbols` | Debug symbols. |
| `-l`, `--line-mapping` | Source line mapping. |
| `-s`, `--stack-trace` | Report a crash at the exact statement. |
| `--no-crash-report` | Drop the default crash report. |
| `--debug-format <fmt>` | `dwarf`, `stabs`, or `map`. Default `dwarf`. |
| `--debug-hooks` | Instrument for the interactive debugger. Needs `-O0`. |

### When a program faults

A linked executable reports a fault by default. It names the exception, the
address, and the function it happened in, with that function's file and line,
then walks the stack:

```text
Unhandled runtime exception 0xC0000005 (access violation)
Attempted to read inaccessible memory at 0x00000000DEADB000
  --> ds_vk.mettle:406:1 in submit_frame
Stack trace:
  #0 submit_frame at ds_vk.mettle:406:1 (0x0000000140001ECA)
  #1 main at ds.mettle:812:1 (0x0000000140001F2A)
Lines above are where each function is declared; rebuild with -s for the exact statement.
```

That costs one debug record per function plus the handler, about 8 KB, and
nothing in code quality: the register-allocating backend is unaffected.
`--no-crash-report` removes it. A bare object (`--emit-obj` with no `--build`)
and a freestanding image never carry it, because neither links through a
runtime this compiler controls.

`-s` records a location per instruction as well, so the report names the exact
statement rather than the function's declaration. That table cannot be carried
by the register-allocating backend, so the functions in a `-s` build are
emitted by the baseline one and run slower.

## Profiling

| Option | Effect |
|--------|--------|
| `--profile` | Per-phase compilation timings. |
| `--profile-runtime` | Function-level run-time timing. Disables inlining. |
| `--profile-runtime-ops` | Op-class counters per function, after optimization. |
| `--profile-blocks` | Per-basic-block counters to a `.mprof` sidecar. Implies `--profile-runtime`. |
| `--tracy` | Link `std/tracy` with the Tracy profiler. Needs `--build`. |
| `--tracy-dir <dir>` | Tracy repo root. Defaults to `TRACY_DIR`, then `.mettle/tracy_dir`. |

`--profile-blocks` writes to the path in `METTLE_PROFILE_OUT`, and fuses with
`--annotate-asm` into a combined codegen profile view.

## GPU

| Option | Effect |
|--------|--------|
| `--gpu-info` | Report local GPUs, driver, ptxas, and the target `--emit-ptx` would pick. Takes no input file. |
| `--gpu-arch=A` | `native`, `gb10`, `portable`, `sm_NN`, or `compute_NN`. |
| `--ptx-version=M.m` | Override the emitted PTX ISA version. |
| `--emit-kernel-decls[=F]` | Also write each kernel's host-side `extern kernel` declaration. |
| `--report-occupancy` | Registers per thread and the occupancy ceiling, from `ptxas -v`. |
| `--sms=N` | SM count for the fill thresholds. |
| `--gpu-checks` | Emit the trap for each `gpu_assert`. |
| `--report-launches` | Every dispatch site with the grid and block the compiler can fold. |
| `--gpu-tensor-tuple-budget=N` | PTX resident-fragment ceiling. |

[GPU offload](gpu.md) covers all of it in context.

## Linking

| Option | Effect |
|--------|--------|
| `--linker <mode>` | `auto`, `internal`, `gcc`, or `msvc`. Default `internal` with `--build`. |
| `--link-arg <arg>` | An extra linker argument. Repeatable. |
| `--static` | Accepted for compatibility. Owned ELF builds are always static. |

## Diagnostics

`--error-format=F` picks `human`, the default, or `json`, which writes one
JSON object per diagnostic on stderr for tooling.
[Diagnostics](diagnostics.md) documents both.

## Subcommands

```bash
mettle help <topic>
mettle docs [topic]
mettle explain <CODE>
mettle test <file> [--filter=S]
mettle trace <file> <fn> [args...]
```

`help` covers build, runtime, interop, stdlib, web, diagnostics, verify, and
test; `help all` prints every topic. `docs` prints the path of the matching
documentation file.

`explain` takes a diagnostic code such as `E0004` or `M0103`, or an
[`--explain` decision code](explain-json.md) such as `dot-shape-address`.
`explain list` prints the index.

`test` runs the `@test` functions in the compile-time interpreter, with no
codegen and no linking. `trace` interprets one function and prints a
line-by-line value trace. [Compile-time execution](testing.md) covers both.

## Environment variables

These change compiler behavior and are meant for debugging the compiler
itself.

| Variable | Effect |
|----------|--------|
| `METTLE_PGO_HOT` | Call-frequency threshold for `--pgo`. |
| `METTLE_SKIP_PASS` | Skip a named optimizer pass, for bisecting a miscompile. |
| `METTLE_TIME_IR_PASSES` | Time each IR pass. |
| `METTLE_TRACE_IR_PASSES` | Trace each IR pass. |
| `METTLE_TIME_CODEGEN` | Time code generation. |
| `METTLE_LINEAR_ALLOC` | Use the linear register allocator in place of the graph-coloring default. |
| `METTLE_MIR_DUMP` | Dump MIR with register assignments. |
| `METTLE_NO_SIMD` | Turn the vectorizers off. |
| `METTLE_LINK_GC_REPORT` | Report what link-time section collection removed. |
| `METTLE_VERIFY_STATS` | Statistics from `--verify`. |
| `METTLE_PROFILE_OUT` | Output path for `--profile-blocks`. |
| `NO_COLOR`, `CLICOLOR_FORCE` | Turn diagnostic color off or on. |
| `TRACY_DIR` | Tracy repo root for `--tracy`. |

## See also

- [Linker and build pipelines](linker-build-pipelines.md)
- [Diagnostics](diagnostics.md)
- [Getting started](getting-started.md)
