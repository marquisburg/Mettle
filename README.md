<div align="center">

<picture>
  <source
    media="(prefers-color-scheme: dark)"
    srcset="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/docs/assets/mettle-dark.svg"
    width="120" height="120" />
  <img
    src="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/docs/assets/mettle-light.svg"
    alt="Mettle" width="120" height="120" />
</picture>

# Mettle

Mettle is a systems language where the toolchain is part of the contract.

Require what the compiler must do. Read what it did. Check that it kept your
program's meaning.

Native x86-64, ARM64 and GPU code generation, with its own optimizer, linker,
runtime and debugger. No LLVM, no VM, no GC.

</div>

## Example

```mettle
import "std/io";
import "std/mem";

@simd! fn checksum(data: uint8*, n: int64) -> int64 {
  var sum: int64 = 0;
  for i: int64 in 0..n {
    sum = sum + (int64)data[i];
  }
  return sum;
}

fn main() -> int32 {
  var n: int64 = 1024;
  var data: uint8* = malloc(n);
  for i: int64 in 0..n { data[i] = (uint8)(i & 255); }

  println("checksum = {checksum(data, n)}");
  free(data);
  return 0;
}
```

```bash
mettle --build --release checksum.mettle
./checksum          # on Windows, .\checksum.exe
```

You write the type on every `var`. Any expression fits between the braces of a
string. And `@simd!` is a demand: that loop vectorizes or the build stops.

Ask what the optimizer did with it.

```bash
mettle --build --release --explain checksum.mettle
```

```text
-- optimization report: checksum.mettle -------------------------------------
  1 missed optimization in this file, none with a fix the compiler can name

  checksum (loop @ line 6): vectorized -> vpsadbw, 32-wide byte sum (AVX2)
  main (loop @ line 15): NOT vectorized  [store-only-fill]
      15 | for i: int64 in 0..n { data[i] = (uint8)(i & 255); }
      \_ reason: the loop fills 1-byte elements, and the fill kernel covers 2-, 4- and 8-byte elements only
      \_ note: nothing to change here: this is a gap in the compiler, not a problem with the loop
  main (call to `println` @ line 17): inlined
  main (call to `checksum` @ line 17): inlined
```

It names the instruction it picked, and it names its own gaps.

Now free the buffer one line early.

```mettle
  free(data);
  println("checksum = {checksum(data, n)}");
```

```text
warning[M0101]: Use of `data` after it was freed (freed at line 17); this is use-after-free

   17 |    free(data);
   18 |    println("checksum = {checksum(data, n)}");
      :            ^
```

It found that by reading the program. You ran nothing and marked nothing.

## Install

Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

Both unpack to `~/.mettle` or `%LOCALAPPDATA%\Mettle` and add that to your PATH.
Neither needs root or admin.

## What it does

**Finds memory bugs while it compiles.** It reads the whole program and reports
use after free, double free, leaks, dangling returns, and pointers `realloc`
left stale. It follows a pointer across a task boundary too: handing a task a
pointer into the frame that spawned it stops the build, and so does writing
through a message the sender already handed over. You write no lifetimes and no
ownership marks. It infers them, and it reports only what it can prove. See
[the memory analyser](docs/borrow-checker.md).

**Checks the rest at run time, cheaply enough to ship.** `--safe` checks every
memory access at every optimization level, then proves away what it can: a
constant index, a counter its loop already bounds, an index its own arithmetic
bounds, one check that covers a whole loop. What survives compares against an
allocation the loop resolved once, and costs a few instructions. A vectorized
dot product pays nothing, a CRC 1.04x, a heapsort whose indices come out of
comparisons 2.5x. See [checked access](docs/memory-safety.md).

**Says what the optimizer did.** `--explain` prints what became of every loop
and every call, what stopped a loop from vectorizing, and what changed since
your last build. It simulates each fix it offers before printing it, so it only
prints fixes that worked. `--explain-json` feeds CI.

**Says what it took on trust.** The same report lists every type it proved,
every effect it inferred, every rule it ran, and, under **beliefs**, the
externs it believed because it could not see them. Each mechanism is on a
ledger and under a budget: `--report-proofs` with `--proof-budget=N`,
`--report-effects` with `--effect-budget=N`, `--report-rules` with
`--rule-budget=N`, `--report-expansion` with `--expansion-budget=N`. No
annotation is believed for speed: `@pure` is checked and never consumed, and a
call is hoisted only where purity was inferred.

**Fails the build when a promise breaks.** `@simd!` demands that a loop
vectorize, `@inline!` that every call site inline, `@noalloc` that a call graph
allocate nothing. When the compiler cannot deliver, it stops and names the site
that defeated it.

**Lets the program add its own promises.** A `@rule fn` is an ordinary
function the compiler runs while compiling, over the checked program as data,
and a failing verdict stops the build at the site it names: no function in
this module recurses, this struct stays under 64 bytes, every variant is
handled in this file. `type Percent = int32 where value >= 0 && value <=
100;` declares a type that carries a rule, and a value becomes one only where
the compiler proves it. `fn tick() where cycles < 400` says the same kind of
thing about a function's longest path, and the compiler costs that path from a
model of the target or stops the build. `effect Render;` declares what a function may do or
need: `fn mix() forbids Render`, `fn job() requires Worker`, inferred through
the whole call graph and refused with the chain that broke it. Saying where
code runs is enough to say which globals two threads share, so one written
from two threads with nothing ordering them is refused too. A frame is data:
a `const` of `std/schedule`'s `Schedule` names its phases, and the compiler
generates the dispatcher for each thread from it, with a `quiesce` at every
phase boundary and a call across one refused. A target is a
Mettle `const` the compiler reads, so `mettle target x86_64-none` prints one
and `--target mine.mettle` builds for it. [Rules](docs/rules.md),
[Effects](docs/effects.md), [Types](docs/types.md) and
[Bare metal](docs/bare-metal.md) cover them.

**Vectorizes for AVX2** across reductions, maps, dot products, byte kernels,
kernels over quantized integers, and some serial recurrences. It beats
`gcc -O3` on several kernels in the benchmark suite.

A branch that only picks a value counts as a value, so a clamp, a floor, a
ReLU, a running extremum and a count of matches all vectorize, in whatever
order you write the tests and whether or not you factor them into a helper.
Buffers at file scope reach the same kernels as pointers passed in. `--explain`
names the reason for every loop it leaves alone.

**Offloads to NVIDIA GPUs**, straight to PTX, with no `nvcc` and no CUDA
runtime. Write `kernel` functions, declare them on the host, and launch them:

```mettle
extern kernel(block = 256) vadd(a: float32*, b: float32*, c: float32*, n: int32);

dispatch vadd[work: n](da, db, dc, n);
```

It checks the arguments against the declaration, and the grid follows from the
declared block. Subgroup collectives, atomics, tensor core operations, `printf`
inside a kernel, and an occupancy report at build time all work. See
[GPU offload](docs/gpu.md).

**Runs your code while it compiles.** `@test` functions run in the compiler and
leave no binary behind. `mettle trace` interprets one function and prints its
values line by line. `--pgo` runs `main` at build time and feeds the call counts
it measured back to the optimizer.

**Debugs and reports crashes on its own.** Breakpoints, stepping, and reading
and writing live variables over `--debug-hooks`, with no gdb, no PDB and no
DWARF. Build with `-s` and a fault reports the bad address, such as a null field
or a freed block.

Windows and Linux are both first-class targets. One source tree builds them and
one test suite gates them. Each owns its runtime: Windows links its own PE
images, Linux emits ELF and reaches the kernel through direct system calls, so
neither carries a libc. Windows has `std/ui` for windows and controls; Linux
does not. See [what is missing](docs/known-limitations.md).

## Documentation

- [Getting started](docs/getting-started.md): install, first program, a tour of
  the language.
- [Quick reference](docs/quick-reference.md): the idioms you look up.
- [Known limitations](docs/known-limitations.md): what does not work yet.
- [Full index](docs/README.md): the language, the backend, and the tooling.

The rendered docs live at <https://suidvandiewereld.github.io/Mettle/>.

## Build from source

This repository holds the whole toolchain under one `src/`: the language and its
frontend, and **libmtlc**, which is the IR, the optimizers, code generation and
native linking. There is nothing to fetch. The build runs offline.

Windows, with gcc or clang:

```powershell
.\build.bat
.\tests\run_tests.ps1
```

Linux:

```bash
make -j"$(nproc)"
make check
```

`make check` runs the same `tests/run_tests.ps1` the Windows build gates on, so
a test written on either platform runs on both. It needs
[PowerShell Core](https://aka.ms/powershell). Without it, `bash
tools/test-elf-native.sh` still covers the owned-ELF product on its own.

To build the backend alone, the archive another frontend links against:

```powershell
.\build.bat --backend-only
```

See [Mettle and libmtlc](docs/mettle-and-libmtlc.md) for the line between the
frontend and the backend.

Samples live in [examples/](examples/). The benchmark suites pair Mettle against
C:

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

The editor extensions live in
[MettleMisc](https://github.com/The-Mettle-Project/MettleMisc): `mettle-syntax`
for VS Code and Cursor, `clion-plugin` for the IntelliJ family.

## License

Apache 2.0. See [LICENSE](LICENSE).
