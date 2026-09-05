# Diagnostics

What the compiler prints when something is wrong, how to read it, and how to
get it as JSON.

## The shape of a diagnostic

```text
-- bad.mettle:2:17 -------------------------------------------------------
error[M0118]: Integer 300 is out of range for 'int8'

  ----+--------------------------------------------------------------------
    1 |  fn main() -> int32 {
    2 |    var x: int8 = 300;
      :                  ^^^ does not fit in 'int8' (-128..127)
    3 |    var y: int64 = 1;
  ----+--------------------------------------------------------------------

  help: 'int8' holds -128..127. Widen the type, or cast to say the wrap is meant: (int8)value
```

The rule gives file, line and column, so a terminal or an editor can turn it
into a jump. The next line gives severity, code, and the problem. The frame
quotes the line with one either side, a caret span under the offending text,
and a short label saying what is wrong there. A `help` line says what to do
about it.

On a terminal the frame is drawn with box characters and the source is
syntax-coloured; redirected output gets the ASCII form above, with no colour
and no line wrapping.

Some diagnostics add `note` lines that point at a second location, the
declaration a call disagrees with, or the iteration a generated declaration
came from.

## Recovery

The compiler does not stop at the first error. It reports every one it can
reach, then summarizes:

```text
error: could not compile `bad.mettle` due to 2 previous errors
help: for more about this error, run `mettle explain M0118`
```

The parser resyncs at block boundaries, so one missing brace does not cascade
into a page of nonsense.

## Severities

`error` fails the build. `warning` does not. Most of the
[memory diagnostics](memory-safety.md) are warnings, because the code they
describe is legal and wrong. The ones that cannot possibly be meant, such as
returning the address of a local, are errors.

## Explaining a code

```bash
mettle explain M0110
```

```text
M0110: Borrowed interior pointer outlives its scope

A pointer into a stack value (a field, an array element) escapes the
scope that owns the value, e.g. saved to an outer variable inside a
block. When the block exits the pointee dies.

Fix: shorten the pointer's lifetime to the value's scope, or move
the value itself to the outer scope / heap.
```

`mettle explain list` prints the index. The same command explains an
[`--explain` decision code](explain-json.md) such as `store-only-fill`.

## The codes

Compiler errors:

| Code | Meaning |
|------|---------|
| E0001 | Lexical error |
| E0002 | Syntax error |
| E0003 | Semantic error |
| E0004 | Type mismatch |
| E0005 | Scope error |
| E0006 | Input or output error |
| E0007 | Internal compiler error |

Memory and range diagnostics:

| Code | Meaning |
|------|---------|
| M0101 | Use after free |
| M0102 | Double free |
| M0103 | Returning the address of a stack local |
| M0104 | Storing a stack address in a global |
| M0105 | Constant array index out of bounds |
| M0106 | Memory operation overflows a stack array |
| M0107 | Memory leak |
| M0108 | Use after call-freed pointer |
| M0109 | Double free via call |
| M0110 | Borrowed interior pointer outlives its scope |
| M0111 | Borrowed pointer invalidated by realloc |
| M0112 | Borrowed pointer invalidated by free |
| M0113 | Dereference of a null pointer |
| M0114 | Dereference of an unmapped constant address |
| M0115 | Shift count at or past the operand width |
| M0116 | Division or modulo by a constant zero |
| M0117 | Loop index runs past the end of the array |
| M0118 | Integer out of range for its destination |
| M0119 | Narrowing conversion needs a cast |
| M0120 | Pointer cast to an integer and back to a pointer |
| M0121 | A task was handed a pointer into the frame that spawned it |
| M0122 | A message was written after it was handed to a task |
| M0123 | A signed operation left its type |

[Memory safety](memory-safety.md) and [Borrow checker](borrow-checker.md)
cover the M codes in context.

Rule diagnostics:

| Code | Meaning |
|------|---------|
| R0001 | A rule gave no usable verdict |
| R0002 | A rule failed the build |
| R0003 | A rule could not decide |
| R0004 | Rules spent more than their budget |

[Rules](rules.md) covers the R codes in context.

Proof diagnostics:

| Code | Meaning |
|------|---------|
| P0001 | A declared type's rule could not be proven here |
| P0002 | A bounds check was proven away by a declared type (reported by `--explain`) |
| P0003 | The declared-type prover spent more than its budget |

[Types](types.md) covers declared types and what the compiler proves.

Effect diagnostics:

| Code | Meaning |
|------|---------|
| F0001 | A function reaches an effect it forbids |
| F0002 | A function requires an effect nothing provides |
| F0003 | A function value does not fit the effects its type declares |
| F0004 | A function declared @pure performs something |
| F0005 | The effect pass spent more than its budget |
| F0006 | Two threads write one global and nothing orders them |

Described machines:

| Code | Meaning |
|------|---------|
| N0001 | A described machine is not one the compiler can read |
| N0002 | A described machine has no program to run |
| N0003 | A line of PROGRAM is not an instruction of this machine |
| N0004 | A described machine does not round-trip |
| N0005 | A described instruction's semantics did not run |
| N0006 | A described machine did not halt |

[Bare metal](bare-metal.md) covers the N codes in context.

Deadlines:

| Code | Meaning |
|------|---------|
| D0001 | A function's longest path costs more than its deadline |
| D0002 | A function's longest path cannot be bounded |

[Types](types.md) covers the D codes in context.

Schedules:

| Code | Meaning |
|------|---------|
| H0001 | A schedule is not written as phases |
| H0002 | A schedule names a phase or an effect twice |
| H0003 | A schedule names an effect nothing declares |
| H0004 | A schedule names an entry nothing declares |
| H0005 | A schedule is a `var` |
| H0006 | The dispatcher generated from a schedule did not parse |
| H0007 | A phase joins the threads and there is nothing to build it out of |

[The runtime model](runtime-model.md) covers the H codes in context.

[Effects](effects.md) covers the F codes in context.

Reference twins:

| Code | Meaning |
|------|---------|
| T0001 | A function and its reference twin disagree |
| T0002 | A reference twin could not be checked |

[Translation validation](translation-validation.md) covers the differential
machinery both the twins and `--verify` run on.

## JSON output

`--error-format=json` writes one JSON object per diagnostic to stderr, for
editors and other tools:

```bash
mettle --error-format=json bad.mettle -o bad.obj
```

```json
{"severity":"error","code":"M0118","message":"Integer 300 is out of range for 'int8'","file":"bad.mettle","line":2,"column":17,"length":3,"label":"does not fit in 'int8' (-128..127)","help":"'int8' holds -128..127. Widen the type, or cast to say the wrap is meant: (int8)value","notes":[]}
```

| Field | Meaning |
|-------|---------|
| `severity` | `error` or `warning` |
| `code` | The diagnostic code |
| `message` | The headline |
| `file`, `line`, `column` | Where it starts |
| `length` | How many columns the caret spans |
| `label` | The short text under the caret |
| `help` | The suggested fix, or absent |
| `notes` | Secondary locations, each with its own message |

The objects are newline-delimited, one per line, so a reader can consume them
as they arrive.

## Color

Diagnostics are colored when stderr is a terminal. `NO_COLOR` turns that off
and `CLICOLOR_FORCE` turns it on regardless.

## Device types

A GPU kernel states in its types where its memory lives and how tightly it is
aligned, and the refusals name the thing that failed rather than the rule. A
space mismatch names both spaces:

```text
error[E0004]: Type mismatch: expected 'float32 shared*', found 'float32 global*'
  help: shared memory is wanted and this address is in global memory; the two
        are different memories, so no cast makes one the other
```

An unproven alignment names the alignment the expression reached:

```text
error[E0003]: this address is 4-byte aligned and the cast claims 16; the offset
              it is built from is not bounded to 16
```

A kernel parameter in a space a launch cannot supply names the space:

```text
error[E0003]: kernel parameter 'tile' is declared shared, and a launch has no
              shared address to pass; declare it 'global', 'constant', or leave
              it generic
```

A view laid out one way where another is wanted:

```text
error[E0004]: Type mismatch: expected 'float32 shared[32,32] layout row',
              found 'float32[32,32] layout swizzle128'
  help: this wants elements laid out 'row' and these are laid out 'swizzle128'
```

An access `@conflict_free!` could not show is one access:

```text
error[E0003]: '@conflict_free!' says one subgroup's addresses fall in distinct
              workgroup banks, and work items 0 and 1 both land in bank 0, so
              this access is two accesses
```

A value a declared type says is uniform, that the compiler cannot show is:

```text
error[P0001]: cannot prove `gpu_tid_x()` is the same in every work item, which
              'Uniform<int32>' requires: thread.x varies by work item
```

A collective reached under a condition the work items do not all decide the
same way:

```text
error[F0002]: 'row_sums' provides 'Warp' and a branch at line 7 takes it away
              again: no work item agrees on that condition, so the group a
              collective there speaks to is not all present
```

`--report-gpu-types` prints what these analyses concluded and what they cost.
[GPU offload](gpu.md) covers the surface.

## Optimization remarks

Diagnostics say what is wrong with the program. Remarks say what the optimizer
did with it, and they come from [`--explain`](compilation.md), a separate
report. Every fix it suggests has been applied to a clone and re-checked
before printing.

## See also

- [Compilation](compilation.md)
- [Memory safety](memory-safety.md)
- [The --explain-json schema](explain-json.md)
