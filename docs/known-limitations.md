# Known limitations

What does not work yet, verified against the compiler in this checkout. Each
entry is something a reader would otherwise hit and have to work out.

## Language

A tagged-enum variant carries at most one payload. `Circle(float64)` works;
`Pair(int32, int32)` does not parse. Wrap two values in a struct.

An array is one-dimensional. `int64[3][4]` does not parse; index a flat
`int64[12]` yourself.

A `T[N]` has its length in its type, so `N` has to be a constant. A length the
program computes belongs to a [slice](types.md), `T[]`.

Two types that store each other by value have no size, and that is reported:

```text
error[E0003]: 'A' and 'B' each store a value of the other, so neither has a
size. Hold one of them by pointer: 'B*'
```

Pointing at each other is fine, in a cycle of any length.

## Aggregate literals

An element of a literal initializing a local may be any expression the field
would accept in an assignment, computed values included. What is known while
compiling is laid out in the object file and copied in; what is not becomes a
store into that image at the element's own offset, so the two mix freely inside
one literal.

A `const` and a module-scope `var` are laid out before any code runs, so their
elements have to be constants: literals, other constants, `sizeof`, arithmetic
over those, `&function`, `&global`, `0`, string literals, and nested literals.
An element that is not reports where it sits:

```text
error[E0003]: a constant and a module-scope variable are laid out before the
program runs, so every element has to be known while compiling, and this one is
not. Make it a constant, or build the value in a function
```

A literal needs a target type, which it takes from the `var`, `const`,
assignment, or parameter it initializes. It cannot stand where no type says
what it is.

A closure cannot appear in one, because its environment is built at run time.
Writing a lambda where a `Fn` field is expected inside a literal reports a
mismatch against `fn(...)`.

Omitted struct fields and short array literals leave the rest zero. Extra
elements, unknown field names, and repeated field names are errors.

## Compile-time expansion

`comptime for` iterates two sequences: `typeof(T).fields`, and `TABLE.rows` for
a `const` holding an array literal. Anything else is refused by name:

```text
error[E0003]: 'comptime for' cannot iterate '.each'; the compile-time sequences
are '.fields' and '.rows'
```

A row of a struct answers to its own columns and to `.index`; a row of a plain
value is that value. Every column has to be a compile-time constant, which a
`const` guarantees.

Compile-time strings compare and nothing else. `==` and `!=` fold, which is
enough to check that two declarations agree by name. There is no
concatenation, ordering, length, or substring. `ident(...)` composes
declaration names under rules the compiler checks.

`ident(...)` composes a declaration's name, not a type. A generated type can be
named where it is used, and not from inside the iteration that generated it.

A `comptime for` reflects on the types the program wrote. Module-scope
expansion runs after those are registered and before the generated ones are, so
`typeof(GeneratedStruct).fields` reports an unknown type.

The binding cannot appear where a type goes. `f.type` answers `f.type.size` and
`f.type.kind`, and cannot be written as a parameter, return, or field type.

## Declared types

The prover behind `type Name = Base where predicate;` is deliberately cheap.
It follows integer ranges through constants, narrower types, `+`, `-`, `*`,
`/` and `%` by a positive constant, `&` with a non-negative mask, `>>` by a
constant, `for` ranges, and the comparisons in dominating `if` and `while`
conditions, including the negated condition after an `if` that returns. A
predicate that is not a comparison, such as `is_valid(value)`, is proven only
by a guard that repeats it on the same expression. Float predicates are proven
by constants and by guards, never by arithmetic. Nothing is proven across a
call: a function that checks its argument does not make the caller's value
proven.

A declared type refines a number, a bool, a char, a string, a pointer or a
slice. A struct, an enum or an array cannot be the base.

`mettle test` checks the range conjuncts of a proven conversion as it runs,
and `--check-proofs` does the same in a compiled program. A conjunct that
calls a function is not re-evaluated in either.

A rule sees a declared type in `p.types` with kind `declared` and its base
named, and does not see the predicate itself.

## Effects

A global written from two places is checked against the effects each writer
needs. A heap object is not: two functions reaching the same allocation
through pointers is the aliasing question, and the analysis does not answer
it, so `F0006` speaks about globals only.

The check needs both writers to need an effect something `provides`. One
writer with no placed requirement means the program has not said where that
code runs, and nothing is claimed. Any effect appearing in both requirement
sets counts as the thing that orders them, because whoever provides it runs
them one at a time; the compiler does not ask whether that effect is a lock,
a phase or an initialised subsystem.

A function type with no `with` clause is open, so a call through it performs
`unknown`, and a function that `forbids` any effect cannot make one. Give the
type a `with` clause; `with none` is the closed empty set.

A call outside the program with no `with` clause is taken to perform `alloc`
and nothing else. It may in truth perform a user effect, and the compiler
cannot know; declare the extern `with` what it does.

A closed `with` clause on a function type includes the built-in effects, so a
function that prints does not fit `fn() -> void with Sim`; the type has to say
`with Sim, alloc`.

`main`, `@interrupt` and `@naked` functions, kernels, tests and the exports of
a `--shared` library are the entry points where requirements have to be
settled. A function whose address is taken and called from outside the
program through some other route is not seen as an entry point.

A method's clauses are checked like a function's. A lambda's effects are
inferred; a lambda cannot declare a clause.

The run-time check keeps one effect frame per function that declares a
clause, on a stack of at most 4096 frames per thread and 256 threads, in
`--check-effects` and `mettle test` only.

## Closures

A plain function value already sitting in a variable is not adapted to a
closure type. Take the address at the point you need it, `&twice`, which is
accepted at a declaration, an argument, a return, and an assignment.

## Borrow analysis

A borrow handed across a call boundary is not followed: the interior pointer
`&buf[4]` passed to a function is not related back to `buf` inside it.

A leak is reported for an allocation the function never frees. One the function
frees on some paths and not on the path an early `return` takes is not reported,
so a `defer free(p)` right after the allocation is still what makes the release
cover every exit.

Ownership itself is inferred per function and iterated over the call graph, so
a free, a store, and a fresh allocation do cross calls. Within a function the
analysis follows paths: a fact inside an `if`, a loop body, a `switch` arm or a
`match` arm is definite for that path, and what survives the join is what every
arm agreed on.

There is no ownership syntax, and the analysis rejects a program in exactly
three places: returning the address of a local, handing a task a pointer into
the frame that spawned it, and writing through a pointer already handed to a
task. Everything else it points at, it points at only when it can prove it.

A task is recognised by the shape of the call, meaning the address of a
function this program defines followed by a pointer argument. A spawn that
takes its entry point out of a struct field, an array or a variable is not
recognised, so nothing is claimed about it. Neither M0121 nor M0122 fires on
one, and `--check-tasks` emits no check there either.

`--check-tasks` bounds the spawning thread's stack exactly where the operating
system hands the bounds out. Where it does not, the check covers a thread
stack's span above the current frame, so an allocation that landed within that
span of the stack would be reported as a capture. No allocator on either
supported platform places one there, and the check is opt-in, so this is a
stated approximation and not a silent one. See
[Borrow checker](borrow-checker.md).

## Null and bounds checks

Constant null dereferences are diagnosed while compiling. Run-time null checks
are emitted for dynamic dereferences in normal builds and dropped under
`--release`.

Fixed-size array indexing is checked at compile time for constant indices and
guarded at run time in normal builds, and those guards are dropped under
`--release`. Use [`--safe`](memory-safety.md) to keep them, including under
`--release`.

Pointer indexing is never bounds-checked, because the compiler does not know
the pointee's extent. A slice does carry one, so `T[]` indexing is checked
against the length the value holds, under the same rule: guarded in normal
builds, dropped under `--release`, kept under `--safe`. Pointers arriving from
C or from inline assembly can be invalid in ways nothing can prove.

## Vectorization

Reductions over `^`, `&`, and `|` have no kernel and are reported as serial.

Float elements have no select kernel, so a clamp over `float32` or `float64`
stays scalar. The same clamp over `int32` vectorizes.

A reduction over a byte array needs a 64-bit accumulator. An `int32`
accumulator summing bytes is reported rather than vectorized.

Depth is bounded by registers. An int32 map has six ymm registers for its
expression, and a deeper one falls back.

`--explain` names the reason for every loop it leaves alone, and
`mettle explain <code>` expands any of them.

## Struct ABI

Struct-by-value arguments and returns work in both directions on both
platforms, under the Microsoft x64 rule on Windows and System V's eightbyte
classification on Linux. See
[C interoperability](c-interop.md).

## Deferred calls

A deferred direct call, `defer f(args)`, captures its argument values at the
defer point and replays them at scope exit.

A deferred method call, `defer obj.m(...)`, and a deferred call through a
function pointer re-evaluate their operands at scope exit. Snapshot into a
local first when you need the value from the defer point.

`errdefer` is function-only and convention-based: any non-zero explicit return
is treated as an error.

## Platform

Shared libraries are ELF `.so` and Windows DLL. On Windows `--build --shared`
emits a DLL whose exports are the user globals (`export fn/var`; compiler-owned
`mettle_*` tables stay private) with no entry point; `-l`, `--export-dynamic`,
`--rpath` and `--dynamic-linker` stay ELF-only, and a DLL takes its libraries
through `--link-arg`. A DLL must load at its preferred base (no `.reloc` is
emitted yet) and ships no import library. A shared object Mettle emits cannot
reference imported data, and holds no
thread-local storage: its build of the runtime keeps `errno` per process. See
[Shared libraries](shared-libraries.md).

The Windows internal linker takes its libraries through `--link-arg`, not
through `-l`.

`std/ui`, the Win32 window and control helpers, is Windows-only. It has no
Linux counterpart.

External Tracy needs a C++ runtime, so `--tracy` fails under the owned runtime
rule. `--profile-runtime` is the built-in alternative.

`--musl` is rejected, because linking musl would break the owned-runtime rule.

## Narrow targets

The 16- and 32-bit targets compute in one register's worth of value. A value
wider than a word -- `int32` in 16-bit code, `int64` or a float in either -- is
a compile error naming the declaration. A struct or an array is a frame region
reached by address, so both work; strings do not travel through these targets
at all. Pointers are near.

Neither target has an object format that carries its relocations, so
`--emit-flat` is their only product; asking for an object or an executable is
an error rather than a file whose code is the wrong width for its header.

Real mode pushes no error code, so a 16-bit `@interrupt` handler takes no
parameters or the interrupt frame alone. `bits 16` and `bits 32` are allowed
inside a `@naked` function, which is the only place the compiler contributes no
bytes of its own.

A freestanding target has no runtime, so it emits no runtime checks and refuses
`--safe`. `--build` needs a linker and a runtime belonging to the machine being
built for, so it is refused for a foreign or freestanding target.
Cross-compiling means emitting the object here and linking it there.

## Compiler

An expression may nest 4096 levels deep, and blocks may nest 4096 deep. Past
either the compiler reports it:

```text
error[E0002]: Expression nests more than 4096 levels deep
```

Nesting is what counts, not length: `a + b + c + ...` folds in a loop and costs
one level however long it runs. The ceiling exists because each level is a
frame in the recursive descent and in every pass that walks the tree
afterwards, and without it deep enough input exhausted the stack and killed the
process with no diagnostic. It sits an order of magnitude below where that
happened.

Unreachable-code analysis is block-local and conservative. Some dead paths in
complex control flow are not diagnosed.

A `@rule` over a `Trace` runs in the compile-time interpreter under
`mettle test` and nowhere else. There is no sampled version of it inside a
compiled program: that would mean linking the rule and building the `Trace` at
run time, and neither exists. A trace rule therefore says what the runs your
tests drive did, and says nothing about a run in production.

Mettle emits no integer overflow check, so a declared range has none to delete;
what a range earns is the bounds check and the divide. Two pointers cannot be
declared disjoint, so a vectorizer that refuses over a possible overlap cannot
be given a proof to proceed on; it refuses rather than emitting a run-time
overlap test, so there is no test for a proof to remove either.

A float accumulator carries a declared bound only where the compiler bounded
the loop's trip count, which means a counter with a constant initialiser, a
constant step and a constant limit. A loop over a runtime length gives the
accumulator no bound, so a declared type on it is refused.

The interpreter watches every declared-`@pure` and every inferred read-only
frame under `mettle test` and traps on a write inside one, but that watch can
only confirm the static proof: purity is proven by refusing every operation
that could write, so no Mettle program can pass the check and then write, and
the watch is proven live by `--check-purity-fault`, which corrupts the analysis
on purpose so the watch has something real to catch.

## See also

- [Memory safety](memory-safety.md)
- [Compilation](compilation.md)
- [Runtime model](runtime-model.md)
