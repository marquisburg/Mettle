# Documentation

This repository holds two things. Mettle is a typed, assembly-inspired
systems language. libmtlc is the native compiler backend it drives, which any
frontend can drive instead. The documentation is organized the same way.

## Start here

- [Getting started](getting-started.md): install, build your first program,
  and a short tour of the language.
- [Quick reference](quick-reference.md): the idioms you look up.
- [Known limitations](known-limitations.md): what does not work yet.

## The language

- [Lexical structure](lexical-structure.md): comments, names, literals,
  keywords.
- [Types](types.md): the type system, sizes, and conversions, and what a
  declared type's predicate is proven from: a call's postcondition, a loop's
  carried bound, a relation to another binding, and a float interval with its
  rounding term.
- [Declarations](declarations.md): functions, variables, structs, enums,
  methods, closures, decorators.
- [Expressions](expressions.md): operators, precedence, casts, interpolation.
- [Control flow](control-flow.md): branching, loops, `match`, `defer`.
- [Modules and imports](modules.md): files, `export`, path resolution.
- [Standard library](standard-library.md): every module under `stdlib/std`.

## Memory

- [Heap allocation](heap-allocation.md): the allocators and how to pair them.
- [Memory safety](memory-safety.md): what the compiler proves, and `--safe`.
- [Borrow checker](borrow-checker.md): pointers that outlive what they point
  at.

## Building and tooling

- [Compilation](compilation.md): the driver and every option.
- [Diagnostics](diagnostics.md): reading errors, the code index, JSON output.
- [Compile-time execution](testing.md): `mettle test` and `mettle trace`.
- [Linker and build pipelines](linker-build-pipelines.md): which linker runs
  when.
- [Runtime model](runtime-model.md): what an emitted program assumes of the OS.
- [Bare metal](bare-metal.md): inline assembly, `volatile`, `@naked` and
  `@interrupt`, cross-compilation, a chosen link address, and 16-bit code.
- [C interoperability](c-interop.md): calling out, being called, the struct
  ABI, and `syscall`.
- [Shared libraries](shared-libraries.md): binding a `.so`, emitting one, and
  what the ELF linker refuses.

## Optimization

- [Profile-guided optimization](pgo.md): `--pgo`, with no training run.
- [Translation validation](translation-validation.md): `--verify`, which
  catches the optimizer's own bugs, and `reference` twins, which point the same
  differential at two functions the program says agree.
- [Rewrite rules](rewrite-rules.md): `rewrite`, identities from your own code
- [Rules](rules.md): `@rule`, a property the program requires of itself, checked on every build
  that the compiler proves, applies, and checks. `--report-rules` and
  `--rule-budget=N` keep the cost on a ledger, as `--report-proofs` /
  `--proof-budget=N` and `--report-effects` / `--effect-budget=N` do for the
  declared-type prover and the effect pass.
- [Effects](effects.md): `effect`, `with`, `forbids`, `requires`, `provides`; what a function does and needs, inferred through the call graph and proven on every build.
- [ML-driven IR optimization](ml-opt.md): `--ml-opt` and its validation gate.
- [The --explain-json schema](explain-json.md): the machine-readable
  optimization report.
- [gnn_oracle](ml-opt-oracle.md): the design of the successor model.

## GPU

- [GPU offload](gpu.md): `kernel`, `dispatch`, and the PTX and SPIR-V targets.
- [GPU architecture](gpu-architecture.md): the target matrix and the
  acceptance gates.

## The backend

libmtlc owns the IR, the optimizers, code generation for four targets, and
linking. Any frontend can build IR and drive it through the C API in
[`include/mtlc/`](../include/mtlc/).

- [Writing a frontend](embedding.md): the tutorial, with a complete non-Mettle
  example.
- [libmtlc reference](libmtlc/README.md): the API, the IR model, the type
  system, the pipeline, and the internals.
- [Mettle and libmtlc](mettle-and-libmtlc.md): where the frontend ends and the
  backend begins.

A second, non-Mettle frontend that exercises the whole public API lives in
[`examples/calc`](../examples/calc).

## Design

- [Ideology](ideology.md): the rules the language holds itself to, and what
  each one costs.
- [Architecture](ARCHITECTURE.md): the pipeline, subsystem by subsystem.
- [Benchmarks](benchmarks/README.md): the suites, how they are measured, and
  how to regenerate them.

## Contributing

[CONTRIBUTING.md](../CONTRIBUTING.md) has the build and test workflow, and the
rules that keep the backend frontend-agnostic.
