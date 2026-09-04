# Control flow

Branching, looping, matching, and the statements that run on the way out of a
scope.

## if

The condition goes in parentheses and the body in braces. Braces are required
even for one statement.

```mettle
if (n < 0) {
  return 0;
} else if (n == 0) {
  return 1;
} else {
  return n;
}
```

The condition is a `bool`. Comparison and the logical operators produce one,
and an integer converts to one, with any nonzero value counting as `true`.

## while

```mettle
var i: int32 = 0;
while (i < 5) {
  total = total + i;
  i = i + 1;
}
```

## for over a range

`for i in lo..hi` counts from `lo` up to but not including `hi`. The counter
is the only binding in the language that takes its type from its surroundings,
and it takes it from the bound:

```mettle
for i in 0..5 {
  total = total + a[i];
}
```

Write the type when you want a different one:

```mettle
for i: int64 in 0..n {
  sum = sum + a[i];
}
```

The bounds are evaluated once, before the first iteration.

## for over a string

`for c in s` walks the bytes of a `string`, binding each as a `char`:

```mettle
var s: string = "hello";
for c in s {
  print("{c}");
}
```

The subject is evaluated once, so `for c in read_line(buf, 256, f)` reads one
line and then walks it.

## break and continue

`break` leaves the innermost loop. `continue` starts its next iteration.

A loop may carry a label, and then `break` and `continue` can name which loop
they mean:

```mettle
outer: for i in 0..3 {
  for j in 0..3 {
    if (j == 2) { continue outer; }
    if (i == 2) { break outer; }
    println("{i},{j}");
  }
}
```

```text
0,0
0,1
1,0
1,1
```

## switch

`switch` branches on an integer. Each `case` takes a constant and a body, and
`default` catches the rest.

A case ends where the next one begins:

```mettle
switch (code) {
  case 1: { println("one"); }
  case 2: { println("two"); }
  default: { println("other"); }
}
```

With `code` set to 1 that prints `one` and nothing else.

A case with no body of its own continues into the next one, which is how
several values share a body:

```mettle
switch (code) {
  case 1:
  case 2: { println("one or two"); }
  default: { println("other"); }
}
```

`fallthrough;` asks a case that does have a body to continue into the next one
as well. It may sit anywhere in the case, including inside an `if`, and the
case it names is the one written next, whatever the value that case tests:

```mettle
switch (code) {
  case 1: { println("one"); fallthrough; }
  case 2: { println("also two"); }
  default: { println("other"); }
}
```

The last case has nothing after it, so `fallthrough` there is an error.

A plain enum is 8 bytes and does not decay to an integer, so cast it first:

```mettle
enum Color { Red = 1, Green = 2, Blue = 3 }
```

```mettle
switch ((int32)c) {
  case 1: { println("red"); }
  case 2: { println("green"); }
  default: { println("other"); }
}
```

## match

`match` reads a tagged enum. Each `case` names a variant, binds its payload,
and takes a braced body:

```mettle
enum Shape {
  Circle(float64),
  Square(int32),
  Empty
}
```

```mettle
match (s) {
  case Circle(r): { return 3.0 * r * r; }
  case Square(w): { return (float64)(w * w); }
  case Empty: { return 0.0; }
}
```

Every variant must have a case, or the match must end with `default`. Leaving
one out fails the build, so adding a variant later shows you every place that
has to change:

```text
error[E0003]: Non-exhaustive match on 'Opt': variant 'None' not covered; add
a 'case None:' arm or a 'default:' arm
```

`match` takes tagged enums only. Handing it a plain enum is an error:

```text
error[E0003]: match expression must be a tagged enum type, got 'Color'
```

The same shape reads a [`Result` or an `Option`](types.md):

```mettle
match (half(8)) {
  case Ok(v): { println("ok {v}"); }
  case Err(e): { println("err {e}"); }
}
```

When two enums in scope share a variant name, qualify the constructor:
`Shape.Square(4)`. The `case` labels stay bare.

## return

`return expr` leaves the function with a value. A function declared with no
return type leaves with a bare `return`, or by running off the end.

## defer and errdefer

`defer` runs a statement when the enclosing scope ends, whichever way control
leaves it: falling off the end, `return`, `break`, `continue`, or a `return`
out of a `switch` case. Deferred statements run in reverse order of
declaration.

Inside a loop body, a `defer` runs at the end of each iteration:

```mettle
for i in 0..2 {
  defer println("end {i}");
  println("body {i}");
}
```

```text
body 0
end 0
body 1
end 1
```

`errdefer` runs only on the error path. It pairs with `defer` for work that has
to be undone when a step fails. [Declarations](declarations.md) shows both
together.

What counts as the error path depends on what the function returns:

| Return type | Error path |
|-------------|------------|
| `Result<T, E>` | returning `Err` |
| an integer or `bool` | returning nonzero, the status-code convention |
| a pointer | returning null |
| a float | never; `0.0` is an ordinary result |
| `void`, a struct, an array | never |

A pointer is the case worth reading twice, because the test is the other way
round from the integer one:

```mettle
fn make(ok: int32) -> Buf* {
  var p: Buf* = new Buf;
  errdefer release(p);
  if (ok == 0) { return (Buf*)0; }   // null: release(p) runs
  return p;                          // non-null: it does not
}
```

## comptime for

`comptime for` runs while compiling and leaves nothing behind at run time. The
loop is expanded once per element and the binding is a compile-time value.

Inside a function it generates statements:

```mettle
comptime for f in typeof(Point).fields {
  println("field {f.name} at {f.offset}");
}
```

At file scope it generates declarations, and each generated name must come
from the binding. [Declarations](declarations.md) covers `ident(...)` and the
rules that go with it.

There are two sequences. `typeof(T).fields` reflects on a type the program
declared. `TABLE.rows` reads a `const` holding an array literal, which is how a
program generates from data it wrote down:

```mettle
struct Op { name: string; code: int32; }

const OPS: Op[2] = [ { name: "add", code: 1 }, { name: "mul", code: 2 } ];

comptime for op in OPS.rows {
  println("{op.name} is {op.code}");
}
```

A row of a struct answers to its own columns, and to `.index`. A row of a plain
value is that value, so a `const int32[4]` binds an `int32` each time round.

A directive may generate a type, and a later directive may reflect on it.
Module-scope expansion retires one directive per round and registers the types
that round generated before the next one resolves its sequence, so a struct
written by the first directive is a type the second reads the fields of. The
ledger counts the rounds it took to settle:

```text
comptime expansion: 2 sites, 11 nodes generated, 2 rounds to settle
```

A directive that generates a directive that generates a directive settles the
same way, and a chain that does not settle after 4096 rounds is a build that
says so.

## Text built while compiling

`textof(x)` is the compile-time spelling of a constant, and `+` joins strings
the compiler already knows:

```mettle
const TAG_ID: string = "wire/" + textof(1);
```

A `const string` whose initializer folds becomes the literal it folds to, so
everything after that point sees one ordinary string literal, `mettle expand`
prints it as one, and both ends of a generated wire format read the same tag
because there is only one. The bytes are on the expansion ledger and under
`--expansion-budget`, so text built while compiling is a cost with a name.

## Constants computed by a function

A global's initializer may be a call to a function this program defines. The
compile-time interpreter runs it, and what it returns is what gets laid out in
the object file:

```mettle
fn squares() -> int32[8] {
  var t: int32[8];
  var i: int64 = 0;
  while (i < 8) { t[i] = (int32)(i * i); i = i + 1; }
  return t;
}

const SQUARES: int32[8] = squares();
```

There is no second evaluator with a smaller language in it: this is the same
interpreter that runs `@test`, holds the optimizer to `--verify` and runs the
rules, so any function it can run can compute a constant. It runs under a fuel
budget, so a table that does not finish computing is a build that stops rather
than one that hangs. A call to an `extern` has no body here to run, and is
refused with a source location as it always was.

A compile-time binding of a type's field cannot become a run-time value.
Assigning `f` itself to an `int64` is an error, because generated code gets the
trust hand-written code gets. A table's columns are ordinary constants, so they
go wherever a constant of their type goes.

## asm

An `asm` block holds x86-64 instructions. Mnemonics and register names inside
it are recognized without regard to case, and they mean nothing outside it.
The optimizer does not look inside a block, and a loop containing one does not
vectorize.

## See also

- [Expressions](expressions.md)
- [Declarations](declarations.md)
- [Types](types.md)
