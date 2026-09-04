# Standard library

The modules under `stdlib/std`. Import one by path, `import "std/io";`, or
pass [`--prelude`](compilation.md) to get the common ones without asking.

Signatures below are copied from the source. A module lists only what it
exports; a module with no `export` at all is entirely public.

Several modules ship a `.linux.mettle` sibling. A `std/` import with no
extension picks the platform half on its own, so `import "std/io"` is right on
both systems.

## std/core

`Result` and `Option`, the two tagged enums the rest of the library returns.

```mettle
export enum Result<T, E> { Ok(T), Err(E) }
export enum Option<T> { Some(T), None }
```

`Result` is for a call that could not do what it was asked, and the error arm
carries why. `Option` is for a value that may not be there. Read either with
[`match`](control-flow.md).

## std/io

Console and file input and output. Both platforms.

| Function | Effect |
|----------|--------|
| `print(msg: string)` | Write to stdout |
| `println(msg: string)` | Write to stdout with a newline |
| `print_err(msg: string)` | Write to stderr |
| `println_err(msg: string)` | Write to stderr with a newline |
| `newline()` | Write one newline |
| `print_cstr(s: cstring)` | Write nul-terminated bytes |
| `println_cstr(s: cstring)` | The same with a newline |

Values reach the output through [interpolation](expressions.md), so there is
one print function and it takes a `string`:

```mettle
var n: int32 = 42;
println("answer is {n}");
```

Files come through the C standard library, declared here so you can call them
directly:

| Function | Effect |
|----------|--------|
| `fopen(filename: cstring, mode: cstring) -> cstring` | Open a file |
| `fclose(fp: cstring) -> int32` | Close it |
| `fread(buf: cstring, size: int64, count: int64, fp: cstring) -> int64` | Read |
| `fwrite(buf: cstring, size: int64, count: int64, fp: cstring) -> int64` | Write |
| `fgets(buf: cstring, size: int32, fp: cstring) -> cstring` | Read a line |
| `get_stdin()`, `get_stdout()`, `get_stderr()` | The standard streams |

Two wrappers make line reading pleasant, returning a `string` view of the
buffer you hand them:

| Function | Effect |
|----------|--------|
| `read_line(buf: cstring, cap: int32, fp: cstring) -> string` | One line from a file |
| `read_line_stdin(buf: cstring, cap: int32) -> string` | One line from stdin |

`cstr(s: string, alloc: fn(int64) -> rawptr) -> cstring` copies a `string`
into nul-terminated memory from the allocator you pass, for handing to C.

## std/conv

Character tests, string slicing and searching, and number conversion. Both
platforms.

Character tests take and return [`char`](types.md):

```text
is_digit  is_upper  is_lower  is_alpha  is_alnum  is_space
to_lower  to_upper  digit_to_char  char_to_digit
```

Building a `string`:

| Function | Effect |
|----------|--------|
| `str_from_bytes(p: cstring, n: int64) -> string` | View n bytes |
| `str_from_cstr(s: cstring) -> string` | View up to the nul |
| `str_slice(s: string, start: int64, len: int64) -> string` | A sub-view |

Searching and trimming:

| Function | Effect |
|----------|--------|
| `str_find(s: string, needle: string) -> Option<int64>` | Byte offset of the first match |
| `str_find_byte(s: string, c: int32) -> Option<int64>` | Offset of a byte |
| `str_contains(s: string, needle: string) -> int32` | 1 when present |
| `str_starts_with(s: string, prefix: string) -> int32` | 1 when it does |
| `str_ends_with(s: string, suffix: string) -> int32` | 1 when it does |
| `str_eq_at(s: string, offset: int64, needle: string) -> int32` | Compare at an offset |
| `str_trim(s: string) -> string` | Both ends |
| `str_trim_start(s: string) -> string` | Leading space |
| `str_trim_end(s: string) -> string` | Trailing space |
| `str_split_once(s: string, sep: string) -> Option<StrSplit>` | First split |

`StrSplit` is a struct holding the two halves.

Numbers:

| Function | Effect |
|----------|--------|
| `str_to_i64(s: string) -> Result<int64, string>` | Parse, with a reason on failure |
| `str_to_i64_or(s: string, fallback: int64) -> int64` | Parse or fall back |
| `i64_to_str(n: int64, buf: uint8*, buf_len: int64) -> string` | Format into your buffer |
| `int_to_dec(buf: uint8*, n: int64) -> int32` | Digits written |
| `format_i64(buf: uint8*, buf_len: int32, pattern: string, value: int64) -> int32` | Pattern format |

`cstr_len`, `streq`, and `cstr_ncmp` work on `cstring` for the C boundary.

## std/math

Floating-point maths in Mettle source, no libm on the link line. Both
platforms.

Constants are functions, so `PI()` and `E()`:

```text
PI  TAU  HALF_PI  QUARTER_PI  E  SQRT2  SQRT1_2
LN2  LN10  LOG2E  LOG10E  LOG10_2  INF  NAN
EPSILON  F32_EPSILON  MAX_FINITE  MIN_POSITIVE
DEG_PER_RAD  RAD_PER_DEG
```

Classification: `is_nan`, `is_inf`, `is_finite`, `signbit`.

Sign and range: `fabs`, `copysign`, `fsign`, `fmin`, `fmax`, `fclamp`,
`saturate`.

Rounding: `trunc`, `floor`, `ceil`, `round`, `fract`, `fmod`.

Float to integer with the mode named, rather than the one a cast picks:
`floor_i32`, `ceil_i32`, `trunc_i32`, `round_i32`, and their `_i64` siblings.
All are `@inline`.

Powers and roots: `sqrt`, `rsqrt`, `hypot`, `cbrt`, `pow`, `ldexp`, `frexp`.

Exponential and logarithm: `exp`, `exp2`, `expm1`, `log`, `log2`, `log10`,
`log1p`.

Trigonometry: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`.

Bit access: `f64_bits(x) -> int64`, `f64_from_bits(b) -> float64`,
`f64_raw_exponent(x) -> int32`.

Every one takes and returns `float64`.

## std/mem

Raw memory moves. Both platforms.

| Function | Effect |
|----------|--------|
| `mem_zero(p: cstring, n: int64) -> cstring` | Fill with zero |
| `mem_fill(p: cstring, value: int32, n: int64) -> cstring` | Fill with a byte |
| `mem_copy(dst: cstring, src: cstring, n: int64) -> cstring` | Copy, no overlap |
| `mem_move(dst: cstring, src: cstring, n: int64) -> cstring` | Copy, overlap allowed |
| `mem_equal(a: cstring, b: cstring, n: int64) -> int32` | 1 when equal |
| `mem_compare(a: cstring, b: cstring, n: int64) -> int32` | Ordering |
| `mem_find_byte(p: cstring, value: int32, n: int64) -> int64` | Offset or -1 |
| `alloc_zeroed(n: int64) -> rawptr` | Allocate and zero |
| `buf_dup(src: rawptr, len: int64) -> rawptr` | Allocate a copy |

## std/alloc

Mettle's own thread-safe heap, written in Mettle. Both platforms.
[Heap allocation](heap-allocation.md) covers when to reach for it.

The short names allocate from a shared default heap:

| Function | Effect |
|----------|--------|
| `mx_alloc(n: int64) -> rawptr` | Allocate |
| `mx_calloc(count: int64, size: int64) -> rawptr` | Allocate and zero |
| `mx_realloc(p: rawptr, newsize: int64) -> rawptr` | Resize |
| `mx_free(p: rawptr)` | Release |
| `mx_stats() -> MemStats` | Counters |

For a heap of your own:

| Function | Effect |
|----------|--------|
| `mem_heap_create() -> MemHeap*` | New heap |
| `mem_alloc(h: MemHeap*, n: int64) -> rawptr` | Allocate from it |
| `mem_calloc(h: MemHeap*, count: int64, size: int64) -> rawptr` | Allocate and zero |
| `mem_realloc(h: MemHeap*, p: rawptr, newsize: int64) -> rawptr` | Resize |
| `mem_free(h: MemHeap*, p: rawptr)` | Release |
| `mem_heap_stats(h: MemHeap*) -> MemStats` | Counters |
| `mem_heap_destroy(h: MemHeap*)` | Tear down |
| `mem_default_heap() -> MemHeap*` | The heap the `mx_` names use |

The `mettle_heap_*` names are the hooks
[`--native-heap`](heap-allocation.md) redirects the language's own allocation
through.

A null result is the integer `0`:

```mettle
var buf: rawptr = mx_alloc(1024);
if (buf == 0) { return 1; }
```

## std/arena

Bump allocation with a whole-region reset. Both platforms. Good for work with
one clear end, a request, a frame, a parse.

| Function | Effect |
|----------|--------|
| `arena_init(default_chunk_size: int64) -> Arena*` | New arena |
| `arena_alloc(a: Arena*, n: int64) -> rawptr` | Bump |
| `arena_alloc_zeroed(a: Arena*, n: int64) -> rawptr` | Bump and zero |
| `arena_alloc_aligned(a: Arena*, n: int64, align: int64) -> rawptr` | Bump to an alignment |
| `arena_save(a: Arena*) -> ArenaSave` | Mark a point |
| `arena_restore(a: Arena*, save: ArenaSave)` | Roll back to it |
| `arena_reset(a: Arena*)` | Release everything, keep the chunks |
| `arena_free(a: Arena*)` | Release everything |
| `arena_stats(a: Arena*) -> ArenaStats` | Counters |

## std/strbuf

A growable byte buffer over an [arena](#stdarena). Both platforms.

| Function | Effect |
|----------|--------|
| `strbuf_new(arena: Arena*, initial_cap: int64) -> StrBuf*` | New buffer |
| `strbuf_len(b: StrBuf*) -> int64` | Bytes written |
| `strbuf_append_byte(b: StrBuf*, c: uint8) -> int32` | One byte |
| `strbuf_append_bytes(b: StrBuf*, src: cstring, n: int64) -> int32` | n bytes |
| `strbuf_append_cstr(b: StrBuf*, s: cstring) -> int32` | Up to the nul |
| `strbuf_append_string(b: StrBuf*, s: string) -> int32` | A string view |
| `strbuf_append_int(b: StrBuf*, n: int64) -> int32` | Signed decimal |
| `strbuf_append_uint(b: StrBuf*, n: uint64) -> int32` | Unsigned decimal |
| `strbuf_append_hex(b: StrBuf*, n: uint64) -> int32` | Hexadecimal |
| `strbuf_finish_cstr(b: StrBuf*) -> cstring` | Terminate and hand back |

## std/utf8

Code points over the byte view a [`string`](types.md) gives you. Both
platforms.

| Function | Effect |
|----------|--------|
| `utf8_at(s: string, i: int64) -> int32` | Code point at a byte offset |
| `utf8_span(s: string, i: int64) -> int64` | Bytes in the character at i |
| `utf8_count(s: string) -> int64` | Characters, not bytes |
| `utf8_offset(s: string, index: int64) -> int64` | Byte offset of character n |
| `utf8_valid(s: string) -> bool` | Well-formed |
| `utf8_len(cp: int32) -> int64` | Bytes a code point needs |
| `utf8_encode(buf: cstring, cp: int32) -> int64` | Write one, return the length |
| `utf8_string(buf: cstring, cp: int32) -> string` | Write one, return a view |

## std/osmem

Pages from the operating system, under the heap. Both platforms, with a
`.linux` half.

| Function | Effect |
|----------|--------|
| `os_page_size() -> int64` | Page size |
| `os_mem_map(n: int64) -> cstring` | Map n bytes |
| `os_mem_unmap(p: cstring, n: int64)` | Unmap |
| `os_mem_protect_noaccess(p: cstring, n: int64)` | Make a range unreadable |

## std/thread

Threads, mutexes, and atomics. Both platforms, with a `.linux` half and a
`thread_posix` variant.

Threads: `thread_join(handle: int64, timeout_ms: uint32) -> uint32`,
`thread_join_infinite`, `thread_detach`, `thread_close`,
`thread_sleep_ms(milliseconds: uint32)`.

Mutexes: `mutex_create`, `mutex_create_owned`, `mutex_lock(mutex, timeout_ms)`,
`mutex_lock_infinite`, `mutex_unlock`, `mutex_close`.

Atomics on an `int32*`: `atomic_compare_exchange_i32`, `atomic_exchange_i32`,
`atomic_inc_i32`, `atomic_dec_i32`.

Spin locks on an `int32*`: `spin_try_lock`, `spin_lock`, `spin_unlock`.

Wait results come back as `WAIT_OBJECT_0()`, `WAIT_TIMEOUT()`, or
`WAIT_FAILED()`, and `INFINITE()` is the timeout that never expires.

## std/machine

One type, `MachineInsn`, and a `const` of it is an instruction set: `name`,
`encoding`, `operands`, `reads`, `writes` and `semantics` per row. The
compiler reads the const, refuses one that cannot be decoded back,
assembles `const PROGRAM: string[N]` into that machine's bytes, and runs the
result by calling each instruction's semantics function in the compile-time
interpreter. `mettle machine <file>` prints the description and
`mettle emulate <file>` runs it. [Bare metal](bare-metal.md) covers it, and
[`examples/machine/`](../examples/machine/) is a worked one.

## std/schedule

One type, `Schedule`, and a `const` of it is a frame written down: `phase`,
`effect`, `entry` and `thread` per row. The compiler reads the const, checks
it, and generates one wrapper per phase and one dispatcher per thread with a
`quiesce` at every phase boundary. Because a wrapper provides only its own
phase's effect, a call across a phase boundary is refused by the effect pass.
[The runtime model](runtime-model.md) covers it, and `mettle expand` prints
what was generated.

## std/rule

The records a `@rule fn` reads and returns: `Program`, `Function`, `TypeInfo`,
`FieldInfo`, `EffectInfo`, `Site` and `Verdict`, with `verdict_pass()`,
`verdict_fail(site, message)` and `verdict_gap(site, message)`. A `Function`
carries its inferred `effects` and `requires` and its declared `forbids` and
`provides`; `function_performs`, `function_requires`, `function_forbids`,
`function_provides` and `program_effect_index` read them.

Verdicts about the program as a whole, for a complaint no line owns:
`verdict_fail_program(message)` and `verdict_gap_program(message)`.

Queries over a program: `function_calls(f, callee) -> bool`,
`function_matches(f, owner, variant) -> bool`,
`program_function_index(p, qualified) -> int64`,
`program_type_index(p, qualified) -> int64`, and
`function_reaches(p, f, callee) -> bool`, which follows direct calls
transitively. [Rules](rules.md) covers what a rule is and what it sees.

## std/target

`TargetDesc`, the record a target description is written as: `name`, `arch`,
`os`, `format`, `pointer_bits`, `stack_alignment`, `shadow_space`,
`red_zone`, `int_args`, `float_args`, `indirect_return`, `separate_classes`,
`widths`, `vector_width` and `address_spaces`. `mettle target <triple>`
prints one for every built-in target and `--target desc.mettle` reads one
back. [Bare metal](bare-metal.md) covers what a description may change.

## std/net

TCP and UDP sockets. Windows, over Winsock. On Linux import
`std/net_posix` instead; `std/prelude` leaves both out for that reason.

Setup: `net_init() -> Result<int32, int32>`, `net_cleanup()`,
`net_is_initialized()`, `net_last_error()`.

Sockets: `socket_tcp() -> Result<int64, int32>`,
`socket_udp() -> Result<int64, int32>`.

Addresses: `sockaddr_in(ip: cstring, port: int32) -> Result<cstring, int32>`,
`sockaddr_in_any(port: int32) -> Result<cstring, int32>`.

Options: `set_reuseaddr`, `set_nonblocking`, `set_nodelay`.

Transfer: `send_all(sock: int64, buf: cstring, len: int32) -> Result<int32, int32>`.

The Winsock constants are functions: `AF_INET()`, `SOCK_STREAM()`,
`SOCK_DGRAM()`, `IPPROTO_TCP()`, `IPPROTO_UDP()`, `SOL_SOCKET()`,
`SO_REUSEADDR()`, `SD_RECEIVE()`, `SD_SEND()`, `SD_BOTH()`, `INADDR_ANY()`,
`INVALID_SOCKET()`, `SOCKET_ERROR()`, `FIONBIO()`, `TCP_NODELAY()`.

## std/http

`http_fetch_to_file(url: cstring, output_path: cstring) -> int32` downloads a
URL to a file.

## std/dir

Paths and directories. Windows.

| Function | Effect |
|----------|--------|
| `dir_exists(path: cstring) -> int32` | 1 when it does |
| `dir_create(path: cstring) -> int32` | Create one |
| `file_exists(path: cstring) -> int32` | 1 when it does |
| `getcwd(buf: cstring, size: int32) -> int32` | Working directory |
| `dir_list_md_files(root_dir, paths_buf, paths_size, max_files) -> int32` | List `.md` files under a root |

## std/process

Three C entry points: `exit(code: int32)`, `rand() -> int32`, and
`srand(seed: int32)`.

## std/system

`system(cmd: cstring) -> int32` runs a shell command.

## std/bench

`bench_time_us() -> int64` reads a microsecond clock. Both platforms, with a
`.linux` half.

## std/gpu

Device memory, streams, and launches for the [GPU targets](gpu.md).

## std/tracy

Zones and frame marks for the Tracy profiler. Needs
[`--tracy`](compilation.md) at build time.

## std/ui

Win32 window and message-loop helpers. Windows.

## std/win32

Raw Win32 declarations the other Windows modules build on.

## std/prelude

Imports `std/core`, `std/io`, `std/math`, `std/conv`, `std/mem`, and
`std/process`, and re-exports them. [`--prelude`](compilation.md) imports it
for you. Networking stays out, because it is platform-split.

## See also

- [Heap allocation](heap-allocation.md)
- [C interoperability](c-interop.md)
- [Modules and imports](modules.md)
