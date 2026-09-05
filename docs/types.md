# Types

Every binding in Mettle carries a written type. The compiler infers none of
them. This page lists the types, their sizes, and the rules for moving a value
from one type to another.

## Nothing is inferred

A `var` needs a type, and so does a local `const`:

```mettle
var count: int64 = 0;
const LIMIT: int32 = 512;
```

Leaving the type off is an error, even when the initializer makes the type
obvious. Two places relieve you of writing one, and both take the type from
the surrounding structure: the counter of a range loop, `for i in 0..n`, and a
global `const` holding an integer literal.

## Sizes and alignment

On x86-64 and AArch64:

| Type | Size | Alignment |
|------|------|-----------|
| `int8`, `uint8`, `bool`, `char` | 1 | 1 |
| `int16`, `uint16` | 2 | 2 |
| `int32`, `uint32`, `float32` | 4 | 4 |
| `int64`, `uint64`, `float64` | 8 | 8 |
| pointers, `rawptr`, `cstring` | 8 | 8 |
| plain enums | 8 | 8 |
| `string` | 16 | 8 |

A struct is laid out in declaration order with each field on its own
alignment, and the whole struct padded to its widest field. A tagged enum is
sized from its tag plus its largest payload. `sizeof(T)` reports the size at
compile time:

```mettle
struct S { a: int8; b: int64; c: int16; }
```

`sizeof(S)` is 24: one byte, seven of padding, eight, two, and six more of
tail padding.

## Integers

Signed: `int8`, `int16`, `int32`, `int64`. Unsigned: `uint8`, `uint16`,
`uint32`, `uint64`.

Arithmetic compiles to the machine's own instructions, so overflow wraps in
both directions. Signed overflow wraps two's complement and unsigned wraps
modulo 2^n. There is no trap and no check.

An integer literal with no other context is `int32`.

## Floats

`float32` and `float64`, IEEE 754 single and double. A literal with a decimal
point is `float64`; cast it to get single precision.

Mixing widths in one expression is allowed, and the narrower side widens.
Storing a `float64` into a `float32` converts on its own, so the cast rule
below covers the integer types only.

Each operation rounds once, at the width it is written in. A summation is the
one place where an optimized build can answer differently from an unoptimized
one: the vectorizers add a float reduction lane by lane and combine the lanes
at the end, which is the same additions in a different order, and floating
point addition is not associative. `METTLE_NO_SIMD=1` turns that off when a
run has to match `-O0` exactly.

## bool

`bool` is one byte and holds `true` or `false`. Comparison and the logical
operators produce it. It converts to and from the integer types with no cast:
assigning an integer gives `true` for any nonzero value, and assigning a
`bool` to an integer gives 1 or 0.

## char

`char` is one byte holding one character. A character literal has this type,
so `'a'` is a `char` and the number 97 is not.

It behaves like a byte where that is what you want. It widens into any wider
integer with no cast, and arithmetic on it promotes to `int32`, because
`c - 'a'` is an index and `c + 1` is the next code point. Going the other way
needs a cast.

```mettle
var c: char = 'h';
var code: int32 = c;
var offset: int32 = c - 'a';
var next: char = (char)(c + 1);
```

The distinct type is what makes printing work. Interpolation writes a `char`
as the character and a `uint8` as its number:

```mettle
println("{c}");
println("{(uint8)c}");
```

```text
h
104
```

## string

`string` is a 16-byte view: a pointer to bytes and a length. It owns nothing.
The bytes behind it may be a literal in read-only memory or a buffer the
program allocated.

`s.length` counts bytes. `s[i]` reads the byte at `i` as a `char`. `for c in s`
walks the bytes one at a time.

```mettle
var s: string = "hello";
var first: char = s[0];
for c in s {
  print("{c}");
}
```

For ASCII, bytes and characters are the same thing. For anything else they are
not: a string holding an e-acute reports a `length` two larger than its
character count, because that character takes two bytes. Use
[std/utf8](standard-library.md) when you need code points.

`s[i]` is a read. Assigning through it is rejected:

```text
error[E0003]: Cannot assign through a string index: a string is a borrowed
view and its bytes may be read-only
```

When the bytes are yours, write through `s.chars`, the `uint8*` behind the
view.

`for c in s` evaluates its subject once, so `for c in read_line(buf, 256, f)`
reads one line and walks it.

## Pointers

`T*` points at a `T`. Take an address with `&`, read through the pointer with
`p->field` for a struct field or `p[i]` for the i-th element.

```mettle
var p: Point;
var q: Point* = &p;
q->x = 3;
```

A null pointer is the integer `0`, and that is how the standard library tests
for one:

```mettle
var buf: rawptr = mx_alloc(1024);
if (buf == 0) { return 1; }
```

`rawptr` is a pointer to nothing in particular, for memory whose type has not
been decided. `cstring` is a pointer to nul-terminated bytes, and it exists for
the C boundary. [C interoperability](c-interop.md) covers converting between
`cstring` and `string`.

`volatile T` says that reading or writing a `T` is observable in itself: no
access to one is removed, merged, hoisted, or served from a register. The
qualifier binds to the value accessed, so `volatile uint16*` points at volatile
`uint16` -- the shape memory-mapped hardware has.

```mettle
var vga: volatile uint16* = (volatile uint16*)0xB8000;
vga[0] = 0x0F41;
```

A global may be `volatile` too, which is the flag an interrupt handler
writes and the main line reads. The qualifier travels with the symbol there,
so a function that names one keeps every value in memory.

[Bare metal](bare-metal.md) covers what the compiler guarantees and what it
costs.

### Device address spaces and alignment

A pointer, slice or view used on a GPU says which memory it names. The
qualifier sits between the element type and the suffix:

```mettle
var rows: float32 global align(16)*;
var tile: float32 shared*;
var table: float32 constant*;
var scratch: int32 local*;
var window: float32 global[];
```

`global` is the device's own memory, `shared` a workgroup's tile, `constant`
memory a kernel only reads, and `local` a work item's private frame. A plain
`T*` is generic: it claims nothing, and a spaced pointer flows into one for
free. The other direction is a claim, so it is an explicit cast the interpreter
re-checks; a pointer in one named space never becomes a pointer in another.

`align(N)` claims the address is N-byte aligned, with N a power of two up to
4096. The claim is proven where the pointer is built -- from a `shared`
declaration, from a pointer that already carried an alignment, or from the
arithmetic of the offset, which a declared type such as
`type Quad = int32 where value % 4 == 0;` can carry through a local. An
unproven claim is refused with the alignment the expression reached. What a
proven one buys is in [GPU offload](gpu.md#address-spaces-and-alignment-in-the-pointer-type).

### Uniform values on a device

`std/warp` declares one more device type:

```mettle
export type Uniform<T> = T where uniform(value);
```

`uniform(value)` says every work item of the group holds the same value. It is
a predicate the compiler discharges from what the value depends on, not a
promise the program makes: a work-item index, a subgroup lane and a load from
memory each differ between work items, so nothing built from one is uniform.
A conversion it cannot prove is refused naming the term that varies, and what a
proven one buys is in [GPU offload](gpu.md#uniformity-as-a-declared-type).

## Arrays

`T[N]` is N elements laid out end to end. The size is part of the type, and it
must be a compile-time constant.

```mettle
var xs: int32[3] = [10, 20, 30];
var scratch: uint8[1024];
```

An array declared without an initializer starts zeroed.

Indexing is unchecked by default. Build with [`--safe`](memory-safety.md) to
have the compiler insert bounds checks and prove away the ones it can.

An array passed where a pointer is expected decays to a pointer to its first
element. The compiler decides that from the destination type, so a parameter
declared `int32*` receives the array.

A suffix may repeat, and the dimensions read left to right the way the
declaration does:

```mettle
var grid: int32[3][4];
grid[2][3] = 7;
```

`int32[3][4]` is three rows of four, `sizeof` is 48, and `grid[i][j]` takes
`i` from the first dimension and `j` from the second. Storage is row-major and
contiguous: `grid[1]` is a row and decays to a pointer to its first element,
and `&grid[0][0]` walks all twelve. Both dimensions are bounds-checked, at
compile time for a constant index and under `--safe` for one computed at
runtime. Further dimensions stack the same way.

## Structs

Fields are separated by semicolons:

```mettle
struct Range {
  lo: int32;
  hi: int32;
}
```

A struct literal names its fields in braces. A local struct with no
initializer starts zeroed:

```mettle
var r: Range = { lo: 5, hi: 7 };
var blank: Range;
```

Arrays of struct literals work the same way, which makes a table a `const`:

```mettle
const BOUNDS: Range[2] = [ { lo: 1, hi: 9 }, { lo: 2, hi: 8 } ];
```

Structs pass and return by value. [Declarations](declarations.md) covers
attaching methods to one.

## Enums

A plain enum is a named set of integer values. It is 8 bytes and it does not
decay to an integer on its own:

```mettle
enum Color { Red = 1, Green = 2, Blue = 3 }
```

Name a variant through its type, `Color.Green`. To use one as a number, cast
it: `(int32)c`. Branch on it with [`switch`](control-flow.md).

A tagged enum gives variants a payload. Each variant carries at most one
value:

```mettle
enum Shape {
  Circle(float64),
  Square(int32),
  Empty
}
```

Build one by calling the variant, `Square(4)`. When two enums in scope share a
variant name, qualify it, `Shape.Square(4)`. Read one with
[`match`](control-flow.md), which binds the payload and must cover every
variant.

## Result and Option

[std/core](standard-library.md) defines two tagged enums that take type
parameters:

```mettle
export enum Result<T, E> { Ok(T), Err(E) }
export enum Option<T> { Some(T), None }
```

`Result` is for a call that could not do what it was asked, and the error arm
says why. `Option` is for a value that may not be there.

```mettle
fn half(n: int32) -> Result<int32, string> {
  if (n % 2 != 0) { return Err("odd"); }
  return Ok(n / 2);
}
```

```mettle
match (half(8)) {
  case Ok(v): { println("ok {v}"); }
  case Err(e): { println("err {e}"); }
}
```

## Slices

`T[]` is a pointer to `T` and a length, in one value. It is what a buffer looks
like when its extent travels with it:

```mettle
fn total(xs: int32[]) -> int64 {
  var sum: int64 = 0;
  for x in xs { sum = sum + (int64)x; }
  return sum;
}
```

A `T[N]` converts to `T[]` wherever one is expected -- a binding, an argument,
a return -- and the length the type carried becomes the length the value
carries. `.length` reads it and `.data` is the pointer, which is what a C
boundary takes.

`new T[n]` allocates `n` elements and answers a `T[]`, which is how a program
writes an array whose size is not known while compiling:

```mettle
var xs: int32[] = new int32[count];
for i in 0..xs.length { xs[i] = (int32)i; }
free(xs.data);
```

Indexing a slice is checked against the length it carries, which a pointer
could never offer. The check is emitted in a normal build, dropped under
`--release`, and kept under [`--safe`](memory-safety.md).

A pointer and a length that came from somewhere else are joined by writing them
down, which is the one place the extent is asserted:

```mettle
var view: int32[] = { data: borrowed, length: 3 };
```

## Views

`T[,]` is the slice one rank up: a pointer to `T`, an extent per dimension, and
a leading dimension per outer dimension, in one value. The innermost dimension
is always contiguous, which is the fact every consumer of a matrix wants and
the one the optimizer would otherwise have to prove. `T[,,]` is the rank-3
form.

```mettle
var m: int32[3][4];
var v: int32[,] = m;
var w: float32[,] = new float32[rows, cols];
```

A `T[N][M]` converts to `T[,]` the way `T[N]` converts to `T[]`, with the
extents the type carried, and `new T[m, n]` allocates `m * n` zeroed elements
whose leading dimension is `n`. The fields are `.data`, `.dims`, and `.lead`,
and `.length` is the outer extent, so `for row in v` walks the rows.

Indexing drops a rank. `v[i]` is the row, a `T[]` of length `v.dims[1]`, and
`v[i][j]` is then ordinary slice indexing. On a `T[,,]` the first index yields
a `T[,]`. Nothing is copied: a row shares the view's memory.

```mettle
fn scale_rows(v: float32[,], k: float32) {
  for i in 0..v.dims[0] {
    var row: float32[] = v[i];
    for j in 0..row.length { row[j] = row[j] * k; }
  }
}
```

A view over padded storage names its own leading dimension, which is how a
tile of a larger matrix is passed without a copy:

```mettle
var tile: int32[,] = { data: &backing[1][2], dims: [2, 3], lead: [8] };
```

The row index is checked against the outer extent in a normal build and under
[`--safe`](memory-safety.md), and dropped under `--release`, the same rule a
slice's check follows. A loop over a row is a unit-stride loop over a pointer
the optimizer can name, so it vectorizes the way the pointer form does, and
[`@simd!`](declarations.md#decorators) holds it to that. A view never owns its data; the result of `new T[m, n]`
is freed through `free(v.data)`.

Three things a view does not offer, each because it would break the promise
above: a column or a transposed view, whose stride would no longer be one; a
strided step; and arithmetic on whole views, which would hide a loop nobody
wrote. A view may be a `kernel` parameter, where it crosses the launch boundary
as a record.

## Function types

`fn(A, B) -> R` is a plain function pointer. It holds a code address and
captures nothing:

```mettle
var dbl: fn(int32) -> int32 = fn(x: int32) -> int32 { return x * 2; };
```

A suffix binds to where it sits, so the `[2]` in `fn(int32) -> int32[2]` is
part of the return type. Parentheses group what the suffix binds to, which is
how a dispatch table is spelled:

```mettle
var table: (fn(int32) -> int32)[2];
table[0] = &add_one;
```

`Fn(A, B) -> R` is a closure. It may capture locals from where it was written,
and it carries an environment alongside the code address:

```mettle
var k: int32 = 10;
var add: Fn(int32) -> int32 = fn(x: int32) -> int32 { return x + k; };
```

Which one you write says which you mean. A plain function converts to a
closure type on assignment and at a call.
[Declarations](declarations.md) covers capture rules and lifetimes.

Either kind may say what a call through it performs and needs:

```mettle
fn run_sim(job: fn() -> void with Sim) { job(); }
fn on_ui(job: Fn() -> void requires MainThread) provides MainThread { job(); }
```

A type with a `with` clause is closed, so a function handed to it has to
perform nothing outside the clause, and the compiler checks that against what
the function actually reaches. [Effects](effects.md) covers the clauses.

## Generic types and functions

A function may take type parameters:

```mettle
fn id<T>(x: T) -> T { return x; }
```

The call site may name them, and does not have to when the arguments already
say what they are. `id(7)` is the call `id<int32>(7)`: a parameter written with
a type parameter is matched against the argument's type, and the binding is
whatever the argument put where the type parameter sits, through pointers and
array elements too.

```mettle
var n: int32 = id(7);
var swapped: int32 = first(&values[0], 3);
var wide: int64 = id<int64>(7);
```

An integer literal says `int32` and a fractional one says `float64`, so name
the argument when a call needs a wider one. What no argument reaches is
reported:

```text
error[E0003]: Nothing in this call says what 'T' is in 'make'. Name it at the
call, as 'make<sometype>(...)'
```

A bound restricts which types are allowed. Declare a trait, say which types
have it, then require it:

```mettle
trait Addable;
impl Addable for int32;

fn add_one<T: Addable>(v: T) -> T { return v + 1; }
```

Calling `add_one<float64>(1.0)` fails:

```text
error[E0003]: Type 'float64' does not implement trait 'Addable' required by
'add_one'
```

Several bounds join with `+`, as in `<T: Addable + SignedNumber>`.

Each set of type arguments produces its own copy of the function at compile
time.

## Declared types

A type can carry a rule of its own. `type Name = Base where predicate;`
declares a type whose every value is a `Base` that satisfies the predicate,
written over `value`:

```mettle
type Percent = int32 where value >= 0 && value <= 100;
type Digit = int32 where value >= 0 && value < 10;
type Meters = float64;
type Seconds = float64;
```

A `Percent` is an `int32` everywhere one is read: it adds, compares, indexes
and prints as one, and it flows into an `int32` binding without ceremony. The
other direction is the point. An `int32` becomes a `Percent` only where the
compiler can prove the predicate, and the proof is never written down:

```mettle
fn report(p: Percent) -> int32 { return p; }

fn from_guard(n: int32) -> int32 {
  if (n >= 0 && n <= 100) {
    var p: Percent = n;
    return report(p);
  }
  return -1;
}

fn from_early_exit(n: int32) -> int32 {
  if (n > 100) { return -1; }
  if (n < 0) { return -1; }
  var p: Percent = n;
  return p;
}

fn from_arithmetic(n: int32, b: uint8) -> int32 {
  var masked: Percent = n & 63;
  var small: Digit = (int32)b % 10;
  return masked + small;
}
```

The compiler proves what is cheap: a constant, the value's own type (a `uint8`
is 0..255, a `Digit` is 0..9), arithmetic it can bound (`&` with a constant
mask, `%`, `+`, `-`, `*`, `/` over known ranges), a `for` range, a dominating
`if` whose condition implies the predicate, an `if` that returns and so rules
the other case out, and a guard that repeats the predicate itself:

```mettle
fn is_even(n: int32) -> bool { return n % 2 == 0; }
type Even = int32 where is_even(value);

fn halve(n: int32) -> int32 {
  if (is_even(n)) {
    var e: Even = n;
    return e / 2;
  }
  return 0;
}
```

Past that it refuses to guess, and it says what it could not prove:

```text
error[P0001]: cannot prove `value <= 100` for `b`, which 'Percent' requires
(its range here is 0..255)
```

A cast, `(Percent)n`, is the same conversion spelled out, and it needs the
same proof. There is no way to assert a value into a declared type.

The predicate speaks about `value` unless it is given another name, which is
worth doing where `value` reads badly:

```mettle
type Slot = uint32 where n: n < 64;
type Even = int32 where k: is_even(k);
```

The name binds only inside the predicate, and the diagnostics use whichever
name was written.

A declared type without a predicate is a unit. `Meters` and `Seconds` never
mix: `m + s` is an error, `m + m` is a `Meters`, `m * 2.0` is a `Meters`, and
`plain / s` is a `float64`. A plain `float64` becomes a `Meters` only by a
cast, so the place the meaning is decided is written where it is decided.

The rule pays back in the optimizer. An index whose declared type pins its
range inside the array needs no bounds check, so none is emitted, in debug,
release and `--safe` builds alike, and `--explain` says so under "proven by
type" with the range and the array it was checked against:

```mettle
fn get(a: int32[10], d: Digit) -> int32 { return a[d]; }
```

`mettle test`, `mettle trace` and `--verify` do not take the prover's word for
it: every proven conversion is checked as the interpreter runs, and a value
that violates its type stops the run naming the type. The prover is code, and
code is not trusted on its own authority.

## What the compiler proves from

The prover establishes a predicate from a constant, from the value's own type,
from arithmetic it can bound, from a `for` range, from a dominating test, from
an early exit, and from a guard that repeats the predicate. The arithmetic
includes an arithmetic right shift by a constant, which is exact and monotone,
so a negative interval narrows through one the same way a positive interval
does. That is what lets fixed-point code carry a declared type at all: a Q15
multiply is `(s * g) >> 15`, and with `Sample` and `Gain` on the operands the
result is provably a `Sample`.

```mettle
type Sample = int32 where value >= -32768 && value <= 32767;
type Gain = int32 where value >= 0 && value <= 32768;

@noalloc fn scale(s: Sample, g: Gain) -> Sample {
  return (Sample)((s * g) >> 15);
}
```

One bit wider and it is refused, with the range it computed:

```text
error[P0001]: cannot prove `value <= 32767` for `s * s >> 15`, which 'Sample'
requires (its range here is -32767..32768)
```

Three more facts reach further, and each is inferred:

**What a call returns.** A function's body exports what it proved about the
value it returns, gathered at every `return` under the guards that reach it. A
`clamp` that returns 0 below zero, 100 above it, and its argument in between
exports 0..100, and `(Percent)clamp(n)` is proven at the call site with no
guard there at all. A return the interval engine cannot bound defeats the
export, because a fact has to hold on every path or it is not one.

**What a loop carries.** A counter that only ever rises keeps the bound its
initialiser gave it, so a `while (k < 100)` bounds `k` above and `var k: int32
= 5;` bounds it below, and `k` is a `Small` inside the body even though it is
never 5 again. A write anywhere in the function that moves the counter the
other way, or by an amount the compiler cannot bound, ends it. A float
accumulator inside a loop whose trip count the compiler bounded gets the same
treatment, widened over the whole run and not to the value it ends with. The
trip count does not have to come from a literal: a loop over a length whose
declared type carries a maximum runs at most that many times, so

```mettle
type Count = int64 where value >= 0 && value <= 64;

fn total(a: Unit*, n: Count) -> float64 {
  var s: Total = (Total)0.0;
  var i: int64 = 0;
  while (i < n) { s = s + a[i]; i = i + 1; }
  return (float64)s;
}
```

proves, and the same function over a plain `int64` length does not.

**A relation to another value.** A predicate may name a binding that is not in
scope where the type is declared:

```mettle
type Index = int64 where value >= 0 && value < buf.length;

fn get(buf: int32[], i: int64) -> int32 {
  if (i >= 0 && i < buf.length) {
    var at: Index = (Index)i;
    return buf[(int64)at];
  }
  return 0;
}
```

`Index` says nothing until there is a `buf`. Every fact it carries is read at
the site, in that scope, and refused there when the name is not in it. A
relational type has no interval of its own, so `--check-proofs` re-checks it by
evaluating the predicate itself at the site, with the binding standing for the
value that was proven.

## Floats

A float predicate is two facts: an interval and the rounding a value inside it
may have accumulated.

```mettle
type Unit = float64 where value >= 0.0 && value <= 1.0;

fn blend(a: Unit, b: Unit) -> Unit {
  return (Unit)((float64)a * (float64)b);
}
```

The product of two values in 0..1 is proven; their sum is not, and the refusal
prints the interval it computed and the relative rounding term it carried.
Endpoints are computed in the widest arithmetic the host has and rounded
outward only where the result is not exactly representable, so a bound the
arithmetic cannot cross is not widened past it. A subtraction, or an addition
of values that can have opposite signs, sets the rounding term to one: after
cancellation there is no relative bound left, and the prover refuses rather
than speaking past what it knows.

There is a pass that reads it. Adding a run of floats four lanes at a time is a
different sum from adding them one at a time, and the two answers differ by a
rounding the compiler cannot bound without knowing how many terms there are.
Where the accumulator carries a declared bound, the float sum vectorizer
refuses the rewrite and `--explain` prints why. Where it does not, the licence
is real, and `--explain` lists it under **beliefs** rather than leaving it
silent.

## Structs

A declared type may refine a struct, and then its predicate speaks about the
fields:

```mettle
struct Span { lo: int32; hi: int32; }

type Ordered = Span where value.lo <= value.hi;
```

An `Ordered` is a `Span` everywhere one is read, and a `Span` becomes an
`Ordered` only where the predicate is proven, the same as any other declared
type. What a struct adds is the second obligation: **a write into a field has
to leave the predicate true.**

```mettle
fn widen(o: Ordered, more: int32) -> int32 {
  if (o.lo <= o.hi + more) {
    o.hi = o.hi + more;
  }
  return o.hi - o.lo;
}
```

The predicate is taken as it will read after the write, with the written field
standing for the value being assigned and every other field for what it already
holds, and that condition has to hold at the write. Nothing is carried over
from the conversion that made the value: a write is a new obligation, and it is
discharged where it happens. A write the compiler cannot prove is refused,
naming the conjunct it could not establish.

`--check-proofs` re-checks a struct refinement by evaluating the predicate
itself, in the scope where the value was proven.

## What a range deletes

`--check-overflow` puts a trap on every signed `+`, `-` and `*` whose result
the compiler cannot show will fit:

```mettle
fn add(a: int32, b: int32) -> int32 { return a + b; }
```

```text
Fatal error: signed '+' overflowed int32
```

A declared type that bounds the operands is what removes it. The operands'
intervals are combined at a width the combination cannot itself overflow, and
if every value the pair can produce lands inside the result's type, no check
is emitted:

```mettle
type Half = int32 where value >= 0 && value <= 1000000000;

fn blend(a: Half, b: Half) -> int32 { return a + b; }
```

Two `Half`s sum to at most two billion, which an `int32` holds. `--explain`
reports the deletion and what proved it:

```text
line 16 in blend: no overflow check emitted, because 'int32' holds every value
the operands can produce here, 0..2000000000, so the check could never fire
```

A program whose every signed operation is proven that way compiles to the same
bytes with the flag as without it, which is the claim stated as a comparison.
Unsigned arithmetic is never checked, because wrapping is what an unsigned
type is reached for.

## Deadlines

A function may declare what its longest path is allowed to cost:

```mettle
fn tick(seed: int32) -> int32 where cycles < 400 { ... }
```

`where cycles < N` reads the same way `where value >= 0` does. It is a claim
the compiler proves or refuses, never a hint. It is a clause like the effect
clauses and sits on either side of them: `fn tick() where cycles < 400
requires Frame` and `fn tick() requires Frame where cycles < 400` are the same
function. The cost of one instruction
comes from the target description, the cost of a call is the callee's own
longest path plus the description's call cost, and the cost of a loop is its
body times a trip count taken from the loop's own bound: the induction
variable's entry value, its step, and what it is tested against, which may be
a constant or a binding the function set once before the loop. A path costing
more than the bound is `D0001`.

```text
error[D0001]: 'tick' declares a deadline of 60 cycles, and its longest path
costs 191 on this target
```

The model is data. `mettle target <triple>` prints it with the rest of the
description, and `--target mine.mettle` builds against a different one, which
is how a machine the compiler has never seen gets its deadlines costed:

```mettle
  cost_op: 1,
  cost_load: 4,
  cost_multiply: 3,
  cost_divide: 26,
  cost_call: 4,
```

`--report-deadlines` prints the model in force and says whether it came from a
description or is the target's own, so a number can always be traced to the
machine it was costed against.

The model costs the IR as lowered, before the optimizer runs, so the number is
an upper bound on the work the program asked for and not a prediction of one
machine's cycles.

A nest costs its inner loop once for every turn of its outer one, and the
calls the compiler itself put in the function are costed too: the trap a
bounds check branches to (which ends the program, so it costs one call and
nothing after), the counters `--check-deadlines` keeps, the lines
`--record-trace` writes, the shadow map behind `--safe`.

That last point is why a checked build is reported rather than refused. A
build carrying `--safe`, `--record-trace`, `--check-overflow`, `--check-tasks`,
`--check-effects`, `--check-proofs` or `--verify` is not the build the deadline
was declared against, and the extra work is on the path. The cost is counted
and printed, so the number is what that build costs; the miss comes out as
`warning[D0001]` and the binary is still produced. Build without the checks, or
declare what the checked build costs. `mettle test` generates no code at all
and so asks no question about generated code: no deadline is checked there.

A path with no bound the compiler can find is `D0002`, and it is a refusal
rather than a silence, because a deadline that cannot be proven is not a
deadline. `--pgo` interprets the program and lets a measured trip count stand
in; the deadline then holds, and the report says on what:

```text
deadline tick: 182 of 4000 cycles, 3817 to spare, held on a measured trip count
deadlines: 1 declared, 1 proven, 1 held on evidence
```

`--report-deadlines` prints the longest path block by block with what each one
costs, which is where the work is:

```text
  longest path through tick:
    ir_entry_82 at line 13 costs 2
    ir_while_83 at line 16 costs 170
```

`--check-deadlines` asks the same question while the program runs. Every basic
block adds its own model cost to the frame on top, and returning past the
proven longest path traps. The model is the one the proof used; what is
re-checked is the claim that no path costs more, which is the half an analysis
can get wrong. A program with no deadline compiles to the same bytes with the
flag as without it.

## What a declared type earns

A property the program declared and the compiler proved earns the treatment one
the compiler discovered earns, and no more. `--explain` lists each under
**proven by type**, naming the type that proved it and the pass that consumed
the proof:

- An index whose declared type pins its range inside the array carries no
  bounds check, in debug, release and `--safe` builds alike. Consumed by
  lowering.
- A pointer whose declared type rules out zero carries no null check, because
  the check could never fire. Consumed by lowering.
- A divisor whose declared type rules out zero lets a loop-invariant divide
  leave the loop; without the proof it stays, because a divide that traps must
  not run on an iteration the loop would never have taken. Consumed by
  invariant-arithmetic LICM.
- A float accumulator whose declared bound would not survive being reassociated
  into lanes stops that rewrite. Consumed by the float sum vectorizer.

Since these are not optimizer decisions, `--explain` reports them without
`-O`, which is where the checks a declared type deleted are visible at all:
`--release` emits no such check for anyone.

`mettle why app.mettle 15 Percent` answers the other half of the question: it
prints the chain and the range for a conversion that succeeded, in the same
words the refusal would have used.

`--report-proofs` prints the ledger: one line per conversion, with the type,
the expression, the site, whether it was proven, what it cost in prover steps,
and the route that settled it (a range the compiler could bound, or a
dominating test that repeats the predicate). `--proof-budget=N` makes that cost
a contract and fails the build with `P0003` when the prover exceeds it.
`--explain` prints the same proofs under **types proven**, each naming the pass
that consumed it.

`--check-proofs` extends that to a compiled program: every proven conversion
traps at run time when the value is not what the compiler proved, in
`--release` too. It is a separate flag from `--safe`, because it answers a
different question. `--safe` distrusts the program's indices; `--check-proofs`
distrusts the compiler's own prover. A program that declares no such type
compiles to the same bytes with the flag as without it.

## Conversions

Widening happens on its own. Assigning an `int32` to an `int64`, or a
`float32` to a `float64`, needs no cast, because no value is lost.

Narrowing one integer type into a smaller one needs a cast, and saying so is
[M0119](diagnostics.md):

```text
error[M0119]: Narrowing conversion from 'int64' to 'int8' needs a cast
```

A literal that cannot fit its destination is rejected outright, and that is
[M0118](diagnostics.md):

```text
error[M0118]: Integer 300 is out of range for 'int8'
```

`(T)expr` is the cast. It converts between integer widths, between integers
and floats, between `char` and integers, and between pointer types.

## Compile-time types

`typeof(T)` yields a `Type`, a value the compiler can ask questions of while
compiling. `Type` has `.fields`, and each `Field` has `.name`, `.type`,
`.offset`, and `.index`. `.type` is itself a `Type`, so `f.type.size` is the
field's size.

```mettle
comptime for f in typeof(Point).fields {
  println("field {f.name} at {f.offset}");
}
```

Neither `Type` nor `Field` exists at run time; both are gone once the loop is
expanded. [Control flow](control-flow.md) covers `comptime for`.

## See also

- [Declarations](declarations.md)
- [Expressions](expressions.md)
- [Memory safety](memory-safety.md)
