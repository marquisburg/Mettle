# Ideology

This document is not a specification and not a roadmap. It is the decision
procedure. When a proposal arrives, whether a feature, a flag, a syntax, or a
dependency, this is what it gets measured against.

Specs say what the language does. Roadmaps say what it will do. This says what
it *is*, and therefore what it must refuse, including things that would be
convenient, fast, or popular.

Everything in Part I is already true and shipped. Parts II through VI are the
argument for where it goes. Part VII is the list of things that must never
happen regardless of who asks. Part VIII is how to use this document on a
concrete question.

---

## Part I: What Mettle already believes

Four commitments run through this compiler. None of them were written down as
principles first; they were arrived at repeatedly, in unrelated subsystems, by
someone solving concrete problems the same way each time. That convergence is
what makes them real, and it is why they get to constrain everything that comes
next.

### I.1: The compiler never asserts what it has not proven

This is the deepest one, and it shows up in five places that were built years
and subsystems apart:

- The borrow analyser reports nothing it cannot prove. It is pure
  inference with no ownership syntax, and it "never rejects a program. It only
  points at provable mistakes." A whole class of false-positive-driven misery,
  the reason people fight their borrow checkers, was designed out by refusing
  to speak without proof. It rejects a program in three places, and each is a
  fact rather than a risk: returning the address of a local, handing a task a
  pointer into the frame that spawned it (`M0121`), and writing through a
  message already handed to a task (`M0122`). Each stays inference: a task is
  recognised by the shape of the call, and `--check-tasks` re-asks the first
  one at run time without consulting what the analysis concluded.
- `--explain` simulates its own suggestions before printing them. A
  compiler that says "try hoisting this bound" without checking is guessing at
  the user's expense. Mettle runs the suggested fix and only prints it if it
  worked. The report is not advice; it is a result.
- Contracts fail the build rather than under-deliver. `@simd!`, `@inline!`
  and `@noalloc` do not request an optimization, they require it, and when the
  compiler cannot deliver one it stops and names the site that defeated it.
  The alternative, silently doing less than asked, is a lie told by omission,
  which is what every other optimizer does with every other pragma.
- `--verify` holds the optimizer to the same standard as user code. Every
  pass, every function, executed before and after on identical inputs, and a
  divergence is reported with a runnable counterexample, quarantined for that
  function, and healed by recompiling from validated IR. The claim "this
  optimization is correct" is not accepted from the optimizer on its own
  authority.
- What cannot be checked is announced. Functions `--verify` skips are
  reported per function: "skipping is loud, never silent." A gap in the
  proof is itself reported as a fact.

The rule generalizes past the optimizer, and `--ml-opt` is where it becomes a
doctrine rather than a habit. A learned model proposes rewrites nobody can
prove in general; the differential harness gates every disposition
unconditionally. The result is stated in the docs better than it could be
restated here: soundness is a property of the pipeline, not of the pass.

That sentence is the most important one in this repository. It says that
trustworthiness does not have to come from every component being trustworthy.
It can be manufactured, structurally, by a pipeline that refuses to emit
anything it has not checked. Once you believe that, you can safely admit
components you do not trust (a learned optimizer, a third-party pass, a
user-written metaprogram, a patched-in function at runtime) because trust was
never the mechanism.

Everything in Parts III, IV and V is an application of that one idea.

### I.2: One machine, many products

libmtlc contains a reference interpreter for its IR. That single artifact is
the engine behind:

| Product | What it is |
|---|---|
| `mettle test` | `@test` functions run with no codegen, no link, no process |
| `mettle trace` | one function interpreted, printed line by line with values |
| `--verify` | before/after IR executed and compared, per pass, per function |
| `--pgo` | `main()` interpreted at build time; call frequencies fed to the optimizer |
| `--ml-opt` gate | every model disposition executed before it is allowed to stand |
| the leak checker | the interpreter owns the heap, so every test is a sanitizer |

Six products, one machine. This is not tidiness. It is the reason the semantics
your tests run on are the semantics the optimizer is held to, the same
interpreter defines both, so they cannot drift. And it is the reason each new
capability costs a fraction of what it would elsewhere: the expensive part was
built once.

The strategic consequence: **prefer the feature that is another use of a
machine you already have over the feature that needs a new one.** A proposal
that reuses the interpreter starts with an enormous advantage over a proposal
that does not, and should usually win even if it is slightly worse in the
abstract.

### I.3: Pay for what you use, and be able to prove you didn't pay

The runtime model is not "we have no runtime." It is that helper objects are
linked only when the emitted object references their symbol prefix, so
`--release` with no thread atomics links zero of them, and `objdump -t` will
show you that. The claim is falsifiable, by you, on your binary.

The same shape appears in `@noalloc`, which proves an allocation-free call
graph rather than asking you to believe one, and in `--release`, which removes
generated checks rather than making them cheap.

Note the pattern: it is not enough for the cost to be absent. The absence has
to be *demonstrable*. A cost you cannot verify you avoided is a cost you are
paying in anxiety.

### I.4: Infer the proofs, declare the intent

Mettle infers no binding types. Every `var` carries one, and function-local
`const` too. Meanwhile the borrow checker and the capability analysis are pure
inference by design, and CONTRIBUTING explicitly forbids growing
`@owned` / `@borrow`-style ownership annotations.

Read together, those are one rule stated twice:

> **What the program means is written down. What the program is safe to do is
> worked out.**

Types are intent; you must say them. Lifetimes, aliasing, allocation behavior
and vectorizability are consequences; the compiler must derive them. A language
that inverts this, inferring meaning while demanding you annotate proofs, is
Rust, and it is why Rust is hard in exactly the places it is hard.

This rule is directly load-bearing for macros, and Part III returns to it.

---

## Part II: The thesis: control is the product

Every systems language is fast. Speed is table stakes and has been for a
decade. The interesting axis is what the language lets you *decide*.

Look at where the decisions actually sit today:

| | What the compiler does | When code is bound | What runs beside your code |
|---|---|---|---|
| C | fixed pipeline, opaque | link time, final | crt0 and libc, unexamined |
| C++ | templates: power without comprehension | link time, final | plus EH tables, static init order |
| Rust | proc macros: a second language | link time, final | panic runtime, unwind tables |
| Zig | comptime: excellent | link time, final | deliberately near-absent |
| Go / Swift | fixed | link time, final | large, mandatory, unremovable |
| Mettle today | contracts + explain: partial | link time, final | opt-in helpers, provable |
| Mettle proposed | yours | yours | yours |

Nobody occupies the bottom row. Zig gets one column and deliberately declines
the third. Rust gets the third column half-right and pays for the first in a
separate language. C gives you control over nothing and calls it freedom,
because there is nothing to control. The pipeline has no exposed decisions,
so "you can do anything" means "you can do it all yourself."

The thesis is that these are one feature, not three:

> **Every point at which the language currently makes a decision on your behalf
> should be a point at which you can make it instead, without leaving the
> language, and without losing the proof.**

Three points, three axes:

- Compile time. What the compiler does with your source. Today: fixed
  pipeline, with contracts as the only steering. Proposed: metaprogramming as a
  first-class stage.
- Bind time. When source becomes running machine code. Today: once, at
  link. Proposed: whenever you say, including into a live process.
- Run time. What executes that you did not write. Today: nearly nothing, by
  removal. Proposed: a small, consented, excisable, replaceable runtime that
  exists *because* you asked for it.

The unifying clause is "without losing the proof." Anyone can hand over
control; C did that in 1972. The difficult and differentiating part is handing
over control while the pipeline keeps making guarantees, which is possible
here, and only here, because of I.1. Soundness is a property of the pipeline.
So the pipeline can absorb a user-written metaprogram, a runtime-patched
function, or a swapped allocator, and still refuse to emit something it has not
checked.

That is the product. Speed and safety are both widely available.
Control that does not cost you the guarantees.

---

## Part III: Compile time: the metaprogram

### III.1: One language, two execution times. Never a preprocessor.

The single most consequential decision in this entire document:

> **A metaprogram is an ordinary Mettle function that runs at compile time. It
> is not a macro language, not a template language, not a preprocessor, and not
> a token substituter.**

Look at what happens when a language declines this.

C chose textual substitution and got a facility that cannot see types, cannot
be type-checked, breaks under parenthesization, captures identifiers silently,
and produces errors that point at expansion sites nobody wrote. Fifty years
later, the standard advice is still "don't."

C++ discovered accidental compile-time computation inside its type system, then
spent twenty-five years retrofitting it: templates, then SFINAE as a technique
nobody designed, then `constexpr`, then `consteval`, then concepts to describe
what the templates meant all along. Four mechanisms for one job, each added
because the previous one could not be repaired, and all four still present.

Rust shipped `macro_rules!`, then discovered it was not enough and shipped
procedural macros, which are a separate compilation target, operating on token
streams, written against an unstable AST library, requiring their own crate.
Then `const fn` for the compile-time evaluation those two could not do. Three
mechanisms, two of which are effectively different languages, and a
`cargo expand` ecosystem to make the output legible after the fact.

Zig is the counterexample. `comptime` is not a macro system; it is the language,
executed earlier. One mechanism. It is the best metaprogramming facility in any
systems language, and the reason is that there is nothing to learn beyond the
language you already know.

Mettle should take Zig's answer, and it is positioned to take it more cheaply
than Zig was, because the hard part is built. libmtlc's interpreter already
executes real Mettle semantics; it is already trusted enough to hold the
optimizer to account under `--verify` and to drive `--pgo`. Metaprogramming is
not a new machine. Per I.2, it is a seventh product of an existing one.

The corollary is a refusal: there must never be a second language. No
preprocessor grammar, no macro-definition syntax, no build DSL, no annotation
mini-language, no `#if` layer. If a metaprogram needs a capability, that
capability is added to Mettle, where everyone gets it and everything already
knows how to check it.

### III.2: Six non-negotiables, each inherited

The design constraints on metaprogramming are not imported from taste. Each one
falls out of a commitment Mettle already holds.

1. Expansion is inspectable. *(from: `--explain` refuses to be a black box.)*

If generated code cannot be read, it cannot be reviewed, diffed, or reasoned
about, and every bug inside it is a bug in a program nobody has seen. Mettle
already rejects black boxes in the optimizer, where the pressure to be opaque
is far higher. `mettle expand` producing readable, diffable Mettle source is
not a debugging aid; it is the same commitment applied to a new stage.

Rust needed a third-party tool for this and it shows. Build it first, not last,
because a metaprogramming culture that grows up without expansion inspection
never acquires the habit.

2. Expansion is attributable. *(from: the diagnostics pipeline.)*

Mettle's diagnostics carry a code, a snippet, a caret, an inline label, related
notes with their own locations, and a verified `help:`. An error surfacing
inside generated code with no path back to what the programmer wrote would be
the single largest regression in diagnostic quality this project could ship.

Every generated node carries its expansion chain. Errors print it. `trace`
steps through it. `--explain` attributes optimizer decisions across it. If the
metaprogram cannot be held to the diagnostic standard the rest of the compiler
meets, it is not ready.

3. Expansion is hygienic. *(from: `4e23760`, "Reject local parameter
shadowing", landed 2026-08-10.)*

Rejecting silent shadowing was a correctness decision: a rebound name that
looks like a use is a defect the compiler can see and the reader cannot. A
metaprogram that introduces bindings into a caller's scope re-opens that hole
at a hundred times the scale, because the shadowing name is now in code the
programmer never wrote and cannot see without asking.

Hygiene is not a nicety here. It is the property that keeps a just-closed hole
closed. **This is an ordering constraint, not a preference: name resolution has
to be settled before expansion ships, because retrofitting hygiene onto an
existing corpus of macros is not possible.**

4. Expansion is budgeted. *(from: build speed is load-bearing.)*

33,040 lines of application code compile in 1.6 seconds through the internal PE
linker. That number is not a benchmark boast; it is what makes `mettle test`
feel instant, what makes `--verify`'s 2-4x multiplier affordable, and what
would make a language server tractable by brute force where every other systems
language had to build an incremental engine to get one.

Metaprogramming is the most reliable way in the history of programming
languages to destroy exactly that number. C++ build times are a template story.
Rust build times are substantially a proc-macro story. In both cases the
collapse was gradual, unattributed, and irreversible by the time anyone
measured it.

So it gets measured from the first day: per-metaprogram compile cost,
attributable, reportable, and, consistent with how `@simd!` and `@noalloc`
already work, *contractable*, so a build can require that expansion stay
within a budget and fail if it does not. The compiler already knows how to
refuse to compile a program that costs more than the author permitted.

5. Expansion is deterministic, or it declares that it isn't.

`--verify`, `--pgo`, translation validation and reproducible builds all rest on
the same input producing the same output. Unrestricted compile-time I/O breaks
that quietly.

The answer is not prohibition. Reading a table, a shader, a schema at build
time is one of the best reasons to have metaprogramming at all, and `import_str`
already establishes the precedent. The answer is *declaration*: a metaprogram
states the inputs it reads, the compiler hashes them, and determinism survives
by being tracked rather than by being forbidden. Undeclared ambient I/O is
refused. This is I.3 again: the cost is allowed, and the accounting is
mandatory.

6. A metaprogram cannot forge a contract.

`@noalloc` proves an allocation-free call graph. `@simd!` requires
vectorization. If generated code could smuggle an allocation past `@noalloc`,
or satisfy `@inline!` by construction rather than by proof, the contracts stop
being proofs and become decorations, and I.1 is finished.

Contracts are checked on the expanded program, always, without exception.
A metaprogram is a source of code, not a source of authority. It gets exactly
the trust an ordinary function gets, which is none: the pipeline checks the
output regardless of who produced it. This is `--ml-opt`'s architecture applied
to a second untrusted producer, and it is the reason user metaprograms can be
admitted at all.

### III.3: What the metaprogram is allowed to see

A tempting design gives metaprograms access to compiler internals: the AST as
the parser builds it, the IR as the optimizer sees it, the symbol tables.
Rust's proc macros did roughly this and permanently froze a token
representation nobody would design today.

The rule instead:

> **A metaprogram sees exactly what a programmer standing at that point in the
> pipeline would see, and nothing more.**

Before type checking, it sees declarations and source structure. After, it sees
types. It never sees register allocation, pass ordering, or IR internals,
because those are libmtlc's business and libmtlc must stay free to change them
`include/mtlc/` is the only header surface a foreign frontend includes, and
that boundary is worth more than any metaprogramming convenience.

This is also I.4 in a new setting. A metaprogram declares intent and receives
meaning; it does not get privileged access to the machinery that computes
proofs. The moment user code depends on how a proof is computed, the proof
cannot be improved.

### III.4: What this is actually for

Not cleverness. Three concrete shapes, all of which currently cost real damage:

Data that has been shredded into code. In DESCENT, the vessel table is 29
`ds_v_*` functions covering 72 branches. It is a spreadsheet, minced across
switch arms. Adding a row means editing 29 places, and the compiler cannot say
which one was missed. Declared once as data, with accessors generated and
completeness checked, that entire class of defect stops existing.

Contracts that span a boundary the compiler cannot see. DESCENT's wire
format is a 16-byte player stride and a 22-byte input packet, hand-packed on
both sides, with items addressed by index, so client and server must agree
bit-for-bit or players teleport into walls. Declared once, generated for both
ends, size-asserted at compile time. The bug class disappears rather than being
tested for.

Tools that reimplement the language to check it. DESCENT ships 1,594 lines
of Python that parse and interpret Mettle in order to verify invariants the
compiler already understands. That code can silently disagree with real Mettle
semantics and there is no mechanism by which anyone would find out. Every line
of it is a symptom of checks that had nowhere to live.

The measure of a metaprogramming facility is not what it makes possible. It is
what it makes *unnecessary*.

---

## Part IV: Bind time: hot swap

### IV.1: The claim

> **When source becomes running machine code is a decision, not a law of
> nature.**

Every language in the comparison table binds once, at link, forever. This is
not a considered position; it is an inherited constraint from a era when
linking was another program's job.

Mettle did not inherit it. Mettle owns its linker, knows every symbol's final
address, and does not negotiate with a dynamic loader written by someone else.
The distance from here to patching a function in a live process is *shorter for
Mettle than for any other systems language*, and the reason is architectural,
not incidental.

### IV.2: Why this matters more than it sounds

The argument is usually made about iteration speed, which undersells it. The
real claim is about what is *observable*.

A bug that requires a specific sequence of events to reach, say the eleven-thousandth
frame, the third round of a match, the state after a particular network
reordering, is expensive to observe in proportion to how hard that state is to
reconstruct. When the fix requires a restart, every attempt pays full price for
reaching the state again. That cost sets how many hypotheses get tested, which
sets whether the bug gets understood or merely suppressed.

Compilation is 1.6 seconds. The loop is minutes, because reaching the state is
the expensive part and a restart destroys it. Hot swap does not make the
compiler faster; it makes the *state* durable across a code change. That is a
different quantity, and it is the one that was actually limiting.

### IV.3: What the ideology says about the hard part

Code is the easy half. State is the hard half, and this is where most hot-reload
systems become unsound: they guess.

> The program declares what survives a swap. The compiler never guesses.

Silent state migration is a miscompile with a friendlier name: a value
reinterpreted under a layout it was not written with. CONTRIBUTING says a single
silent miscompile that escapes review is worse than a missed optimization; a
silent state migration is the same event, relocated.

Three consequences:

- Layout change is detected, not tolerated. The compiler knows the old and
  new layout. Detection is cheap and mandatory.
- Migration is written, or the swap is refused. Refusing is a perfectly good
  outcome. "This swap needs a migration you have not written" is an honest
  answer, and it is the same shape as `@simd!` failing the build.
- This is where Part III pays for itself. A metaprogram can see both
  layouts and generate the migration. Hot reload elsewhere cannot see types;
  metaprogramming elsewhere cannot see the running process. Here they are the
  same system, and neither one alone is the interesting part.

### IV.4: Consent, again

The swap point is the program's decision, a quiescence point the programmer
names, such as the top of a frame loop. Never preemption, never a stop-the-world
the program did not ask for. This is the same rule Part V applies to the
runtime, and it is not a coincidence that it keeps recurring: **nothing runs at
a point you did not author.**

Swappability is per module and opt-in, so indirection is paid where requested
and nowhere else, the helper-object linking model (I.3) applied to call
binding.

And it must work in release builds. A capability that exists only under `-d` is
a debugging toy, and debugging toys do not get to shape a language. The
interesting uses (live-tuning a shipped simulation, patching a running server,
swapping a policy under load) are all release-mode uses.

### IV.5: The part that is unique to Mettle

A hot swap asks: *is the new function compatible with the old one at this
boundary?*

`--verify` already answers a question of exactly that shape. It executes two
versions of a function on generated inputs and compares every observable:
return value, final buffer bytes, ordered extern-call trace with pointed-to
bytes, and touched globals. It reports divergence with a counterexample.

That is a swap gate. Not by analogy: the same machinery, pointed at the old and
new function instead of the pre-pass and post-pass IR. A swap that changes
observable behavior at the boundary in ways the author did not intend can be
caught *before* it enters the live process, with a counterexample naming the
inputs.

No other language can do this, and the reason is not effort. It is that
translation validation, a compile-time interpreter, and an owned linker had to
already exist in one codebase. They do here. **This is the strongest single
argument that Mettle should build hot swap: it is the only language that can
build a verified one.**

---

## Part V: Run time: the honest runtime

### V.1: The history, stated plainly

Mettle used to ship a large runtime: garbage collector, async executor,
coroutine scheduler, channels, a tracked heap. It was removed. Programs now
link libc and two optional helper objects pulled in by symbol prefix.

Removing it was correct. It is important to be precise about *why*, because the
wrong lesson is easy to draw and would foreclose everything in this section.

The old runtime failed on three properties:

- It was mandatory. Every program paid, including programs that used none
  of it.
- It was invisible. Work happened at points the programmer did not write,
  a collection or a scheduler tick, and nothing named the moment.
- It was unremovable. No flag produced a binary without it, and no tool
  proved it was gone.

It did not fail because it was a runtime. It failed because it was mandatory,
invisible, and unremovable. Those are three fixable properties, and confusing
them with the category is how a project ends up defining itself by an absence.

### V.2: Everyone has a runtime; most lie

C has `crt0`, static initializers, `errno`'s thread-local machinery, `atexit`
chains, and an allocator with its own locks and heuristics. Rust has a panic
runtime, unwinding tables, std initialization, and thread-local infrastructure.
Both are described as having "no runtime," which is a claim about marketing
rather than about linkers.

Mettle is already more honest than either: `runtime-model.md` states exactly
what is emitted and when, and the helpers are opt-in by symbol prefix. But the
framing is still defensive: "rough analogy: these objects are like `crt0.o` for
C: small linker glue, not a language runtime." That sentence is arguing with an
accusation.

And Mettle already pays for a runtime in the place it matters most: generated
null and bounds checks are runtime cost, present in every non-`--release`
build, and they are one of the best things about the language.

### V.3: The four rules

A runtime is acceptable exactly when:

1. Consented. Nothing executes that the programmer did not ask for.
No hidden initialization, no background thread, no work injected at a point
they did not write. This is the anti-GC rule and the anti-ARC rule, and it
explains precisely why those two are resented while `memcpy` is not: it is not
the cost, it is the *unauthored control flow*.

2. Excisable. A build configuration produces a binary containing none of
it, and the compiler can prove the absence. `@noalloc` already demonstrates
this shape: a proven property of a call graph. What is
excisable is optional; what is optional is a feature; what is mandatory is a
tax.

3. Legible. You can ask what it is doing and get an answer. The runtime is
written in Mettle, ships as source, steps in the same debugger, appears in the
same profiler, reports through the same diagnostics. A runtime you cannot
inspect is indistinguishable from a runtime that is lying.

4. Replaceable. Allocator, panic policy, trap handler, swap policy, clock.
Substitutable without forking the compiler. Every serious program eventually
replaces its allocator; the only question is whether the language helps or is
worked around.

### V.4: What the four rules buy

Each of the following is *forbidden* by a strict no-runtime stance, and each is
something real programs need:

- Hot swap needs a symbol table and a quiescence protocol. Part IV does not
  exist without a runtime; it just needs a small, consented one.
- Deterministic record/replay needs hooks on the sources of nondeterminism:
  clock, input, network, RNG. With them, a bug reached at frame 8,412 can be
  re-entered offline and fed to `mettle trace`. Without them it is reachable
  only by asking a human to reproduce it, which is how debugging worked before
  anyone measured how bad it was.
- Per-subsystem allocation policy: arenas, frame allocators, pools. Every
  game and every server builds these. Today they are built around the language
  with `std/mem`; a replaceable allocator makes them a supported shape, and
  makes `@noalloc`-style proofs available about *which* allocator a call graph
  reaches, not merely whether it allocates.
- A chosen panic boundary. Process death is the right policy for a CLI and
  the wrong one for a frame loop or a request handler. The crash forensics
  already exist and are excellent: symbolized backtrace, source snippet,
  faulting index and length. What is missing is the ability to say where
  recovery happens.
- Runtime-toggled instrumentation. `--profile-runtime` and
  `--profile-blocks` are build-time decisions today. A legible runtime makes
  them a live one.

Notice that every item is something the *programmer asked for at a point they
wrote*. That is the whole distinction, and it is the entire content of the
"acceptable runtime" idea.

### V.5: The line

> No hidden control flow. No hidden allocation.

That is the test, and it is short enough to apply without arguing. If a feature
requires the runtime to do something at a point the programmer did not author,
it does not ship, no matter how good the feature is, no matter how small the
cost, no matter who is asking.

This is what makes the position defensible rather than a slow return to the
runtime that was correctly deleted. The old one violated the rule constantly
and by design; a collection point is definitionally unauthored control flow.
Nothing proposed here does.

### V.6: What a program does now

The deletion in V.1 left a question this document had not answered: with the
scheduler gone, what does a program that needs one do? The answer is that the
execution model is a thing the program builds. A job system, a frame graph, a
fiber scheme, a request pipeline, a state machine, an actor mailbox: every
serious program builds one already, and Mettle's position is that it builds
it in Mettle, and that the compiler understands what it built.

`std/thread` is the whole of what ships: threads, mutexes, atomics and spin
locks, mapped onto Kernel32 on Windows and onto `clone` with futexes on
Linux. Nothing above that is linked unless the program wrote it, which is
V.3.2 applied to scheduling.

What makes that a supported shape has two halves. `quiesce` is the point the
program names, and staged work lands there and nowhere else; it is the proof
that the consented model works in release builds. And the properties a model
depends on are written in the program and checked on every build: a job that
may not allocate is `@noalloc`, a slot that stays in the ring is a declared
type the compiler proves in range, where code may run is an effect the
program declares (`requires Worker` on the job, `provides Worker` on the
thread's entry, and the compiler infers everything between and refuses a job
reached from `main` with the chain that reached it), and what the compiler has
no word for is a `@rule`, such as a state machine's `step` deciding every
state. `examples/job_system/` is that shape in one file, `examples/engine/`
is the same queue with a schedule, phase effects, an ordered shared global and
a declared deadline around it, and a module offers
its rules and its effects to whoever imports it, which is how a house style
stops being a script in another language.

Effects are the general form of what `@noalloc` and `@pure` were two fixed
cases of. `alloc`, `asm` and `syscall` are built in because the compiler knows
their sources; everything else the program declares, and the compiler holds it
to the same standard: inferred through the call graph, refused with the chain,
gapped at a call it cannot follow, and re-checked at run time by a machine
that does not trust the analysis.

`@pure` came out the other side of that as a contract and nothing else. The
loop-invariant call hoist reads a whole-program purity fixpoint and never the
decorator, so a function that writes nothing has its call hoisted whether or
not anyone said so, and a function that carries `@pure` and writes anything
fails the build with `F0004`. A program compiles to the same instructions with
the decorator and without it. That is what I.4 asks of every annotation: it
records intent, and the proof stays the compiler's.

Every one of these mechanisms is on a ledger and under a budget, because
VII.10 does not exempt the compiler's own passes: `--report-proofs` with
`--proof-budget=N`, `--report-effects` with `--effect-budget=N`,
`--report-rules` with `--rule-budget=N`, `--report-expansion` with
`--expansion-budget=N`. And `--explain` prints a **beliefs** section naming
every claim the build rested on and did not establish: each `extern` whose
`with` clause was read as written, each one on the known-clean list, and each
one with no clause at all. A build that assumed nothing says so. An assumption
nobody can name is the failure mode the whole document is about.

VII.7 has already decided the other half. The model where the points are
inserted, `async`/`await` with a compiler-placed yield, a pool that steals
work at a point nobody wrote, is foreclosed, and the reason is worth stating
so it is not rediscovered: a yield point the compiler inserts is the same
event as a collection point wearing better clothes.

---

## Part VI: Why these are one product

Presented as three features, this is a wish list. It is not three features.

- The metaprogram generates the state migration for a hot swap, because it
  is the only thing that can see both layouts. *(III → IV)*
- The runtime's patch table and quiescence protocol are what a swap targets.
  *(V → IV)*
- The interpreter that runs `@test` and validates optimizer passes is the
  same one that gates a swap on observational compatibility. *(I.2 → IV.5)*
- Record/replay needs runtime hooks; replay plus `trace` plus swap is a
  debugging loop that does not currently exist anywhere: re-enter the exact
  failing state, watch the values line by line, patch the function, and
  continue from the same state without restarting. *(V → IV → I.2)*
- Contracts hold across all three: proven on expanded code, preserved
  across a swap, and provable about which runtime a call graph touches.
  *(I.1 everywhere)*

Each pair is stronger than either half. That is the definition of a coherent
design, and it is the reason to build all three rather than the best one.

There is also a single sentence that covers the whole thing, and it is already
written in this repository:

> Soundness is a property of the pipeline, not of the pass.

Metaprograms are untrusted passes. Hot-swapped functions are untrusted passes.
A replaced allocator is an untrusted pass. Mettle already knows how to build a
pipeline that stays sound while admitting components it does not trust. It
does it today for a *neural network*. Extending that architecture to code the
programmer wrote is not a leap. It is the obvious next application of a pattern
that already works.

---

## Part VII: Refusals

Things that do not happen, regardless of the benefit, the benchmark, or who is
asking. A design document without this section is a marketing document.

1. Codegen never routes through LLVM, Cranelift, or an external assembler.
   not even as a reference oracle. Instructions are built from the ISA. This is
   already the ground rule; it is repeated here because it is the load-bearing
   one. Everything distinctive in this document (the owned linker, the
   verified swap, the 1.6-second build, translation validation) exists because
   the whole pipeline is in the building.

2. No diagnostic the compiler has not verified. No suggestion that was not
   simulated, no contract that quietly under-delivers, no unchecked claim about
   the program. If it cannot be proven, it is not said, or it is said as an
   explicit gap.

3. No second language. Not a preprocessor, not a macro grammar, not a build
   DSL, not an annotation mini-language. A metaprogram is Mettle. If it needs a
   capability, Mettle gets that capability. This governs the GPU too: CUDA C
   is a second language and so is GLSL, and Mettle declined both for the same
   reason it declines a macro grammar. `kernel`, `dispatch` and `shared` are
   Mettle, checked by the one type checker, and a target's description is a
   Mettle `const`, read by the Mettle parser.

4. No metaprogram may forge a contract. Checks run on the expanded program.
   Generated code receives exactly the trust hand-written code receives, which
   is none.

5. No feature that only works in debug builds. If it does not survive
   `--release`, it is a toy, and toys do not shape the language.

6. Nothing mandatory in the runtime. Every component is opt-in, excisable,
   and provably absent when unused. The moment something cannot be removed, it
   has become a tax, and the previous runtime is being rebuilt.

7. No unauthored control flow, ever. No collection point, no implicit
   refcount, no injected yield, no scheduler tick. The point of execution is
   always a point in the source.

8. A silent miscompile is never traded for a benchmark number. Already
   policy for the optimizer; it extends unchanged to expansion, swapping, and
   migration, all three of which are new opportunities to change a program's
   meaning without saying so.

9. Types are never inferred; proofs are never annotated. The inversion
   stays inverted. `@owned` and `@borrow` do not arrive, and neither does
   `var x = 5`.

10. Build speed is not spent without a ledger. Any feature that can
    consume compile time must be able to report what it consumed. Unattributed
    build time is how 1.6 seconds becomes 90 and nobody can name the cause.

---

## Part VIII: How to use this document

For any proposal, in order. The first failure is disqualifying.

1. Does it require the compiler to assert something it cannot prove?
   If yes, reject it or reduce it to the provable part. *(I.1)*

2. Does it reuse a machine that already exists?
   The interpreter, the diagnostics reporter, the linker, the differential
   harness, the contract checker. A proposal needing a new machine must be
   dramatically better than one that does not. *(I.2)*

3. Can a program that does not use it prove it paid nothing?
   The question is whether the absence is demonstrable, and not whether the
   cost is small. *(I.3)*

4. Does it ask the programmer to annotate a proof, or to declare an intent?
   Intent is written down. Proofs are inferred. *(I.4)*

5. Does it introduce control flow at a point nobody wrote?
   If yes, it does not ship. No exceptions, and this one is not negotiable
   against benefit. *(V.5)*

6. Does it work in `--release`?
   If not, it is a debugging aid, and it does not get language-level surface
   area. *(VII.5)*

7. Can the compiler explain what it did?
   Expansion, swap decisions, migrations, and runtime substitutions: all of it
   inspectable, attributable, diffable. If a stage cannot be explained, it will
   eventually be distrusted, and distrusted stages get worked around instead of
   used. *(I.1)*

8. What does it make unnecessary?
   The best answer names something that gets deleted: a class of bug, a manual
   check, a shadow toolchain in another language. A feature that only adds is a
   feature the language has to carry forever. *(III.4)*

---

## Part IX: Sequence

Not a schedule. An ordering, where each item is a precondition for the next.

Already landed *(2026-08-10)*: local parameter shadowing rejected, multiple
return values, `defer` on global assignment, float constants, sized array
bounds. The first of those is the precondition for everything in Part III.

Before metaprogramming ships:

- ~~Name resolution and hygiene settled.~~ *(III.2.3, the only hard ordering
  constraint in this document; hygiene cannot be retrofitted onto an existing
  macro corpus.)* Landed in `4e23760`, the commit III.2.3 cites.
- ~~`mettle expand`, built at the same time as expansion, not after.~~
  *(III.2.1)* Landed, and it prints every declaration the programmer
  wrote: a module-scope `static_assert` used to come back as a comment saying
  it had no source form, which also made expand disclaim the whole file.
- ~~Expansion-chain attribution in diagnostics and `trace`.~~ *(III.2.2)*
  Landed, both halves. Every error raised inside an expansion carries a
  note naming the iteration and the field, gated by
  `err_comptime_contract_mismatch`. `trace` names them too: each instruction
  carries the expansion that generated it, so the values of one written line
  separate per iteration (``(field `kind`) total = 100; (field `seq`) total =
  505``) instead of merging into one run. The note reaches the IR because
  lowering stamps it; the error reporter's note frames only exist while
  checking, and the interpreter walks IR long afterwards.
- ~~Comptime cost accounting, from the first metaprogram.~~ *(III.2.4,
  VII.10)* Landed: `--report-expansion` prints what each site cost and
  `--expansion-budget=N` makes it a contract that fails the build.

Before hot swap ships:

- ~~A declared quiescence point.~~ *(IV.4)* Landed, and the swap works.
  `quiesce;` names the point, contextually, so a program already using the
  name keeps working. `@swappable` is the opt-in unit, per function, and it
  keeps the call boundary a swap redirects: it implies noinline, and
  `@swappable @inline` is refused. Staging a swap records an intent and
  changes nothing; `quiesce;` is the only place it takes effect, in both
  `--release` and debug. The binding is a slot holding a function pointer, so
  applying a swap is one pointer-sized store: no code is modified, no page is
  made writable, and a caller already inside the old body finishes there.
  Rewriting instructions instead would need every other thread halted, and
  halting them is the unauthored control flow this document refuses. A program
  with no quiesce point never names `mettle_swap_` and does not link the
  runtime, which `swap_runtime_excisable` proves on every build.
- ~~Layout-change detection with refusal as the default outcome.~~ *(IV.3)*
  Landed. `layoutof(T)` digests kind, size, alignment and every field's
  name, offset and width. Pinning it with `static_assert` makes a layout
  change fail the build, which is refusal as the default outcome. It detects
  renames and reorderings that leave the size unchanged, and the metaprogram
  that would write the migration can already read both layouts through
  `typeof`, `fieldof` and compile-time string comparison.
- ~~Swap gating through the existing differential harness.~~ *(IV.5)*
  Landed. `mettle swap-check <file> --old F --new G` aims
  `ir_verify_check_rewrite` at two functions instead of at one function across
  a pass, refuses a changed signature outright, and reports divergence with a
  counterexample. Inputs are the fixed shape table plus the constants the two
  functions compare against, tested on both sides of each, so a rewrite that
  moves a boundary is caught at that boundary. It remains a differential test
  and the verdict says so; what it does not reach is a boundary neither
  function names as a constant.

Before the runtime grows:

- ~~Each component independently excisable, with the absence provable.~~
  *(V.3.2)* Landed. The safety runtime, crash handler, profiler and debug
  hook server are each absent from a binary that did not request them, and
  `runtime_components_excisable` proves it on every build by searching the
  emitted binary for a string only that component carries. It checks both
  directions, since an absence that is never present proves nothing.
- Written in Mettle, in the same debugger, in the same profiler. *(V.3.3)*
  Started, and the pattern works. The swap runtime is written in Mettle
  (`src/runtime/swap.mettle`), compiled by the compiler it ships with, after
  that compiler is built. It ships as source, steps in Mettle's own debugger
  and appears in its own profiler because it is ordinary Mettle code, and it
  came out smaller than the C it replaced. The newest runtime component leads
  rather than being exempt. What remains is the older five, and they are
  harder for a real reason rather than an accident of order: `crash_handler`,
  `profile`, `safety`, `debug` and `freestanding` each reach an operating
  system directly, so porting them means giving Mettle bindings for page
  mapping, exception handling and process control first. `swap` was a fair
  starting point precisely because it touches none of that.
- The Part V.5 test applied to every single addition, individually, with no
  aggregate exceptions.

Before the language can be reshaped around a program, each of the five
things a program might need and the language might lack has to have its
answer, and each answer has to discharge the five obligations: understood by
the checker and the interpreter, enforced by a build that fails, explained as
ordinary Mettle, verified by a machine that does not trust it, and optimized
only where proven.

- ~~An abstraction.~~ Part III, landed above.
- ~~A rule.~~ *(I.2, VII.3)* Landed. `@rule fn f(p: Program) -> Verdict` is
  an ordinary function the compiler runs in its own interpreter after type
  checking, on the checked program as data: every function with its direct
  callees, matched variants and decorators, every declared type with its
  layout digest, the modules, the target. A failing verdict stops the build
  at the site it names; a gap is announced; a site the program never handed
  out is refused, because a rule is code the compiler does not trust. Rules
  never link, cannot be called, and their cost is a ledger
  (`--report-rules`) and a contract (`--rule-budget=N`). Not a plugin, not
  a predicate language.
- ~~A type.~~ *(I.4)* Landed. `type Percent = int32 where value >= 0 &&
  value <= 100;` declares a type that carries a rule. A value of the base
  becomes one only where the compiler proves the predicate, and the proof
  is inferred from a constant, the value's own type, bounded arithmetic, a
  loop range, a dominating test or an early exit; what it cannot prove it
  refuses with the conjunct and the range it knew. A declared type without
  a predicate is a unit that never mixes. The rule pays back where the
  compiler already optimizes: an index whose type pins its range inside the
  array carries no bounds check, `--explain` names the proof, and the
  interpreter re-checks every proven conversion under `mettle test`, so a
  wrong proof is caught by a machine that does not trust the prover.
  `--check-proofs` extends that check into a compiled program.
- ~~An execution model.~~ *(V.6)* Landed as a position and a worked shape:
  the model is the program's, `quiesce` is its point, and its properties are
  a contract, a declared type, effects and rules that hold on every build.
  Effects are what made the compiler see the model: `with`, `forbids`,
  `requires` and `provides` are declared at the edges, inferred through the
  whole call graph, refused with the chain that broke them (F0001 to F0003),
  carried on function types so a call through a pointer stays honest, read by
  rules, and re-checked under `mettle test` and `--check-effects` by a
  machine that does not trust the analysis. Saying where code runs says which
  globals two threads share: one written from two disjoint requirement sets
  is refused as F0006, and an effect both writers require is what orders
  them, so a lock becomes a requirement the compiler carries rather than a
  convention. The borrow analyser closed the other half, following a pointer
  handed to a task (M0121, M0122) and re-asking the question at run time
  under `--check-tasks`. A frame is data the compiler reads: a `const` of
  `std/schedule`'s `Schedule` names each phase, its effect, its entry and its
  thread, and the compiler generates the dispatchers and the `quiesce` at
  every phase boundary. That is not an injected yield: the program wrote the
  order, and `mettle expand` prints what came out as ordinary Mettle. Time
  joined the list of things a program can declare and the compiler proves:
  `where cycles < N` is costed from a model of the target, refused when the
  path costs more (D0001) and refused again when the path cannot be bounded
  at all (D0002), with `--check-deadlines` counting what a path really cost
  while the program ran.
- ~~A machine concept, the reachable half.~~ *(III.3)* Landed. A target's
  description is a Mettle `const` the compiler reads: `mettle target
  <triple>` prints every built-in one, `--target desc.mettle` builds for a
  described one, and a printed description fed back reproduces the built-in
  target byte for byte on every triple. A freestanding x86_64 target chooses
  its calling convention; a hosted one cannot rewrite the platform's, and a
  description that claims what the emitter cannot honour is refused and told
  which fact it contradicted. Whether a description is enough for a genuinely
  new machine remains unproven, and is said to be.

---

## Coda

Mettle's advantage is not that it is fast, or safe, or dependency-free, though
it is all three. It is that the entire pipeline is inside the building,
frontend, IR, optimizer, interpreter, code generator, linker, debugger. Nobody
else has that, because nobody else was willing to write all of it.

Everything in this document is only possible because of that. Verified hot swap
needs an owned linker *and* translation validation *and* a compile-time
interpreter in one codebase. Metaprogramming without a second language needs an
interpreter that already runs real semantics. A consented runtime needs a
linker that can prove what it left out.

The refusal to depend on LLVM looked like stubbornness. It was leverage, and
this document is the argument for spending it.
