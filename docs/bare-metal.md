# Bare metal

A program with no operating system under it needs things an ordinary program
never asks for: the exact instruction the manual specifies, a load address the
firmware chose, a function the CPU enters rather than a caller, memory whose
reads and writes are the point rather than a means to a value, and sometimes a
processor mode that is forty years old.

This page covers the six features that make that possible: inline assembly,
`volatile`, `@naked` and `@interrupt`, cross-compilation, a chosen link
address, and 16-bit code generation.

## Inline assembly

An `asm` block holds Intel-syntax assembly. It is assembled by the compiler
itself into the function it appears in: there is no external assembler, no
text handed to a tool, and no separate object to link.

```mettle
fn count_set_bits(value: uint64) -> int64 {
    var total: int64 = 0
    asm {
        mov rcx, {value}
        xor rax, rax
    next:
        test rcx, rcx
        je done
        mov rdx, rcx
        and rdx, 1
        add rax, rdx
        shr rcx, 1
        jmp next
    done:
        mov {total}, rax
    }
    return total
}
```

### Operand bindings

`{name}` names a Mettle local, parameter, or global, and expands to where that
variable lives. A local or parameter expands to its stack home; a global
expands to a rip-relative reference in 64-bit code and an absolute one in
16- and 32-bit code, where there is no such addressing. Write it wherever an
operand goes:

```mettle
mov rax, {a}        // read a
mov {result}, rax   // write result
```

The binding is a memory operand, so a pointer variable is *loaded* by
`mov rax, {p}`; to reach what it points at, load it first and then dereference
the register.

A machine register is written directly (`rax`, `xmm3`, `cr0`), never through a
binding. The compiler keeps no value in a register across an `asm` block, so a
block may clobber whatever it likes; it must still preserve the callee-saved
registers its calling convention names, and restore the stack it moves.

### What the assembler accepts

Intel syntax, one instruction per line, `;`, `#`, `//` and `/* */` comments.

- The full integer instruction set a systems program reaches for: the ALU
  group, `mov`/`movzx`/`movsx`/`movsxd`/`movabs`/`movbe`, `lea`, `test`,
  `xchg`, the shift and rotate group including `shld`/`shrd`, `imul` in all
  three forms, `div`/`idiv`, `push`/`pop`, `inc`/`dec`, `not`/`neg`, `bswap`,
  `setcc`, `cmovcc`, `bt`/`bts`/`btr`/`btc`,
  `bsf`/`bsr`/`popcnt`/`lzcnt`/`tzcnt`, `xadd`, `cmpxchg`,
  `cmpxchg8b`/`cmpxchg16b`, and the string instructions with
  `rep`/`repe`/`repne`.
- Control transfer: `jmp`, `call`, `ret`, `retf`, `jcc`, `loop`, `jrcxz`, far
  `jmp`/`call` in the `selector:offset` form.
- System instructions: `cli`, `sti`, `hlt`, `cld`, `std`, `int`, `iret`,
  `iretd`, `iretq`, `in`, `out`, `lgdt`, `lidt`, `sgdt`, `sidt`, `lldt`, `ltr`,
  `lmsw`, `smsw`, `invlpg`, `cpuid`, `rdmsr`, `wrmsr`, `rdtsc`, `rdtscp`,
  `swapgs`, `xgetbv`, `xsetbv`, `syscall`, `sysret`, `sysenter`, `sysexit`,
  `wbinvd`, `invd`, `clts`, `lar`, `lsl`, `verr`, `verw`, `arpl`, and `mov` to
  and from the control and debug registers.
- Machine state and the cache: `fxsave`, `fxrstor`, `xsave`, `xrstor`,
  `clflush`, `prefetchnta`, `prefetcht0`, `prefetcht1`, `prefetcht2`,
  `mfence`, `lfence`, `sfence`, `pause`, and `endbr64`/`endbr32`.
- SSE and SSE2 moves and arithmetic, `movd`/`movq`, and the conversions.
- Prefixes: `lock`, `rep`, segment overrides written `fs:[...]`.
- Directives: `db`, `dw`, `dd`, `dq` (constants, symbols, and strings), `resb`
  and friends, `align`, `times`, and `bits 16` / `bits 32` / `bits 64` inside a
  `@naked` function.
- `$` is the current address and `$$` the image origin, so
  `times 510-($-$$) db 0` means what it means everywhere else.

Labels defined in a block are local to it. A name the block does not define is
a symbol reference, and the compiler relocates it like any other: `call
some_function` reaches a Mettle function, and `dq some_global` stores its
address.

Branches are relaxed: a branch that reaches its target in one signed byte takes
the short form, and the assembler repeats the layout until no more shrink. That
is what keeps 16-bit output inside encodings an 8086 accepts.

An unrecognized instruction, an ambiguous operand size, or an unreachable
branch is a compile error naming the line inside the block.

### What it costs

A function containing an `asm` block is emitted by the baseline code generator
rather than the register allocator, and nothing is promoted to a register
across it. That is the price of letting the block clobber registers freely and
of `{x}` resolving to a stack home that actually exists.

### No runtime under a freestanding target

A debug build normally checks a pointer before dereferencing it and calls into
the runtime to report a null one. A freestanding target has no runtime to
report to, and no operating system to have left address zero unmapped: there,
zero is the interrupt vector table and the low 64K is whatever the firmware
put in it. So a `*-none` target emits those checks the way `--release` does,
which is to say not at all, and the diagnostics that warn about a constant low
address stay quiet.

`--safe` is refused on a freestanding target for the same reason: its checks
consult a shadow map the runtime owns, and a flat image links no library.

## `volatile`

`volatile T` says that reading or writing a `T` is observable in itself. Such
an access is never removed, never merged with another, never hoisted out of a
loop, and never served from a register.

```mettle
fn wait_for_ready(status: volatile uint32*) {
    while (*status == 0) { }
}
```

The qualifier binds to the value being accessed, so `volatile uint16*` is a
pointer to volatile `uint16` -- the shape memory-mapped hardware has. Write it
anywhere a type goes: on a parameter, a local, a global, or a struct field.

A `volatile` global is the flag an interrupt handler writes and the main line
reads. Its accesses are not loads through a pointer, so the qualifier travels
with the symbol: every instruction that names such a global is marked, no pass
may drop or move one, and the function holding it keeps every value in memory.

```mettle
var vga: volatile uint16* = (volatile uint16*)0xB8000
vga[0] = 0x0F41
```

The guarantee is enforced twice. Each optimization pass that could move or drop
a memory access is taught to leave volatile ones alone, and the pass driver
takes a signature of the function's volatile accesses -- how many, in what
order, at what width -- before and after every pass. A pass that changes it
fails the compile naming itself, so a missed guard is a loud error rather than a
silent miscompile.

A function holding a volatile access is emitted by the baseline code generator,
for the same reason an `asm` block is: the register allocator's job is to keep
values out of memory, which is the one thing volatile forbids.

## `@naked` and `@interrupt`

### `@naked`

A `@naked` function has no prologue, no frame, and no epilogue. Its body may
hold only `asm` blocks, and the block returns by itself.

```mettle
@naked fn cpu_vendor_ebx() -> uint32 {
    asm {
        push rbx
        xor eax, eax
        cpuid
        mov eax, ebx
        pop rbx
        ret
    }
}
```

`@naked` is where `bits 16` and `bits 32` are allowed, because a naked function
is the only one whose bytes the compiler contributes nothing to. It is how a
boot sector's entry point is written.

### `@interrupt`

An `@interrupt` function is entered by the CPU, not by a caller. The compiler
emits the entry sequence: it clears the direction flag, saves all fifteen
general-purpose registers, aligns the stack, runs the body, restores the
registers, and returns with `iretq`.

```mettle
@interrupt fn timer_isr() {
    ticks = ticks + 1
}

@interrupt fn page_fault(frame: InterruptFrame*, error_code: uint64) {
    handle_fault(frame, error_code)
}
```

The parameters say what the vector pushes:

| Parameters | Meaning |
| --- | --- |
| none | a vector with no error code |
| one pointer | the pointer receives the interrupt frame |
| a pointer and an integer | the vector pushes an error code, which the entry reads and pops before `iretq` |

An `@interrupt` function returns nothing: the interrupt return has nowhere to
hand a value back to.

The 16- and 32-bit targets take `@interrupt` too. There the entry saves the
general and segment registers one at a time, so the sequence is one an 8086
accepts, and the exit returns through `iret` at the mode's own width. Real
mode pushes no error code, so a 16-bit handler takes no parameters or the
frame pointer alone, and asking for the error code is a compile error.

## Cross-compilation

`--target <triple>` compiles for a machine that is not this one. It picks the
object format, the calling convention, and the width the code generators and
the inline assembler emit for.

| Triple | Output |
| --- | --- |
| `x86_64-windows` | COFF object, MS-x64 convention |
| `x86_64-linux` | ELF object, System V AMD64 convention |
| `x86_64-none` | ELF object, freestanding |
| `aarch64-linux` | AArch64 ELF object |
| `aarch64-none` | AArch64 ELF object, freestanding |
| `i386-none`, `i686-none` | 32-bit, flat image only |
| `i8086-none` | 16-bit real mode, flat image only |

```bash
mettle server.mettle --target x86_64-linux --emit-obj -o server.o
```

A trailing vendor or environment is accepted and ignored, so
`x86_64-unknown-linux-gnu` selects the same target as `x86_64-linux`.

The 16- and 32-bit targets have no object format that carries their
relocations, so they produce a flat image and nothing else.

`--target` produces an object for the named machine. It does not link one:
`--build` runs this machine's linker against this machine's runtime, and
neither belongs to the machine you named, so `--build` with a foreign or
freestanding target is a compile error saying so. Cross-compiling means
emitting the object here and linking it there. Naming this machine's own
triple changes nothing at all: the object is byte-identical to the one that
comes out with no `--target` at all, which is what the test suite checks.

## Describing a target

A target is data the compiler reads. `mettle target <triple>` prints a
built-in target's description as Mettle:

```bash
mettle target x86_64-none
```

```mettle
import "std/target";

export const TARGET: TargetDesc = {
  name: "x86_64-none",
  arch: "x86_64",
  os: "none",
  format: "elf",
  pointer_bits: 64,
  stack_alignment: 16,
  shadow_space: 0,
  red_zone: 0,
  int_args: ["rdi", "rsi", "rdx", "rcx", "r8", "r9"],
  float_args: ["xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"],
  indirect_return: "rdi",
  separate_classes: true,
  widths: [8, 16, 32, 64],
  vector_width: 256,
  address_spaces: []
};
```

That is an ordinary Mettle module: `TargetDesc` is a struct in `std/target`,
the literal is checked by the type checker, and there is no second syntax to
learn. Save it, edit it, and hand it back:

```bash
mettle app.mettle --target mine.mettle --emit-obj -o app.o
```

Feeding a printed description back unchanged reproduces the built-in target
byte for byte, which the test suite checks for every triple. That is the
description's proof: it is verified against the machinery it describes, not
believed.

What a description may change is what an emitter can honour. A freestanding
`x86_64` target chooses its calling convention: which of `rdi`, `rsi`, `rdx`,
`rcx`, `r8` and `r9` carry integer arguments and in what order, which of
`xmm0` to `xmm7` carry floats, whether the two classes count separately, the
shadow space, and the indirect-return register (always the first integer
register). A hosted target's convention is the platform's, and a description
that rewrites it is refused, because the code on the other side of every
`extern` call was compiled by someone who did not read the description.
Pointer width, stack alignment, the integer widths, the vector width and the
absence of address spaces are the machine's; a description that claims
otherwise is refused and told which emitter fact it contradicted. The
register file's callee-saved set is fixed per architecture and is not
described.

`--report-target` prints the description in effect, built-in or described, so
a build can be read back as the machine it was made for.

A freestanding aarch64 target chooses its integer argument registers the same
way, as a reordering, a shortening, or both:

```mettle
int_args: ["x3", "x2", "x1", "x0"],
```

That is a convention no ARM document describes: four registers, reversed, with
everything past the fourth on the stack. It reaches the emitted code, which is
the whole of the claim and is checked rather than asserted. The build with the
described order and the build with the architecture's own order are different
bytes, and both give the same answer under an emulated AArch64 CPU, which is
what `arm64_convention_described_in_data` runs on every build where a
`qemu-aarch64` is reachable and reports as skipped where one is not.

A register outside x0 to x7 cannot carry an integer argument, no register may
appear twice, and the indirect-return register is `x8` because the
architecture fixes it; each is refused and told which fact it contradicted.
The float argument registers are `v0` to `v7` and the callee-saved set is per
architecture; neither is chosen here, and a description that tries is refused
rather than quietly ignored.

This is the reachable half of a larger claim. Handing out the description
keeps the machinery, and every concept above arrived by editing that
machinery: `kernel`, `@interrupt`, the 16-bit mode, a new object format. A
description cannot add one. Whether a description is enough for a genuinely
new machine is not proven, and this document does not claim it.

## Describing a machine

A target description says what an existing machine has. A machine description
says what a machine *is*: `std/machine`'s `MachineInsn`, one row per
instruction, carrying its mnemonic, its encoding, which operands it reads and
writes, and the name of the Mettle function that says what it does.

```mettle
import "std/machine";

var REG: int64[8];

fn ins_add(a: int64, b: int64, c: int64) -> int64 {
  REG[a] = REG[b] + REG[c];
  return -1;
}

const ISA: MachineInsn[1] = [
  { name: "add", encoding: "11 %0 %1 %2", operands: 3,
    reads: "%1,%2", writes: "%0", semantics: "ins_add" },
];

const PROGRAM: string[1] = [ "add 0, 1, 2" ];
```

Nothing here is a compiler concept. The registers are a global array, the
meaning of an instruction is a function, and the encoding is a string: pairs
of hex digits, with `%N` standing for the byte an operand occupies. A
semantics function takes the three operand slots and returns the index of the
next instruction, or -1 to fall through.

```bash
mettle machine app.mettle    # print the machine
mettle emulate app.mettle    # assemble PROGRAM, decode it back, run it
```

The description is checked before anything runs on it: every instruction
starts with at least one fixed byte and no two share their fixed prefix,
because that prefix is what a decoder matches on; `operands` matches the slots
the encoding carries; `reads` and `writes` name only operands that are there;
and the semantics function exists with the right shape. Each is `N0001`,
reported at the row.

Then `PROGRAM` is assembled into the machine's own bytes and decoded back out
of them by a separate walk over the same description. Re-assembling what the
decoder read has to give the same bytes, or the description is not one
machine: it writes bytes it cannot read (`N0004`). That is the same shape as a
printed target description having to reproduce its built-in target byte for
byte, and for the same reason: a description is verified, never believed.

Each decoded instruction's semantics function then runs in the compile-time
interpreter, on one interpreter that lasts the whole program, which is why the
register file carries from one instruction to the next. `--verify` validates
those functions exactly as it validates any other Mettle, so what an
instruction means is held to the standard the rest of the program is held to.
[`examples/machine/`](../examples/machine/) is a worked one: eight registers,
six instructions, and the tenth Fibonacci number computed by a machine nobody
built.

What this does not do is aim the compiler's backend at a described machine.
The register allocator does not schedule around a described instruction, and a
rewrite rule cannot map IR onto one, because both would mean handing out the
allocator and the emitters. III.3 is the reason, the same one that keeps pass
ordering private, and [known limitations](known-limitations.md) says so.

## What is proven, and on which machine

A claim about a machine is worth exactly what ran it. This is what each of
Mettle's machine claims rests on, and where the evidence stops.

| Claim | What ran it | What is not claimed |
|---|---|---|
| x86-64 code is correct | The whole suite, natively on Windows, and the same suite on Linux through `run_tests.ps1` against a Linux-built compiler | Nothing about a CPU older than SSE2, or about AVX-512 |
| AArch64 code is correct | Every `tests/arm64/` fixture emitted by this compiler and run under a user-mode `qemu-aarch64`, plus the from-scratch encoder's own ELFs | No ARM hardware has run any of it; QEMU is the CPU, and where no emulator is reachable the gate reports itself skipped rather than passed |
| A described aarch64 calling convention reaches the code | The same emulator, on a probe whose answer depends on every argument landing in the right parameter, built three ways and giving the same answer each time with different bytes | Nothing about interoperating with code built by anything else; a described convention is self-consistent and calls nothing outside its own build |
| A printed target description is the built-in target | Every built-in triple printed, fed back with `--target`, and compared byte for byte | Nothing about a triple that is not built in |
| PTX is well-formed | The portable modules assembled by NVIDIA's `ptxas` where one is installed | No GPU has run any of it in the suite; a device is needed and the gate skips without one |
| SPIR-V is well-formed | Structural validation only | No Vulkan driver has consumed it here |
| 16-bit and flat images are correct | Structural validation and the byte layout of the emitted image | No 8086 and no bootloader has been booted from one in the suite |
| A described machine runs | The compile-time interpreter, executing the semantics functions the description names, with `--verify` validating those functions | Nothing about a physical machine; a described machine is one this compiler runs, not one it targets |

Two lines of that table are the honest edge of the whole chapter. AArch64 is
proven on an emulator and not on silicon, and the GPU backends are proven
well-formed and not executed. Both are stated here rather than left for a
reader to discover, because a gate that skips quietly is the same as no gate.

And the machinery stays the compiler's. The emitters, the register allocator
and the pass ordering are libmtlc's, and no description reaches them: a
description says what a machine has, and the emitter decides what to do about
it. III.3 in [the ideology](ideology.md) is the reason, and it is the same
reason a metaprogram never sees pass ordering. Everything above is what
handing out the description bought while keeping that line.

## A chosen link address

`--image-base <addr>` sets where the linked image is loaded, replacing the
format's default. It applies to all three products: a PE's `ImageBase`, an ELF
executable's load address, and where a flat image's first byte lands. A PE
loads on a 64K boundary and an ELF on a page, and a base that does not sit on
one is a compile error rather than an image nothing can load.

`--emit-flat <file>` writes a raw image with no container at all: sections laid
out from the image base, every relocation resolved against the final addresses,
and the bytes written out.

```bash
mettle boot.mettle --target i8086-none --image-base 0x7c00 --emit-flat boot.bin
```

The entry point is a function named `_start`, or `main` when there is none, and
it is placed at offset zero. At image base `0x7c00` the compiler recognizes a
boot sector: the image is padded to 512 bytes and signed `0x55AA`.

A flat image links no library, so every name it uses has to be defined in it.
A program that reaches the runtime -- string formatting, the allocator --
names symbols nothing in the image provides, and the compiler says which
section referenced what. Define them yourself, or keep the image to code that
does not need them. A freestanding target emits no null-pointer check, so an
ordinary dereference costs the image nothing.

Zero-initialized data is laid out last and written into the image as zeros.
There is no loader to reserve it and no header to ask one to, so a stack or a
table declared without an initializer takes up its own size in the file. That
is why a boot sector keeps them small: the 512-byte limit counts them.

## 16-bit code generation

With `--target i8086-none` the compiler generates 16-bit real-mode code for
ordinary Mettle functions, not only for `asm` blocks.

```mettle
fn putchar(c: int16) -> int16 {
    asm {
        mov ax, {c}
        mov ah, 0x0e
        mov bx, 7
        int 0x10
    }
    return c
}

fn main() -> int16 {
    var i: int16 = 0
    while (i < 5) {
        putchar((int16)(65 + i))
        i = (int16)(i + 1)
    }
    return 0
}
```

Real mode is 16 bits wide, and the code generator says so:

- A value computed in a register is `int8`, `uint8`, `int16`, `uint16`, or a
  near pointer. There is no `int32`, no `int64`, and no floating point, and a
  declaration that asks for one is a compile error naming it.
- A struct or an array is a region of the frame rather than a value in a
  register, so both work: the frame sizes each local by its declared type, and
  the code reaches into it by address the way the 64-bit backend does.
- A range-`for` counter takes the target's word, so `for i in 0..n` walks an
  array without a cast.
- Arguments are pushed right to left and the caller cleans up; the result comes
  back in `AX`. Locals and parameters live in a `bp`-relative frame.
- Arithmetic, comparison, control flow, calls, loads and stores, address-of,
  `@interrupt` handlers, and `asm` blocks with `{}` bindings all work. Anything
  else is a compile error naming the construct.

The test suite compiles three images out of Mettle and then *runs* each in a
real-mode emulator, checking what it printed through the BIOS. One prints
letters from a counted loop; one computes through a struct, an array indexed by
a range-`for` counter, a pointer and a global, and prints a digit per answer;
one installs an `@interrupt` handler in the vector table, raises `int 0x40`
three times, and checks both the count the handler kept and that the
interrupted code got its registers back. Bytes that look right are not evidence
that real-mode code runs.

## A complete boot sector

```mettle
@naked fn _start() {
    asm {
        bits 16
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7c00
        sti
        call main
        cli
    halt:
        hlt
        jmp halt
    }
}

fn putchar(c: int16) -> int16 {
    asm {
        mov ax, {c}
        mov ah, 0x0e
        mov bx, 7
        int 0x10
    }
    return c
}

fn main() -> int16 {
    var i: int16 = 0
    while (i < 5) {
        putchar((int16)(65 + i))
        i = (int16)(i + 1)
    }
    putchar(13)
    putchar(10)
    return 0
}
```

```bash
mettle boot.mettle --target i8086-none --image-base 0x7c00 --emit-flat boot.bin
```

512 bytes, signed, and it prints `ABCDE`.
