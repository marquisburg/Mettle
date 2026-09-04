# Expressions

Operators, precedence, casts, calls, and string interpolation.

## Precedence

Tightest first. Every binary operator groups left to right.

| Level | Operators |
|-------|-----------|
| 13 | `.` member access |
| 11 | `*` `/` `%` |
| 10 | `+` `-` |
| 9 | `<<` `>>` |
| 8 | `<` `<=` `>` `>=` |
| 7 | `==` `!=` |
| 6 | `&` |
| 5 | `^` |
| 4 | `|` |
| 3 | `&&` |
| 2 | `||` |

Two of these catch people out. Addition binds tighter than shift, so
`1 << 2 + 1` is `1 << 3`, which is 8. The bitwise operators bind looser than
comparison, so `a & b == c` is `a & (b == c)`. Parenthesize when you mean the
other thing.

Unary `-`, `!`, `~`, `&`, and a cast bind tighter than any binary operator.

## Arithmetic

`+`, `-`, `*`, `/`, `%`, and unary `-`. Integer `/` truncates toward zero and
`%` takes the sign of the left operand.

Overflow wraps. The compiler emits the machine's own instructions and adds no
check.

Dividing by a constant zero fails the build, as [M0116](diagnostics.md):

```text
error[M0116]: Division by a constant zero; this traps the moment it executes
```

## Bitwise and shifts

`&`, `|`, `^`, `~`, `<<`, `>>`. A right shift of a signed value is
arithmetic and of an unsigned value is logical.

Shifting by a constant at or past the operand's width draws
[M0115](diagnostics.md), a warning, because the hardware masks the count:

```text
warning[M0115]: Shift by 32 on a 32-bit value (`int32`); the hardware masks
the shift count, so this does not produce the zero the code reads as
```

## Comparison and logic

`<`, `<=`, `>`, `>=`, `==`, `!=` produce a `bool`. `&&` and `||` produce a
`bool` and stop as soon as the answer is known, so the right side is not
evaluated when the left settles it.

`!` negates, and produces a `bool` like the rest of them:

```mettle
println("{!(a > 3)}");
```

```text
false
```

A `bool` converts to the integer types with no cast, so `var n: int32 = !flag;`
still gives 1 or 0.

## Assignment

`=` assigns. The compound forms `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`,
`^=`, `<<=`, `>>=` read, apply, and write back.

`++` and `--` are statements that add or subtract one:

```mettle
var i: int32 = 0;
i++;
i--;
```

## Casts

`(T)expr` converts. It moves between integer widths, between integers and
floats, between `char` and integers, and between pointer types.

```mettle
var c: char = 'h';
var code: int32 = c;
var back: char = (char)(code + 1);
```

Widening needs no cast. Narrowing one integer into a smaller one does, and the
compiler says which conversion it wanted:

```text
error[M0119]: Narrowing conversion from 'int64' to 'int8' needs a cast
```

One spelling covers three different jobs, so it is worth knowing which one you
are asking for. Between numeric types it converts the value, losing precision
where the destination is narrower. Between pointer types, and between a pointer
and an integer, it reinterprets the address and changes nothing. And from a
float to an integer it rounds -- **toward zero**, always.

That last one is the rule worth keeping in mind, because toward zero is the
wrong direction on the negative side of an axis: `(int32)(-2.7)` is `-2`, so a
grid lookup written with a cast reads one cell off there. `std/math` names the
mode instead of leaving it implied:

```mettle
floor_i32(-2.7)   // -3, toward negative infinity
ceil_i32(-2.7)    // -2, toward positive infinity
trunc_i32(-2.7)   // -2, toward zero: what the cast does
round_i32(-2.7)   // -3, nearest, halfway away from zero
```

Each has an `_i64` sibling, and each is `@inline`, so it costs a call site
nothing over writing the same arithmetic by hand.

A pointer cast to an integer and straight back to a pointer is reported as
M0120. Both halves are legitimate alone -- an operating-system handle becomes a
pointer, a pointer becomes an integer to be printed or aligned -- but the round
trip drops the provenance the borrow checker and `--verify` follow, in exchange
for nothing.

## Member access and indexing

`a.b` reads a field of a struct value. `p->b` reads a field through a pointer.
`a[i]` indexes an array, a pointer, or a `string`.

```mettle
var p: Point = { x: 3, y: 4 };
var q: Point* = &p;
println("{p.x} {q->y}");
```

Indexing a `string` yields a `char`. Indexing a pointer walks by the pointee's
size, so `b[i].x` on a `Body*` reads the i-th element's field.

`s.length` is the byte count of a `string` and `s.chars` is the `uint8*`
behind it.

## Address-of

`&x` takes the address of a variable, a field, or an element. `&f` takes the
address of a named function, which is how a plain function reaches a `Fn`
parameter or field.

The compiler reports an address that would outlive what it points at, as
[M0103](diagnostics.md) for a returned stack local and
[M0104](diagnostics.md) for one stored in a global.

## Calls

```mettle
var n: int32 = add(2, 3);
```

Arguments pass by value, structs included. A generic call names its type
arguments: `id<int64>(7)`.

A method call is `value.method(args)` or `pointer->method(args)`.
[Declarations](declarations.md) covers how the compiler finds the function
behind it.

Writing a function's name with no parentheses gives you the function itself,
which has a function-pointer type. It does not call it.

## sizeof and typeof

`sizeof(T)` is the size of a type in bytes, settled at compile time.
`typeof(T)` is a compile-time `Type` value, for use with
[`comptime for`](control-flow.md).

## syscall

`syscall(number, ...)` is the machine's system-call instruction, written where
the call appears. The first operand is the number and the rest are its
arguments; the result is the register the kernel returns.

```mettle
var written: int64 = syscall(1, 1, &message[0], 12);   // Linux write(2)
```

[C interoperability](c-interop.md) has the register conventions, the argument
ceiling per target, and why a Windows number is read out of `ntdll` rather than
written down.

## Lambdas

A lambda is an expression:

```mettle
var dbl: fn(int32) -> int32 = fn(x: int32) -> int32 { return x * 2; };
```

Stored in a `Fn(...)` it may capture. [Declarations](declarations.md) covers
the capture rules.

## String interpolation

Every string literal is scanned for `{expr}`. The expression is evaluated and
its text spliced in. `{{` writes one literal `{`.

```mettle
var i: int32 = 7;
var d: float64 = 2.5;
var c: char = 'z';
var s: string = "txt";
println("{i} {d} {c} {s} {i * 2 + 1} {(int64)i}");
```

```text
7 2.5 z txt 15 7
```

Any expression may appear inside the braces: arithmetic, a cast, a field
access, an index, a call. Every scalar type prints in its own way. A `char`
prints as the character and a `uint8` as the number. A `bool` prints `true` or
`false`.

The braces are part of the literal, so interpolation works in any string, and
[`print`](standard-library.md) and `println` are ordinary functions taking one
`string`.

### What it costs, and who releases it

Interpolation desugars to `+` over the pieces, and `+` on strings builds a new
one. Each conversion takes one block sized to what it writes, and every piece
of the chain is dead the moment its bytes have been copied into the next one,
so the compiler releases it there: an interpolated string costs one allocation,
not one per `{expr}`. A `{b}` on a bool takes nothing at all, because the
answer is one of two literals.

The last block is released too, where the compiler can see the end of it. A
string built for one call and handed to a function that neither keeps nor
returns it is unreachable the moment that call returns, and that is not a
promise anyone writes: it is what the [memory analysis](borrow-checker.md)
inferred about that parameter. `println("frames {n}")` therefore allocates once
and frees once. Where there is no such fact, an extern, a call through a
pointer, a callee that stores the view or hands it back, nothing is released
and the string is the program's, exactly as the result of `a + b` is:

```mettle
println("frames {n}");        // one block, taken and released
var label: string = "f {n}";  // one block, and it is yours
println(label);
free(label.chars);
```

A `string` is a borrowed view, so a function that keeps one past the call is
keeping a borrow. The compiler reads a callee that stores its parameter or
returns it as doing exactly that and releases nothing; what it cannot see is a
callee that hands the bytes to an `extern` which keeps them, since nothing
states what an `extern` does with a pointer.

`--record-trace` puts this on a ledger: every block a run takes and every one
it releases, with the site, for `mettle check-trace` to hold a rule to.

## See also

- [Types](types.md)
- [Control flow](control-flow.md)
- [Declarations](declarations.md)
