# Mettle language reference

Mettle is a typed, assembly-inspired systems language. Its frontend lowers
Mettle source into the [libmtlc](../README.md) IR, which the backend optimizes
and compiles to native machine code, x86-64 or ARM64, or to a GPU target, PTX
or SPIR-V. There is no external assembler and no assembly text anywhere in the
pipeline.

This index covers the language. To drive the backend from a different
frontend, see [Writing a frontend](embedding.md).

## The language

1. [Lexical structure](lexical-structure.md)
2. [Types](types.md)
3. [Declarations](declarations.md)
4. [Expressions](expressions.md)
5. [Control flow](control-flow.md)
6. [Modules and imports](modules.md)
7. [Standard library](standard-library.md)

## Memory

8. [Heap allocation](heap-allocation.md)
9. [Memory safety](memory-safety.md)
10. [Borrow checker](borrow-checker.md)

## Building

11. [Getting started](getting-started.md)
12. [Compilation](compilation.md)
13. [Diagnostics](diagnostics.md)
14. [Compile-time execution](testing.md)
15. [C interoperability](c-interop.md)
16. [Runtime model](runtime-model.md)
17. [Rewrite rules](rewrite-rules.md)
18. [Rules](rules.md)
19. [Effects](effects.md)

## Beyond the CPU

20. [GPU offload](gpu.md)

## Reference

21. [Quick reference](quick-reference.md)
22. [Known limitations](known-limitations.md)

Every mechanism above reports what it spent and what it assumed: `--explain`
carries sections for the types proven, the effects held, the rules run, and
the beliefs the build rested on, and each has its own `--report-*` flag and
`--*-budget=N` contract. [Compilation](compilation.md) lists them.

For the backend itself, the IR, the optimizers, the code generators, the
linker, and the public C API, see the [documentation index](README.md).
