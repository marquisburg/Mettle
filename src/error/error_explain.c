// `mettle explain <CODE>`: extended documentation for diagnostic codes,
// modeled on `rustc --explain`. One entry per stable code.
//
// Two tables live here. DOCS covers the compile diagnostics (E0001..E0007,
// M0101..M0119). DECISIONS covers the optimizer decision codes the --explain
// report prints in brackets after each verdict, so a reader who sees
// `[dot-shape-address]` in the report can ask for the long version of it
// without leaving the terminal.
#include "error_explain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *code;
  const char *title;
  const char *body;
} ErrorCodeDoc;

static const ErrorCodeDoc DOCS[] = {
    {"E0001", "Lexical error",
     "The compiler could not turn part of the source text into tokens.\n"
     "Typical causes: an unterminated string literal, an invalid numeric\n"
     "literal (e.g. 0x with no digits), or a stray character that is not\n"
     "part of the language.\n"
     "\n"
     "Example:\n"
     "    var s: string = \"unterminated;\n"
     "\n"
     "Fix: close the string, correct the literal, or delete the stray\n"
     "character. The caret in the diagnostic points at the first byte the\n"
     "lexer could not process.\n"},
    {"E0002", "Syntax error",
     "The source tokenized correctly but does not follow Mettle's grammar.\n"
     "\n"
     "Common cases and their fixes:\n"
     "  - `if`/`while`/`match` conditions require parentheses:\n"
     "        if (x > 0) { ... }        not    if x > 0 { ... }\n"
     "  - every statement ends with ';'\n"
     "  - a '{' must have a matching '}'\n"
     "  - type annotations use ':', assignment uses '=':\n"
     "        var x: int64 = 1;\n"
     "\n"
     "After a syntax error the parser resynchronizes at the next statement\n"
     "boundary and keeps going, so one mistake reports once, not as a\n"
     "cascade.\n"},
    {"E0003", "Semantic error",
     "The code is grammatically valid but does not make sense: an\n"
     "undefined variable or function, a duplicate declaration, a wrong\n"
     "argument count, `break` outside a loop, and similar.\n"
     "\n"
     "Example:\n"
     "    fn main() -> int64 {\n"
     "        return cout;      // error: Undefined variable 'cout'\n"
     "    }\n"
     "\n"
     "The compiler suggests the closest in-scope name (\"did you mean\n"
     "'count'?\") and, for duplicate declarations and call errors, points\n"
     "at the previous declaration / the function definition in a note.\n"},
    {"E0004", "Type mismatch",
     "A value of one type was used where a different type is required.\n"
     "Mettle never infers `var`/local `const` binding types, so both sides\n"
     "must line up.\n"
     "\n"
     "Example:\n"
     "    var x: int64 = \"hello\";   // expected 'int64', found 'string'\n"
     "\n"
     "Fixes:\n"
     "  - change the declared type to match the value, or the value to\n"
     "    match the type\n"
     "  - for numeric conversions, cast explicitly: (int32)value\n"
     "\n"
     "Integers are the exception, and only in one direction: a conversion\n"
     "that every value survives happens on its own (int32 -> int64), and one\n"
     "that can change the value needs the cast. Those report M0119 rather\n"
     "than this code, so `mettle explain M0119` has the rule.\n"
     "\n"
     "`Type` and `Field` are compile-time reflection values (a TypeRef is\n"
     "a type-table index; a FieldRef is {type_index, field_index}). They\n"
     "have no runtime representation and cannot escape into runtime code.\n"
     "Bind them with `const` and keep them at compile time.\n"},
    {"E0005", "Scope error",
     "A name was used outside the region where it is visible. Variables\n"
     "live from their declaration to the end of the enclosing block.\n"
     "\n"
     "Fix: declare the variable in a scope that encloses every use, or\n"
     "move the use into the variable's block.\n"},
    {"E0006", "I/O error",
     "The compiler could not read an input file or write an output file.\n"
     "Check that the path exists, that the file is readable, and that the\n"
     "output directory is writable. Import search paths can be extended\n"
     "with -I <dir>.\n"},
    {"E0007", "Internal compiler error",
     "The compiler itself hit a bug - this is never your program's fault.\n"
     "Re-run with --debug-compiler for a detailed report, and please file\n"
     "the reproducing source at the Mettle issue tracker.\n"},
    {"M0101", "Use after free",
     "A heap pointer is dereferenced after `free(p)` on the same path.\n"
     "The memory may already be reused; reads are garbage and writes\n"
     "corrupt other data.\n"
     "\n"
     "Fix: free last, or set the pointer aside until every use is done.\n"
     "This analysis is conservative: it only warns when the freed pointer\n"
     "and the use are provably the same allocation on the same path.\n"},
    {"M0102", "Double free",
     "`free` is called twice on the same allocation along one path, which\n"
     "corrupts the allocator's bookkeeping.\n"
     "\n"
     "Fix: free exactly once, typically at the single owner of the\n"
     "allocation. If two branches both free, hoist the free after the\n"
     "branches.\n"},
    {"M0103", "Returning the address of a stack local",
     "A function returns a pointer into its own stack frame. The frame is\n"
     "reclaimed on return, so the caller receives a dangling pointer.\n"
     "This is a hard error.\n"
     "\n"
     "Example:\n"
     "    fn bad() -> *int64 {\n"
     "        var local: int64 = 1;\n"
     "        return &local;        // M0103\n"
     "    }\n"
     "\n"
     "Fix: heap-allocate the value (`new`/`malloc`) or return it by value.\n"},
    {"M0104", "Storing a stack address in a global",
     "A pointer to a stack local is stored in a global variable. Once the\n"
     "function returns the global points at a dead frame.\n"
     "\n"
     "Fix: store heap memory in globals, or copy the value itself.\n"},
    {"M0105", "Constant array index out of bounds",
     "An array is indexed with a compile-time constant that is negative or\n"
     ">= the array length. This would always fault at runtime, so it is a\n"
     "hard error.\n"
     "\n"
     "Fix: correct the index or the array size. Remember indices are\n"
     "0-based: the last element of `var a: int64[4]` is a[3].\n"},
    {"M0106", "Memory operation overflows a stack array",
     "A memcpy/memset-style operation writes more bytes than the\n"
     "destination stack array holds, smashing adjacent stack memory.\n"
     "\n"
     "Fix: pass the correct byte count (element count * element size), or\n"
     "grow the buffer.\n"},
    {"M0107", "Memory leak",
     "An allocation never escapes the function (not returned, not stored,\n"
     "not passed on) and is never freed: the memory is unreachable after\n"
     "the function returns.\n"
     "\n"
     "Fix: free it on every path (a `defer free(p);` right after the\n"
     "allocation covers early returns), or hand ownership out.\n"},
    {"M0108", "Use after call-freed pointer",
     "A pointer is used after being passed to a function that frees it\n"
     "(directly or transitively - ownership summaries are inferred over\n"
     "the whole call graph).\n"
     "\n"
     "Fix: treat the pointer as consumed by that call; don't touch it\n"
     "afterwards.\n"},
    {"M0109", "Double free via call",
     "An allocation is freed both by a callee that takes ownership and by\n"
     "the caller (or by two consuming calls).\n"
     "\n"
     "Fix: decide which side owns the pointer and free only there.\n"},
    {"M0110", "Borrowed interior pointer outlives its scope",
     "A pointer into a stack value (a field, an array element) escapes the\n"
     "scope that owns the value, e.g. saved to an outer variable inside a\n"
     "block. When the block exits the pointee dies.\n"
     "\n"
     "Fix: shorten the pointer's lifetime to the value's scope, or move\n"
     "the value itself to the outer scope / heap.\n"},
    {"M0111", "Borrowed pointer invalidated by realloc",
     "A pointer into a heap buffer is used after the buffer was passed to\n"
     "realloc. realloc may move the allocation, leaving every old interior\n"
     "pointer dangling - even when the call \"usually\" grows in place.\n"
     "\n"
     "Fix: recompute interior pointers from the (new) base pointer after\n"
     "every realloc.\n"},
    {"M0112", "Borrowed pointer invalidated by free",
     "A pointer derived from a heap buffer (base + offset, field address)\n"
     "is used after the underlying buffer was freed.\n"
     "\n"
     "Fix: finish all uses of derived pointers before freeing the base,\n"
     "or free later.\n"},
    {"M0113", "Dereference of a null pointer",
     "The pointer was assigned null and never reassigned before this use,\n"
     "so the dereference traps the moment it runs.\n"
     "\n"
     "Fix: assign a real address first, or guard the access:\n"
     "    if (p != 0) { ... }\n"
     "\n"
     "Only reported when the null assignment and the use are provably on\n"
     "the same path, so this is a fact rather than a suspicion.\n"},
    {"M0114", "Dereference of an unmapped constant address",
     "The pointer holds a small integer constant. The low 64K of the\n"
     "address space is never mapped on any supported platform, so this\n"
     "dereference faults.\n"
     "\n"
     "Usually a placeholder that was never filled in, or an integer that\n"
     "was cast to a pointer by mistake.\n"},
    {"M0115", "Shift count at or past the operand width",
     "Shifting a 32-bit value by 32, or a 64-bit value by 64, does not\n"
     "produce zero. The hardware masks the count (x86 uses the low 5 or 6\n"
     "bits), so `x << 32` on an int32 is `x << 0`, which is `x`.\n"
     "\n"
     "Fix: shift by less than the width, or widen the operand first:\n"
     "    var wide: int64 = (int64)x;\n"
     "    var shifted: int64 = wide << 32;\n"},
    {"M0116", "Division or modulo by a constant zero",
     "The divisor is the literal zero, so the operation traps the moment it\n"
     "executes. This is a hard error: no input can make it work.\n"
     "\n"
     "Fix: correct the constant, or guard the divisor when it is meant to\n"
     "be a variable.\n"},
    {"M0117", "Loop index runs past the end of the array",
     "The loop's upper bound and the array's length are both known at\n"
     "compile time, and the bound is the larger. The final iteration reads\n"
     "or writes past the end.\n"
     "\n"
     "Example:\n"
     "    var a: int64[8];\n"
     "    for i in 0..9 { a[i] = i; }   // M0117: valid indexes are 0..7\n"
     "\n"
     "Fix: match the bound to the length. Indexes are 0-based, so an array\n"
     "of 8 runs 0..7 and an off-by-one here is the usual cause.\n"},
    {"M0118", "Integer out of range for its destination",
     "A compile-time integer is stored somewhere that cannot hold it. The\n"
     "value is known here, so this is a fact rather than a risk: the store\n"
     "would keep the low bits and discard the rest.\n"
     "\n"
     "Example:\n"
     "    var h: int32 = 2654435761;    // M0118: int32 holds ..2147483647\n"
     "\n"
     "Fixes:\n"
     "  - widen the destination: `var h: int64 = 2654435761;`\n"
     "  - pick the type that holds it: `var h: uint32 = 2654435761;`\n"
     "  - cast, when the wrap is the point: `(int32)2654435761`\n"
     "\n"
     "The last one is not a workaround. A cast at the site is how a reader\n"
     "learns the truncation was intended, which is the whole reason this is\n"
     "reported instead of performed.\n"},
     {"M0119", "Narrowing conversion needs a cast",
     "A value flows into a type that cannot hold every value the source type\n"
     "can. Mettle converts silently in one direction only:\n"
     "\n"
     "    Widen silently. Narrow loudly.\n"
     "\n"
     "So int32 -> int64 and uint32 -> int64 need nothing written, because\n"
     "every value survives them. int64 -> int32, uint64 -> int64 and\n"
     "int32 -> uint32 do, because some values do not.\n"
     "\n"
     "Example:\n"
     "    fn f(n: int64) {\n"
     "        var small: int32 = n;         // M0119\n"
     "        var ok:    int32 = (int32)n;  // says the wrap is meant\n"
     "    }\n"
     "\n"
     "A compile-time constant is exempt: its value is known, so\n"
     "`var b: uint8 = 200;` is checked rather than refused (see M0118 for\n"
     "the case where it does not fit).\n"
     "\n"
     "Two destinations sit outside the rule because they are not range\n"
     "conversions: `bool` is a truth coercion, and an enum names a set.\n"
     "float32 -> float64 widens silently and float64 -> float32 stays silent.\n"
     "float32 -> float16 and float32 -> bfloat16 narrow loudly, like int64 ->\n"
     "int8. A float literal that is exactly representable needs no cast.\n"},
    {"M0120", "Pointer cast to an integer and back to a pointer",
     "A pointer is cast to an integer and straight back to a pointer. The\n"
     "integer in the middle holds the same address the pointer already held,\n"
     "so nothing about the value changes. What changes is what the compiler\n"
     "can say about it: an integer has no provenance, so the borrow checker,\n"
     "the alias analysis and --verify all have to give up on where that\n"
     "address came from.\n"
     "\n"
     "Example:\n"
     "    var p: float32* = (float32*)((int64)(&(buf[0])));  // M0120\n"
     "    var q: float32* = &(buf[0]);                       // the same value\n"
     "\n"
     "Lowering sees through the round trip, so the analyses are not blinded\n"
     "while the spelling is cleaned up. The value is not wrong; the detour\n"
     "is.\n"
     "\n"
     "Neither half is reported on its own, because each is sometimes the only\n"
     "way to say a thing. An integer that really is an address -- a handle\n"
     "from the operating system, a device pointer -- becomes a pointer by\n"
     "cast. A pointer becomes an integer to be printed, hashed or aligned.\n"
     "Only the round trip carries no information.\n"},
    {"M0121", "A task was handed a pointer into the frame that spawned it",
     "A call hands the address of a function this program defines to a callee\n"
     "whose body is not here, with a pointer straight after it. That is what\n"
     "every thread-spawn interface looks like, so the pointer is what the new\n"
     "task will read. The pointer given points into the spawning frame, and\n"
     "the frame's storage is gone the moment it returns.\n"
     "\n"
     "Example:\n"
     "    var arg: Work;\n"
     "    CreateThread(0, 0, &worker, &arg, 0, 0);   // M0121\n"
     "\n"
     "The compiler cannot see when the task stops, so it cannot prove the\n"
     "frame outlives it. This is refused rather than warned about, because\n"
     "the read happens on another thread and the corruption it causes has no\n"
     "line number on it.\n"
     "\n"
     "Fixes:\n"
     "  - hand over an allocation, which the task or a later join frees\n"
     "  - hand over the address of a global, which outlives every frame\n"
     "  - pass the value by copy where the interface allows it\n"},
    {"M0122", "A message was written after it was handed to a task",
     "A pointer was given to a task and then written through here. Both ends\n"
     "hold the same object from that point on, with nothing ordering the two\n"
     "writes, so what the task reads depends on which core got there first.\n"
     "\n"
     "Example:\n"
     "    CreateThread(0, 0, &worker, msg, 0, 0);\n"
     "    msg.count = 7;                             // M0122\n"
     "\n"
     "Fixes:\n"
     "  - finish writing the message before the task starts\n"
     "  - hand the task its own copy and keep writing this one\n"
     "  - take the pointer back out of a structure the two ends share under a\n"
     "    lock, so the write is ordered by something the reader also takes\n"},
    {"R0001", "A rule gave no usable verdict",
     "A `@rule fn` ran while compiling and did not answer with a verdict the\n"
     "compiler could act on: it trapped, ran out of its step budget, used\n"
     "something the compile-time interpreter cannot run, or returned a site\n"
     "that is not in the Program it was handed.\n"
     "\n"
     "A rule answers with verdict_pass(), verdict_fail(site, message) or\n"
     "verdict_gap(site, message), and every site it names has to be one it\n"
     "read from the Program: a function's or a type's `.site`. The compiler\n"
     "checks that, because a rule is code it does not trust.\n"
     "\n"
     "Fix: return one of the three verdicts, take the site from the\n"
     "Program, and if the rule is expensive raise `--rule-budget=N`.\n"},
    {"R0002", "A rule failed the build",
     "A `@rule fn` the program declares returned verdict_fail for the site\n"
     "the diagnostic points at, with the message it wrote. The build stops\n"
     "here the way `@noalloc` or `@simd!` stops it: the rule is a property\n"
     "the program requires of itself, and this site does not have it.\n"
     "\n"
     "The note names the rule. Read it as ordinary Mettle: it is one, and\n"
     "`mettle expand` prints it with the rest of the program.\n"
     "\n"
     "Fix: change the code the rule points at, or change the rule.\n"},
    {"R0003", "A rule could not decide",
     "A `@rule fn` returned verdict_gap: at this site it could neither\n"
     "prove nor refute the property it checks, and it says so rather than\n"
     "guessing. The build goes on. This is a warning so the gap is loud.\n"
     "\n"
     "A rule reports what it can prove and announces what it cannot, the\n"
     "same standard the borrow analyser is held to. Typical gaps are a call\n"
     "through a function pointer or an extern the rule has no model for.\n"
     "\n"
     "Fix: give the rule what it needs to decide (a direct call, a named\n"
     "function), or accept the gap.\n"},
    {"R0005", "A rule answered differently on a second run",
     "Under `mettle test` every `@rule` is run again over a freshly placed\n"
     "copy of the same program image, and this rule did not give the same\n"
     "verdict twice. A rule is code the compiler runs and does not trust,\n"
     "so a verdict that moves is reported rather than picked from.\n"
     "\n"
     "A rule that is a function of the program it reads answers the same\n"
     "way every time. One that does not is reading something else: an\n"
     "uninitialised local, an address, or an order the compiler is free to\n"
     "change.\n"
     "\n"
     "Fix: make the rule depend only on the Program it was handed.\n"},
    {"R0004", "Rules spent more than their budget",
     "`--rule-budget=N` makes the cost of running the program's rules a\n"
     "contract: the interpreter steps they spend together may not exceed\n"
     "N. They did. `--report-rules` prints what each rule cost.\n"
     "\n"
     "Fix: make the rules cheaper, or raise the budget deliberately.\n"},
    {"P0001", "A declared type's rule could not be proven here",
     "A value flows into a type declared with `type Name = Base where\n"
     "predicate;`, and the compiler could not prove the predicate for it.\n"
     "The message names the conjunct it could not establish, the\n"
     "expression, and the range it knew for it.\n"
     "\n"
     "The proof is the compiler's job and never an annotation. It proves a\n"
     "conversion from a constant, from the value's own type (a uint8 is\n"
     "0..255, a Digit is 0..9), from arithmetic it can bound (`x & 63`,\n"
     "`n % 10`, `a + b` over known ranges), from a loop bound, from a\n"
     "dominating `if` whose condition implies the predicate, from an early\n"
     "exit that rules the other case out, and from a guard that repeats the\n"
     "predicate itself (`if (is_valid(s))` for `where is_valid(value)`).\n"
     "Past that it refuses to guess, which is this error.\n"
     "\n"
     "A declared type with no predicate (`type Meters = float64;`) is a\n"
     "unit: a plain base value becomes one only by a cast, and two such\n"
     "types never mix in arithmetic.\n"
     "\n"
     "Fix: guard the value where it is converted, or narrow what it is\n"
     "computed from.\n"
     "\n"
     "The predicate speaks about `value` unless the declaration names the\n"
     "binding itself: `type Slot = uint32 where n: n < 64;`.\n"},
    {"F0001", "A function reaches an effect it forbids",
     "A function declared `forbids E` performs E itself or reaches a\n"
     "function that does. The message gives the call chain from the\n"
     "forbidding function to the source, and the notes mark every call on\n"
     "the way and the line that performs the effect.\n"
     "\n"
     "What a function performs is inferred, never annotated in the middle:\n"
     "a function performs what it is declared `with`, and everything its\n"
     "callees perform. `alloc`, `asm` and `syscall` are built in, with\n"
     "`new`, allocator calls, string concatenation, inline assembly and the\n"
     "`syscall` built-in as their sources. A call outside the program with\n"
     "no `with` clause may allocate; a call through a function type with no\n"
     "`with` clause may perform anything, and a forbidding function that\n"
     "reaches one is refused for that reason.\n"
     "\n"
     "Fix: remove the effect from the path, declare the extern `with none`\n"
     "or `with` what it does, or give the function type a `with` clause.\n"},
    {"F0002", "A function requires an effect nothing provides",
     "A function declared `requires E` may only run where E is provided,\n"
     "and the compiler found a way to reach it from a place that provides\n"
     "nothing: `main`, an interrupt handler, a naked function, a kernel, a\n"
     "test, or an exported function of a shared library.\n"
     "\n"
     "A requirement travels up the call graph until a caller declares\n"
     "`provides E`, which grants the effect to everything its body calls.\n"
     "This is how thread affinity, a held lock, or an initialised subsystem\n"
     "becomes something the compiler checks: the function that owns the\n"
     "thread provides the effect, and every function that needs it says so.\n"
     "\n"
     "Fix: call the function from inside something that `provides E`, or\n"
     "make the entry point provide it because it really does.\n"},
    {"F0003", "A function value does not fit the effects its type declares",
     "A function is handed to a function type that declares `with ...` or\n"
     "`requires ...`, and what the function actually performs or requires\n"
     "does not fit. A type with a `with` clause is a closed promise: a value\n"
     "of it performs those effects and nothing else, so a call through it\n"
     "is as good as a direct call. A value with no such clause could be\n"
     "anything, which is why it cannot flow into a type that has one.\n"
     "\n"
     "Passing a function by name lets the compiler check its inferred\n"
     "effects against the type. A value already in a function-typed\n"
     "variable carries only what its own type says.\n"
     "\n"
     "Fix: widen the type's `with` clause, remove the effect from the\n"
     "function, or pass the function by name.\n"},
    {"N0001", "A described machine is not one the compiler can read",
     "A machine is a `const` of `std/machine`'s `MachineInsn`, one row per\n"
     "instruction, and every field of a row is a literal, because all of it\n"
     "is answered before anything runs.\n"
     "\n"
     "Example:\n"
     "    const ISA: MachineInsn[1] = [\n"
     "      { name: \"addi\", encoding: \"10 %0 %1\", operands: 2,\n"
     "        reads: \"%1\", writes: \"%0\", semantics: \"ins_addi\" },\n"
     "    ];\n"
     "\n"
     "An encoding is pairs of hex digits and operand slots, it starts with at\n"
     "least one fixed byte, and no two instructions share their fixed prefix,\n"
     "because that prefix is what a decoder matches on. `reads` and `writes`\n"
     "may only name operands the encoding actually carries. A semantics\n"
     "function takes the three operand slots and returns the next instruction\n"
     "index or -1.\n"},
    {"N0002", "A described machine has no program to run",
     "`mettle emulate` assembles `const PROGRAM: string[N]`, one assembly\n"
     "line per row, into the machine's own encoding. A file describing a\n"
     "machine and nothing to run on it has only half of what is needed.\n"
     "\n"
     "Example:\n"
     "    const PROGRAM: string[2] = [ \"seti 0, 5\", \"out 0\" ];\n"},
    {"N0003", "A line of PROGRAM is not an instruction of this machine",
     "The mnemonic is not one the machine describes, the operand count is\n"
     "not the one the instruction takes, or an operand does not fit the byte\n"
     "the encoding gives it.\n"},
    {"N0004", "A described machine does not round-trip",
     "Assembling and decoding are separate walks over the same description,\n"
     "and re-assembling what the decoder read back is what says the two\n"
     "agree. When they do not, the description is not one machine: it writes\n"
     "bytes it cannot read.\n"
     "\n"
     "The usual cause is two instructions whose fixed prefixes overlap, which\n"
     "is refused up front, or an encoding whose fixed bytes appear where\n"
     "another instruction's operand lands.\n"},
    {"N0005", "A described instruction's semantics did not run",
     "The function an instruction names is what the instruction does, and the\n"
     "compile-time interpreter runs it. This reports that it was not there,\n"
     "that it trapped, or that it ran out of steps.\n"},
    {"N0006", "A described machine did not halt",
     "The emulator runs until the program's bytes run out or an instruction\n"
     "branches past the end. A machine that does neither inside its step\n"
     "budget is reported rather than left running.\n"},
    {"D0001", "A function's longest path costs more than its deadline",
     "`where cycles < N` on a function is a claim about its longest path, and\n"
     "the compiler costs that path from a model of the target: one cost per\n"
     "instruction, a call costed at what the callee's own longest path costs,\n"
     "and a loop costed at its body times a trip count taken from the loop's\n"
     "own bound.\n"
     "\n"
     "Example:\n"
     "    fn tick(seed: int32) -> int32 where cycles < 60 {   // D0001\n"
     "      var i: int32 = 0;\n"
     "      while (i < 8) { seed = mix(seed); i = i + 1; }\n"
     "      return seed;\n"
     "    }\n"
     "\n"
     "`--report-deadlines` prints the path block by block with what each one\n"
     "costs, which is where the work actually is.\n"
     "\n"
     "The model costs the IR as lowered, before the optimizer runs, so the\n"
     "number is an upper bound on the work the program asked for rather than\n"
     "a prediction of a particular machine's cycles.\n"
     "\n"
     "Fixes:\n"
     "  - raise the deadline to what the path costs\n"
     "  - take work off the longest path, which the report names\n"},
    {"D0002", "A function's longest path cannot be bounded",
     "A deadline that cannot be proven is not a deadline. The path runs\n"
     "through something with no bound the compiler can find: a loop whose\n"
     "trip count depends on a value it cannot see, a call through a pointer,\n"
     "a call to a function with no body here, or an asm block.\n"
     "\n"
     "Example:\n"
     "    fn tick() where cycles < 4000 {\n"
     "      while (i < limit) { ... }   // D0002: `limit` is a global\n"
     "    }\n"
     "\n"
     "Fixes:\n"
     "  - bound the loop with a constant, or with a declared type that carries\n"
     "    one, so the trip count is a fact rather than a guess\n"
     "  - build with `--pgo`, which interprets the program and lets a measured\n"
     "    trip count stand in; the deadline then holds on evidence, and\n"
     "    `--report-deadlines` says so in those words\n"
     "  - take the unbounded work out of the function that carries the\n"
     "    deadline\n"},
    {"H0001", "A schedule is not written as phases",
     "A `const` of `std/schedule`'s `Schedule` is read while compiling, so it\n"
     "has to be there to read: an array literal of rows, each naming the\n"
     "phase, the effect that holds while it runs, the function it runs, and\n"
     "the thread it runs on.\n"
     "\n"
     "Example:\n"
     "    const FRAME: Schedule[2] = [\n"
     "      { phase: \"input\", effect: \"Input\", entry: \"read\", thread: 0 },\n"
     "      { phase: \"sim\", effect: \"Sim\", entry: \"step\", thread: 1 },\n"
     "    ];\n"
     "\n"
     "Every field is a literal. Nothing here can be computed at run time,\n"
     "because the dispatcher is generated before the program runs.\n"},
    {"H0002", "A schedule names a phase or an effect twice",
     "Each phase has its own name and its own effect. One effect per phase is\n"
     "what makes a call across a phase boundary something the compiler can\n"
     "refuse: the wrapper for a phase provides that phase's effect and no\n"
     "other, so reaching code that requires a different phase's effect lands\n"
     "somewhere nothing provides it.\n"
     "\n"
     "Two phases sharing an effect would make that boundary invisible.\n"},
    {"H0003", "A schedule names an effect nothing declares",
     "A phase runs under an effect, and the effect has to be declared where\n"
     "the schedule can see it. The compiler does not invent one, because an\n"
     "effect a program never wrote is one it cannot say anything about.\n"
     "\n"
     "Fix: `effect <name>;` beside the schedule.\n"},
    {"H0004", "A schedule names an entry nothing declares",
     "A phase runs a function of this module, named as it was written. The\n"
     "name is a string in the schedule, so a typo is a name that resolves to\n"
     "nothing rather than a call that does not compile; this is the check\n"
     "that turns the first into the second.\n"},
    {"H0005", "A schedule is a `var`",
     "A schedule is read while compiling, so it has to be a `const`. A `var`\n"
     "can change while the program runs, and the dispatcher was generated\n"
     "before it started.\n"},
    {"H0006", "The dispatcher generated from a schedule did not parse",
     "This is a compiler fault rather than a program's. The generated\n"
     "dispatcher is ordinary Mettle and `mettle expand` prints it; if that\n"
     "text is not valid, the generator produced something wrong. Please\n"
     "report it with the schedule that caused it.\n"},
    {"F0006", "Two threads write the same global and nothing orders them",
     "Two functions write one global, and the effects each needs place them\n"
     "somewhere different: one runs where an effect it requires is provided,\n"
     "the other where a different one is, and no effect appears in both\n"
     "requirement sets. Nothing in the program says which write lands last.\n"
     "\n"
     "Example:\n"
     "    effect Sim;\n"
     "    effect Render;\n"
     "    var frame: int32 = 0;\n"
     "    fn tick() requires Sim { frame = frame + 1; }   // F0006\n"
     "    fn draw() requires Render { frame = 0; }\n"
     "\n"
     "An effect BOTH writers require is what says they are ordered, because\n"
     "whoever provides it runs them one at a time. That is what a lock is,\n"
     "spelled as a requirement:\n"
     "\n"
     "    effect FrameLock;\n"
     "    fn tick() requires Sim, FrameLock { frame = frame + 1; }\n"
     "    fn draw() requires Render, FrameLock { frame = 0; }\n"
     "\n"
     "Fixes:\n"
     "  - give both writers a requirement they share, and provide it around\n"
     "    each write with the lock the program already holds\n"
     "  - give each thread its own copy and combine them at a point one\n"
     "    thread owns\n"
     "  - move one of the writes to where the other one runs\n"
     "\n"
     "Nothing is said about a global whose writers need no placed effect at\n"
     "all. A program that declares no `provides` is not making a claim about\n"
     "where its code runs, and this check makes none about it.\n"},
    {"F0005", "The effect pass spent more than its budget",
     "`--effect-budget=N` makes the cost of inferring the program's effects\n"
     "a contract: the steps the pass spends scanning bodies and running its\n"
     "fixpoint may not exceed N. They did.\n"
     "\n"
     "`--report-effects` prints what the pass settled, one line per\n"
     "function that performs or needs anything, and the totals: functions\n"
     "seen, functions with an effect, fixpoint rounds, steps.\n"
     "\n"
     "Fix: declare fewer effects, cut the call graph the fixpoint walks, or\n"
     "raise the budget deliberately.\n"},
    {"T0001", "A function and its reference twin disagree",
     "`fn fast(...) -> R reference slow;` says the two compute the same\n"
     "thing, and the build checks it. The differential prober generates\n"
     "input sets from the parameter shapes plus the constants both\n"
     "functions compare against, runs both in the interpreter, and this\n"
     "pair disagreed. The message carries the input that shows it.\n"
     "\n"
     "This is a differential test and it says so: agreement is evidence,\n"
     "not equivalence. Disagreement is decisive, because one input where\n"
     "they differ is all a counterexample needs.\n"
     "\n"
     "The same check runs again after the optimizer under `--verify`, so a\n"
     "pass that breaks the fast one is caught by the reference the program\n"
     "already supplied.\n"
     "\n"
     "Fix: make them agree, or stop claiming they do.\n"},
    {"T0002", "A reference twin could not be checked",
     "The pair was declared and the prober could not generate inputs it\n"
     "could run: a parameter shape it has no values for, or a construct\n"
     "the interpreter cannot execute. The build goes on and this warning\n"
     "stands, because a check that did not run must not read as one that\n"
     "passed.\n"
     "\n"
     "`--report-twins` prints what each pair was checked on, so a pair that\n"
     "was checked on nothing is visible beside the pairs that were.\n"
     "\n"
     "Fix: narrow the parameters to shapes the prober can build, or accept\n"
     "that this pair is unchecked and know that it is.\n"},
    {"F0004", "A function declared @pure performs something",
     "`@pure` is a contract, and this build checked it. The function\n"
     "carries the decorator and its body, or something it calls, writes\n"
     "observable state: a global, memory through a pointer, an allocation,\n"
     "inline assembly, or a call the compiler cannot see into.\n"
     "\n"
     "The decorator buys no optimization. Purity is inferred by a\n"
     "whole-program fixpoint, and the loop-invariant call hoist reads only\n"
     "what that pass proved, so a pure function has its call hoisted\n"
     "whether or not anyone wrote `@pure` on it. What the decorator does is\n"
     "make the claim checkable: write it down and the build fails the day\n"
     "the body stops being pure.\n"
     "\n"
     "Fix: remove the write, or remove the decorator.\n"},
    {"P0003", "The declared-type prover spent more than its budget",
     "`--proof-budget=N` makes the cost of proving declared types a\n"
     "contract: the interval and guard steps the prover spends may not\n"
     "exceed N. They did.\n"
     "\n"
     "`--report-proofs` prints one line per conversion the prover settled:\n"
     "the type, the expression, the site, whether it was proven, what it\n"
     "cost, and which route settled it (a range, or a dominating test).\n"
     "\n"
     "Fix: declare fewer refined types on hot paths, narrow what they are\n"
     "computed from so the prover settles sooner, or raise the budget\n"
     "deliberately.\n"},
    {"P0002", "A bounds check was proven away by a declared type",
     "Not an error. `--explain` reports it: an index whose declared type\n"
     "pins its range inside the array it indexes needs no bounds check, so\n"
     "none was emitted, in debug, release and --safe builds alike. The\n"
     "proof is the type's predicate; the pass that consumed it is lowering,\n"
     "which decides check emission per access.\n"
     "\n"
     "A property the program declared and the compiler proved earns the\n"
     "same treatment as one the compiler discovered on its own, and no\n"
     "more: an index of a plain integer type keeps its check.\n"
     "\n"
     "The prover is code, so it is not believed on its own authority.\n"
     "`mettle test`, `mettle trace` and `--verify` re-check every proven\n"
     "conversion as the interpreter runs, and `--check-proofs` does the\n"
     "same inside a compiled program, in `--release` too.\n"},
};

/* ---- optimizer decision codes ----------------------------------------------
 * The `--explain` report tags every verdict with one of these, and the
 * `--explain-json` sidecar carries the same string in its `code` field. The
 * prose in the report is one line, because a report of forty one-liners is
 * readable and a report of forty paragraphs is not. The paragraph lives here.
 *
 * `group` sorts the index; `applies` is the one-line gloss the index shows. */

typedef enum {
  DECISION_VECTOR_REFUSAL,
  DECISION_INLINE_REFUSAL,
  DECISION_APPLIED
} DecisionGroup;

typedef struct {
  const char *code;
  DecisionGroup group;
  const char *title;
  const char *body;
} DecisionDoc;

static const DecisionDoc DECISIONS[] = {
    /* ---- vectorization refusals ------------------------------------------ */
    {"call-in-body", DECISION_VECTOR_REFUSAL,
     "The loop body calls a function",
     "A SIMD kernel runs eight lanes at once. A call runs one. So a loop that\n"
     "calls anything on every iteration cannot become a kernel until the call\n"
     "is gone.\n"
     "\n"
     "The inliner runs first and removes most of these on its own, so a loop\n"
     "that still reports this one has a call the inliner declined. The remark\n"
     "for that call, on the same line, says which rule stopped it.\n"
     "\n"
     "Fix: mark the callee @inline, or lift the call out of the loop when its\n"
     "result does not change between iterations.\n"},
    {"extern-call-in-body", DECISION_VECTOR_REFUSAL,
     "The loop body calls an external function",
     "The body calls a function declared `extern`. The compiler has no body\n"
     "for it, so it can never inline the call away, and the loop can never\n"
     "vectorize while the call is there.\n"
     "\n"
     "Fix: move the extern call out of the loop. Where the loop computes the\n"
     "values and the extern call only consumes them, split it in two: a\n"
     "vectorizable loop that fills a buffer, then one call.\n"},
    {"indirect-call", DECISION_VECTOR_REFUSAL,
     "The loop body calls through a function pointer",
     "The callee is not known until the loop runs, so there is nothing to\n"
     "inline and no kernel to match.\n"
     "\n"
     "Fix: where the pointer holds the same function for the whole loop,\n"
     "switch on it once outside and write a direct call in each arm. Each arm\n"
     "then vectorizes on its own.\n"},
    {"alloc-in-body", DECISION_VECTOR_REFUSAL,
     "The loop body allocates",
     "Every iteration runs `new`. Allocation is a call into the allocator, it\n"
     "can fail, and it has an order the lanes would have to keep.\n"
     "\n"
     "Fix: allocate once before the loop and reuse the buffer.\n"},
    {"inline-asm", DECISION_VECTOR_REFUSAL,
     "The loop body contains inline assembly",
     "The compiler treats an asm block as opaque. It cannot know which\n"
     "registers or memory the block touches, so it cannot prove the lanes are\n"
     "independent.\n"
     "\n"
     "Fix: move the asm out of the loop, or accept the scalar loop. Hand-written\n"
     "asm in a hot loop is usually already the vector code you wanted.\n"},
    {"control-flow", DECISION_VECTOR_REFUSAL,
     "The loop body branches",
     "The body has a nested loop or a data-dependent `if`. Lanes execute in\n"
     "lockstep, so a branch that goes one way for lane 3 and the other way for\n"
     "lane 4 has no single instruction to be.\n"
     "\n"
     "Note the compiler already if-converts short branches into masked\n"
     "arithmetic where it can (see `if-converted`). This code fires when the\n"
     "branch was too big or too effectful for that.\n"
     "\n"
     "Fixes:\n"
     "  - hoist the condition out when it does not depend on the index\n"
     "  - replace a small `if` with arithmetic: `x = c * a + (1 - c) * b`\n"
     "  - split one loop into two, each with a straight-line body\n"},
    {"early-exit", DECISION_VECTOR_REFUSAL,
     "The loop can leave before its trip count",
     "A `break` or an early `return` means the number of iterations is not\n"
     "known when the loop starts. A kernel that processes eight elements per\n"
     "step could run past the element the loop meant to stop at.\n"
     "\n"
     "Fix: where the search and the work are separable, do the work in a\n"
     "counted loop and the search in its own. A `break` on the last iteration\n"
     "costs nothing to keep scalar.\n"},
    {"int16-elements", DECISION_VECTOR_REFUSAL,
     "16-bit integer elements have no kernel",
     "The loop loads or stores int16/uint16. Mettle ships kernels for 8-bit\n"
     "and 32-bit integers and for both float widths, but not for 16-bit.\n"
     "\n"
     "Fix: widen the array to int32. The loop then runs eight lanes at a time\n"
     "instead of one, which more than pays for the extra memory in a hot loop.\n"
     "Where the values fit in a byte, int8 is faster still.\n"
     "\n"
     "This is a gap in the compiler, not a fact about your code. The report\n"
     "simulates the widening and tells you which kernel you would get.\n"},
    {"int64-elements", DECISION_VECTOR_REFUSAL,
     "64-bit integer elements have no kernel",
     "The loop loads or stores int64/uint64. There is no 64-bit integer kernel\n"
     "yet, so the loop stays scalar.\n"
     "\n"
     "Fix: use int32 where the range allows it. Four lanes per vector become\n"
     "eight. Where the values genuinely need 64 bits, this loop is at its\n"
     "floor for now.\n"},
    {"reloaded-base", DECISION_VECTOR_REFUSAL,
     "The base pointer is re-read every iteration",
     "The array the loop indexes is reached through a pointer stored in a\n"
     "struct: `t->counts[i] = 0`. The body writes through that pointer, and\n"
     "nothing rules out the write landing on the pointer field itself, so the\n"
     "compiler must re-read `t->counts` on every iteration. A kernel needs one\n"
     "base for the whole loop, so the loop stays scalar.\n"
     "\n"
     "Fix: read the pointer once into a local and index the local.\n"
     "\n"
     "    var counts: int32* = t->counts;\n"
     "    while (i < n) { counts[i] = 0; i += 1; }\n"
     "\n"
     "The local says the base does not change, which is what the loop meant.\n"
     "This is a gap in the compiler's alias reasoning, not a fact about your\n"
     "code.\n"},
    {"serial-recurrence", DECISION_VECTOR_REFUSAL,
     "The loop carries a serial recurrence",
     "A value is computed from its own previous value through an operation\n"
     "that does not reassociate: `*`, `/`, a shift, or a bitwise or xor op.\n"
     "Iteration 2 cannot start until iteration 1 has finished, so the lanes\n"
     "form a chain rather than eight independent pieces of work.\n"
     "\n"
     "This is the one refusal that is a fact about the algorithm rather than a\n"
     "gap in the compiler. A hash, a linear congruential generator, an IIR\n"
     "filter: the running state IS the algorithm, and the loop is already at\n"
     "its scalar floor.\n"
     "\n"
     "What does vectorize: `+` and `-` reductions. Those reassociate, so the\n"
     "compiler splits them into per-lane partial sums and adds the lanes at\n"
     "the end.\n"
     "\n"
     "Fix: none, unless the algorithm itself can change. Some hashes have a\n"
     "parallel form that hashes N independent streams and combines them.\n"},
    {"mixed-float-widths", DECISION_VECTOR_REFUSAL,
     "The loop mixes float32 and float64",
     "One vector register holds eight float32 or four float64. A loop that\n"
     "touches both would need two lane counts at once, and the conversion\n"
     "between them costs more than the kernel saves.\n"
     "\n"
     "Fix: pick one width for the whole loop. Convert on the way in or on the\n"
     "way out, outside the loop.\n"},
    {"byte-sum-narrow-acc", DECISION_VECTOR_REFUSAL,
     "Byte sum into an accumulator narrower than int64",
     "Summing bytes has a very fast kernel (`vpsadbw`, 32 bytes per step), but\n"
     "it accumulates into int64: 32 lanes of a byte each can carry further\n"
     "than 32 bits, and the kernel will not silently give you a different\n"
     "answer from the scalar loop.\n"
     "\n"
     "Example:\n"
     "    var total: int64 = 0;\n"
     "    for i in 0..n {\n"
     "        total = total + (int64)data[i];\n"
     "    }\n"
     "\n"
     "Fix: declare the accumulator int64. The report simulates that change,\n"
     "re-runs the optimizer, and only prints the advice once the loop really\n"
     "does vectorize with it.\n"},
    {"int32-sum-narrow-acc", DECISION_VECTOR_REFUSAL,
     "int32 sum into an accumulator narrower than int64",
     "Same rule as `byte-sum-narrow-acc`, one width up. The int32 sum kernel\n"
     "(`vpaddd`) folds eight lanes into an int64 accumulator, because eight\n"
     "int32 values summed can overflow int32 while the scalar loop, adding one\n"
     "at a time, would not have.\n"
     "\n"
     "Fix: declare the accumulator int64. The elements stay int32; only the\n"
     "running total widens.\n"},
    {"inlined-param-local", DECISION_VECTOR_REFUSAL,
     "An inlined parameter copy survived in the body",
     "The inliner left a `__inl_*` temporary inside the loop, and the\n"
     "recognizers see a local write per iteration where they expected a plain\n"
     "array access.\n"
     "\n"
     "This is a compiler limitation, not a problem in your code. Report the\n"
     "loop if you hit it: the shape that produced it is worth fixing.\n"},
    {"body-local", DECISION_VECTOR_REFUSAL,
     "A local is declared inside the loop body",
     "The recognizers match on a straight run of loads, arithmetic and stores.\n"
     "A local declared in the body adds a stack slot written every iteration,\n"
     "which none of the shapes cover.\n"
     "\n"
     "Fix: declare the variable before the loop and assign it inside, or fold\n"
     "the expression into the statement that uses it.\n"},
    {"dot-shape-address", DECISION_VECTOR_REFUSAL,
     "Dot product with an address the kernel cannot follow",
     "The loop is a float multiply-accumulate, which is exactly the FMA dot\n"
     "product kernel's shape, but the addresses do not match. The kernel needs\n"
     "each base to be a plain pointer indexed by the loop counter: `a[i]`, not\n"
     "`a[r * cols + i]`.\n"
     "\n"
     "Example, an inner product over one row of a matrix:\n"
     "    for r in 0..rows {\n"
     "        var row: float32* = &m[r * cols];   // hoisted, invariant here\n"
     "        for c in 0..cols {\n"
     "            acc = acc + row[c] * x[c];      // now `base[i]`\n"
     "        }\n"
     "    }\n"
     "\n"
     "Fix: lift the invariant part of the index into a pointer before the loop.\n"
     "The report checks first: when the other half of the index changes every\n"
     "iteration the access really is non-unit-stride, and the report says so\n"
     "instead of giving advice that cannot work.\n"},
    {"store-only-fill", DECISION_VECTOR_REFUSAL,
     "Fill loop the fill kernel could not claim",
     "The loop writes the same value over and over, which is the fill shape,\n"
     "but one of the kernel's requirements was not met. The remark names\n"
     "which, because the answer differs:\n"
     "\n"
     "  - Several stores in one body. The kernel fills one region per loop.\n"
     "    Split it into one loop per destination and each becomes a kernel.\n"
     "\n"
     "  - 1-byte elements. The kernel covers 2, 4 and 8 bytes. This is a gap\n"
     "    in the compiler; there is nothing to change in the loop.\n"
     "\n"
     "  - A stack array as the destination. `a[i]` on a local array recomputes\n"
     "    the array's address every iteration, and the kernel indexes off one\n"
     "    invariant base. Bind it once and write through the pointer:\n"
     "        var p: float32* = &a[0];\n"
     "        for i in 0..n { p[i] = 1.0; }\n"
     "    The generated code is the same either way; only the shape the\n"
     "    recognizer sees changes.\n"
     "\n"
     "  - An address the kernel cannot follow. It handles `a[i]`, `a[c + i]`\n"
     "    with `c` invariant, and a pointer walked by a constant stride. Lift\n"
     "    the invariant part of the index into a base pointer before the loop.\n"},
    {"extremum-shape", DECISION_VECTOR_REFUSAL,
     "Running minimum/maximum the kernel just missed",
     "`if (a[i] > best) { best = a[i]; }` is a shape the compiler vectorizes:\n"
     "it reads the diamond as the operator it is and emits vmaxps/vmaxpd (or\n"
     "vpmaxsd for int32), seeding every lane with the accumulator's incoming\n"
     "value. So the branch is not what stopped this loop. Something else did:\n"
     "\n"
     "  - The counter does not start at 0. A scan seeded from `a[0]` and run\n"
     "    from `i = 1` is outside the kernel, which treats the loop's bound as\n"
     "    an element count from the base. Start at 0 and seed the accumulator\n"
     "    with a sentinel instead.\n"
     "\n"
     "  - The elements are not float32, float64 or int32. Those are the lane\n"
     "    widths the extremum kernel carries, and the width it reads is the\n"
     "    ELEMENT's. A uint8 or int16 array stays scalar however the\n"
     "    accumulator is declared, so widening `best` alone moves nothing;\n"
     "    widen the array. uint32 is refused because vpmaxsd compares signed.\n"
     "\n"
     "  - The elements are narrower than the accumulator. `(int32)bytes[i]`\n"
     "    widens per element; the kernel's lanes are the element width. This\n"
     "    is a gap, not a problem with the loop.\n"
     "\n"
     "  - The body also stores. Then it is a clamp, not a reduction. See\n"
     "    `mettle explain clamp-store`.\n"},
    {"predicated-count", DECISION_VECTOR_REFUSAL,
     "An accumulator updated only on the taken arm",
     "Over int32 elements, `if (a[i] > t) { c = c + 1; }` and\n"
     "`if (a[i] < 0) { s = s + a[i]; }` both vectorize: the comparison already\n"
     "holds 0 or 1, so the compiler multiplies the addend by it and the body\n"
     "becomes straight line.\n"
     "\n"
     "This code fires when that rewrite does not apply. Three shapes it will\n"
     "not take:\n"
     "\n"
     "  - Float elements. There is no predicated float reduction, and the\n"
     "    branchless form has no kernel either, so there is nothing to change\n"
     "    in the loop.\n"
     "  - An else arm that also writes the accumulator. That is a select on a\n"
     "    loop-carried value, not an accumulate; there is no one addend.\n"
     "  - A guard on a computed value. The conversion reads a comparison of a\n"
     "    loaded element (`a[i] > t`, `a[i] == 0`); `if (a[i] & 6)` is a test\n"
     "    of an expression, and respelling it `(a[i] & 6) != 0` keeps it one.\n"
     "    Add the comparison rather than branching on it and the kernel takes\n"
     "    it, for any comparison:\n"
     "\n"
     "        c = c + ((a[i] & 6) != 0);\n"},
    {"clamp-store", DECISION_VECTOR_REFUSAL,
     "A value clamped or selected before it is stored",
     "`if (v > hi) { v = hi; }` before `a[i] = v` is a clamp, and over int32\n"
     "elements it vectorizes: the branch becomes vpminsd or vpmaxsd, or a\n"
     "lane select when the arms are not the two compared values. A chain of\n"
     "them, including the one an inlined helper's early returns leave behind,\n"
     "folds the same way.\n"
     "\n"
     "This code fires when that conversion did not carry the loop. Two things\n"
     "stop it: float32 and float64 elements, which have no select kernel yet,\n"
     "and a nest deep enough that the values live at once outnumber the ymm\n"
     "registers the kernel has, which is six for an int32 map. Three levels of\n"
     "`if`/`else` over distinct expressions is past that.\n"
     "\n"
     "Splitting the deepest arm into its own loop is what usually helps.\n"},
    {"strided-access", DECISION_VECTOR_REFUSAL,
     "The loop steps more than one element at a time",
     "`rgb[i * 3 + 1]`, `dst[i * 2]`, and any other non-unit stride. Every\n"
     "kernel walks its arrays one contiguous vector per iteration, so a\n"
     "strided access has no kernel to land in. There is no gather or\n"
     "scatter form.\n"
     "\n"
     "The stride is usually the data layout, not an accident, so there is\n"
     "often nothing to change. When the layout IS free, splitting an\n"
     "interleaved array into one array per component makes every loop over it\n"
     "unit-stride:\n"
     "\n"
     "    // instead of rgb[i*3+0], rgb[i*3+1], rgb[i*3+2]\n"
     "    r[i], g[i], b[i]        // three loops, all vectorized\n"},
    {"unbounded-shift", DECISION_VECTOR_REFUSAL,
     "A right shift whose input cannot be bounded",
     "Integer lanes are 32 bits wide. `+ - * & | ^ <<` are congruent mod 2^32,\n"
     "so the low 32 bits of a result depend only on the low 32 bits of its\n"
     "inputs and a lane reproduces them whatever width the scalar code used.\n"
     "`>>` is the exception: it reads bits back DOWN, so a lane that wrapped\n"
     "where the scalar did not would shift different bits in.\n"
     "\n"
     "The kernel takes a right shift only where the shifted value is provably\n"
     "inside int32, and a multiply is not provable on its own. This is refused:\n"
     "\n"
     "    dst[i] = (r[i]*77 + g[i]*150 + b[i]*29) >> 8;\n"
     "\n"
     "Masking bounds it, at the cost of one op per lane, and this vectorizes:\n"
     "\n"
     "    dst[i] = ((r[i]*77 + g[i]*150 + b[i]*29) & 65535) >> 8;\n"
     "\n"
     "Keep the destination int32. Narrowing the store to a `uint8*` puts the\n"
     "loop outside the kernels for a separate reason, so masking alone will\n"
     "not move it.\n"},
    {"variable-shift", DECISION_VECTOR_REFUSAL,
     "A shift by a distance read at run time",
     "The kernels carry a shift only when the distance is written in the\n"
     "source. A distance the loop reads has no kernel, in either direction:\n"
     "\n"
     "    b[i] = a[i] >> k;      // `k` a parameter or a variable\n"
     "    b[i] = a[i] << k;\n"
     "\n"
     "Both stay scalar. A constant distance vectorizes:\n"
     "\n"
     "    b[i] = a[i] >> 8;\n"
     "\n"
     "There is no source rewrite that reaches a kernel while the distance\n"
     "stays a run-time value, so this is a gap in the compiler rather than a\n"
     "problem with the loop. Splitting into one loop per distance works when\n"
     "the distances are few and known.\n"},
    {"unrecognized-shape", DECISION_VECTOR_REFUSAL,
     "No recognizer claimed this loop",
     "The honest fallback. The compiler ruled out every disqualifier it can\n"
     "detect and still found no kernel for the loop, so it says that rather\n"
     "than guessing at a cause.\n"
     "\n"
     "What does vectorize, as a checklist:\n"
     "  - unit-stride accesses, `a[i]` and not `a[i * k]`\n"
     "  - int8, int32, float32 or float64 elements\n"
     "  - a straight-line body: no calls, no branches, no allocation\n"
     "  - one of the known shapes: a map (`a[i] = expr`), a `+` reduction\n"
     "    (`s = s + expr`), a dot product, or a fill\n"
     "\n"
     "A loop that meets all of that and still lands here is worth reporting.\n"},

    /* ---- inlining refusals ----------------------------------------------- */
    {"callee-no-body", DECISION_INLINE_REFUSAL,
     "The callee has no body to inline",
     "The function is declared but not defined in this program: an `extern`,\n"
     "or a runtime entry point. There is nothing to copy into the caller.\n"
     "\n"
     "This is not a problem. A call to a C function or an OS API is a call.\n"},
    {"callee-noinline", DECISION_INLINE_REFUSAL,
     "The callee is marked @noinline",
     "An absolute veto. @noinline is not a hint, and no budget or heuristic\n"
     "overrides it.\n"
     "\n"
     "Fix: remove @noinline where inlining is what you want. Where it is on\n"
     "the function deliberately (to keep a call boundary for profiling, or to\n"
     "shape code layout), this remark is the compiler agreeing with you.\n"
     "\n"
     "The report simulates removing it and tells you what inlining would have\n"
     "unlocked, so the cost of the decorator is visible rather than assumed.\n"},
    {"callee-denylisted", DECISION_INLINE_REFUSAL,
     "The callee is on the compiler's inline denylist",
     "A small set of names is refused by hand, as a guard against compile-time\n"
     "blowup in shapes known to explode.\n"
     "\n"
     "Fix: mark the callee @inline to override the denylist. Watch compile\n"
     "time afterwards, since that is the cost the guard exists to prevent.\n"},
    {"too-many-parameters", DECISION_INLINE_REFUSAL,
     "The callee takes more than 16 parameters",
     "Past sixteen parameters the inliner's argument-binding work grows faster\n"
     "than the inlining is worth.\n"
     "\n"
     "Fix: pass a struct. That is usually the better interface anyway, and it\n"
     "brings the function back under the cap.\n"},
    {"callee-parameter-names", DECISION_INLINE_REFUSAL,
     "The callee's parameter names are unavailable",
     "The inliner binds arguments to parameters by name, and this callee\n"
     "reached the optimizer without them.\n"
     "\n"
     "A compiler limitation rather than anything in your code.\n"},
    {"callee-over-budget", DECISION_INLINE_REFUSAL,
     "The callee's body is over the inline size budget",
     "Inlining copies the body into every call site, so a large callee called\n"
     "in ten places makes the program larger and the instruction cache colder.\n"
     "The budget is the line the compiler draws, and --pgo moves that line for\n"
     "callees measured hot.\n"
     "\n"
     "Fixes:\n"
     "  - mark the callee @inline to override the budget\n"
     "  - compile with --pgo, so a measured-hot callee earns the larger budget\n"
     "    on evidence rather than on a decorator\n"
     "\n"
     "Before printing the @inline advice the report applies it to a scratch\n"
     "copy and re-runs the check, so it never suggests a decorator that a\n"
     "structural guard behind the budget would refuse anyway.\n"},
    {"callee-call-count", DECISION_INLINE_REFUSAL,
     "The callee makes more calls than the inline call-count budget allows",
     "The callee's body is small enough, but it is mostly calls to other\n"
     "functions: a glue routine that orchestrates helpers. Inlining one of\n"
     "those copies the orchestration into the caller and gains nothing, since\n"
     "the calls it makes are still calls.\n"
     "\n"
     "Fix: mark the callee @inline to override the cap, where the caller\n"
     "really does benefit from seeing through the glue.\n"},
    {"callee-inline-asm", DECISION_INLINE_REFUSAL,
     "The callee contains inline assembly",
     "An asm block can depend on the frame it was written for. Copying it into\n"
     "another function is not safe in general, so the inliner does not.\n"
     "\n"
     "A structural guard: @inline does not override it.\n"},
    {"callee-has-loop", DECISION_INLINE_REFUSAL,
     "The callee's loop body is over the inline size budget",
     "A small loop-bearing callee inlines, because that is what exposes its\n"
     "loop to values the caller already holds in registers. Past the budget\n"
     "the call is noise next to the loop it reaches, and the caller growth\n"
     "is not repaid. The budget doubles at sites two or more loops deep.\n"
     "\n"
     "@inline inlines it anyway. Where the loop is hot, look at its own\n"
     "remark first: that is usually where the time goes.\n"},
    {"callee-no-return",  DECISION_INLINE_REFUSAL,
     "The callee has no return instruction to rewrite",
     "Inlining works by rewriting the callee's returns into assignments in the\n"
     "caller. A body with no return has nothing to rewrite. A function that\n"
     "only ever traps or loops forever looks like this.\n"
     "\n"
     "A structural guard: @inline does not override it.\n"},
    {"callee-has-kernel", DECISION_INLINE_REFUSAL,
     "The callee's loops became SIMD kernels",
     "Not a refusal to worry about. The callee's loops vectorized, which\n"
     "happens after inlining runs, so the call survives to the end of the\n"
     "pipeline and gets reported here.\n"
     "\n"
     "The kernel runs at the same speed either way. One call to reach it is\n"
     "not measurable against the loop it contains.\n"},
    {"recursive", DECISION_INLINE_REFUSAL,
     "The call is directly recursive",
     "A function calling itself cannot be inlined without bound. Mettle\n"
     "expands bounded self-recursion automatically where it can prove the\n"
     "depth; this remark is what is left over.\n"
     "\n"
     "Fix: rewrite as a loop where the recursion is hot and the depth is not\n"
     "provable. A loop also gives the vectorizer something to work with.\n"},
    {"caller-over-budget", DECISION_INLINE_REFUSAL,
     "The calling function is over its budget and this site is cold",
     "The caller has grown past the point where more inlining pays, and this\n"
     "particular call site is neither inside a loop nor measured hot. It runs\n"
     "at most once per call of the caller, so leaving it a real call costs\n"
     "nothing you can measure.\n"
     "\n"
     "Sites exempt from this rule, and so still inlined: calls inside loops,\n"
     "sites measured hot under --pgo, tiny call-free callees, and callees\n"
     "marked @inline.\n"
     "\n"
     "There is deliberately no fix advice. For a cold one-shot site, not\n"
     "inlining is the right answer.\n"},
    {"argument-count", DECISION_INLINE_REFUSAL,
     "The call's argument count does not match the callee",
     "The site passes a different number of arguments than the callee declares,\n"
     "or more than the inliner handles. A variadic call looks like this.\n"
     "\n"
     "A structural guard: @inline does not override it.\n"},
    {"rounds-exhausted", DECISION_INLINE_REFUSAL,
     "Inlining rounds ran out before this call was revisited",
     "The callee passes every check. The call site only appeared late, exposed\n"
     "by an earlier round of inlining, and the round limit was reached before\n"
     "the inliner came back to it.\n"
     "\n"
     "Fix: mark the callee @inline, which raises its priority. Deep call\n"
     "chains hit this most: each level of nesting costs a round.\n"},

    /* ---- applied optimizations ------------------------------------------- */
    {"vectorized", DECISION_APPLIED,
     "The loop became a SIMD kernel",
     "The whole loop now runs as vector instructions, several elements per\n"
     "step. The remark names the kernel and the width, for example\n"
     "`vfmadd231ps, 8-wide float32`, so you can check that the width matches\n"
     "the element type you meant to use.\n"},
    {"vectorized-inner", DECISION_APPLIED,
     "The inner loop of a nest became a kernel",
     "The inner loop vectorized and the outer loop stays scalar, driving it.\n"
     "This is the normal outcome for a nest, and usually the one you want: the\n"
     "inner loop is where the elements are.\n"},
    {"outer-of-nest", DECISION_APPLIED,
     "The outer loop of a nest, reported for context",
     "Recorded so the report accounts for every loop rather than going quiet\n"
     "about the outer one. Only innermost loops vectorize. The verdict that\n"
     "matters is on the inner loop's own remark.\n"},
    {"eliminated", DECISION_APPLIED,
     "The loop was removed",
     "The optimizer proved the loop had no effect anyone could observe, or\n"
     "folded it to a constant, so it emits nothing at all.\n"
     "\n"
     "Worth a glance in a benchmark. A loop that was meant to measure\n"
     "something and got eliminated measures nothing.\n"},
    {"rewrite-applied", DECISION_APPLIED,
     "An expression was rewritten by a `rewrite` rule",
     "The expression had the shape of a rule's `from` side, the rule's\n"
     "`where` guard (if any) held on the constants at this site, and the\n"
     "expression became the rule's `to` side. The function was then executed\n"
     "before and after the change on generated inputs, and only a match in\n"
     "every observation lets the rewrite stand.\n"},
    {"rewrite-rule-checked", DECISION_APPLIED,
     "A `rewrite` rule's two sides were shown to agree",
     "Before any rule is applied, its `from` and `to` sides are executed in\n"
     "the compile-time interpreter on generated inputs: the fixed table the\n"
     "translation validator uses, the constants the rule itself compares\n"
     "against, a set of awkward integers (negative, one past each power of\n"
     "two, the extremes of every width), and, for rules with two or three\n"
     "integer parameters, every combination of those. A rule with a `where`\n"
     "clause is checked on the inputs the clause admits, and the remark says\n"
     "how many that was. Disagreement on any input is a compile error with\n"
     "the input printed.\n"},
    {"rewrite-rule-applied", DECISION_APPLIED,
     "How many expressions a `rewrite` rule rewrote",
     "The count over the whole program, so a rule written for a hot loop can\n"
     "be seen to have reached it.\n"},
    {"rewrite-rule-unused", DECISION_VECTOR_REFUSAL,
     "A `rewrite` rule matched nothing",
     "No expression had the shape of the rule's `from` side at any point the\n"
     "compiler looked: before inlining, when calls are still calls, and after\n"
     "it, when arithmetic has been folded and propagated. The rule costs\n"
     "nothing, but it is doing nothing.\n"
     "\n"
     "The usual causes: the rule's parameter types differ from the code's (a\n"
     "rule over int64 does not match int32 arithmetic), or the compiler's own\n"
     "folding reached the expression first.\n"},
    {"rewrite-rule-guard-undecided", DECISION_VECTOR_REFUSAL,
     "A `rewrite` rule matched, but its guard needed a runtime value",
     "A `where` clause is evaluated at compile time, on the constants bound at\n"
     "the match site. Every match bound a parameter the guard reads to a\n"
     "value only known at run time, so the guard could not be decided and the\n"
     "rule did not apply.\n"
     "\n"
     "Fix: if the rule holds for every value, drop the guard. If it does not,\n"
     "the guard is doing its job, and only sites with constant arguments\n"
     "qualify.\n"},
    {"rewrite-rule-guard-false", DECISION_VECTOR_REFUSAL,
     "A `rewrite` rule matched, but its guard was false",
     "Every match bound constants on which the `where` clause evaluated to\n"
     "false. The rule declined correctly; nothing to change.\n"},
    {"inlined", DECISION_APPLIED,
     "The call was inlined",
     "The callee's body was copied into the caller and the call is gone. This\n"
     "is also what makes the surrounding loop vectorizable, since a call in\n"
     "the body would have blocked it.\n"},
    {"unrolled", DECISION_APPLIED,
     "The loop was unrolled",
     "The body was replicated so fewer iterations do more work each, which\n"
     "removes counter arithmetic and branch overhead. A loop with a constant\n"
     "trip count small enough is unrolled fully, and then no loop is left.\n"},
    {"hoisted", DECISION_APPLIED,
     "A call was lifted out of the loop",
     "The callee is @pure and its arguments do not change across iterations,\n"
     "so the result cannot change either. It is computed once before the loop.\n"
     "\n"
     "This is what @pure buys. Without it the compiler must assume the call\n"
     "could do anything, and must repeat it.\n"},
    {"if-converted", DECISION_APPLIED,
     "A branch became branchless arithmetic",
     "A short `if` in the body was rewritten as a select or a mask, so both\n"
     "sides are computed and one is chosen. That removes a mispredictable\n"
     "branch, and it is what lets a loop with a small conditional vectorize.\n"},
    {"prefetched", DECISION_APPLIED,
     "A prefetch was inserted",
     "The loop walks memory with a stride the compiler can see ahead of, so it\n"
     "asks for the line some iterations early. This hides part of the memory\n"
     "latency on a loop whose limit is bandwidth rather than arithmetic.\n"},
    {"layout-optimized", DECISION_APPLIED,
     "A data layout was reshaped",
     "Fields or elements were reordered so the accesses in the hot loop fall\n"
     "in the same cache lines. Nothing in the program's meaning changes.\n"},
    {"noalloc-verified", DECISION_APPLIED,
     "The @noalloc contract holds",
     "The function is marked @noalloc, and the compiler walked everything it\n"
     "can reach and found no allocation on any path. The contract is proved,\n"
     "not assumed. A violation would have been a compile error, not a warning.\n"},
};

#define DOCS_COUNT (sizeof(DOCS) / sizeof(DOCS[0]))
#define DECISIONS_COUNT (sizeof(DECISIONS) / sizeof(DECISIONS[0]))

static const char *decision_group_title(DecisionGroup group) {
  switch (group) {
  case DECISION_VECTOR_REFUSAL:
    return "Why a loop did not vectorize";
  case DECISION_INLINE_REFUSAL:
    return "Why a call was not inlined";
  case DECISION_APPLIED:
    return "What the optimizer applied";
  }
  return "Optimizer decisions";
}

static void explain_json_string(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
    switch (*p) {
    case '"':
      fputs("\\\"", stdout);
      break;
    case '\\':
      fputs("\\\\", stdout);
      break;
    case '\n':
      fputs("\\n", stdout);
      break;
    case '\r':
      fputs("\\r", stdout);
      break;
    case '\t':
      fputs("\\t", stdout);
      break;
    default:
      if (*p < 0x20) {
        printf("\\u%04x", *p);
      } else {
        putchar((int)*p);
      }
    }
  }
  putchar('"');
}

static const char *decision_group_slug(DecisionGroup group) {
  switch (group) {
  case DECISION_VECTOR_REFUSAL:
    return "vectorization-refusal";
  case DECISION_INLINE_REFUSAL:
    return "inline-refusal";
  case DECISION_APPLIED:
    return "applied";
  }
  return "other";
}

static void print_code_json(void) {
  printf("{\"schema\":1,\"codes\":[");
  for (size_t i = 0; i < DOCS_COUNT; i++) {
    printf("%s{\"code\":", i ? "," : "");
    explain_json_string(DOCS[i].code);
    printf(",\"kind\":\"diagnostic\",\"group\":\"diagnostic\",\"groupTitle\":");
    explain_json_string("Compile diagnostics");
    printf(",\"title\":");
    explain_json_string(DOCS[i].title);
    printf(",\"body\":");
    explain_json_string(DOCS[i].body);
    printf("}");
  }
  for (size_t i = 0; i < DECISIONS_COUNT; i++) {
    printf(",{\"code\":");
    explain_json_string(DECISIONS[i].code);
    printf(",\"kind\":\"decision\",\"group\":");
    explain_json_string(decision_group_slug(DECISIONS[i].group));
    printf(",\"groupTitle\":");
    explain_json_string(decision_group_title(DECISIONS[i].group));
    printf(",\"title\":");
    explain_json_string(DECISIONS[i].title);
    printf(",\"body\":");
    explain_json_string(DECISIONS[i].body);
    printf("}");
  }
  printf("]}\n");
}

static void print_code_list(void) {
  printf("Diagnostic codes (compile errors and warnings):\n");
  for (size_t i = 0; i < DOCS_COUNT; i++) {
    printf("  %-22s %s\n", DOCS[i].code, DOCS[i].title);
  }

  const DecisionGroup groups[] = {DECISION_VECTOR_REFUSAL,
                                  DECISION_INLINE_REFUSAL, DECISION_APPLIED};
  for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
    printf("\n%s (--explain decision codes):\n",
           decision_group_title(groups[g]));
    for (size_t i = 0; i < DECISIONS_COUNT; i++) {
      if (DECISIONS[i].group == groups[g]) {
        printf("  %-22s %s\n", DECISIONS[i].code, DECISIONS[i].title);
      }
    }
  }

  printf("\nRun `mettle explain <CODE>` for details on any of them, for "
         "example:\n");
  printf("  mettle explain E0004\n");
  printf("  mettle explain dot-shape-address\n");
}

/* Strip the punctuation a code picks up when it is pasted out of a report:
 * brackets from `[E0004]`, backticks and quotes from prose, and a trailing
 * comma or period. Case is left alone; the callers fold it themselves. */
static void normalize_code(const char *code, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = code; *p && n + 1 < cap; p++) {
    if (*p == '[' || *p == ']' || *p == '`' || *p == '\'' || *p == '"' ||
        *p == ',' || *p == '.') {
      continue;
    }
    out[n++] = *p;
  }
  out[n] = '\0';
}

static int code_equal_fold(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (char)(cb - 'A' + 'a');
    }
    /* '_' and '-' are the same separator as far as a reader is concerned. */
    if (ca == '_') {
      ca = '-';
    }
    if (cb == '_') {
      cb = '-';
    }
    if (ca != cb) {
      return 0;
    }
  }
  return *a == '\0' && *b == '\0';
}

/* Does `code` contain `fragment`, ignoring case and treating '_' as '-'? */
static int code_contains_fold(const char *code, const char *fragment) {
  size_t n = strlen(fragment);
  size_t len = strlen(code);
  if (n == 0 || n > len) {
    return 0;
  }
  for (size_t start = 0; start + n <= len; start++) {
    size_t k = 0;
    for (; k < n; k++) {
      char a = code[start + k], b = fragment[k];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a == '_') {
        a = '-';
      }
      if (b == '_') {
        b = '-';
      }
      if (a != b) {
        break;
      }
    }
    if (k == n) {
      return 1;
    }
  }
  return 0;
}

/* Edit distance, capped at a small band: the suggestion is only worth making
 * when the input is close, so anything past `limit` returns limit + 1. */
static size_t edit_distance(const char *a, const char *b, size_t limit) {
  size_t la = strlen(a), lb = strlen(b);
  if (la > lb ? la - lb > limit : lb - la > limit) {
    return limit + 1;
  }
  size_t row[128];
  if (lb + 1 > sizeof(row) / sizeof(row[0])) {
    return limit + 1;
  }
  for (size_t j = 0; j <= lb; j++) {
    row[j] = j;
  }
  for (size_t i = 1; i <= la; i++) {
    size_t diagonal = row[0];
    row[0] = i;
    for (size_t j = 1; j <= lb; j++) {
      size_t previous = row[j];
      char ca = a[i - 1], cb = b[j - 1];
      if (ca >= 'A' && ca <= 'Z') {
        ca = (char)(ca - 'A' + 'a');
      }
      if (cb >= 'A' && cb <= 'Z') {
        cb = (char)(cb - 'A' + 'a');
      }
      size_t substitute = diagonal + (ca == cb ? 0 : 1);
      size_t insert = row[j - 1] + 1;
      size_t remove = row[j] + 1;
      size_t best = substitute < insert ? substitute : insert;
      row[j] = best < remove ? best : remove;
      diagonal = previous;
    }
  }
  return row[lb];
}

/* The closest known code to `code`, or NULL when nothing is close enough. */
static const char *nearest_code(const char *code) {
  const char *best = NULL;
  size_t best_distance = 4; /* anything further away is not a typo */

  for (size_t i = 0; i < DECISIONS_COUNT; i++) {
    size_t d = edit_distance(code, DECISIONS[i].code, best_distance);
    if (d < best_distance) {
      best_distance = d;
      best = DECISIONS[i].code;
    }
  }
  for (size_t i = 0; i < DOCS_COUNT; i++) {
    size_t d = edit_distance(code, DOCS[i].code, best_distance);
    if (d < best_distance) {
      best_distance = d;
      best = DOCS[i].code;
    }
  }
  return best;
}

int mettle_explain_error_code(const char *code) {
  if (!code || strcmp(code, "list") == 0 || strcmp(code, "all") == 0) {
    print_code_list();
    return 0;
  }
  /* site/explain/ is generated from these tables, so a page cannot drift
   * from what the compiler says in the terminal. */
  if (strcmp(code, "--json") == 0) {
    print_code_json();
    return 0;
  }

  char normalized[128];
  normalize_code(code, normalized, sizeof(normalized));

  for (size_t i = 0; i < DOCS_COUNT; i++) {
    if (code_equal_fold(DOCS[i].code, normalized)) {
      printf("%s: %s\n\n%s", DOCS[i].code, DOCS[i].title, DOCS[i].body);
      return 0;
    }
  }

  for (size_t i = 0; i < DECISIONS_COUNT; i++) {
    if (code_equal_fold(DECISIONS[i].code, normalized)) {
      printf("%s: %s\n", DECISIONS[i].code, DECISIONS[i].title);
      printf("(%s, reported by --explain)\n\n%s",
             decision_group_title(DECISIONS[i].group), DECISIONS[i].body);
      return 0;
    }
  }

  /* A fragment of a code, which is what anyone types when they remember the
   * distinctive half of it. One hit resolves; several list, since guessing
   * between them would be worse than showing the choice. */
  if (strlen(normalized) >= 3) {
    const DecisionDoc *only = NULL;
    size_t hits = 0;
    for (size_t i = 0; i < DECISIONS_COUNT; i++) {
      if (code_contains_fold(DECISIONS[i].code, normalized)) {
        only = &DECISIONS[i];
        hits++;
      }
    }
    if (hits == 1) {
      printf("%s: %s\n", only->code, only->title);
      printf("(%s, reported by --explain)\n\n%s",
             decision_group_title(only->group), only->body);
      return 0;
    }
    if (hits > 1) {
      printf("'%s' matches %zu codes:\n", code, hits);
      for (size_t i = 0; i < DECISIONS_COUNT; i++) {
        if (code_contains_fold(DECISIONS[i].code, normalized)) {
          printf("  %-22s %s\n", DECISIONS[i].code, DECISIONS[i].title);
        }
      }
      return 0;
    }
  }

  fprintf(stderr, "error: unknown code '%s'\n", code);
  if (normalized[0] == 'R' && normalized[1] >= '1' && normalized[1] <= '9') {
    fprintf(stderr,
            "help: a code above R0999 belongs to a rule the program wrote, "
            "and the text lives on the declaration: run `mettle explain %s "
            "<file.mettle>`\n",
            normalized);
    return 1;
  }
  const char *near = nearest_code(normalized);
  if (near) {
    fprintf(stderr, "help: did you mean `%s`? Run `mettle explain %s`\n", near,
            near);
  } else {
    fprintf(stderr, "help: run `mettle explain list` for the full index\n");
  }
  return 1;
}
