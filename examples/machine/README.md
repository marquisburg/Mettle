# A machine that does not exist

`little.mettle` describes an eight-register machine with six instructions,
writes a program for it, and runs that program. Nothing about the machine is a
compiler concept: the registers are an ordinary global array, each
instruction's meaning is an ordinary function, and the encoding is a string.

```bash
mettle machine examples/machine/little.mettle
```

```text
machine ISA: 6 instructions
  seti     10 %0 %1  reads nothing, writes %0, does ins_seti
  add      11 %0 %1 %2  reads %1,%2, writes %0, does ins_add
  mov      12 %0 %1  reads %1, writes %0, does ins_mov
  subi     13 %0 %1  reads %0, writes %0, does ins_subi
  jnz      20 %0 %1  reads %0, writes nothing, does ins_jnz
  out      30 %0  reads %0, writes nothing, does ins_out
```

```bash
mettle emulate examples/machine/little.mettle
```

```text
r0 = 55
r2 = 0
  REG = 55 34 0 55 0 0 0 0
ISA: 11 instructions in 32 bytes, 65 executed
```

55 is the tenth Fibonacci number, computed by a machine nobody built.

## What the compiler contributes

Three things, and it is worth being exact about which.

**It reads the description and refuses one that does not hold together.** An
encoding is pairs of hex digits and operand slots. Every instruction starts
with at least one fixed byte, and no two share their fixed prefix, because
that prefix is what a decoder matches on. An instruction's `operands` count
has to match the slots its encoding carries, and `reads` and `writes` may only
name operands that are there. The function an instruction names has to exist
and take the operand slots. Each of those is an `N0001` with the row in front
of you.

**It assembles and decodes, and checks the two agree.** `PROGRAM` is assembled
into the machine's own bytes, and then decoded back out of them by a separate
walk over the same description. Re-assembling what the decoder read has to
give the same bytes, or the description is not one machine: it writes bytes it
cannot read. That is `N0004`, and it is how the description is verified rather
than believed.

**It runs the semantics.** Each decoded instruction's function runs in the
compile-time interpreter, on one machine that persists across the whole
program, which is why the register file carries from one instruction to the
next. `--verify` validates those functions exactly as it validates any other
Mettle, so the meaning of an instruction is held to the same standard as the
rest of the program.

## What it does not contribute

The register allocator does not schedule around a described instruction, and
a rewrite rule cannot map IR onto one. Both would mean handing out the
emitters and the allocator, and those are libmtlc's, for the reason
[the ideology](../../docs/ideology.md) gives under III.3: a metaprogram sees
what a programmer at that point in the pipeline would see, and it never sees
register allocation or pass ordering. A described machine here is one you
write programs for and run; it is not one the compiler's backend can be aimed
at. [Known limitations](../../docs/known-limitations.md) says so in the same
words.
