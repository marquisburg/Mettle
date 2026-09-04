# Borrow checker

Mettle's borrow analysis finds pointers that outlive what they point at. It is
pure inference: there is no ownership syntax to write, no lifetime to name, and
nothing to annotate.

It never rejects a program to protect itself. It reports only what it can
prove, which is why there is no escape hatch and no way to silence it.

## What it tracks

A borrow is a pointer derived from something else: `&local`, `&p.field`,
`&buf[4]`, a pointer arithmetic result. The analysis follows each borrow to
its source and asks whether the source is still alive at every use.

Three things end a source's life: its scope closing, a `free` of the block it
sits in, and a `realloc` that may move that block.

## Interior pointer outlives its scope

```mettle
struct P { x: int32; y: int32; }
```

```mettle
var outer: int32* ;
{
  var p: P;
  p.x = 1;
  outer = &p.x;
}
return *outer;
```

```text
warning[M0110]: Use of `outer` after the scope of `p` ended at line 5; `outer`
borrows into `p`, whose storage is reclaimed when its block exits, so this
pointer is dangling
```

The fix is to shorten the pointer's life to the value's scope, or to move the
value out to the wider scope or the heap.

## Invalidated by realloc

```mettle
var base: int32* = malloc(64);
var inner: int32* = &base[4];
base = realloc(base, 128);
return inner[0];
```

```text
warning[M0111]: Use of `inner` after `base` was reallocated at line 6;
`realloc` may move the block, so this pointer is dangling
```

`realloc` is allowed to move the allocation, and it does so unpredictably.
Recompute every interior pointer from the new base after the call.

## Invalidated by free

```mettle
var base: int32* = malloc(64);
var inner: int32* = &base[4];
free(base);
return inner[0];
```

```text
warning[M0112]: Use of `inner` after `base` was freed at line 5; `inner`
borrows into `base`'s block, so this is use-after-free through an interior
pointer
```

Finish with the derived pointers before freeing the base.

## Paths

A branch is not a blind spot. Each arm is followed with its own facts holding,
because within one execution of a block the statements do run in order:

```mettle
if (flag > 0) {
  free(p);
  return p[0];
}
```

```text
warning[M0101]: Use of `p` after it was freed (freed at line 4); this is
use-after-free
```

What the arms disagree about is where it goes quiet. After an `if`, a pointer
counts as freed only when every arm freed it; freed on one path and read on
another is two paths that disagree, and nothing is said. A loop body is one
path and not entering the loop is another, so an allocation freed in the body
is accounted for, and one the body never frees is a leak.

## Across a task boundary

Two lifetime questions cross out of one thread and into another, and the
analysis answers both.

A task is recognised by shape, not by name. A call hands a function value to
a callee whose body is not here, with a pointer as the argument straight after
it. That is what `CreateThread`, `pthread_create` and a program's own spawn
wrapper all look like, so all three are seen the same way, and no interface is
on a list the compiler believes.

The function value is recognised by its type, so how the program got hold of
it does not matter:

```mettle
CreateThread(0, 0, &worker_main, msg, 0, 0);      // written at the spawn
CreateThread(0, 0, start, msg, 0, 0);             // held in a variable
CreateThread(0, 0, g_spawn.entry, msg, 0, 0);     // read out of a field
```

The pointer that follows the entry point is what the task will read.

```mettle
var work: Work;
CreateThread(0, 0, &worker_main, (cstring)(&work), 0, 0);   // M0121
```

`work` lives in the frame that spawns the task, and the frame's storage goes
when it returns. Nothing here says when the task stops reading, so this is
refused rather than warned about: the read happens on another thread and the
corruption it causes carries no line number. A global, an allocation, or a
value passed by copy all outlive the frame and all compile.

The second question is what the sender may still do with what it handed over.

```mettle
var h: int64 = CreateThread(0, 0, &worker_main, msg, 0, 0);
msg[0] = 2;                                                // M0122
```

From the spawn onwards both ends hold the same bytes with nothing ordering
them, so what the worker reads depends on which core got there first. A
pointer reassigned before the write is a different object and says nothing;
the finding is reported once per pointer, so a loop that writes it prints one
line.

`--check-tasks` asks the same question from the other side. It emits a call at
every recognised spawn that compares the handed pointer against the spawning
thread's stack and traps when it lies inside, without consulting the analysis
at all, which is what catches a capture routed through a global first:

```bash
mettle --build app.mettle -o app.exe --check-tasks
```

A program with no task spawn compiles to the same bytes with the flag as
without it, because nothing is emitted and no helper is named. See
[known limitations](known-limitations.md) for what the run-time check can and
cannot bound exactly.

## Where it stays quiet

The analysis proves things about code it can follow. It says nothing when it
cannot:

- A pointer whose source came in as a parameter from another translation unit
  or from C.
- A borrow stored in a struct field or an array that then flows through code
  the analysis cannot relate back to the source.
- Anything reached through a `rawptr` after the type is gone.
- Two pointers it cannot prove alias.
- An allocation released on some paths and not on the one an early `return`
  takes. `defer free(p)` right after the allocation covers every exit, which is
  what the language offers in place of a report.

That silence is the design. A checker that guessed would produce the false
positives people spend their days fighting, and there would be an annotation to
write to make it stop.

## Relationship to the other checks

The borrow analysis handles derived pointers. The plainer mistakes belong to
the [memory diagnostics](memory-safety.md): a direct use after free is M0101, a
double free is M0102, and returning `&local` is M0103.

`--safe` is a different axis again. It adds bounds checks at run time for
accesses that nothing proved. The borrow analysis costs nothing at run time
and adds no code.

## See also

- [Memory safety](memory-safety.md)
- [Heap allocation](heap-allocation.md)
- [Diagnostics](diagnostics.md)
