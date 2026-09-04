# Effects

An effect is a fact about what running a function does, or what it needs, that
the compiler carries through the whole call graph and proves on every build.
The program declares the effects it cares about, says where each one comes
from and where it may not go, and the compiler infers everything in between.

```mettle
effect Render;
effect MainThread;

fn gl_draw() with Render { ... }

fn mix_audio() forbids Render, alloc { ... }

fn touch_window() requires MainThread { ... }

fn main_loop() provides MainThread { ... }
```

Nothing is annotated in the middle. A function performs what it is declared
`with` and everything its callees perform; a function needs what it is
declared to `require` and everything its callees need, less what it itself
`provides`. The compiler computes both to a fixed point, then checks the four
claims a program can make.

## The four clauses

A clause follows the signature, after the return type, in any order:

| Clause | Meaning | Checked how |
|--------|---------|-------------|
| `with E` | This function is a source of `E`. | Nothing to check; it is a declaration. |
| `forbids E` | Nothing this function reaches performs `E`. | Proven over the call graph, `error[F0001]` with the chain when it fails. |
| `requires E` | This function may only run where `E` is provided. | Every path from an entry point to it passes a `provides E`, `error[F0002]` when one does not. |
| `provides E` | Everything this function's body calls may require `E`. | Nothing to check; it discharges requirements below it. |

`with` is the only clause that adds an effect. Everything else is inference.
`fn frame() { draw_scene(); }` performs `Render` because `draw_scene` reaches
`gl_draw`, and nobody wrote that down.

```text
-- app.mettle:17:4 ----------------------------------------------------------
error[F0001]: 'mix' forbids 'Render' but reaches it: mix -> frame -> draw_scene -> gl_draw

   17 |  fn mix() forbids Render {
      :     ^^^ the forbidding function
      |  note  `mix` calls `frame` here
      |  note  `frame` calls `draw_scene` here
      |  note  `draw_scene` calls `gl_draw` here
      |  note  `gl_draw` is declared `with Render`
```

Every hop is a note at the call that took it, so the fix is one of the lines
on screen.

## Built-in effects

Three effects exist without a declaration, because the compiler knows their
sources:

| Effect | Performed by |
|--------|--------------|
| `alloc` | `new`, the allocator entry points, string concatenation, and a call outside the program that declares no effects |
| `asm` | an inline `asm` block |
| `syscall` | the `syscall` built-in |

`@noalloc` and `forbids alloc` hold a function to the same standard: anything
that cannot be proven allocation-free counts against it. A call to a function
outside the program is unprovable unless the declaration says otherwise, so an
extern carries its own `with` clause, and `with none` says it performs nothing:

```mettle
extern fn glFinish() with none;
extern fn glMapBuffer(target: int32, access: int32) -> rawptr with alloc;
```

The compiler believes an extern's `with` clause for what it cannot see, and
that belief is checked at run time under `mettle test` and `--check-effects`
(below). A known allocator such as `malloc` performs `alloc` whatever its
declaration says.

`mettle why app.mettle main Audit` prints the chain for an effect that holds:
every call from the named function down to the line that performs it, the same
chain a `forbids` refusal would have printed.

A belief is never silent. `--explain` prints a **beliefs** section listing
every extern the build took on trust: the ones whose `with` clause was read as
written, the ones on the compiler's known-clean list, and the ones with no
clause at all, which are taken to allocate. A build with nothing on that list
says so in as many words.

`--report-effects` prints what the pass settled, one line per function that
performs or needs anything, with the totals: functions seen, functions with an
effect, fixpoint rounds, steps. `--effect-budget=N` makes that cost a
contract and fails the build with `F0005` when the pass exceeds it.

## Capabilities

`requires` and `provides` are how an execution model becomes something the
compiler enforces. Thread affinity is the plain case:

```mettle
effect MainThread;

fn touch_window() requires MainThread { ... }

fn ui_tick() { touch_window(); }

fn main_loop() provides MainThread { ui_tick(); }

fn main() -> int32 {
  main_loop();
  ui_tick();
  return 0;
}
```

`ui_tick` requires `MainThread` because `touch_window` does; nobody wrote that.
`main_loop` provides it, so the first call is fine. The second call reaches
`touch_window` from `main` with nothing provided:

```text
error[F0002]: 'main' reaches a function that requires 'MainThread', and nothing
on the way provides it: main -> ui_tick -> touch_window
```

A held lock, an initialised subsystem, a frame phase, a GPU context: anything a
function must not run without is an effect it requires, and the one place that
establishes it provides it. The places where requirements have to be settled
are the entry points, where nothing is provided: `main`, an `@interrupt` or
`@naked` function, a `kernel`, a `@test`, and an exported function of a
`--shared` library.

## Two threads writing one global

Once a program says where its code runs, it has already said which globals two
threads share, and the compiler reads that rather than asking for it again. A
global written by two functions whose requirements are disjoint is refused,
naming both writers and the effect each one runs under:

```mettle
effect Sim;
effect Render;

var frame: int32 = 0;

fn tick() requires Sim { frame = frame + 1; }
fn draw() requires Render { frame = 0; }
```

```text
error[F0006]: 'frame' is written by 'draw', which runs where 'Render' is
provided, and by 'tick', which runs where 'Sim' is provided, and nothing
either one needs orders the two writes
```

An effect both writers require is what orders them, because whoever provides
it runs them one at a time. That is what a lock is, written as a requirement
instead of a convention:

```mettle
effect FrameLock;

fn tick() requires Sim, FrameLock { frame = frame + 1; }
fn draw() requires Render, FrameLock { frame = 0; }
```

The claim is only ever made about effects a function somewhere `provides`. A
program that declares none is not saying where its code runs, and this says
nothing about it. `--report-effects` prints the ledger either way:

```text
shared frame: tick (Sim), draw (Render), ordered by an effect both require
shared globals: 1 written from more than one place, 1 ordered
```

The verdict rests on the placement facts and nothing else, and those are what
`mettle test` and `--check-effects` re-check at run time, so a program that
lied about where a function runs is caught by the machine rather than by the
analysis that drew this conclusion from it. The check itself emits no code: a
build with it and a build with it skipped are byte-identical.

## Function types

A function type may carry `with` and `requires`:

```mettle
fn run_sim(job: fn() -> void with Sim) { job(); }
fn on_ui(job: fn() -> void requires MainThread) provides MainThread { job(); }
```

A type with a `with` clause is closed: a value of it performs those effects and
nothing else, so a call through it is as good as a direct call. A function
passed by name is checked against the clause, and its inferred effects have to
fit:

```text
error[F0003]: 'paint' is handed to a type declaring `with Sim`, but it may
perform 'Render': paint
```

`with none` is the closed empty set. A type with no `with` clause is open: a
call through it may perform anything, which is why a function that `forbids`
an effect cannot call through one, and why a value of an open type cannot flow
into a closed one. The compiler has nothing to check it against.

`requires` on a type says what a caller through it must provide. A function
that requires `MainThread` cannot be handed to a `fn() -> void`, because
whoever calls through that type would not know to provide it; the type has to
say `requires MainThread`. This is what keeps a thread entry honest: the
function you spawn is the one that `provides` the thread's effect to
everything it calls.

Closures, `Fn(...) -> R`, take the same clauses.

## Effects a module exports

`export effect Render;` makes the effect visible to whoever imports the
module, and the functions the module declares `with` it carry it into the
importing program. An effect that is not exported is the module's own.

## What a rule sees

Every function in a `@rule`'s `Program` carries `effects` (what it performs,
inferred), `requires` (what it needs, inferred), `forbids` and `provides` (as
declared), and `p.effects` lists every effect with its declaring site.
`function_performs`, `function_requires`, `function_forbids`,
`function_provides` and `program_effect_index` are in `std/rule`. A rule can
therefore say what the language has no clause for: every function in this
module that performs `Render` is named `gl_*`, or nothing outside `audio/`
requires `AudioThread`. See [Rules](rules.md).

## Checked by something that does not trust the prover

Under `mettle test`, `mettle trace` and `--verify`, every function with a
clause maintains an effect frame as it runs, every source of a built-in
effect announces itself, and a violation traps with the two names involved:

```text
Fatal error: effect violation: `quiet` performs `alloc`, which `mix` forbids
```

`--check-effects` puts the same frames into a compiled program, `--release`
included, and a program that declares no effect is byte-identical with the flag
and without it. `--safe` distrusts the program's indices; `--check-effects`
distrusts the compiler's effect analysis and any extern's `with` clause.

## `mettle expand`

The clauses print where they were written, so a program that was generated,
imported or rewritten shows exactly what the compiler checked:

```text
fn mix() forbids Render, alloc {
```

## Cost

Nothing at run time. The analysis is one fixed point over the call graph per
build, and a program that declares no effect and hands no function to a typed
function pointer never runs it.

## See also

- [Rules](rules.md)
- [Types](types.md), for declared types, the other half of a program stating
  its own properties
- [Runtime model](runtime-model.md), for the execution model the program builds
- [Diagnostics](diagnostics.md), for the F codes
