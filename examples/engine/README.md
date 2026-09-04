# An engine

`examples/job_system/` is a queue and the rules that hold it. This is the same
queue with a frame around it, and it is here to show what a program gets when
its execution model is something the compiler can read.

Nothing below is a comment or a convention. Every line of it is a claim the
build stops on.

| What the program says | Where it says it | What stops the build |
|---|---|---|
| The frame runs input, then simulate, then present, on two threads | `const FRAME: Schedule[3]` | A phase with no effect, no entry, or a name used twice (`H0001` to `H0005`) |
| A call may not reach across a phase | Each phase's effect | `F0002`, with the chain, landing on the schedule's own row |
| `g_world` is written from two phases | The effects each writer needs | `F0006`, unless an effect both require orders them |
| The blend fits in 1200 cycles | `where cycles < 1200` | `D0001` when the longest path costs more, `D0002` when it cannot be bounded |
| A slot stays in the ring | `type Slot = uint32 where value < 64` | The proof, at the conversion |
| A job does not allocate | `@noalloc` and a rule | `R3001` |
| Every phase names a function that exists | A rule | `R3002` |
| The worker gets nothing from a frame that will return | The borrow analyser | `M0121`, `M0122` |

## Run it

```bash
mettle --build examples/engine/engine.mettle -o engine.exe
engine.exe
```

## Read what the schedule generated

```bash
mettle expand examples/engine/engine.mettle
```

The dispatchers and the `quiesce` at every phase boundary come out as ordinary
Mettle. The type checker, the borrow analyser and every contract meet them the
same way they meet the rest of the file.

## Read what it cost

```bash
mettle --build examples/engine/engine.mettle -o engine.exe --report-deadlines --report-effects --report-rules --report-expansion
```

```text
shared g_world: world_add (Sim), world_present (Render), ordered by an effect both require
deadline blend: 614 of 1200 cycles, 585 to spare, proven from the cost model
schedules: 1 read as data, 3 phases, 5 functions generated
```

## What it declines to claim

`drain` carries no deadline, and the reason is worth reading. It reaches
`queue_take`, which takes a spin lock, and a spin lock has no bound. Writing
`where cycles < N` on it is refused with `D0002` rather than accepted and
quietly wrong, which is the whole point: a deadline nobody proved is not a
deadline. The blend carries one because its path really is bounded.

`--pgo` would let a measured trip count stand in, and the report would then
say the deadline held on evidence rather than on proof, in those words.

## Try breaking it

Add `world_add(1);` to `read_input`, which is the input phase reaching into
the simulation:

```text
error[F0002]: 'main' reaches a function that requires 'Sim', and nothing on
the way provides it: main -> FRAME_thread_0 -> FRAME_phase_input ->
read_input -> world_add
```

Take `requires WorldLock` off `world_present`, and the two writers of
`g_world` no longer share anything that orders them:

```text
error[F0006]: 'g_world' is written by 'world_present', which runs where
'Render' is provided, and by 'world_add', which runs where 'Sim' is provided,
and nothing either one needs orders the two writes
```

Raise the blend's loop to 64 iterations and leave the deadline where it is,
and the longest path outgrows it:

```text
error[D0001]: 'blend' declares a deadline of 1200 cycles, and its longest path
costs 2342 on this target
```
