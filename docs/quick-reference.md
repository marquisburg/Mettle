# Quick reference

The things you look up. Every snippet here compiles.

## A program

```mettle
import "std/io";

fn main() -> int32 {
  println("hello");
  return 0;
}
```

```bash
mettle --build hello.mettle
```

## Variables

```mettle
var count: int64 = 0;
var scratch: uint8[256];
const LIMIT: int32 = 512;
```

Every one carries a type. A local with no initializer starts zeroed. At file
scope, `const MAX = 100;` may leave the type off when it holds an integer
literal.

## Interpolation

```mettle
var n: int32 = 7;
var who: string = "world";
println("hello {who}, n={n}, twice={n * 2}, brace={{");
```

```text
hello world, n=7, twice=14, brace={
```

Any expression fits in the braces. Every string literal is scanned, so there is
one print function.

## Arrays and slices

```mettle
var xs: int32[3] = [10, 20, 30];
var total: int32 = 0;
for i in 0..3 {
  total = total + xs[i];
}
```

`T[]` is a pointer and a length in one value. A `T[N]` converts to it, so a
function is written once for any extent:

```mettle
fn sum(values: int32[]) -> int32 {
  var total: int32 = 0;
  for v in values { total = total + v; }
  return total;
}
```

```mettle
var total: int32 = sum(xs);
```

## Structs and methods

```mettle
struct Point { x: int32; y: int32; }

fn Point_sum(p: Point) -> int32 { return p.x + p.y; }
fn Point_scale__ptr(p: Point*, k: int32) { p->x = p->x * k; p->y = p->y * k; }
```

```mettle
var p: Point = { x: 3, y: 4 };
println("{p.sum()}");

var q: Point* = &p;
q->scale(2);
```

A method taking the value is `Type_name`. One taking a pointer ends `__ptr` and
is called through `->`.

## Enums

```mettle
enum Color { Red = 1, Green = 2, Blue = 3 }
```

```mettle
var c: Color = Color.Green;
switch ((int32)c) {
  case 1: { println("red"); }
  case 2: { println("green"); }
  default: { println("other"); }
}
```

A case ends where the next one begins. An empty case body continues into the
next case, and `fallthrough;` makes a case with a body do the same.

## Tagged enums and match

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

One payload per variant. Every variant needs a case, or a `default`.

## Result and Option

```mettle
import "std/core";

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

## Loops

```mettle
for i in 0..n { }
for i: int64 in 0..n { }
for c in s { }
while (i < n) { i = i + 1; }

outer: for i in 0..3 {
  for j in 0..3 {
    if (j == 2) { continue outer; }
    if (i == 2) { break outer; }
  }
}
```

## Closures

```mettle
fn apply(f: Fn(int32) -> int32, v: int32) -> int32 { return f(v); }
```

```mettle
var k: int32 = 10;
var add: Fn(int32) -> int32 = fn(x: int32) -> int32 { return x + k; };
println("{apply(add, 5)}");
```

`Fn(...)` captures by value. `fn(...)` is a bare function pointer. A named
function reaches a `Fn` through its address, `&twice`.

## defer

```mettle
var p: int32* = malloc(64);
defer free(p);
```

Runs on every path out of the scope, in reverse order of declaration.
`errdefer` runs only when the function returns an `Err`.

## Heap

```mettle
import "std/io";
import "std/mem";

fn main() -> int32 {
  var xs: int32* = malloc(4 * 8);
  if (xs == 0) { return 1; }
  defer free(xs);
  for i in 0..8 { xs[i] = i * i; }
  println("{xs[7]}");
  return 0;
}
```

```text
49
```

`new T` allocates one zeroed `T`. A null result is `0`.

`new T[n]` allocates `n` of them and answers a `T[]`, so the length travels
with the pointer:

```mettle
var xs: int32[] = new int32[count];
for i in 0..xs.length { xs[i] = (int32)i; }
free(xs.data);
```

## Strings

```mettle
import "std/conv";
```

```mettle
var s: string = "  hello world  ";
var t: string = str_trim(s);
println("[{t}] starts={str_starts_with(t, "hello")}");

match (str_find(t, "world")) {
  case Some(i): { println("at {i}"); }
  case None: { println("none"); }
}

match (str_to_i64("1234")) {
  case Ok(v): { println("n={v}"); }
  case Err(e): { println("bad {e}"); }
}
```

```text
[hello world] starts=1
at 6
n=1234
```

`s.length` counts bytes, `s[i]` reads one as a `char`, `s.chars` is the
`uint8*` behind it.

## Building a string

```mettle
import "std/arena";
import "std/strbuf";
```

```mettle
var a: Arena* = arena_init(4096);
defer arena_free(a);

var b: StrBuf* = strbuf_new(a, 64);
strbuf_append_string(b, "count=");
strbuf_append_int(b, 42);
strbuf_append_byte(b, 10);
print_cstr(strbuf_finish_cstr(b));
```

```text
count=42
```

## Reading a file

```mettle
var fp: cstring = fopen("input.txt", "r");
if (fp == 0) { println("cannot open"); return 1; }
defer fclose(fp);

var buf: uint8[256];
var line: string = read_line((cstring)&buf[0], 256, fp);
while (line.length > 0) {
  println("got: {line}");
  line = read_line((cstring)&buf[0], 256, fp);
}
```

## Reading stdin

```mettle
var buf: uint8[128];
var line: string = read_line_stdin((cstring)&buf[0], 128);
println("read: {line}");
```

## Command-line arguments

```mettle
import "std/io";

fn main(argc: int32, argv: cstring*) -> int32 {
  println("argc={argc}");
  return 0;
}
```

## Calling C, and being called

```mettle
extern fn strlen(s: cstring) -> int64;

export fn add_two(a: int32, b: int32) -> int32 { return a + b; }
```

```mettle
println("{strlen("hello")} {add_two(20, 22)}");
```

```text
5 42
```

A string literal is already nul-terminated. For a run-time string, copy it with
`cstr(name, &malloc)` and free the copy.

## Asking the kernel

```mettle
var written: int64 = syscall(1, 1, &message[0], 12);   // Linux write(2)
```

The number first, then the arguments. Six of them on Linux, fifteen on Windows,
where the number itself is read out of the `ntdll` stub because it moves between
builds. [C interoperability](c-interop.md) has both conventions.

## Imports

```mettle
import "std/io";
import "lib/helper" as h;
import { twice } from "lib/helper";
import "std/net" if windows;

var page: string = import_str "template.html";
```

## Generics

```mettle
fn id<T>(x: T) -> T { return x; }

trait Addable;
impl Addable for int32;
fn add_one<T: Addable>(v: T) -> T { return v + 1; }
```

```mettle
var n: int32 = id(7);
var wide: int64 = id<int64>(7);
var m: int32 = add_one(41);
```

The call site may name the type arguments, and does not have to when the
arguments already say what they are.

## Gathered parameters

```mettle
fn sum(xs: int32[..]) -> int32 {
  var total: int32 = 0;
  for x in xs { total = total + x; }
  return total;
}
```

```mettle
var a: int32 = sum(1, 2, 3);
var b: int32 = sum();
```

The last parameter takes whatever follows the fixed ones, as a `T[]`.

## Compile-time tests

```mettle
@test fn adds() -> int32 {
  if (1 + 1 != 2) { return 1; }
  return 0;
}
```

```bash
mettle test program.mettle
```

## Reflection

```mettle
comptime for f in typeof(Point).fields {
  println("field {f.name} at {f.offset}");
}
```

`Field` has `.name`, `.type`, `.offset`, and `.index`.

## Rewrite rules

```mettle
rewrite rem_pow2(x: uint64, m: uint64) -> uint64 {
  from x % m;
  to x & (m - 1);
  where m > 0 && (m & (m - 1)) == 0;
}
```

The compiler runs both sides on generated inputs before using the rule, and
refuses a rule whose sides disagree, naming the input. `--explain` reports
where each rule fired.

## Common flags

```bash
mettle --build app.mettle              # executable
mettle --release --build app.mettle    # optimized
mettle --safe --build app.mettle       # bounds-checked
mettle --release --explain app.mettle  # what the optimizer did
mettle test app.mettle                 # run @test functions
mettle trace app.mettle total 4        # trace one function
mettle explain M0110                   # explain a diagnostic
```

## See also

- [Getting started](getting-started.md)
- [Standard library](standard-library.md)
- [Compilation](compilation.md)
