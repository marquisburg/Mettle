# Rewrite rules

A `rewrite` rule teaches the optimizer an identity from your own code. The
compiler proves the rule before it uses it, applies it wherever the pattern
appears, and checks every function it changed.

```mettle
fn clamp(x: int64, lo: int64, hi: int64) -> int64 {
    if (x < lo) { return lo; }
    if (x > hi) { return hi; }
    return x;
}

rewrite clamp_twice(x: int64, lo: int64, hi: int64) -> int64 {
    from clamp(clamp(x, lo, hi), lo, hi);
    to clamp(x, lo, hi);
    where lo <= hi;
}
```

A rule has parameters, a result type, and two expressions over the parameters.
`from` is the shape to look for. `to` is what replaces it. An optional `where`
narrows the rule to the arguments on which it holds.

Rules run under `-O` and `--release`. Without optimization they are checked
and ignored.

## What a rule can say

Both sides are ordinary expressions: arithmetic, comparisons, casts, and calls
to functions in the program or to `extern` functions. Every parameter `from`
mentions is bound at the match site; `to` and `where` may use only those.

The `from` side has to apply an operator, a cast, or a call at its top. A bare
parameter would match every value in the program.

Reads of globals, memory access, and control flow are outside what a rule can
express, and the compiler says so when it meets one.

A rule declared in a module travels with it. Importing the module brings its
rules along, so a library can ship the identities its own functions satisfy.

## What the compiler checks

Each rule is checked before it is ever applied. The compile-time interpreter
runs `from` and `to` on generated inputs and compares the results, the memory
each touched, and the calls each made to code outside the program. The inputs
are the table the [translation validator](translation-validation.md) uses, the
constants the rule itself compares against, a set of awkward integers
(negative values, the neighbors of every power of two, the extremes of each
width), and, for two or three integer parameters, every combination of those.

A rule that disagrees with itself is a compile error, and the input that
exposed it is printed:

```text
rules.mettle:1:1: error: rewrite `bad_div` changes meaning: its `from` and `to` sides disagree
  counterexample (input set 8) bad_div(-1)
  divergence: return value was 0, is now -1
```

`x / 2` rounds toward zero; `x >> 1` rounds down. They agree on every
non-negative input the fixed table tries, and the probe at `-1` tells them
apart.

Floating-point results are compared the way `--verify` compares them, within
one part in a million. A rule that is true in real arithmetic and false in
float32 rounding is reported against the input that shows the gap.

Every function a rule changed is then executed before and after the change on
the same kind of generated inputs. A difference undoes the rewrite and fails
the build. With `--verify` on, the pass driver checks the same function a
second time, alongside every other pass.

## Guards

`where` is evaluated at compile time, on the constants bound at the match
site. A rule whose guard reads `m` applies at `x % 8`, where `m` is `8`, and
declines at `x % n`, where `m` is whatever `n` holds at run time.

```mettle
rewrite rem_pow2(x: uint64, m: uint64) -> uint64 {
    from x % m;
    to x & (m - 1);
    where m > 0 && (m & (m - 1)) == 0;
}
```

The rule's own check runs on the inputs the guard admits, and the report says
how many that was. A guard that admits no generated input leaves the rule
unchecked, which is an error: an unchecked rule is never applied.

The same rule over `int64` fails its check at a negative `x`, because `%`
keeps the sign of its left operand and `&` does not. The guard cannot help,
since `x` is not a constant at the sites the rule is for. Widening the rule to
what is actually true, unsigned operands, is the fix.

## When rules run

Rules are tried before inlining, while a call is still a call, so a rule over
`length(normalize(v))` sees the two calls. They are tried again inside the
optimizer's main fixpoint, after constants have been folded and copies
propagated, so a rule over arithmetic sees the shape the compiler arrived at.

Commutative operators match in either order. A rule over `a + a` matches
`a + a` and nothing else; a rule over `x * 2` also matches `2 * x`.

The rule's parameter types must agree with the expression's. A rule over
`int64` never matches `int32` arithmetic, because the two wrap differently.

## Reading the report

`--explain` accounts for every rule:

```text
clamp_twice (rewrite rule @ line 34): rule checked
    \_ verified: `from` and `to` agree on the 18 generated input sets that satisfy `where` (24 generated in all)
clamp_twice (rewrite rule @ line 34): applied 1 time
use (expression @ line 43): rewritten by rule `clamp_twice`
```

A rule that matched nothing says so, with the reason the compiler can name:

```text
length_of_normalize (rewrite rule @ line 12): matched 2 expressions, none rewritten: `where` could not be decided  [rewrite-rule-guard-undecided]
    \_ reason: `where` reads `v`, and at every match `v` was a runtime value; ...
```

`mettle explain rewrite-rule-unused` prints the long form of each code.

## Limits

A rule matches one expression at a time. It cannot describe a loop, a
statement sequence, or a change of data layout.

A rule is checked on generated inputs. The check finds the disagreement in
every rule the fixed table and the probes reach, and a rule that is wrong only
on an input none of them produce will pass it. The per-function check after
each application is a second net, with the same limit.

## See also

- [Translation validation](translation-validation.md)
- [Compile-time execution](testing.md)
- [Compilation](compilation.md)
