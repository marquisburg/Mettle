#ifndef METTLE_RUNTIME_SAFETY_H
#define METTLE_RUNTIME_SAFETY_H

/* Runtime half of Mettle's checked-access memory safety.
 *
 * The compiler emits a check at every access it cannot prove safe, and the
 * optimizer deletes the ones it can prove. What survives lands here. The
 * runtime checks whether the byte range lies inside the current live record
 * for the base address. It does not track a pointer's allocation generation.
 *
 * Answering it needs a map from an address to the allocation that owns it.
 * That map is a three-level table over the address space, one 32-bit region id
 * per 16-byte granule. Region id zero means untracked memory, which passes.
 * Freed records keep their ids until new records replace them. Ids
 * index a descriptor array holding the allocation's start and length, which is
 * what makes the bounds answer exact rather than approximate: an access that
 * runs off one live allocation into the next is still a violation, because the
 * check compares against the descriptor the BASE pointer resolved to, not
 * against whatever the final address happens to land in.
 *
 * The shadow map costs a quarter of the address space it covers, and it is
 * paid only for memory the program actually registers. That is the price of an
 * exact answer with no allocator surgery and no change to pointer layout: a
 * Mettle pointer under --safe is an ordinary machine pointer, so the ABI, the
 * struct layouts and every foreign call stay exactly as they were.
 *
 * Memory the program never registers reads as unowned, and an access to it is
 * allowed rather than trapped. Foreign libraries hand back pointers Mettle did
 * not allocate and cannot describe; trapping on them would reject correct
 * programs, which is the one thing this design refuses to do.
 *
 * Heap blocks are described as they are allocated, globals once at start-up,
 * and a stack local for as long as its frame is alive. Locals are described
 * only where a pointer to one actually leaves the function, because indexing
 * one directly never reaches this map: the size is in the program, so that
 * check is a comparison against a constant. A described local is aligned and
 * padded to the granule, since two objects sharing one cannot both be
 * described and the runtime will not guess between them. See
 * docs/memory-safety.md.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of address space described by one shadow entry. Allocations are
 * registered at granule resolution, so two live allocations never share a
 * granule; the allocator's own alignment guarantees it. */
#define METTLE_SAFETY_GRANULE 16u

/* What kind of access failed, reported in the trap message. */
typedef enum {
  METTLE_SAFETY_ACCESS_READ = 0,
  METTLE_SAFETY_ACCESS_WRITE = 1
} MettleSafetyAccessKind;

/* Record `size` bytes at `pointer` as one live allocation. A null pointer or a
 * zero size is ignored. Re-registering an address that is already live
 * replaces the old record. An unsupported address range or a failure to
 * allocate metadata traps rather than silently losing coverage. */
void mettle_safety_register(void *pointer, uint64_t size);
void mettle_safety_register_static(void *pointer, uint64_t size);

/* Retire the allocation starting exactly at `pointer`. The granules keep
 * naming it, so a pointer kept across the free is reported as use-after-free
 * rather than read back as untracked memory. Retiring an address that is not a
 * live allocation start is ignored: freeing foreign memory is not an error the
 * runtime can judge. */
void mettle_safety_unregister(void *pointer);

/* Bracket a call into a Mettle-implemented allocator, which is the one thing
 * that legitimately touches memory the model calls dead. It writes a header
 * below the pointer it returns, it poisons blocks the program has released,
 * and it hands a recycled block back only after clearing it. Checked against
 * the model those reads as overruns and use-after-free, and they are neither.
 *
 * Bracketing the CALL rather than exempting a function is what keeps this
 * precise. The allocator reaches for the same byte-filling helpers ordinary
 * code does, and those stay fully checked everywhere else; only the work done
 * on the allocator's behalf is skipped. Nested calls count, so an allocator
 * calling itself unwinds correctly, and the count is per thread, so one thread
 * allocating never blinds another. */
void mettle_safety_enter_allocator(void);
void mettle_safety_leave_allocator(void);

/* Update records after realloc. A null result with a nonzero size preserves
 * the old record. A null result with zero size retires it, matching the owned
 * allocators. A nonnull result describes the new extent. This cannot detect
 * stale aliases when realloc returns the same address. */
void mettle_safety_reregister(void *old_pointer, void *new_pointer,
                              uint64_t size);

/* The check itself. `base` is the pointer the access derives from and carries
 * the lookup address; `offset` is the signed byte displacement applied to it and
 * `size` the number of bytes touched. Returns when the access is inside the
 * allocation that owns `base`, and does not return otherwise. `line` is the
 * source line, which the report falls back on when the build carries no debug
 * information to resolve the return address against.
 *
 * A size of zero or less means the access touches nothing and returns at once,
 * before `base` is even looked at. That is what lets the compiler replace a
 * whole loop's checks with one covering the range the loop walks: the length
 * it computes comes out zero or negative for a loop that never runs, and a
 * loop that never runs may well have been handed a pointer that was never
 * valid. Without the early return that pointer would be accused, and the
 * alternative is a branch around the check at every hoist site.
 *
 * The access is deliberately not described in words. The crash handler already
 * names the function, file, line and source text from the return address, and
 * it does it better; passing a string as well would cost a load per check at
 * every site, for a worse version of what the report already prints. */
void mettle_safety_check(const void *base, int64_t offset, int64_t size,
                         uint32_t access_kind, uint32_t line);

/* Bytes from `base` to the end of the live allocation it belongs to.
 *
 * For code that will check many accesses against one pointer, this is asked
 * once and the accesses become `(unsigned)offset <= span - size`, which is a
 * subtract, a compare and a branch rather than a call and a walk through the
 * shadow map. That is the difference between a checked inner loop costing a
 * few percent and costing several times the work it does.
 *
 * The comparison is a fast path, never a verdict. Failing it means asking
 * mettle_safety_check properly, which is what keeps this exact: an access
 * behind an interior pointer has a negative offset and fails the unsigned
 * compare, and the full check then allows it. So the answers here only have to
 * be safe in one direction.
 *
 * A huge value where nothing owns the address, since the full check would let
 * those through anyway. Zero where the allocation is gone, so every access
 * goes the long way round and is reported as the use-after-free it is. */
int64_t mettle_safety_span(const void *base);

/* Hidden value metadata used by instrumented code. Identity zero denotes an
 * origin outside the registered allocation model. Nonzero identities include
 * a generation and never become valid again after their allocation ends. */
uint64_t mettle_safety_identity(const void *pointer);
void mettle_safety_check_identity(const void *base, int64_t offset, int64_t size,
                                  uint32_t kind, uint32_t line, uint64_t identity);
int64_t mettle_safety_span_identity(const void *base, uint64_t identity);
uint64_t mettle_safety_merge_identity(uint64_t a, uint64_t b);
uint64_t mettle_safety_subtract_identity(uint64_t a, uint64_t b);
uint64_t mettle_safety_value_load(const void *slot, uint64_t value, uint64_t size);
void mettle_safety_value_store(void *slot, uint64_t value, uint64_t identity,
                               uint64_t size);
void mettle_safety_value_copy(void *destination, const void *source, uint64_t size);
void mettle_safety_value_clear(void *destination, uint64_t size);
void mettle_safety_free_identity(void *pointer, uint64_t identity);
void mettle_safety_buffer_check(void *pointer, int64_t size, uint32_t kind,
                                uint32_t line, uint64_t identity);
void *mettle_safety_call_push(void *callee, uint64_t count);
void *mettle_safety_call_enter(void *callee);
void mettle_safety_call_arg(void *call, uint64_t index, uint64_t identity);
uint64_t mettle_safety_call_param(void *call, uint64_t index);
void mettle_safety_call_return(void *call, uint64_t identity);
uint64_t mettle_safety_call_pop(void *call);

/* Live allocations, for tests. Advisory and lock-free.
 *
 * There is deliberately no count of checks performed. Keeping one meant a
 * locked read-modify-write on a shared line for every access in the program,
 * which measured as a large part of what a check cost, to feed an accessor
 * nothing called. */
uint64_t mettle_safety_live_region_count(void);

/* Distinct descriptor slots ever handed out. A program that allocates and
 * frees in a loop must hold this steady rather than let it climb, which is the
 * observable form of "freed descriptors get recycled once their memory is
 * handed to someone else". */
uint64_t mettle_safety_descriptor_high_water(void);

/* Release every table the shadow map allocated. Only for tests that want to
 * measure a clean run; a program that simply exits does not need it. */
void mettle_safety_reset(void);

/* The run-time half of M0121, emitted at every task spawn under
 * --check-tasks. It answers the same question the borrow analyser answered
 * while compiling, from the other side: does this pointer lie in the stack of
 * the thread that is spawning the task? The analysis is not consulted, so a
 * capture it could not see is caught here anyway.
 *
 * The upper bound is exact on both platforms. Windows keeps the thread's
 * stack base in the TEB. Linux has the owned runtime record it: the initial
 * stack block for the first thread, and the block it allocated for every
 * thread it created. A thread the runtime did not create and cannot see, one
 * started by foreign code that then calls back in, has no recorded bound and
 * falls back to a thread stack's span above the current frame, which
 * docs/known-limitations.md says. */
void mettle_safety_task_capture_check(const void *pointer, const char *task,
                               const char *sender, uint32_t line);

/* The run-time half of a `where cycles < N` deadline, emitted under
 * --check-deadlines. Every basic block adds its own model cost to the frame
 * on top, and leaving compares what the path actually spent against the
 * longest path the compiler proved. The model is the same one the proof used;
 * what is re-checked is the claim that no path costs more, which is the half
 * an analysis can get wrong. */
void mettle_safety_deadline_enter(const char *name, int64_t limit,
                                  int64_t proven);
void mettle_safety_deadline_step(int64_t cost);
void mettle_safety_deadline_leave(void);

#ifdef __cplusplus
}
#endif

#endif /* METTLE_RUNTIME_SAFETY_H */
