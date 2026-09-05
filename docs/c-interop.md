# C interoperability

Mettle calls symbols that follow the target's native ABI, and C calls back
into Mettle. This page covers the declarations, the types that cross the
boundary, the struct rules, and linking.

There is no C runtime underneath. A Mettle program's `malloc` and `puts` come
from Mettle's own runtime, and a link argument naming a C or compiler runtime
fails. Reach the operating system through `std/win32`, `std/thread`, and
`std/net`.

## Calling out

Declare the function with `extern fn`. Add `= "symbol"` when the link name
differs from the Mettle name.

```mettle
extern fn puts(msg: cstring) -> int32 = "puts";
extern fn malloc(size: int64) -> rawptr = "malloc";
```

```mettle
puts("Hello");
var p: int32* = malloc(100);
```

Parameter and return types must match what the other side declares. The
convention comes from the target: the Microsoft ABI on Windows x86-64, System
V on Linux x86-64, AAPCS64 on Linux AArch64.

## What a name can resolve to

`extern` declares a name; it does not conjure one. Nothing checks at compile
time that the symbol exists, so a name nothing provides compiles cleanly and
fails at the link:

```text
Error: Unresolved external symbol 'sqrt' (referenced by 'app.obj')
help: `extern` declares a name; it does not provide one. Nothing on the link provided 'sqrt'.
help: provide it with `--link-arg your.obj`, `--link-arg -lname` or an import library path; check the `= "symbol"` link name and spelling.
help: on Windows the internal linker searches the owned runtime plus kernel32, user32, gdi32, advapi32, ws2_32, winmm (UCRT/MSVCRT are excluded); on Linux add `-l` for the library that defines it. See docs/c-interop.md.
help: 'sqrt' is double-precision math; prefer import "std/math" (on Windows the owned runtime carries only the float32 'sqrtf', and UCRT/MSVCRT are excluded).
```

```text
(.text.main+0x18): undefined reference to `sqrt'
collect2: error: ld returned 1 exit status
```

Two things can satisfy a name on Windows:

- **The owned runtime.** The libc subset Mettle implements itself: `malloc`,
  `free`, `printf`, `fopen`, `fwrite`, `memcpy`, `strlen`, `atoi`, `strtod`,
  `qsort`, `rand`, `srand`, `abort`, `exit`, and around two hundred more.
- **The Win32 DLL probe.** `kernel32`, `user32`, `gdi32`, `advapi32`, `ws2_32`.
  This is where `GetStdHandle`, `ReadFile` and `CreateWindowExA` come from.
  **UCRT and MSVCRT are not in the set**, so a C-library name that the owned
  runtime does not carry has nowhere left to come from.

On Linux the owned runtime is what a bare build resolves against, plus the
syscall, socket and pthread entries the Linux build of it adds. Naming a shared
library with `-l` adds its dynamic symbols to that set; see
[Shared libraries](shared-libraries.md).

To see the list for a target, read the symbols out of the runtime objects the
build stages beside the compiler:

```bash
nm -g --defined-only bin/runtime/*.o | awk '$2=="T" {print $3}' | sort -u
```

### The edge that catches people

**The float32 math functions are there and the float64 ones are not.**
`sqrtf`, `sinf`, `cosf`, `powf`, `expf`, `logf` and `tanhf` all resolve,
because code generation itself needs them. `sqrt`, `sin`, `cos`, `pow`, `log`
and `exp` do not resolve at all.

Do not reach for them through `extern`. Double-precision math is
[`std/math`](standard-library.md), written in Mettle, so it has nothing to
resolve and behaves the same on every target:

```mettle
import "std/math";

var r: float64 = sqrt(16.0);      // 4.0
var p: float64 = pow(2.0, 10.0);  // 1024.0
```

`std/math` exports its constants as functions - `PI()`, `TAU()`, `E()` and
nine more - so they occupy those names. Declaring a `var TAU` of your own
beside the import collides with one.

### Where to go instead

| Wanted | Not this | This |
|---|---|---|
| Double-precision math | `extern fn sqrt(...) = "sqrt"` | `import "std/math"` |
| Wall clock, an RNG seed | `extern fn time(...) = "time"` | `bench_time_us()` from `std/bench` |
| Win32 calls | a raw `extern` per entry | `import "std/win32"` |
| Sockets | `extern` per platform | `import "std/net"` |
| `rand`, `srand`, `exit` | — | `import "std/process"` (these *are* owned) |

`time` is the one that bites, because seeding an RNG from it is such a common
habit. It is a C-library name, not a Win32 one, so on Windows the DLL probe
never sees it and MSVCRT is excluded; on Linux nothing provides it either.
`clock` and `gettimeofday` *are* owned on both, if you want them raw.

Linking a C runtime to fill the gap is not the way out. The build refuses
the argument outright:

```text
Error: --link-arg '-lmsvcrt' names a forbidden C or compiler runtime
```

## Calling in

Mark a Mettle function `export` and C can call it by name:

```mettle
export fn add_two(a: int32, b: int32) -> int32 {
  return a + b;
}
```

```c
extern int32_t add_two(int32_t a, int32_t b);
```

Calls between two Mettle functions use Mettle's own convention. Both sides
agree, so it makes no difference to a Mettle program, and it is why the
platform rule is applied to the functions reachable from outside: `extern`
callees, `export`ed functions, and `main`.

## cstring and rawptr

`cstring` is `uint8*`: a pointer to bytes that a C function reads up to a nul.
Use it for C's `char*`. `cstring` and `uint8*` are interchangeable.

`rawptr` is an address with no element type, C's `void*`, and what an
allocator hands out. It converts to and from every pointer type in both
directions, so `var p: int32* = malloc(n);` and `free(p)` need no cast. It
cannot be indexed, dereferenced, or offset, because it names no element.

## Passing a string to C

A [`string`](types.md) is a pointer and a length, with no terminator.
Termination is a property of this boundary.

A string literal is already terminated in read-only memory, so it flows
straight into a `cstring` parameter and allocates nothing:

```mettle
var fp: cstring = fopen("data.txt", "rb");
```

Anything built at run time needs a terminated copy. `cstr` from
[`std/io`](standard-library.md) makes one, and it takes the allocator to make
it from, so the cost sits in the signature:

```mettle
var path: cstring = cstr(name, &malloc);
defer free(path);
var fp: cstring = fopen(path, "rb");
```

`cstr` returns 0 when the allocator does.

For a C function that takes a pointer and a length, pass `s.chars` and
`s.length` and copy nothing.

## Structs by value

Define the struct to match the C layout: same field order, same types. Fields
are laid out in declaration order, each on its own alignment, with the whole
struct padded to its widest member.

```mettle
struct SockAddrIn {
  sin_family: int16;
  sin_port: uint16;
  sin_addr: uint32;
  sin_zero: uint8[8];
}
```

On Windows, Mettle follows the Microsoft x64 aggregate rule:

- A struct of exactly 1, 2, 4, or 8 bytes passes and returns in one integer
  register.
- Every other size passes indirectly, by pointer.
- An indirect return uses a hidden first argument in RCX, and the callee
  returns that pointer in RAX.

On Linux, Mettle follows System V, which cuts the struct into eight-byte
chunks and classifies each:

- 16 bytes or less passes in registers, one per eightbyte. A chunk holding
  integers or pointers takes a general register; a chunk holding only floats
  takes an XMM. So `{int64, double}` arrives as one of each, and
  `{double, double}` as two XMMs.
- Anything larger is MEMORY: the caller copies the bytes into the outgoing
  stack area.
- A struct of 16 bytes or less returns the same way, in RAX and RDX or XMM0
  and XMM1, with no hidden pointer.

Both rules work in both directions: Mettle calling a C function that takes or
returns a struct by value, and C calling an exported Mettle function that
does.

When the C API wants a pointer to a struct, pass `&my_struct` or a `T*`.

## Win32

Import [`std/win32`](standard-library.md) rather than repeating raw `extern`
declarations:

```mettle
import "std/win32";
```

```mettle
win32_write_stdout("hello\n", 6);
win32_sleep_ms(10);
```

The internal linker probes the common Windows DLLs directly, so an ordinary
build needs no import libraries:

```bash
mettle --build main.mettle -o main.exe
```

The default import set is `kernel32`, `user32`, `gdi32`, `advapi32`, and
`ws2_32`. UCRT and MSVCRT are excluded. For another DLL, pass
`--link-arg -lname` or an import library path. Raw COFF objects go through
`--link-arg` too, and the PE import audit still runs afterwards.

## Linux

A Linux build is freestanding by default: no libc on the link line, and the
ownership audit refuses a `PT_INTERP`. Static archives work against the owned
subset.

`-lname` binds a shared library instead, and the audit then allows the dynamic
segments that link asked for. `sqrt` through `-lm` and `getpid` through `-lc`
both resolve this way, versioned symbols included:

```mettle
extern fn getpid() -> int32 = "getpid";
```

```bash
mettle --build app.mettle -o app -lc
```

[Shared libraries](shared-libraries.md) has the options, what the linker emits,
and what it refuses.

For sockets, `std/net` covers both platforms from one source. `std/net_posix`
is the older Linux-only path. Its socket, error, atomic, and yield names come
from Mettle's own syscall runtime, so no helper C source and no pthread flag
is needed.

## System calls

`syscall` asks the kernel directly. It is a built-in, not a function: the
compiler writes the machine's system-call instruction where the call appears,
so there is no stub to link against and nothing to resolve.

```mettle
var written: int64 = syscall(1, 1, &message[0], 12);   // Linux write(2)
```

The first operand is the system-call number and the rest are its arguments. An
argument is an integer, a boolean, a character, or a pointer - whatever fits
one register. A float or a struct is refused rather than reinterpreted, and a
`string` is a two-word record, so pass `&text[0]` or a `cstring`.

The result is the whole register the kernel returns, which is a count or a
negative errno on Linux and an NTSTATUS on Windows.

| Target | Number | Arguments | Instruction |
| --- | --- | --- | --- |
| x86-64 Linux | RAX | RDI, RSI, RDX, R10, R8, R9 | `syscall` |
| x86-64 Windows | EAX | R10, RDX, R8, R9, then `[rsp+0x28]` onward | `syscall` |
| AArch64 Linux | X8 | X0..X5 | `svc #0` |

Linux fills its six argument registers and has nowhere to put a seventh, so
seven arguments is a compile error there. Windows reads the rest off the stack
and accepts up to fifteen. Both numbers come from the selected target, so
`--target x86_64-linux` is checked against Linux's limit whatever host you are
compiling on.

The instruction clobbers RCX and R11 on x86-64. Both are caller-saved under
either convention, so nothing you were holding is lost.

### On Windows, read the number rather than writing it

NT system-call numbers change between Windows builds and are not an interface
Microsoft keeps. The number every version agrees on is the one inside the
`ntdll` stub that would otherwise issue the call, which opens with
`mov r10, rcx` (`4C 8B D1`) and then `mov eax, <number>` (`B8` and four bytes):

```mettle
extern fn GetModuleHandleA(name: cstring) -> rawptr = "GetModuleHandleA";
extern fn GetProcAddress(module: rawptr, name: cstring) -> rawptr = "GetProcAddress";

fn ntdll_number(name: cstring) -> int64 {
    var stub: rawptr = GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
    var code: uint8* = (uint8*)stub;
    var number: int64 = (int64)code[4];
    number = number | ((int64)code[5] << 8);
    number = number | ((int64)code[6] << 16);
    number = number | ((int64)code[7] << 24);
    return number;
}
```

Going around `ntdll` also goes around everything it does on the way through, so
prefer the documented Win32 entry points above for ordinary work. A raw system
call is for the cases that have no documented entry point.

### Where it is refused

A GPU kernel has no operating system to ask, and a 16- or 32-bit target has no
instruction the built-in knows how to write; both are compile errors. The
compile-time interpreter refuses it too, so a `@test` calling one is reported
as skipped rather than passing on a number nothing performed. Run such code
natively through `main` and `--build`.

## See also

- [Runtime model](runtime-model.md)
- [Shared libraries](shared-libraries.md)
- [Linker and build pipelines](linker-build-pipelines.md)
- [Types](types.md)
