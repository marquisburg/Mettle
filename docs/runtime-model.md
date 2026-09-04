# Runtime model

What an emitted Mettle program assumes of the operating system, and what it
carries with it.

Every native product uses a runtime this project owns. Nothing links libc, a C
startup package, a compiler support library, pthread, musl, UCRT, MSVCRT, or a
C++ runtime.

There is still no garbage collector, no async scheduler, no thread pool, and no
background service. The runtime is small and it is real: it owns the startup
path and each basic service that generated code needs.

## What the runtime provides

| Service | Windows x86-64 | Linux x86-64 and AArch64 |
|---------|----------------|--------------------------|
| Entry | `mettle_start` | `_start` |
| Arguments | `GetCommandLineA` parser | Initial process stack |
| Exit | `ExitProcess` | `exit` system call |
| Heap | Process heap APIs | Anonymous memory maps |
| Files and console | Kernel32 file APIs | File system calls |
| Threads | Kernel32 thread and wait APIs | `clone` and `futex` |
| Thread-local state | Fiber local storage | Owned TLS image and thread pointer |
| Sockets | Winsock, when asked for | Socket system calls |
| Clocks | Kernel32 clocks | Clock system calls |
| Process launch | `CreateProcessA` | `fork`, `execve`, `wait4` |

The source is `src/runtime/freestanding.c`. It includes no standard C header,
compiles with freestanding flags, and has no unresolved symbol on Linux. Its
Windows object refers only to OS imports.

It exports familiar ABI names: `malloc`, `calloc`, `memcpy`, `puts`, `strtod`,
`clock_gettime`, `pthread_create`. Mettle supplies all of them. The pthread
names are a source-compatibility layer over the owned clone and futex code.

## Startup

`src/codegen/binary/startup.c` writes the startup object for the target. It
covers the Windows x86-64 COFF ABI, the Linux x86-64 ELF ABI, and the Linux
AArch64 ELF ABI. The startup code initializes the runtime, passes arguments to
`main`, runs any diagnostic hooks, and exits through the OS.

`src/runtime/host_startup.c` gives the same entry contract to the reference
compiler and to C programs embedding libmtlc.

## Optional objects

The linker adds these only when the program asks for the feature:

| Object | Provides |
|--------|----------|
| `crash_handler.o` | Source locations and stack frames on a fault |
| `profile.o` | The runtime profile |
| `debug.o` | Interactive debug hooks on Windows |
| `swap.o` | Code swapping |
| `tracy_helpers.o` | A local no-op Tracy ABI |

They use the same owned ABI and add no host runtime.

External Tracy needs a C++ runtime, so `--tracy` fails under the owned runtime
rule. `--profile-runtime` is the built-in alternative.

## Link rules

Windows uses the internal PE linker by default. The GCC fallback passes
`-nostdlib`, `-nostartfiles`, and `-nodefaultlibs`, selects `mettle_start`, and
links only the OS libraries asked for.

Linux uses `ld` directly when it can, and uses GCC purely as a link driver with
the same three switches otherwise. Every Linux executable is a static
`ET_EXEC`. `--static` is accepted and does nothing, since that is already true.
`--musl` fails, because linking musl would break the rule.

[Linker and build pipelines](linker-build-pipelines.md) has the full matrix.

## The checks

The compiler audits each executable before reporting success.

For PE32+ it reads the normal and delayed import tables and rejects names from
UCRT, MSVCRT, VCRuntime, the Microsoft C++ library, libgcc, libstdc++, and
libwinpthread.

For ELF64 it requires `ET_EXEC` and rejects `PT_INTERP` and `PT_DYNAMIC`, which
is what proves a plain Linux build carries no foreign runtime. Asking for `-l`,
`--shared` or `--export-dynamic` states the opposite intent, so that link is
audited without the dynamic-segment rule and with everything else unchanged.
See [Shared libraries](shared-libraries.md).

The driver also rejects a link argument naming a C, compiler, or thread
runtime. The build scripts audit the compiler itself, and the libmtlc build
combines the whole archive and checks its final external symbol set.

## Adding a target

A native target is finished when it has four things:

1. An owned entry object.
2. An owned service layer for every ABI symbol code generation can emit.
3. A link path with all default startup files and libraries disabled.
4. A format check that rejects hidden runtime dependencies.

This holds for the reference compiler, for libmtlc embedders, for generated
programs, for the optional diagnostics, and for both linker paths.

## Excision is gated

Every optional component is absent from a binary that did not ask for it, and
the absence is checked on every build.

| Component | Present only with |
|-----------|-------------------|
| Safety runtime | `--safe` |
| Crash handler and backtrace | `-s`, `--stack-trace` |
| Runtime profiler | `--profile-runtime` |
| Debug hook server | `--debug-hooks` |

The `runtime_components_excisable` test builds
`tests/runtime_excision_probe.mettle` twice per component and searches the
binary for a string only that component contains. Absent without the flag,
present with it.

The second direction is the point. An absence check alone passes when the
marker never appears at all, which makes the gate a decoration. Requiring the
marker to appear when the feature is requested is what gives the absence
meaning. The sizes tell the same story: the probe links at 67 KB plain and
141 KB under `--safe`.

A component that cannot be left out has stopped being optional, and this gate
is what would notice.

## Code swapping

A function can be replaced in a running process. The compiler contributes a
boundary and a point; the runtime contributes a staged store.

```mettle
import "std/io";

extern fn mettle_swap_stage(slot: rawptr, replacement: rawptr) -> int32;

@swappable fn policy_v1(n: int32) -> int32 { return n + 1; }
@swappable fn policy_v2(n: int32) -> int32 { return n * 10; }

var policy: fn(int32) -> int32 = &policy_v1;

fn main() -> int32 {
  println("{policy(5)}");
  mettle_swap_stage(&policy, &policy_v2);
  println("{policy(5)}");
  quiesce;
  println("{policy(5)}");
  return 0;
}
```

```text
6
6
50
```

Staging is separate from applying. `mettle_swap_stage` records an intent, and
the call right after it still runs the old function. Only `quiesce;` applies
staged swaps, so a replacement cannot land halfway through an operation the
programmer treated as one. Nothing is applied on a timer, at a safepoint the
compiler picked, or inside the staging call.

The binding is a slot. Applying a swap is one pointer-sized store. Nothing
writes to executable memory, the process never toggles W^X, and a thread
already inside the old body finishes there, because that body is still
resident. Rewriting instructions in place would mean halting every other
thread first, and stopping the world is the unauthored control flow these
rules exist to forbid.

`@swappable` keeps the call site. A swap redirects a call, so the call has to
survive, and the decorator implies `@noinline`. `@swappable` together with
`@inline` is refused: an inlined body has no call to redirect and no single
place to name.

It is opt-in and provable. A program with no `quiesce;` never references
`mettle_swap_` and never links `swap.o`, which the excision gate checks on
every build.

The staging table is fixed-size and allocates nothing, so a swap cannot fail
for want of memory at the moment a program is trying to replace the code that
was going wrong. Restaging a slot replaces the earlier intent.

## The execution model is the program's

Mettle used to ship a scheduler, and it was deleted for the reasons above. What
a program does now is build the one it needs, in the language, and say what it
must satisfy. `std/thread` maps to Kernel32 threads on Windows and to `clone`
with futexes on Linux, and stops there: no thread pool, no executor, no
message loop is linked unless the program wrote one.

Two things make that a supported shape and not an absence. The first is
`quiesce`: a point the program names, at which staged work lands and at no
other time. Every model worth having has such points, a frame boundary or the
top of a request, and the program writes them. Nothing yields, ticks or
collects at a point nobody wrote.

The second is that the properties a model depends on are stated in the program
and checked on every build. A job that must not allocate is `@noalloc`. A
queue index that must stay in the ring is a [declared type](types.md) the
compiler proves in range. Where code may run is an [effect](effects.md): a job
`requires Worker`, the worker's entry `provides` it, and a job reached from
anywhere else is refused with the call chain, so thread affinity is a fact the
compiler holds and never a comment. What the compiler has no word for is a
[rule](rules.md): a state machine's `step` decides every state, every function
that performs `Render` lives in one module. A rule that cannot decide says so,
which is what happens the moment a call goes through a function pointer whose
type says nothing about its effects.

[`examples/job_system/`](../examples/job_system/) is the whole shape in one
file: a queue on `std/thread`, a priority function swapped at `quiesce`, a
`Slot` type, a `Worker` effect, and three rules the build stops on.
[`examples/engine/`](../examples/engine/) is that queue with a frame around
it: a schedule read as data, a phase effect per phase, a global two phases
write with a lock effect ordering them, a declared deadline, and the three
ways to break it written down beside what each one prints.

## A schedule is data the compiler reads

A frame is an order, and an order is data. `std/schedule` gives it a type, and
a `const` of that type is the whole frame written down:

```mettle
import "std/schedule";

effect Input;
effect Sim;
effect Render;

const FRAME: Schedule[3] = [
  { phase: "input", effect: "Input", entry: "read_input", thread: 0 },
  { phase: "sim", effect: "Sim", entry: "step_world", thread: 1 },
  { phase: "render", effect: "Render", entry: "draw_world", thread: 0 },
];
```

Each row is a phase: what it is called, the effect that holds while it runs,
the function it runs, and the thread it runs on. From that the compiler
generates one wrapper per phase and one dispatcher per thread, with a
`quiesce` at every phase boundary:

```mettle
fn FRAME_phase_input() provides Input { read_input(); }
fn FRAME_phase_sim() provides Sim { step_world(); }
fn FRAME_phase_render() provides Render { draw_world(); }

fn FRAME_thread_0(arg: cstring) -> uint32 {
  var frames: int32 = (int32)(int64)arg;
  var frame: int32 = 0;
  while (frame < frames) {
    FRAME_phase_input();
    quiesce;
    FRAME_phase_render();
    quiesce;
    frame = frame + 1;
  }
  return 0;
}

fn FRAME_thread_1(arg: cstring) -> uint32 {
  var frames: int32 = (int32)(int64)arg;
  var frame: int32 = 0;
  while (frame < frames) {
    FRAME_phase_sim();
    quiesce;
    frame = frame + 1;
  }
  return 0;
}
```

A dispatcher runs for as many frames as its argument says, which is why the
loop is not control flow at a point nobody wrote: the count came from the
call. `FRAME_thread_0((cstring)60)` runs sixty frames and
`FRAME_thread_0((cstring)1)` runs one.

That is what `mettle expand` prints, and it is ordinary Mettle: the type
checker, the borrow analyser and every contract meet it exactly as they meet
hand-written code. The dispatcher is not control flow at a point nobody wrote.
The program wrote the schedule, and the schedule is the order; every `quiesce`
sits at a phase boundary the program named, which is where a staged swap lands
and where a frame is allowed to end.

A thread's dispatcher takes the `cstring` and returns the `uint32` a thread
entry point takes, so the program starts the others itself and keeps the
handles:

```mettle
var worker: int64 = CreateThread(0, 0, &FRAME_thread_1, (cstring)60, 0, 0);
FRAME_thread_0((cstring)60);
thread_join_infinite(worker);
```

## Where the threads meet

A phase may end where every thread has to arrive before any of them starts the
next frame:

```mettle
const FRAME: Schedule[2] = [
  { phase: "simulate", effect: "Sim", entry: "step_world", thread: 1, joins: true },
  { phase: "present", effect: "Render", entry: "draw_world", thread: 0, joins: true },
];
```

Every thread calls the wait, whether or not it runs that phase, because a
barrier the threads that skip the phase walk past is not one. The counter
behind it only ever rises: a thread arriving for frame `g` leaves the count at
`g` times the number of threads, so there is nothing to reset and nothing to
race over, and it is `volatile`, so the spin reads memory every time round.

```mettle
var FRAME_arrived_simulate: volatile int32 = 0;

fn FRAME_wait_simulate(generation: int32) {
  atomic_inc_i32(&FRAME_arrived_simulate);
  while (FRAME_arrived_simulate < generation * 2) { }
}
```

That comes out of `std/thread`, so a schedule with a join in it needs the
module to import one; `H0007` says so when it does not. A schedule with no
`joins` generates none of this and the threads run independently, which is
what the shape without a join means.

Because each wrapper provides its own phase's effect and no other, a call that
crosses a phase boundary arrives somewhere its requirement is not provided,
and the ordinary effect pass refuses it with the chain, landing on the row of
the schedule that took it there:

```text
error[F0002]: 'main' reaches a function that requires 'Sim', and nothing on
the way provides it: main -> FRAME_thread_0 -> FRAME_phase_input ->
read_input -> touch_world
```

The schedule itself is checked before anything is generated: a phase names
itself, an effect and an entry (`H0001`); no two phases share a name or an
effect, since one effect per phase is what makes the boundary visible
(`H0002`); the effect is declared where the schedule can see it (`H0003`); and
the entry is a function of the module (`H0004`). A schedule is read while
compiling, so it is a `const` (`H0005`).

`--report-expansion` says what a schedule cost, and says so when there was
none:

```text
schedules: 1 read as data, 3 phases, 5 functions generated
schedules: none; nothing generated
```

What a program cannot get is the other kind of model: `async`/`await` with a
yield the compiler inserts, or a work-stealing pool that runs code at a point
the program did not name. A yield point the compiler inserts is unauthored
control flow, the same event as a collection point, and the line above forbids
it however good the feature would be.

## The runtime is moving to Mettle

A runtime you cannot inspect is indistinguishable from a runtime that is
lying. The rule is that it reads as source, steps in the same debugger, and
appears in the same profiler, which a component written in C does not do for a
Mettle programmer.

The swap runtime complies first. `src/runtime/swap.mettle` is compiled by the
compiler this build produces, after that compiler is linked, and staged into
`bin/runtime/` beside its source. It is ordinary Mettle code, so `-d`,
`--debug-hooks`, and `--profile-runtime` reach it as they reach a program. It
is also smaller than the C it replaced.

The newest component leads rather than being exempt, because a rule that
applies only to future work is a rule nobody has tested. The remaining five,
`crash_handler`, `profile`, `safety`, `debug`, and `freestanding`, each call an
operating system directly, so porting them needs Mettle bindings for page
mapping, exception handling, and process control first. `swap` was the honest
place to start: a fixed table and pointer-sized stores, and no OS surface at
all.

## See also

- [Linker and build pipelines](linker-build-pipelines.md)
- [C interoperability](c-interop.md)
- [Heap allocation](heap-allocation.md)
