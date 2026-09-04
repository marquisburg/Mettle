# A mixing desk

Four channels of fixed-point audio, a stereo bus, a peak meter, and a frame
that meets on two threads. It is about 380 lines, and every claim it makes
about itself is one the compiler either proves or refuses.

```bash
mettle --build examples/desk/desk.mettle -o desk.exe --release
```

```text
calibration span 15.75
ramp apart 15.75, overlapped 0.5
desk 6 frames, peak 7598, checksum 4904
bounce 6 frames, peak 7598, energy 346377624
```

`desk.exe offline` runs the same six frames on one thread, in schedule order,
and prints the same four lines. That the live run and the offline run agree is
the schedule's doing, and it is checked below.

## The frame is a table

```mettle
const DESK: Schedule[3] = [
  { phase: "capture", effect: "Capture", entry: "capture_block", thread: 0, joins: true },
  { phase: "mix", effect: "Mix", entry: "mix_block", thread: 1, joins: true },
  { phase: "master", effect: "Master", entry: "master_block", thread: 0, joins: true },
];
```

That is the whole of the concurrency. `mettle expand examples/desk/desk.mettle`
prints what the compiler wrote from it:

```mettle
var DESK_arrived_capture: volatile int32 = 0;
fn DESK_wait_capture(generation: int32) {
    atomic_inc_i32(&(DESK_arrived_capture));
    while ((DESK_arrived_capture < (generation * 2))) { ... }
}

fn DESK_thread_0(arg: cstring) -> uint32 {
    var frames: int32 = (int32)(int64)arg;
    var frame: int32 = 0;
    while ((frame < frames)) {
        DESK_phase_capture();
        quiesce;
        DESK_wait_capture((frame + 1));
        DESK_wait_mix((frame + 1));
        DESK_phase_master();
        quiesce;
        DESK_wait_master((frame + 1));
        frame = (frame + 1);
    }
    return 0;
}
```

Each phase wrapper provides its phase's effect and nothing else, so a call that
reaches across a phase is refused by the ordinary effect pass with the chain
that got there. `joins: true` on all three rows is why capture finishes before
mix starts and mix finishes before master starts, on both threads, every frame.

## One meter, two writers

`g_peak` is written by the mix stage and by the master stage. What orders them
is an effect both writers need:

```mettle
fn meter_input(level: Sample) requires Mix, MeterLock { ... }
fn meter_output(level: Sample) requires Master, MeterLock { ... }
fn under_meter_mix(level: Sample) provides MeterLock requires Mix {
  spin_lock(&g_meter_lock);
  meter_input(level);
  spin_unlock(&g_meter_lock);
}
```

```bash
mettle --build examples/desk/desk.mettle -o desk.exe --release --report-effects
```

```text
effects mix_block: performs Dsp, needs Mix
shared g_peak: meter_input (Mix), meter_output (Master), ordered by an effect both require
shared globals: 1 written from more than one place, 1 ordered
```

Take `MeterLock` off either writer and the build stops with `F0006`, naming
the object, both writers, and the effect each runs under.

## The shaper is chosen at run time and still bounded

```mettle
var g_shape: fn(Sample) -> Sample with Dsp = &dsp_clean;
fn mix_block() requires Mix forbids alloc { ... g_shape(raw) ... }
```

The function-pointer type carries `with Dsp`, so it is closed: a value of it
performs `Dsp` and nothing else. That is what lets `mix_block` forbid `alloc`
across an indirect call. Assign something that allocates to `g_shape` and the
build stops with `F0003` before the call is ever made.

## The arithmetic is proven, so the checks are deleted

The audio path is fixed point, and the widths are written down:

```mettle
type Sample = int32 where value >= -32768 && value <= 32767;
type Gain   = int32 where value >= 0 && value <= 32768;
type Bus    = int32 where value >= -262144 && value <= 262143;
```

```bash
mettle --build examples/desk/desk.mettle -o desk.exe --release --check-overflow --explain
```

```text
19 optimizations a declared type earned
  line 64 in dsp_scale: no overflow check emitted, because 'int32' holds every value the
  operands can produce here, -1073741824..1073709056, so the check could never fire
  line 53 in bus_add: ... -294912..294910 ...
```

`(s * g) >> 15` is the multiply at the centre of the mixer, and it is the one
the checks are deleted from, because `Sample` and `Gain` say what reaches it.
Nothing was annotated: the declared types are proven where the conversions are
written, and the deletion follows from the proof.

## Two blocks name what they may cost

```mettle
@noalloc fn dsp_master() -> Sample where cycles < 6000 { ... }
```

```bash
mettle --build examples/desk/desk.mettle -o desk.exe --release --report-deadlines
```

```text
deadline dsp_capture: 45939 of 48000 cycles, 2060 to spare, proven from the cost model
deadline dsp_master: 4422 of 6000 cycles, 1577 to spare, proven from the cost model
cost model: op 1, load 4, store 1, branch 1, multiply 3/4, divide 26/14, call 4, allocate 120
```

The cost model is the target's, and a target is data. `board.mettle` here is
this machine's own description with four numbers changed, the way a slower part
would read:

```bash
mettle --build examples/desk/desk.mettle -o desk.obj --target examples/desk/board.mettle
```

```text
error[D0001]: 'dsp_capture' declares a deadline of 48000 cycles, and its longest path costs 94731 on this target
error[D0001]: 'dsp_master' declares a deadline of 6000 cycles, and its longest path costs 8452 on this target
```

Same source, same claim, a machine that cannot keep it.

The deadlines stop at the kernels and do not cover the lock, because a lock has
no bound. That is why `dsp_master` is a separate function from `master_block`.

A build that carries run-time checks costs more, and the report says so rather
than pretending otherwise:

```text
warning[D0001]: 'dsp_capture' declares a deadline of 48000 cycles, and its longest path costs 56211 on this target
help: this build carries run-time checks the deadline was not declared against
```

## A proof deletes a run-time test

`dsp_ramp` writes two buffers a caller names, so nothing here says they are
distinct. Multi-store fission keeps the loop and puts a test in front of the
kernels:

```text
%.ovl1 = @left + (@n * 8)
%.ovl3 = @right + (@n * 8)
%.ovl6 = (%.ovl1 <= @right) | (%.ovl3 <= @left)
branch_zero %.ovl6 -> ir_while_620
@left <- simd_vloop_f64(map nodes=3)
@right <- simd_vloop_f64(map nodes=3)
```

`calibrate` runs the same loop over two buffers it allocated itself. There the
regions are provably distinct, and no test is emitted at all.

The test earns its keep. `verify_ramp` calls `dsp_ramp` once on two halves of
one allocation and once on two regions overlapping by one element:

```text
ramp apart 15.75, overlapped 0.5
```

`0.5` is the answer the program wrote. Under the kernels it would have been
`0.0`.

## Eight rules, over three different things

```bash
mettle --build examples/desk/desk.mettle -o desk.exe --release --report-rules
```

```text
rule dsp_kernels_never_allocate: pass, 5640 steps
rule the_meter_has_one_lock: pass, 6321 steps
rule every_phase_has_an_entry: pass, 9363 steps
rule the_kernels_keep_their_registers: pass, 873 steps
```

`the_meter_has_one_lock` reads the globals out of the checked program and
requires that every function writing the meter needs `MeterLock`. That is
`F0006` restated by the program, in the program's own words.
`the_kernels_keep_their_registers` reads what the code became: a kernel that
spills spends time the cost model never priced, so the deadline it was proven
against would stop meaning what it said.

## The same rules over a run

```bash
mettle test examples/desk/desk.mettle --report-rules
```

```text
test the_offline_bounce_is_deterministic ... ok
rule the_audio_path_never_allocates: gap, 191 steps
rule every_block_it_takes_is_released: gap, 172 steps
rule every_lock_is_released: gap, 170 steps
rule every_frame_that_started_finished: gap, 193 steps
```

Gaps, honestly reported: the interpreter models a spin lock as the ordinary
code it is, so that run took no lock and the rule says it proved nothing.

Record a native run and ask again:

```bash
mettle --build examples/desk/desk.mettle -o desk.exe --record-trace
METTLE_TRACE=desk.trace ./desk.exe offline
mettle check-trace examples/desk/desk.mettle desk.trace --report-rules
```

```text
trace: 17915 events from 'desk.trace'
rule the_audio_path_never_allocates: pass, 1559408 steps
rule every_block_it_takes_is_released: pass, 1433391 steps
rule every_lock_is_released: pass, 1666249 steps
rule every_frame_that_started_finished: pass, 2524528 steps
```

`the_audio_path_never_allocates` is `@noalloc` and `forbids alloc` asked again
of a run that actually happened, by something that did not make the proof.
`every_block_it_takes_is_released` counts what the run took against what it
gave back, and the strings the printing builds are on that ledger like
anything else: this run took 27 blocks and released 27.

## Everything else it holds up under

```bash
mettle --build examples/desk/desk.mettle -o desk.exe --release --verify
mettle --build examples/desk/desk.mettle -o desk.exe --release --safe
mettle --build examples/desk/desk.mettle -o desk.exe --release --check-tasks
mettle --build examples/desk/desk.mettle -o desk.exe --release --check-effects
```

`--verify` executes every changed function's before and after IR on generated
inputs and compares: `translation validation: OK - 459 pass applications
validated on 6 input sets each`. `--check-tasks` traps at run time if a pointer
handed to a thread lies in the frame that spawned it; the desk hands its
recorder a record on the heap, so it passes. `--safe` and `--verify` both cost
cycles the deadline was not declared against, and both say so.

## What is where

| File | What it is |
|------|------------|
| `desk.mettle` | The desk |
| `board.mettle` | A described target with a slower cost model |
