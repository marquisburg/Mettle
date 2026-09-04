/* Shadow map and region registry for checked-access memory safety.
 * See safety.h for the design and for what the guarantee covers. */

#include "safety.h"
#include "crash_handler.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Address space split. A granule index is the address with the low 4 bits
 * dropped, and the three table levels carve up the 44 bits that remain. One
 * level-three table describes a megabyte of address space in 256KB of shadow,
 * and the program pays for it only where it registers memory. */
#define SAFETY_L1_BITS 12
#define SAFETY_L2_BITS 16
#define SAFETY_L3_BITS 16
#define SAFETY_L1_SIZE (1u << SAFETY_L1_BITS)
#define SAFETY_L2_SIZE (1u << SAFETY_L2_BITS)
#define SAFETY_L3_SIZE (1u << SAFETY_L3_BITS)
#define SAFETY_ADDRESS_BITS \
  (SAFETY_L1_BITS + SAFETY_L2_BITS + SAFETY_L3_BITS + 4)

/* Reserved shadow values. Zero means Mettle never described the granule, and
 * an access there is allowed: foreign libraries hand back memory the runtime
 * cannot judge, and refusing it would reject correct programs. One means two
 * registrations wanted the same granule and the runtime declined to choose;
 * see safety_claim_granules. */
#define SAFETY_ID_UNOWNED 0u
#define SAFETY_ID_CONTESTED 1u
#define SAFETY_ID_FIRST 2u

/* Descriptors live in fixed blocks that are never moved or freed, so a reader
 * that resolved an id can always dereference it even while another thread is
 * growing the array. */
#define SAFETY_BLOCK_SHIFT 12
#define SAFETY_BLOCK_SIZE (1u << SAFETY_BLOCK_SHIFT)
#define SAFETY_MAX_BLOCKS 4096

typedef enum {
  SAFETY_STATE_FREELIST = 0, /* unused descriptor, id available again */
  SAFETY_STATE_LIVE = 1,     /* a live allocation */
  SAFETY_STATE_DEAD = 2      /* freed, and still named by its old granules */
} SafetyState;

typedef struct {
  uintptr_t start;
  uint64_t size;
  /* Shadow entries still naming this descriptor. A freed allocation keeps its
   * descriptor for exactly as long as this is nonzero, which is what lets a
   * stale pointer be reported as use-after-free instead of read back as
   * untracked memory. The count reaches zero when a later allocation has taken
   * every granule, at which point the id is recycled. */
  uint64_t granules;
  uint32_t state;
  uint32_t next_free;
} SafetyRegion;

#if defined(__GNUC__)
#define SAFETY_COLD __attribute__((noinline, cold))
#else
#define SAFETY_COLD
#endif

static uint32_t **g_safety_l1[SAFETY_L1_SIZE];
static SafetyRegion *g_safety_blocks[SAFETY_MAX_BLOCKS];
static uint32_t g_safety_region_next = SAFETY_ID_FIRST;
static uint32_t g_safety_region_free;
static volatile long g_safety_lock;
static volatile uint64_t g_safety_live_regions;

/* ---- platform memory ------------------------------------------------------ */

/* The shadow map cannot come from the allocator it describes, so it comes
 * straight from the operating system and arrives zeroed. */
static void *safety_map(size_t bytes) {
#if defined(_WIN32) || defined(_WIN64)
  return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return memory == MAP_FAILED ? NULL : memory;
#endif
}

static void safety_unmap(void *memory, size_t bytes) {
  if (!memory) {
    return;
  }
#if defined(_WIN32) || defined(_WIN64)
  (void)bytes;
  VirtualFree(memory, 0, MEM_RELEASE);
#else
  munmap(memory, bytes);
#endif
}

/* ---- lock ----------------------------------------------------------------- */

static void safety_lock(void) {
  while (__atomic_exchange_n(&g_safety_lock, 1L, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&g_safety_lock, __ATOMIC_RELAXED)) {
#if defined(__i386__) || defined(__x86_64__)
      __builtin_ia32_pause();
#endif
    }
  }
}

static void safety_unlock(void) {
  __atomic_store_n(&g_safety_lock, 0L, __ATOMIC_RELEASE);
}

/* ---- descriptors ---------------------------------------------------------- */

static SafetyRegion *safety_region(uint32_t id) {
  uint32_t block = id >> SAFETY_BLOCK_SHIFT;
  if (block >= SAFETY_MAX_BLOCKS) {
    return NULL;
  }
  SafetyRegion *entries =
      __atomic_load_n(&g_safety_blocks[block], __ATOMIC_ACQUIRE);
  return entries ? &entries[id & (SAFETY_BLOCK_SIZE - 1)] : NULL;
}

/* Caller holds the lock. Returns zero when descriptor space is exhausted,
 * which leaves the allocation undescribed rather than misdescribed. */
static uint32_t safety_region_acquire(uintptr_t start, uint64_t size) {
  uint32_t id = g_safety_region_free;
  if (id != 0) {
    SafetyRegion *reused = safety_region(id);
    g_safety_region_free = reused->next_free;
    reused->start = start;
    reused->size = size;
    reused->granules = 0;
    reused->next_free = 0;
    __atomic_store_n(&reused->state, (uint32_t)SAFETY_STATE_LIVE,
                     __ATOMIC_RELEASE);
    return id;
  }

  id = g_safety_region_next;
  uint32_t block = id >> SAFETY_BLOCK_SHIFT;
  if (block >= SAFETY_MAX_BLOCKS) {
    return 0;
  }
  if (!g_safety_blocks[block]) {
    SafetyRegion *entries =
        (SafetyRegion *)safety_map(SAFETY_BLOCK_SIZE * sizeof(SafetyRegion));
    if (!entries) {
      return 0;
    }
    __atomic_store_n(&g_safety_blocks[block], entries, __ATOMIC_RELEASE);
  }
  g_safety_region_next = id + 1;

  SafetyRegion *fresh = safety_region(id);
  fresh->start = start;
  fresh->size = size;
  fresh->granules = 0;
  fresh->next_free = 0;
  __atomic_store_n(&fresh->state, (uint32_t)SAFETY_STATE_LIVE,
                   __ATOMIC_RELEASE);
  return id;
}

/* Caller holds the lock. */
static void safety_region_recycle(uint32_t id, SafetyRegion *region) {
  region->size = 0;
  region->granules = 0;
  __atomic_store_n(&region->state, (uint32_t)SAFETY_STATE_FREELIST,
                   __ATOMIC_RELEASE);
  region->next_free = g_safety_region_free;
  g_safety_region_free = id;
}

/* Caller holds the lock. One granule stopped naming `id`. */
static void safety_region_drop_granule(uint32_t id) {
  SafetyRegion *region = safety_region(id);
  if (!region || region->granules == 0) {
    return;
  }
  region->granules--;
  if (region->granules == 0 &&
      __atomic_load_n(&region->state, __ATOMIC_RELAXED) ==
          (uint32_t)SAFETY_STATE_DEAD) {
    safety_region_recycle(id, region);
  }
}

/* ---- shadow map ----------------------------------------------------------- */

static uint32_t *safety_slot(uintptr_t address, int create) {
  if ((address >> SAFETY_ADDRESS_BITS) != 0) {
    return NULL;
  }
  uintptr_t granule = address / METTLE_SAFETY_GRANULE;
  size_t l1 = (size_t)(granule >> (SAFETY_L2_BITS + SAFETY_L3_BITS)) &
              (SAFETY_L1_SIZE - 1);
  size_t l2 = (size_t)(granule >> SAFETY_L3_BITS) & (SAFETY_L2_SIZE - 1);
  size_t l3 = (size_t)granule & (SAFETY_L3_SIZE - 1);

  uint32_t **level2 = __atomic_load_n(&g_safety_l1[l1], __ATOMIC_ACQUIRE);
  if (!level2) {
    if (!create) {
      return NULL;
    }
    level2 = (uint32_t **)safety_map(SAFETY_L2_SIZE * sizeof(uint32_t *));
    if (!level2) {
      return NULL;
    }
    __atomic_store_n(&g_safety_l1[l1], level2, __ATOMIC_RELEASE);
  }

  uint32_t *level3 = __atomic_load_n(&level2[l2], __ATOMIC_ACQUIRE);
  if (!level3) {
    if (!create) {
      return NULL;
    }
    level3 = (uint32_t *)safety_map(SAFETY_L3_SIZE * sizeof(uint32_t));
    if (!level3) {
      return NULL;
    }
    __atomic_store_n(&level2[l2], level3, __ATOMIC_RELEASE);
  }

  return &level3[l3];
}

static uint32_t safety_lookup(uintptr_t address) {
  uint32_t *slot = safety_slot(address, 0);
  return slot ? __atomic_load_n(slot, __ATOMIC_RELAXED) : SAFETY_ID_UNOWNED;
}

/* Caller holds the lock. Stamps `id` across every granule the range touches
 * and records how many it took.
 *
 * A granule a LIVE allocation already owns is not taken. Stealing it would
 * make that allocation's own accesses resolve to the wrong descriptor and trap
 * on correct code, so the granule is marked contested instead: both sides lose
 * coverage there and neither is ever falsely accused. This only arises when
 * two registered objects sit closer than a granule apart, which the compiler
 * prevents by aligning everything it registers.
 *
 * A granule a DEAD allocation still names is taken freely, and that is how the
 * dead descriptor is eventually reclaimed. */
static void safety_claim_granules(uintptr_t start, uint64_t size, uint32_t id) {
  SafetyRegion *region = safety_region(id);
  uint64_t claimed = 0;
  uintptr_t first = start / METTLE_SAFETY_GRANULE;
  uintptr_t last = (start + size - 1) / METTLE_SAFETY_GRANULE;

  for (uintptr_t granule = first; granule <= last; granule++) {
    uint32_t *slot = safety_slot(granule * METTLE_SAFETY_GRANULE, 1);
    if (!slot) {
      continue;
    }
    uint32_t current = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (current == id) {
      continue;
    }
    if (current >= SAFETY_ID_FIRST) {
      SafetyRegion *owner = safety_region(current);
      uint32_t state =
          owner ? __atomic_load_n(&owner->state, __ATOMIC_RELAXED) : 0;
      if (state == (uint32_t)SAFETY_STATE_LIVE) {
        __atomic_store_n(slot, SAFETY_ID_CONTESTED, __ATOMIC_RELAXED);
        continue;
      }
      __atomic_store_n(slot, id, __ATOMIC_RELAXED);
      safety_region_drop_granule(current);
      claimed++;
      continue;
    }
    if (current == SAFETY_ID_CONTESTED) {
      continue;
    }
    __atomic_store_n(slot, id, __ATOMIC_RELAXED);
    claimed++;
  }

  if (region) {
    region->granules += claimed;
  }
}

/* ---- registration --------------------------------------------------------- */

void mettle_safety_register(void *pointer, uint64_t size) {
  if (!pointer || size == 0) {
    return;
  }
  uintptr_t start = (uintptr_t)pointer;

  safety_lock();
  /* A live region already starting here means the allocator reused a block
   * without a free reaching the runtime. Retire it so the map describes only
   * the allocation that is actually there. */
  uint32_t existing = safety_lookup(start);
  if (existing >= SAFETY_ID_FIRST) {
    SafetyRegion *previous = safety_region(existing);
    if (previous && previous->start == start &&
        __atomic_load_n(&previous->state, __ATOMIC_RELAXED) ==
            (uint32_t)SAFETY_STATE_LIVE) {
      __atomic_store_n(&previous->state, (uint32_t)SAFETY_STATE_DEAD,
                       __ATOMIC_RELEASE);
      __atomic_sub_fetch(&g_safety_live_regions, 1, __ATOMIC_RELAXED);
    }
  }

  uint32_t id = safety_region_acquire(start, size);
  if (id != 0) {
    safety_claim_granules(start, size, id);
    __atomic_add_fetch(&g_safety_live_regions, 1, __ATOMIC_RELAXED);
  }
  safety_unlock();
}

void mettle_safety_unregister(void *pointer) {
  if (!pointer) {
    return;
  }
  uintptr_t start = (uintptr_t)pointer;

  safety_lock();
  uint32_t id = safety_lookup(start);
  if (id >= SAFETY_ID_FIRST) {
    SafetyRegion *region = safety_region(id);
    if (region && region->start == start &&
        __atomic_load_n(&region->state, __ATOMIC_RELAXED) ==
            (uint32_t)SAFETY_STATE_LIVE) {
      /* The granules keep naming this descriptor. That is deliberate: a
       * pointer kept across the free still resolves here, and the check
       * reports it as use-after-free rather than as untracked memory. */
      __atomic_store_n(&region->state, (uint32_t)SAFETY_STATE_DEAD,
                       __ATOMIC_RELEASE);
      __atomic_sub_fetch(&g_safety_live_regions, 1, __ATOMIC_RELAXED);
        }
  }
  safety_unlock();
}

void mettle_safety_reregister(void *old_pointer, void *new_pointer,
                              uint64_t size) {
  if (old_pointer && old_pointer != new_pointer) {
    mettle_safety_unregister(old_pointer);
  }
  mettle_safety_register(new_pointer, size);
}

/* Per thread, so one thread inside the allocator never suppresses another's
 * checks. Nesting is counted rather than flagged: the allocator's entry points
 * call one another. Mingw gcc 16 emits native PE TLS only, which the internal
 * linker cannot carry (no TLS directory); degrade to a shared counter there. */
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__) && \
     __GNUC__ >= 16) ||                                            \
    defined(MT_SHARED_RUNTIME)
static unsigned g_safety_allocator_depth;
#else
static __thread unsigned g_safety_allocator_depth;
#endif

void mettle_safety_enter_allocator(void) { g_safety_allocator_depth++; }

void mettle_safety_leave_allocator(void) {
  if (g_safety_allocator_depth > 0) {
    g_safety_allocator_depth--;
  }
}

/* ---- reporting ------------------------------------------------------------ */

static void safety_append(char *buffer, size_t capacity, size_t *offset,
                          const char *text) {
  while (*text && *offset + 1 < capacity) {
    buffer[(*offset)++] = *text++;
  }
  buffer[*offset] = '\0';
}

static void safety_append_unsigned(char *buffer, size_t capacity,
                                   size_t *offset, uint64_t value) {
  char digits[24];
  size_t index = sizeof(digits);
  digits[--index] = '\0';
  do {
    digits[--index] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value != 0 && index > 0);
  safety_append(buffer, capacity, offset, digits + index);
}

static void safety_append_signed(char *buffer, size_t capacity, size_t *offset,
                                 int64_t value) {
  if (value < 0) {
    safety_append(buffer, capacity, offset, "-");
    safety_append_unsigned(buffer, capacity, offset,
                           (uint64_t)(-(value + 1)) + 1u);
    return;
  }
  safety_append_unsigned(buffer, capacity, offset, (uint64_t)value);
}

/* Built once per failing run. The process is about to end, so a static buffer
 * is safe and keeps the reporting path free of allocation. */
static char g_safety_message[512];

typedef struct {
  const char *headline;
  uint32_t line;
  int64_t offset;
  int64_t size;
  uint64_t extent;
  int have_extent;
} SafetyFailure;

static void safety_report_and_trap(const SafetyFailure *failure,
                                   void *program_counter,
                                   void *frame_pointer) {
  size_t at = 0;
  const size_t capacity = sizeof(g_safety_message);
  g_safety_message[0] = '\0';

  safety_append(g_safety_message, capacity, &at, "Fatal error: ");
  safety_append(g_safety_message, capacity, &at, failure->headline);
  safety_append(g_safety_message, capacity, &at, ": ");
  safety_append_signed(g_safety_message, capacity, &at, failure->size);
  safety_append(g_safety_message, capacity, &at, " bytes at offset ");
  safety_append_signed(g_safety_message, capacity, &at, failure->offset);
  if (failure->have_extent) {
    safety_append(g_safety_message, capacity, &at, " of a ");
    safety_append_unsigned(g_safety_message, capacity, &at, failure->extent);
    safety_append(g_safety_message, capacity, &at, " byte allocation");
  }
  if (failure->line != 0) {
    safety_append(g_safety_message, capacity, &at, " (line ");
    safety_append_unsigned(g_safety_message, capacity, &at, failure->line);
    safety_append(g_safety_message, capacity, &at, ")");
  }

  mettle_crash_trap_ex(METTLE_CRASH_TRAP_UNKNOWN, g_safety_message,
                       program_counter, frame_pointer, 0, 0);
}

/* ---- the check ------------------------------------------------------------ */

/* Everything a failing check needs and a passing one must not pay for.
 *
 * Kept out of line and cold so the fast path holds no frame, captures no
 * return address, and builds no report. It re-derives what it needs, which
 * costs nothing on a path that ends the process. */
SAFETY_COLD static void safety_check_failed(const void *base, int64_t offset,
                                            int64_t size, uint32_t line,
                                            void *program_counter,
                                            void *frame_pointer) {
  /* Only here, because only a failing access can be the allocator's. Its
   * headers and poisoned blocks are the accesses that reach this point, and
   * asking a thread-local on every check to spare them would charge the whole
   * program for the exception. */
  if (g_safety_allocator_depth != 0) {
    return;
  }

  SafetyFailure failure;
  failure.line = line;
  failure.offset = offset;
  failure.size = size;
  failure.extent = 0;
  failure.have_extent = 0;

  if (!base) {
    failure.headline = "null pointer dereference";
    safety_report_and_trap(&failure, program_counter, frame_pointer);
    return;
  }

  uint32_t id = safety_lookup((uintptr_t)base);
  if (id < SAFETY_ID_FIRST) {
    return; /* unowned or contested: not ours to judge */
  }
  const SafetyRegion *region = safety_region(id);
  if (!region) {
    return;
  }
  uint32_t state = __atomic_load_n(&region->state, __ATOMIC_RELAXED);
  if (state == (uint32_t)SAFETY_STATE_FREELIST) {
    return;
  }

  failure.extent = region->size;
  failure.have_extent = 1;
  failure.headline = state == (uint32_t)SAFETY_STATE_DEAD
                         ? "use of memory after it was freed"
                         : "memory access outside its allocation";
  safety_report_and_trap(&failure, program_counter, frame_pointer);
}

void mettle_safety_check(const void *base, int64_t offset, int64_t size,
                         uint32_t access_kind, uint32_t line) {
  (void)access_kind;
  /* An access of no bytes reaches no memory, so there is nothing about `base`
   * worth asking. This is also what lets a loop's checks be replaced by one
   * covering its range without a guard branch: the length comes out zero or
   * less for a loop that never runs. */
  if (size <= 0) {
    return;
  }

  uintptr_t start = (uintptr_t)base;
  uintptr_t address = start + (uintptr_t)offset;

  /* Walk the map: find the allocation, confirm the access is inside it and
   * that it is still live. Everything else a check might need to do belongs to
   * the access that fails, and lives in safety_check_failed. */
  uint32_t *slot = safety_slot(start, 0);
  if (slot) {
    uint32_t id = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (id >= SAFETY_ID_FIRST) {
      const SafetyRegion *region = safety_region(id);
      if (region &&
          __atomic_load_n(&region->state, __ATOMIC_RELAXED) ==
              (uint32_t)SAFETY_STATE_LIVE) {
        uint64_t extent = region->size;
        uint64_t width = (uint64_t)size;
        if (width <= extent && address >= region->start &&
            (uint64_t)(address - region->start) <= extent - width) {
          return;
        }
      }
      safety_check_failed(base, offset, size, line,
                          __builtin_return_address(0),
                          __builtin_frame_address(0));
      return;
    }
  }

  /* No live allocation owns this address. Allowed, except for the one case
   * worth naming: a null pointer is nobody's memory by mistake, not by
   * provenance. */
  if (!base) {
    safety_check_failed(base, offset, size, line, __builtin_return_address(0),
                        __builtin_frame_address(0));
  }
}

int64_t mettle_safety_span(const void *base) {
  /* Large enough that no real access can exceed it, small enough that adding
   * an access width to it cannot overflow. */
  const int64_t unbounded = (int64_t)1 << 56;

  if (!base) {
    return 0;
  }
  uintptr_t start = (uintptr_t)base;
  uint32_t *slot = safety_slot(start, 0);
  if (!slot) {
    return unbounded;
  }
  uint32_t id = __atomic_load_n(slot, __ATOMIC_RELAXED);
  if (id < SAFETY_ID_FIRST) {
    return unbounded;
  }
  const SafetyRegion *region = safety_region(id);
  if (!region || __atomic_load_n(&region->state, __ATOMIC_RELAXED) !=
                     (uint32_t)SAFETY_STATE_LIVE) {
    return 0;
  }
  uintptr_t end = region->start + region->size;
  if (start < region->start || start >= end) {
    return 0;
  }
  return (int64_t)(end - start);
}

uint64_t mettle_safety_live_region_count(void) {
  return __atomic_load_n(&g_safety_live_regions, __ATOMIC_RELAXED);
}

uint64_t mettle_safety_descriptor_high_water(void) {
  safety_lock();
  uint64_t high_water = g_safety_region_next - SAFETY_ID_FIRST;
  safety_unlock();
  return high_water;
}

void mettle_safety_reset(void) {
  safety_lock();
  for (size_t l1 = 0; l1 < SAFETY_L1_SIZE; l1++) {
    uint32_t **level2 = g_safety_l1[l1];
    if (!level2) {
      continue;
    }
    for (size_t l2 = 0; l2 < SAFETY_L2_SIZE; l2++) {
      safety_unmap(level2[l2], SAFETY_L3_SIZE * sizeof(uint32_t));
    }
    safety_unmap(level2, SAFETY_L2_SIZE * sizeof(uint32_t *));
    g_safety_l1[l1] = NULL;
  }
  for (size_t block = 0; block < SAFETY_MAX_BLOCKS; block++) {
    safety_unmap(g_safety_blocks[block],
                 SAFETY_BLOCK_SIZE * sizeof(SafetyRegion));
    g_safety_blocks[block] = NULL;
  }
  g_safety_region_next = SAFETY_ID_FIRST;
  g_safety_region_free = 0;
  __atomic_store_n(&g_safety_live_regions, 0, __ATOMIC_RELAXED);
  safety_unlock();
}

/* ---- the task-capture check ----------------------------------------------- */

#define SAFETY_TASK_STACK_SPAN (8u * 1024u * 1024u)

void *mettle_thread_stack_high(void);

static uintptr_t safety_stack_base(void) {
#if defined(_WIN64)
  const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
  return tib ? (uintptr_t)tib->StackBase : 0;
#elif defined(_WIN32)
  return 0;
#else
  return (uintptr_t)mettle_thread_stack_high();
#endif
}

static char g_task_message[512];

void mettle_safety_task_capture_check(const void *pointer, const char *task,
                               const char *sender, uint32_t line) {
  volatile char here = 0;
  uintptr_t low = (uintptr_t)(void *)&here;
  uintptr_t value = (uintptr_t)pointer;
  uintptr_t base = safety_stack_base();
  size_t at = 0;
  const size_t capacity = sizeof(g_task_message);
  (void)here;
  if (!pointer) {
    return;
  }
  if (base == 0 || base <= low) {
    base = low + SAFETY_TASK_STACK_SPAN;
  }
  if (value < low || value >= base) {
    return;
  }
  g_task_message[0] = '\0';
  safety_append(g_task_message, capacity, &at,
                "Fatal error: a pointer into the spawning thread's stack was "
                "handed to the task '");
  safety_append(g_task_message, capacity, &at, task ? task : "?");
  safety_append(g_task_message, capacity, &at, "' by '");
  safety_append(g_task_message, capacity, &at, sender ? sender : "?");
  safety_append(g_task_message, capacity, &at, "'");
  if (line != 0) {
    safety_append(g_task_message, capacity, &at, " (line ");
    safety_append_unsigned(g_task_message, capacity, &at, line);
    safety_append(g_task_message, capacity, &at, ")");
  }
  mettle_crash_trap_ex(METTLE_CRASH_TRAP_UNKNOWN, g_task_message, 0, 0, 0, 0);
}

/* ---- the deadline check --------------------------------------------------- */

#define SAFETY_DEADLINE_DEPTH 64

typedef struct {
  const char *name;
  int64_t limit;
  int64_t proven;
  int64_t spent;
} SafetyDeadline;

#if defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)
static SafetyDeadline g_deadline_stack[SAFETY_DEADLINE_DEPTH];
static unsigned g_deadline_depth;
#else
static __thread SafetyDeadline g_deadline_stack[SAFETY_DEADLINE_DEPTH];
static __thread unsigned g_deadline_depth;
#endif

static char g_deadline_message[512];

void mettle_safety_deadline_enter(const char *name, int64_t limit,
                                  int64_t proven) {
  if (g_deadline_depth >= SAFETY_DEADLINE_DEPTH) {
    g_deadline_depth++;
    return;
  }
  g_deadline_stack[g_deadline_depth].name = name;
  g_deadline_stack[g_deadline_depth].limit = limit;
  g_deadline_stack[g_deadline_depth].proven = proven;
  g_deadline_stack[g_deadline_depth].spent = 0;
  g_deadline_depth++;
}

void mettle_safety_deadline_step(int64_t cost) {
  if (g_deadline_depth == 0 || g_deadline_depth > SAFETY_DEADLINE_DEPTH) {
    return;
  }
  g_deadline_stack[g_deadline_depth - 1].spent += cost;
}

void mettle_safety_deadline_leave(void) {
  SafetyDeadline *frame = NULL;
  size_t at = 0;
  const size_t capacity = sizeof(g_deadline_message);
  if (g_deadline_depth == 0) {
    return;
  }
  g_deadline_depth--;
  if (g_deadline_depth >= SAFETY_DEADLINE_DEPTH) {
    return;
  }
  frame = &g_deadline_stack[g_deadline_depth];
  if (g_deadline_depth > 0) {
    g_deadline_stack[g_deadline_depth - 1].spent += frame->spent;
  }
  if (frame->spent <= frame->proven) {
    return;
  }
  g_deadline_message[0] = '\0';
  safety_append(g_deadline_message, capacity, &at,
                "Fatal error: the path '");
  safety_append(g_deadline_message, capacity, &at,
                frame->name ? frame->name : "?");
  safety_append(g_deadline_message, capacity, &at, "' actually took cost ");
  safety_append_signed(g_deadline_message, capacity, &at, frame->spent);
  safety_append(g_deadline_message, capacity, &at,
                ", more than the longest path the compiler proved (");
  safety_append_signed(g_deadline_message, capacity, &at, frame->proven);
  safety_append(g_deadline_message, capacity, &at, ")");
  mettle_crash_trap_ex(METTLE_CRASH_TRAP_UNKNOWN, g_deadline_message, 0, 0, 0,
                       0);
}
