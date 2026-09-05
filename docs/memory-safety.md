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

## Run-time checks with --safe

`--safe` keeps bounds checks under `--release` and checks instrumented pointer
accesses against registered heap, stack, and global storage. It also detects
null access and use of a freed region while that region still has a record.
It does not provide a complete memory safety guarantee.

```bash
mettle --safe --build program.mettle
```

A failed check stops the program and names the access:

```text
Fatal error: `a[]` is outside its bounds
```

The process exits with status 1.

Some checks cost nothing, because the compiler removes the ones it can settle
statically. It recognizes an index written as a multiple of a loop counter plus
an invariant plus a constant, and proves that shape in bounds against the
array's length. A loop over `0..n` indexing an array of `n` elements gets no
checks at all.

What is left is the accesses that genuinely depend on run-time values.
Remaining pointer checks copy the allocation record under the registry lock.
That prevents checks from reading fields while another thread reuses a record.
It adds work and may cause contention; earlier timing results for the runtime
without that lock do not measure this version.

The runtime stops if it cannot allocate safety metadata or describe the
requested address range. It does not silently omit the record. A failed
`realloc` with a nonzero size leaves the old record live. A zero size retires
it, matching the owned allocators. Allocator check suppression belongs to the
current thread, including on Windows builds that lack a native TLS directory.

## What is not covered

The compiler proves what it can see. The runtime has these limits:

| Case | Limit |
|------|-------|
| Unregistered memory from C or a custom allocator | Unknown addresses pass through the registry check. Pointer type alone does not supply an extent. |
| Address reuse | An old pointer can match a new allocation at the same address. This includes stack frame reuse and realloc that keeps its address. |
| Lost base address | A derived pointer that reaches unregistered storage can lose the original bounds. A machine pointer carries no separate allocation identity. |
| Objects sharing a registry granule | Conflicting live records leave that granule unchecked. The compiler aligns registered stack and global objects to avoid this case. |
| Concurrent free and access | The registry lock protects metadata, not the program's access after the check. Program data races remain unsafe. |
| C code and assembly | The compiler cannot insert checks into their memory operations. |
| Integer overflow | Language arithmetic still wraps. Allocation size expressions can overflow before registration. |

Complete protection needs bounds and lifetime identity to follow pointers
through copies, fields, calls, and conversions, plus a defined policy for
foreign memory and concurrent access. The current address registry does not
carry that identity. `--safe` adds useful checks without changing syntax,
pointer layout, or the C ABI; it does not prove arbitrary pointer use safe.

## See also

- [Borrow checker](borrow-checker.md)
- [Heap allocation](heap-allocation.md)
- [Diagnostics](diagnostics.md)
