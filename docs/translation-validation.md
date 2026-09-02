# Translation validation

`--verify` holds the optimizer to the standard the optimizer holds your code
to. After every pass, on every function the pass changed, it runs the IR from
before and the IR from after on the same generated inputs and compares what
they did.

```bash
mettle --verify --build program.mettle
```

`--verify` implies `-O`.

## What it checks

Each changed function is executed on six input sets in the
[compile-time interpreter](testing.md), before the pass and after it. Matching
behavior validates that application of that pass. Differing behavior is a
miscompile, caught at the moment it is introduced.

The claim "this optimization is correct" is not accepted from the optimizer on
its own authority.

## A clean run

```text
translation validation: OK - 70 pass applications validated on 6 input sets each
```

The count is pass applications, one per pass per function that pass touched.

## A caught miscompile

```text
verify: MISCOMPILE CAUGHT: pass 'sroa' changed the observable behavior of
function 'total'
  counterexample (input set 2): total(7)
  divergence: return value was 28, is now 21
  action: pre-pass IR restored; 'sroa' quarantined for 'total'; compilation
  continues from validated IR
```

Four things happen, in order. The pass is named. A runnable counterexample is
printed, the call with the arguments that exposed it, where a pointer argument
shows as `<buf:N elems>` and a string as `<string:N>`. The divergence is
stated: a changed return value, a changed global, a changed buffer, or a
changed sequence of calls out to `extern` functions. Then the compiler
restores the pre-pass IR, quarantines that pass for that function only, and
carries on.

The build finishes. The binary is correct, and it is built from IR that
validated. The summary counts what was caught:

```text
translation validation: 1 MISCOMPILE CAUGHT & QUARANTINED (69 validated)
```

## What it skips

Some functions cannot be executed, and the summary says which and why:

```text
  not validated: read_config (no executable inputs (traps/fuel on all sets))
```

The interpreter models memory, strings, structs, globals, and closures. It
stops at calls into foreign code whose behavior it cannot know, at inline
assembly, and at anything that reads the operating system. It also stops when
a function exhausts its step budget on every input set.

Skipped applications are counted separately, so a run that validated little
does not look like a run that validated everything:

```text
translation validation: OK - 12 pass applications validated on 6 input sets each; 40 skipped
```

## What it cannot see

Validation compares observable behavior on generated inputs. Three limits follow
from that.

Inputs are generated, so a bug that needs one specific value may go unseen. The
input sets are small and structured rather than exhaustive.

Behavior reached only through an `extern` pointer's bytes is invisible, because
the interpreter does not know what is behind that pointer.

The comparison is exact, so a kernel that approximates on purpose reads as a
divergence. `simd_exp_f32` and `simd_silu_f32` evaluate a polynomial where the
scalar loop called `expf`, and validation quarantines them for the functions
they touch. The build stays correct; it loses those two kernels while `--verify`
is on.

## Cost

Every changed function is executed twelve times, so `--verify` is far slower
than a normal build. It is a tool for a suspicious build or a bisect, not for
every compile.

`METTLE_VERIFY_STATS=1` prints where the time went:

```text
  verify stats: snapshots 15000 ms (77 copies, 433 cache hits), machine setup
  0 ms, before-runs 109000 ms (420), after-runs 16000 ms (420)
```

## Bisecting to a pass

When you have a miscompile and want to know which pass caused it,
`METTLE_SKIP_PASS` turns passes off by name or numeric id:

```bash
METTLE_SKIP_PASS=sroa mettle --release --build program.mettle
```

It takes a comma-separated list, and the names cover both the fixpoint passes
and the named-sequence stages such as the vectorizers and SLP. Turn suspects
off until the program is correct, and the last one you turned off is the
culprit.

`--verify` usually finds it for you first, and with a counterexample attached.

## See also

- [Compile-time execution](testing.md)
- [Compilation](compilation.md)
- [ML-driven IR optimization](ml-opt.md)
- [Rewrite rules](rewrite-rules.md), which put the same check behind
  identities you write yourself
