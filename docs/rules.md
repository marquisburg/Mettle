# Rules

A rule is a property a program requires of itself, written as Mettle and
checked by the compiler on every build. The compiler ships a closed set of
contracts: `@noalloc` proves an allocation-free call graph, `@simd!` requires
vectorization, `@inline!` requires inlining. A rule is the open end of that
set. It is a function, it runs while compiling, and it stops the build the way
those three do.

```mettle
import "std/rule";

@rule fn no_recursion(p: Program) -> Verdict {
  for f in p.functions {
    if (f.module == "" && f.is_recursive) {
      return verdict_fail(f.site, "this module forbids recursion");
    }
  }
  return verdict_pass();
}
```

A rule is declared `@rule fn name(p: Program) -> Verdict`, with both types
from `std/rule`. It is type-checked, borrow-checked, and lowered like any
other function, then run in the compile-time interpreter after type checking
and before any code is generated. It is not part of the program: it is dropped
before optimization, the program cannot call it, and a binary carries nothing
of it.

## What a rule sees

The `Program` a rule reads is what a programmer standing after the type
checker would see. It does not see the optimizer, register allocation, or any
IR, so the compiler stays free to change those.

| Field | What it holds |
|-------|---------------|
| `p.functions` | Every function in the program, imported modules included: `Function[]` |
| `p.types` | Every declared struct and enum: `TypeInfo[]` |
| `p.modules` | The module paths that were imported: `string[]` |
| `p.target` | The target triple being built |
| `p.file` | The root source file |

A `Function` carries `name`, `qualified` (`std/io.println`; a root-file
function is just its name), `module` (`""` for the root file), `site`,
`callees` (the qualified names it calls directly), `matches` (the enum
variants its `match` and `switch` arms name, as `Shape.Circle`), `param_types`,
`return_type`, and the flags `is_extern`, `is_exported`, `is_recursive`
(part of a call cycle, itself included), `is_address_taken`,
`has_indirect_calls`, `is_noalloc`, `is_pure`, `is_inline`, `is_swappable`
and `is_kernel`. Its [effects](effects.md) are `effects` (what it performs,
inferred, `alloc` and `unknown` included), `requires` (what it needs,
inferred), and `forbids` and `provides` as declared.

`p.effects` lists every effect the program can name, built-in ones included,
as `EffectInfo` with `name`, `module`, `site` and `is_builtin`.

A `TypeInfo` carries `name`, `qualified`, `module`, `site`, `kind`
(`struct`, `enum`, `tagged_enum` or `declared`), `base` (what a declared type
refines, empty for the rest), `size`, `align`, `layout` (the same digest
`layoutof` answers), `fields` as `FieldInfo[]` with `name`, `type_name`,
`offset` and `size`, and `variants`.

`std/rule` also has `function_calls(f, callee)`, `function_matches(f, owner,
variant)`, `program_function_index(p, qualified)`, `program_type_index(p,
qualified)`, `function_reaches(p, f, callee)`, which follows direct calls
transitively, and `function_performs`, `function_requires`,
`function_forbids`, `function_provides` and `program_effect_index` over the
effects.

Compile-time functions marked `@test` and the rules themselves are not in
`p.functions`. They never ship, so no property of the shipped program depends
on them.

## The verdict

A rule answers with one of three:

| Verdict | Effect |
|---------|--------|
| `verdict_pass()` | Nothing is printed. |
| `verdict_fail(site, message)` | `error[R0002]` at the site, and the build stops. |
| `verdict_gap(site, message)` | `warning[R0003]` at the site, and the build goes on. |

`verdict_fail_program(message)` and `verdict_gap_program(message)` say the
same about the program as a whole, for a complaint no single line owns. They
report at the rule's own site.

The gap is the important one. A rule reports what it can prove and announces
what it cannot, which is the standard the borrow analyser is held to. A rule
that sees a call through a function pointer and cannot follow it says so
rather than guessing either way.

The site has to be one the rule read from the Program, a function's or a
type's `.site`. A verdict naming any other location is refused with
`error[R0001]`, because a rule is code the compiler does not trust. A rule that
traps, leaks, or runs past its step budget is reported the same way, and a
rule that leaks is told where, since the interpreter owns the heap it ran on.

```text
-- app.mettle:7:1 -----------------------------------------------------------
error[R0002]: rule 'no_recursion' failed: this module forbids recursion

  ----+------------------------------------------------------------------------
    7 |  fn depth(n: int32) -> int32 {
      :  ^ the rule points here
  ----+------------------------------------------------------------------------
      |  note  the rule that failed the build
   12 |  @rule fn no_recursion(p: Program) -> Verdict {
      :        ^^^^
```

## Cost

Every rule spends interpreter steps, and the ledger is always available:

```bash
mettle --build app.mettle --report-rules
```

```text
rule no_recursion: pass, 2164 steps
rule small_jobs: pass, 321 steps
rules: 2 run, 2 passed, 0 failed, 0 gaps, 2485 steps
```

`--rule-budget=N` makes that a contract. A build whose rules spend more than
`N` steps together fails with `error[R0004]`, and a single rule that runs past
`N` gives no verdict. A program with no rules pays nothing: no reflection is
built and no interpreter runs.

## Three things a rule can read

A rule takes exactly one argument, and its type is the question:

| Parameter | What it holds | When it runs |
|-----------|---------------|--------------|
| `Program` | The checked program: every function with its callees, effects, allocations and frees; every declared type with its size, alignment and layout; the modules; the globals with what writes each; the target. | After type checking, before any code is generated. |
| `Machine` | What the program became: per function, its frame size, spill count, instruction count, whether the register-allocating backend took it, how many calls were inlined, and whether each loop vectorized, beside the effects it holds. | After code generation, before anything is written. |

There is no second keyword and no decorator to write. The parameter type is
the whole dispatch, and a signature that is neither is refused with the three
it could have been.

```mettle
@rule fn hot_loops_vectorize(m: Machine) -> Verdict {
  for f in m.functions {
    if (f.name == "total") {
      for l in f.loops {
        if (!l.vectorized) {
          return verdict_fail(machine_loop_site(f, l), "this loop has to vectorize");
        }
      }
    }
  }
  return verdict_pass();
}
```

That is `@simd!` written by the program instead of by the compiler, and the
same shape says a frame stays under a size, a function keeps its call, or a
hot path reaches codegen with the register allocator. The optimization
contracts the compiler ships are the built-in special case of this.

The image is a snapshot, and it is the whole of what crosses the line. There is
no IR in it, no pass state, no allocator, no scheduler. III.3 is why: a rule
sees what a programmer at that point in the pipeline would see, and libmtlc
stays free to change how it got there. Reading the snapshot costs the program
nothing at run time, because a rule never links.

## Rules over what the program does with memory

The `Program` image carries the facts an arena or a region discipline is
written against: where each function allocates, where it frees, which globals
it writes, and for each global, every function that writes it.

```mettle
@rule fn only_the_arena_allocates(p: Program) -> Verdict {
  for f in p.functions {
    if (f.module != "") { continue; }
    if (function_allocates(f) && f.name != "arena_take") {
      return verdict_fail(f.allocations[0], "only arena_take may allocate");
    }
  }
  return verdict_pass();
}

@rule fn one_writer_per_global(p: Program) -> Verdict {
  for g in p.globals {
    if (g.written_by.length > 1) {
      return verdict_fail(g.site, "this global is written by more than one function");
    }
  }
  return verdict_pass();
}
```

These come from the same walk that finds the callees, so they see what the call
graph sees: a name the compiler can follow, and nothing behind a function
pointer. A rule that cares about what it cannot see reads `has_indirect_calls`
and answers `verdict_gap`, which is the standard the borrow analyser is held to
and the standard a rule is held to.

## A rule that explains itself

A rule knows why it exists and the compiler does not. It can say so, and claim
its own diagnostic code while it is there:

```mettle
@rule fn no_recursion(p: Program) -> Verdict explain R1001 "This codebase runs on fixed stacks, so a recursive function is a stack overflow waiting for the right input. Rewrite the recursion as a loop with an explicit worklist." {
  for f in p.functions {
    if (f.module == "" && f.is_recursive) {
      return verdict_fail(f.site, "this module forbids recursion");
    }
  }
  return verdict_pass();
}
```

The failure is reported under `R1001` instead of the generic `R0002`, the text
comes with it as a note, and `mettle explain R1001 house.mettle` prints it on
its own. The code lives on the declaration, so the file is part of the
question: a code above `R0999` belongs to a rule the program wrote, and
`mettle explain R1001` without a file says so. `explain` is a contextual
keyword and sits on the signature line, beside `with` and `forbids`.

## A verdict that is checked

A rule is code the compiler runs and does not trust, which cuts both ways: a
rule that answers differently on two runs over the same program decided by
accident. Under `mettle test` every rule is run twice more over a freshly
placed copy of the same image and the verdicts compared. A verdict that held
says so under `--report-rules`:

```text
rule no_recursion: pass, 3033 steps
rule no_recursion: verdict held on a second run
```

One that moved is `error[R0005]`, and the build stops. A rule that is a
function of the Program it was handed answers the same way every time; one
that is not is reading something else.

## Rules a module offers

A rule can live in a module and apply to whoever imports it, which is how a
team keeps one copy of its house style:

```mettle
// house.mettle
import "std/rule";

@rule export fn no_recursion(p: Program) -> Verdict {
  for f in p.functions {
    if (f.module == "" && f.is_recursive) {
      return verdict_fail(f.site, "the house style forbids recursion");
    }
  }
  return verdict_pass();
}
```

```mettle
import "house";
```

The import is the opt-in: a rule applies when it is `export`ed and the
program imports the module. The error points at the importing program's own
line, and the note names the module and line the rule came from, so a
failing build says both what broke and whose rule it broke.

Inside a rule, `f.module` is the module a function came from, and `""` is the
program's own file. A house rule that means "in the program that imported me"
tests for that.

## What a rule is not

It is not a plugin. There is no shared library, no ABI, and nothing loaded
from outside the program; the rule is source in the program it checks, and
`mettle expand` prints it with the rest.

It is not a predicate language. There is nothing to learn beyond Mettle: a
rule is a loop, a comparison and a return.

It cannot exempt anything. A rule reads the program; it does not change it,
and the contracts, checks and borrow analysis run on the program regardless of
what any rule said.

## Examples

Every function reachable from `main` stays off the network:

```mettle
@rule fn no_network(p: Program) -> Verdict {
  var index: int64 = program_function_index(p, "main");
  if (index < 0) { return verdict_pass(); }
  if (function_reaches(p, p.functions[index], "std/net.send")) {
    return verdict_fail(p.functions[index].site, "main may not reach the network");
  }
  return verdict_pass();
}
```

A record stays under 64 bytes on every target the program is built for:

```mettle
@rule fn packets_are_small(p: Program) -> Verdict {
  for t in p.types {
    if (t.qualified == "Packet" && t.size > 64) {
      return verdict_fail(t.site, "Packet must stay under 64 bytes");
    }
  }
  return verdict_pass();
}
```

Every variant of an enum is handled somewhere in this file:

```mettle
@rule fn every_shape_handled(p: Program) -> Verdict {
  var index: int64 = program_type_index(p, "Shape");
  if (index < 0) { return verdict_pass(); }
  var shape: TypeInfo = p.types[index];
  for v in shape.variants {
    var handled: bool = false;
    for f in p.functions {
      if (f.module != "") { continue; }
      if (function_matches(f, "Shape", v)) { handled = true; }
    }
    if (!handled) {
      return verdict_fail(shape.site, "a variant of Shape is handled nowhere in this file");
    }
  }
  return verdict_pass();
}
```

Two declarations agree on a layout:

```mettle
@rule fn wire_matches(p: Program) -> Verdict {
  var a: int64 = program_type_index(p, "client.Packet");
  var b: int64 = program_type_index(p, "server.Packet");
  if (a < 0 || b < 0) { return verdict_pass(); }
  if (p.types[a].layout != p.types[b].layout) {
    return verdict_fail(p.types[b].site, "server.Packet no longer matches client.Packet");
  }
  return verdict_pass();
}
```

## See also

- [Declarations](declarations.md) for the decorators the compiler owns
- [Compile-time execution](testing.md) for the interpreter rules run on
- [Diagnostics](diagnostics.md) for the R codes
