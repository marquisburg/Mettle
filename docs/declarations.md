# Declarations

What you can declare in a Mettle file: functions, variables, constants,
structs, enums, methods, closures, and declarations the compiler generates for
you.

## Functions

```mettle
fn add(a: int32, b: int32) -> int32 {
  return a + b;
}
```

Parameters carry types. The return type follows `->`. A function that returns
nothing leaves the arrow off:

```mettle
fn greet(name: string) {
  println("hello {name}");
}
```

Every program has `fn main() -> int32`. Its return value is the process exit
code.

Parameters pass by value, including structs. To let a function write to the
caller's value, pass a pointer.

## Variables

`var` declares a local or a global. Every one carries a type:

```mettle
var count: int64 = 0;
var scratch: uint8[256];
```

Every `var` is mutable. A local with no initializer starts zeroed, and so does
an array or struct declared without one.

A local you declare and never read draws a warning. A leading underscore says
you meant it.

## Constants

`const` at file scope holds a compile-time value:

```mettle
const MAX = 100;
const NAME: string = "mettle";
```

A global `const` holding a numeric or string literal may leave its type off,
and it takes the literal's type. Everything else, including every local
`const`, names its type. A float is a constant like any other:

```mettle
const DS_PI = 3.14159265358979;
const HALF: float32 = 0.5;
```

`export const` puts a constant on the module's public surface, where an
importer can use it wherever a constant goes -- an array size included:

```mettle
export const MAX_CARGO = 24;
```

A `const` may hold an aggregate, which is how you write a table:

```mettle
struct Range { lo: int32; hi: int32; }

const BOUNDS: Range[2] = [ { lo: 1, hi: 9 }, { lo: 2, hi: 8 } ];
```

## Globals

A `var` at file scope lives for the whole run and starts zeroed unless you
give it a value:

```mettle
var counter: int64 = 0;
var table: int32[4];

fn bump() {
  counter = counter + 1;
}
```

## Structs

Fields are separated by semicolons:

```mettle
struct Point {
  x: int32;
  y: int32;
}
```

Build one with a field-named literal, or declare it blank and assign:

```mettle
var a: Point = { x: 3, y: 4 };
var b: Point;
b.x = 3;
```

An element may be anything the field could be assigned, so a value computed
where the literal is written needs no separate statement:

```mettle
fn make(width: int32, height: int32) -> Point {
  var p: Point = { x: width * 2, y: height + offset() };
  return p;
}
```

A `const` and a module-scope `var` are the exception: both are laid out before
any code runs, so every element of theirs has to be known while compiling.

## Gathered parameters

A last parameter written `T[..]` takes whatever the call passes after the fixed
ones. Inside the function it is an ordinary `T[]`:

```mettle
fn sum(xs: int32[..]) -> int32 {
  var total: int32 = 0;
  for x in xs { total = total + x; }
  return total;
}
```

`sum(1, 2, 3)` gathers three, `sum(5)` gathers one, and `sum()` gathers none
and reads as an empty slice. Only the last parameter may gather, because
anything after it would have nothing left to take.

One argument that is already the whole run is passed through rather than
wrapped: a `T[]` or a `T[N]` where the gather goes is what the callee receives.
That is how one variadic call forwards to another:

```mettle
fn forward(xs: int32[..]) -> int32 {
  return sum(xs);
}
```

## Methods

A method is a plain function whose name is the type, an underscore, and the
method name. The receiver is the first parameter.

```mettle
struct Point { x: int32; y: int32; }

fn Point_sum(p: Point) -> int32 {
  return p.x + p.y;
}
```

Call it through a value with `.`:

```mettle
var p: Point = { x: 3, y: 4 };
println("{p.sum()}");
```

To let a method write to the receiver, take a pointer and end the name with
`__ptr`. That form is called through a pointer with `->`:

```mettle
fn Point_scale__ptr(p: Point*, k: int32) {
  p->x = p->x * k;
  p->y = p->y * k;
}
```

```mettle
var q: Point* = &p;
q->scale(2);
```

Naming the wrong form gets a diagnostic that says which function it looked
for:

```text
error[E0003]: Undefined method 'Point.scale' (expected function 'Point_scale')
```

## Enums

A plain enum names integer values:

```mettle
enum Color { Red = 1, Green = 2, Blue = 3 }
```

A tagged enum gives variants a payload, at most one value each:

```mettle
enum Shape {
  Circle(float64),
  Square(int32),
  Empty
}
```

[Types](types.md) covers how each behaves and how to read one back.

## Closures and lambdas

A lambda is written `fn(params) -> R { ... }` and may appear anywhere a value
may. What it can capture depends on the type you store it in.

`fn(...) -> R` is a plain function pointer. It captures nothing:

```mettle
var dbl: fn(int32) -> int32 = fn(x: int32) -> int32 { return x * 2; };
```

`Fn(...) -> R` is a closure. It captures the locals it names, by value, into a
heap environment allocated where the lambda is written:

```mettle
fn apply(f: Fn(int32) -> int32, v: int32) -> int32 {
  return f(v);
}
```

```mettle
var k: int32 = 10;
var add: Fn(int32) -> int32 = fn(x: int32) -> int32 { return x + k; };
println("{apply(add, 5)}");
```

Capture is by value, so the closure keeps the value `k` held when the lambda
was written. A later assignment to `k` does not reach it.

A lambda inside a lambda reaches only that lambda's own names.

A named function reaches a closure through its address. `&twice` is accepted
wherever a `Fn` of the same signature is expected: a declaration, an argument,
a return, and an assignment to a variable, a struct field, an array element,
or a field reached through a pointer.

```mettle
struct Holder { step: Fn(int64) -> int64; }
```

```mettle
var g: Fn(int64) -> int64 = &twice;
var h: Holder;
h.step = &twice;
h.step = fn(x: int64) -> int64 { return x + 1; };
```

A plain function value already sitting in a variable is not adapted. Take the
address at the point you need the closure.

## Decorators

A decorator sits before a declaration and asks the compiler for something.

| Decorator | Effect |
|-----------|--------|
| `@inline` | Inline this callee past the size budget |
| `@noinline` | Never inline this callee |
| `@pure` | A contract: the build fails if the function writes anything |
| `@simd` | Report whether this loop vectorized |
| `@unroll(n)` | Unroll this loop n times |
| `@test` | A compile-time test, run by `mettle test` |
| `@rule` | A property the program requires of itself, checked on every build; see [Rules](rules.md) |
| `@naked` | No prologue, no frame, no epilogue; the body is `asm` only |
| `@interrupt` | Entered by the CPU; the compiler emits the save/restore and the interrupt return |

```mettle
@inline fn hot(x: int32) -> int32 { return x * 3; }
@noinline fn cold(x: int32) -> int32 { return x - 1; }
@pure fn square(x: int32) -> int32 { return x * x; }
```

`@pure` buys no optimization. Purity is inferred by a whole-program fixpoint,
and the loop-invariant call hoist reads only what that pass proved, so a
function whose body writes nothing has its call hoisted whether or not anyone
wrote `@pure` on it. What the decorator does is make the claim checkable: write
it down and the build fails with `F0004` the day the body starts writing. A
program compiles to the same instructions with the decorator and without it.

`@simd`, `@inline!`, `@pure` and `@noalloc` are contracts. The build fails when
the compiler cannot deliver what they ask, and the message names the site that
defeated it. Vectorization contracts are only checked when optimization is on:

```text
note: 1 `@simd` loop present but not verified; vectorization contracts are
only checked with -O/--release
```

`@rule` is the open end of the contract set. A `@rule fn` takes the checked
program as data and returns a verdict with a location, and a failing verdict
stops the build the way `@noalloc` does. [Rules](rules.md) covers it.

Two clauses sit on the signature line beside the effect clauses, both
contextual keywords. `reference NAME` says this function computes what `NAME`
computes, and every build runs both on generated inputs and refuses the build
when they disagree; [Translation validation](translation-validation.md) covers
it. `explain RNNNN "text"` gives a `@rule` its own diagnostic code and its own
explanation; [Rules](rules.md) covers it.

```mettle
fn abs_fast(x: int32) -> int32 reference abs_slow { ... }
@rule fn no_recursion(p: Program) -> Verdict explain R1001 "..." { ... }
```

`textof(x)` joins the `sizeof` / `typeof` / `fieldof` / `layoutof` family: it
answers a constant's text, so a name or a tag can be built while compiling.
[Control flow](control-flow.md) covers it beside `comptime for`.

A function may also declare what its longest path is allowed to cost:
`fn tick() where cycles < 400`. It reads the same way a declared type's
predicate does, and the compiler proves it or refuses the build.
[Types](types.md) covers deadlines beside the rest of `where`.

`@naked` and `@interrupt` are for code the operating system is not running.
[Bare metal](bare-metal.md) covers both, along with inline assembly.

[Compilation](compilation.md) covers the flags, and
[`--explain`](compilation.md) reports what each decision came to.

## Rewrite rules

A `rewrite` declaration gives the optimizer an identity: an expression to
look for, the expression to put in its place, and an optional `where` that
narrows it to the arguments on which it holds.

```mettle
rewrite twice_plus(a: int64) -> int64 {
    from a + a;
    to a * 2;
}
```

The compiler runs both sides on generated inputs before it uses the rule, and
a rule whose sides disagree is a compile error naming the input. Every
function the rule changes is checked the same way. [Rewrite
rules](rewrite-rules.md) covers what a rule can say and how the report
accounts for it.

## Imports and visibility

`import "std/io";` brings a module's exported names into the file.
`export` on a declaration makes it visible to importers:

```mettle
export fn add_two(a: int32, b: int32) -> int32 {
  return a + b;
}
```

Without `export`, a declaration is private to its file.
[Modules](modules.md) covers path resolution and search order.

## extern

`extern` declares something the linker will supply. The declaration is a
promise, not a check: nothing verifies at compile time that the symbol
exists, and a name nothing provides fails at the link instead.
[C interoperability](c-interop.md) covers what can satisfy one.

Without a link name, the Mettle name *is* the symbol:

```mettle
extern fn strlen(s: cstring) -> int64;
```

With `= "symbol"`, the two differ, which is how a C name that collides with
something of yours gets a different spelling on this side:

```mettle
extern fn c_strlen(s: cstring) -> int64 = "strlen";
```

A name may carry only one link name. Declaring it twice against the same
symbol is fine; against two different ones is an error:

```text
error[E0003]: Function 'c_print' redeclared with conflicting link name
```

`export extern fn` re-exports the binding, so a module can hand a symbol on to
whatever imports it. That is how `std/process` offers `rand` and `srand`:

```mettle
export extern fn rand() -> int32 = "rand";
```

### extern var

A global the linker supplies. It takes the same optional link name:

```mettle
extern var g_counter: int32;
extern var g_flag: int32 = "c_side_flag";
```

Two rules, both because the definition lives on the other side:

- The type must be written out. There is nothing here to infer it from.

  ```text
  error[E0002]: Extern variable declarations require an explicit type
  ```

- It cannot have an initializer. Whoever defines it initializes it.

  ```text
  error[E0002]: Extern variable declarations cannot have an initializer
  ```

[C interoperability](c-interop.md) covers the calling convention, the types
that cross the boundary, and which names actually resolve.

## defer and errdefer

`defer` runs a statement when the enclosing scope ends, on every path out,
including `break`, `continue`, and a `return` from inside a `switch`. Deferred
statements run in reverse order of declaration.

```mettle
fn work(n: int32) -> int32 {
  defer println("leaving work");
  if (n < 0) { return 0; }
  return n;
}
```

A `defer` inside a loop body runs at the end of each iteration.

`errdefer` runs only when the function returns an error, which means an `Err`
arm of a [`Result`](types.md). Use it to undo work a failed call leaves
behind:

```mettle
fn step(n: int32) -> Result<int32, string> {
  errdefer println("rolling back");
  defer println("always");
  if (n < 0) { return Err("negative"); }
  return Ok(n);
}
```

Calling `step(1)` prints `always`. Calling `step(-1)` prints `always` then
`rolling back`.

## Generated declarations

`comptime for` at file scope generates declarations, one set per iteration.
Each generated name must come from the loop binding, and `ident(...)` joins
compile-time strings into one:

```mettle
struct Pair { a: int32; b: int32; }

comptime for f in typeof(Pair).fields {
  const ident("OFFSET_", f.name): int64 = f.offset;

  fn ident("end_of_", f.name)(base: int64) -> int64 {
    return base + f.offset;
  }
}
```

That declares `OFFSET_a`, `OFFSET_b`, `end_of_a`, and `end_of_b`.

A `const` array is a table, and `.rows` generates from it. The columns are read
by name, so the declaration and what it is built from sit next to each other:

```mettle
struct Op { name: string; code: int32; }

const OPS: Op[2] = [ { name: "add", code: 1 }, { name: "mul", code: 2 } ];

comptime for op in OPS.rows {
  fn ident("code_of_", op.name)() -> int32 { return op.code; }
}
```

A body whose declaration name does not come from the binding generates the
same name every iteration, and the compiler rejects it rather than dropping
half the output.

The binding is a compile-time value. It cannot leak into a run-time one, so
assigning `f` itself to an `int64` is an error.

## See also

- [Types](types.md)
- [Control flow](control-flow.md)
- [Modules](modules.md)
