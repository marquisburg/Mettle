# Memory safety

What the compiler proves about memory before your program runs, and what
`--safe` checks while it runs.

Two rules shape all of it. The compiler reports only what it can prove, so a
diagnostic here is a fact rather than a suspicion. And it never rejects a
program for want of an annotation: there is nothing to write, and nothing to
silence.

## Compile-time diagnostics

These run on every build. Each is reported once, at the line that does the
damage, with the line that set it up named in the message.

| Code | What it catches |
|------|-----------------|
| M0101 | Use after free |
| M0102 | Double free |
| M0103 | Returning the address of a stack local |
| M0104 | Storing a stack address in a global |
| M0105 | Constant array index out of bounds |
| M0106 | A memory operation that overflows a stack array |
| M0107 | Memory leak |
| M0108 | Use after a pointer freed by a call |
| M0109 | Double free through a call |
| M0110 | A borrowed interior pointer outliving its scope |
| M0111 | A borrowed pointer invalidated by realloc |
| M0112 | A borrowed pointer invalidated by free |
| M0113 | Dereference of a null pointer |
| M0114 | Dereference of an unmapped constant address |
| M0117 | A loop index running past the end of an array |

`mettle explain M0101` prints the reasoning and the fix for any of them.

### Use after free and double free

```mettle
var p: int32* = malloc(16);
free(p);
free(p);
```

```text
warning[M0102]: Double free of `p` (already freed at line 6)
```

Reading through the pointer instead gives:

```text
warning[M0101]: Use of `p` after it was freed (freed at line 6); this is
use-after-free
```

Both are conservative. They fire when the freed pointer and the second use are
provably the same allocation on the same path.

### Escaping stack addresses

Returning the address of a local, or of a field inside one, fails the build:

```mettle
fn leak() -> int32* {
  var p: P;
  return &p.x;
}
```

```text
error[M0103]: Returning the address of stack local `p`; the frame is destroyed
when this function returns, so the caller receives a dangling pointer
```

Storing one in a global is the same mistake with a longer fuse, and it draws
M0104.

### Leaks

M0107 fires when an allocation never escapes the function, is never returned,
stored, or passed on, and is never freed. It stays quiet when ownership leaves
the function, because then the leak is somebody else's to prove.

### Constant out-of-bounds

An index the compiler can fold is checked against the array's declared size:

```mettle
var a: int32[4];
a[7] = 1;
```

```text
error[E0003]: Array index 7 is out of bounds for 'int32[4]' (size 4)
```

M0117 covers the loop version, where the bound and the length are both known
and the bound is larger:

```mettle
var a: int64[8];
for i in 0..9 { a[i] = (int64)i; }
```

```text
error[M0117]: This loop runs `i` up to 8, but `a` has 8 elements (valid
indexes 0..7); the final iteration reads or writes past the end
```

The same three spellings are covered: `for i in 0..9`, `for i in 0..=8`, and
the `while` loop they desugar to. The check needs a constant start, a constant
bound, one `i = i + 1`, and no way out of the loop early, so anything it cannot
prove it leaves alone.

## Runtime checks with `--safe`

```bash
mettle --safe --release --build program.mettle
```

`--safe` keeps bounds checks in release builds. Each tracked allocation has an
identity that includes a generation. The compiler carries that identity beside
values, leaving source syntax, native pointer width, struct layout and calling
conventions intact. Checks use the original allocation's bounds and lifetime.
Reusing an address does not make a stale identity valid again.

The compiler carries identities through scalar copies, pointer arithmetic,
integer conversions, fields, byte copy loops, `memcpy`, `memmove`, compiled
calls, recursion and function pointers. Aggregate arguments and results carry
byte metadata through a separate call frame. Return metadata is saved before
stack records are retired. Static pointer initializers and string literals
receive metadata too.

Known allocator calls register new regions and retire freed regions. Taking a
known allocator's address selects a checked wrapper with the same signature.
The checks reject double free, interior free and free of stack or global memory.
Successful `realloc` creates a new identity even if the address stays the same.
A failed resize with a nonzero size keeps the old identity live. A zero size
retires it, matching the owned allocators.

Checks reject null access, an unknown identity, a dead identity and an access
outside the original allocation. Exact bounds also apply when two live objects
share a registry granule. Metadata allocation failure and unsupported address
ranges stop the program rather than silently skipping protection. A failed
check reports the access and exits with status 1.

### What survives, and what it costs

A check that cannot fail is deleted. What is left is resolved into one of
three shapes, cheapest first.

- A comparison against an extent the program already states.
- One check in front of a loop, covering the whole range the loop walks.
  The counted span saturates rather than wrapping, so a trip count that
  cannot be bounded reaches the check as an oversized range and not as an
  empty one. The failure still names the first element that left the
  allocation, not the offset the loop started from.
- One resolution in front of a loop the analysis cannot settle, and a
  comparison at each access against what it resolved. A resolution comes
  back empty when the origin names nothing live, and that case is sent to
  the full check: reading it as a limit would let every access in the loop
  through, which is where an escape costs the most.

Everything else asks the runtime which allocation the pointer came from.
`--explain` reports the split for a build, and names the reason each
survivor survived.

The `main` a program declares takes its arguments from the process rather
than from a compiled caller, so no call frame carries their origins. The
argument vector and each of its strings are described at entry instead,
which is what makes indexing either one a checked access.

## Foreign memory

An unknown C pointer has no bounds or lifetime that the compiler can verify.
Dereferencing it under `--safe` traps. A caller that knows the foreign owner's
contract can describe a borrowed region through `std/mem`:

```mettle
memory_region(pointer, byte_count);
// Use pointer while the foreign owner keeps these bytes live.
memory_region_end(pointer);
```

`memory_region` asserts that the whole range exists and stays live. It does not
validate the foreign allocation. `memory_region_end` retires the record and
leaves the actual release to its owner. Keep the original address for ending
the region. Describe the region before making aliases that need its identity.
Declaring the exact same live range keeps its identity. A different range gets
its own record. Replacing a record at the same start address retires its old
identity, so use this API for foreign memory whose contract you control.

This API is a trust boundary. A false extent or a foreign owner that frees the
memory too soon can defeat the checks. Foreign code and assembly still control
their own memory operations. Rebuild Mettle dependencies with `--safe` to carry
identities through their calls; an unchecked library does not supply metadata.

## Scope and remaining limits

These checks do not yet establish complete memory safety for every program.

| Case | Current limit |
|------|---------------|
| Concurrent access and free | Locks protect the registry and byte metadata. They do not hold the allocation live between the check and the machine access. The program must synchronize its accesses and lifetimes. |
| Concurrent value and metadata writes | The machine store and its metadata update are separate operations. Data races can make them disagree. |
| Foreign code, assembly and callbacks | Their accesses and lifetime changes need a trusted contract. A callback entered from unchecked code has no argument identities unless it establishes valid regions. |
| Foreign pointer reconstruction | C can transform or copy a pointer without supplying its origin. A later checked dereference rejects the unknown result. |
| Allocation size arithmetic | Arithmetic still follows the core language. A size expression can wrap before it reaches the allocator. Checks use the resulting allocation extent. |
| Other targets | The regression matrix covers native Windows and Linux on x64. It does not establish equivalent coverage for other backends. |

### What it costs

The compiler still removes checks it can prove unnecessary. What is left is not
cheap. Carrying an origin beside a value means a metadata read for every load
that could be part of a pointer, and a byte loaded from the heap qualifies:
reassembling a pointer one byte at a time is a thing programs do, and the
checks would be defeated by refusing to follow it.

Measured on this repository's example kernels, each reporting its own inner
loop rather than process time, `--release --safe` against `--release`:

| Kernel | Plain | Checked | Ratio |
|--------|------:|--------:|------:|
| heapsort | 23.8 ms | 377.9 ms | 16x |
| base64_encode | 47.2 ms | 1.59 s | 34x |
| sort_insertion | 9.7 ms | 830.7 ms | 85x |
| binary_search | 7.0 ms | 2.51 s | 359x |
| crc32 | 2.3 ms | 1.04 s | 457x |

The worst of these walk a heap buffer a byte at a time, and each byte asks the
runtime what allocation it came from. The analysis that removes those questions
proves an allocation carries no origins only when it can see the whole graph,
so a buffer that arrives from a library function keeps them all. Two kernels,
`aos_sum` and `transpose`, run faster checked than plain, which is not a claim
about the checks: `--safe` runs a scalar analysis stage that `--release` alone
does not, and these two benefit from it.

Treat `--safe` as a mode for testing and for programs where the guarantee is
worth the factor, not as a release default. Earlier timing claims for the
address registry alone do not measure this implementation.

## Tests

Run the focused compiler matrix from the repository root:

```bash
python tests/run_safety_identity.py --compiler bin/mettle.exe
```

On Linux, pass the Linux compiler path. The matrix checks valid results and the
expected trap messages in debug and release builds with both allocators.

```bash
python tests/run_safety_optimizer.py --compiler bin/mettle.exe
```

This one holds the resolution to what it resolved before. It checks that the
two vectorized fixtures still vectorize under `--safe --release`, that the
targeted regressions still trap, and that every source in a fixed corpus
reports the same diagnostic, character for character, that it reported when the
corpus was recorded. A resolution that quietly stops proving what it used to
prove costs speed; one that quietly stops checking what it used to check looks
identical from outside, which is what the corpus is for.

`tests/safety_runtime_test.c` separately checks registry bounds, generations,
metadata copies, allocation failure, call frames and the counted-span helper.
The Windows harness also checks thread isolation and concurrent registry
replacement. Both runners are cases in `tests/run_tests.ps1`, so a full suite
run covers them.

## See also

- [Borrow checker](borrow-checker.md)
- [Heap allocation](heap-allocation.md)
- [Diagnostics](diagnostics.md)
