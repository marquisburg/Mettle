/* Exercises the checked-access safety runtime on its own.
 *
 * Everything here is a claim about what the runtime must answer, independent
 * of any compiler work: which accesses are inside their allocation, which run
 * off the end, which touch memory that has been freed, and which the runtime
 * has no business judging at all. The real trap ends the process, so this
 * harness supplies its own and jumps back to the case. */

#include "runtime/safety.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
/* The standalone harness has no owned thread runtime. */
void *mettle_thread_stack_high(void) { return NULL; }
#endif

#ifdef METTLE_SAFETY_TESTING
static int g_maps_before_failure = -1;
int mettle_safety_test_fail_map(void) {
  if (g_maps_before_failure < 0) return 0;
  if (g_maps_before_failure == 0) return 1;
  g_maps_before_failure--;
  return 0;
}
#endif

#if defined(_WIN32)
static HANDLE g_allocator_entered;
static HANDLE g_allocator_release;
static DWORD WINAPI allocator_thread(void *unused) {
  (void)unused;
  mettle_safety_enter_allocator();
  SetEvent(g_allocator_entered);
  WaitForSingleObject(g_allocator_release, INFINITE);
  mettle_safety_leave_allocator();
  return 0;
}

static DWORD WINAPI registry_thread(void *pointer) {
  for (int i = 0; i < 100000; i++) {
    mettle_safety_register(pointer, 64);
    mettle_safety_register(pointer, 128);
  }
  return 0;
}
#endif

static jmp_buf g_landing;
static int g_trapped;
static char g_message[512];

void mettle_crash_trap_ex(uint32_t kind, const char *message,
                          const void *program_counter,
                          const void *frame_pointer, uint64_t arg0,
                          uint64_t arg1) {
  (void)kind;
  (void)program_counter;
  (void)frame_pointer;
  (void)arg0;
  (void)arg1;
  g_trapped = 1;
  g_message[0] = '\0';
  if (message) {
    strncpy(g_message, message, sizeof(g_message) - 1);
    g_message[sizeof(g_message) - 1] = '\0';
  }
  longjmp(g_landing, 1);
}

static int g_failures;
static int g_cases;

static void report(const char *name, int expected, int actual) {
  if (expected != actual) {
    g_failures++;
    printf("  FAIL %s: expected %s, got %s\n", name,
           expected ? "a trap" : "no trap", actual ? "a trap" : "no trap");
    if (actual) {
      printf("       message: %s\n", g_message);
    }
    return;
  }
  printf("  ok   %s%s\n", name, expected ? " (trapped)" : "");
}

#define CASE(name, expect_trap, body)                                          \
  do {                                                                         \
    g_cases++;                                                                 \
    g_trapped = 0;                                                             \
    g_message[0] = '\0';                                                       \
    if (setjmp(g_landing) == 0) {                                              \
      body                                                                     \
    }                                                                          \
    report(name, expect_trap, g_trapped);                                      \
  } while (0)

#define READ METTLE_SAFETY_ACCESS_READ

/* Everything registered is granule aligned, exactly as the compiler arranges
 * for the stack and global objects it registers. */
static void *aligned_block(size_t size) {
#if defined(_WIN32) || defined(_WIN64)
  return _aligned_malloc(size, METTLE_SAFETY_GRANULE);
#else
  void *memory = NULL;
  if (posix_memalign(&memory, METTLE_SAFETY_GRANULE, size) != 0) {
    return NULL;
  }
  return memory;
#endif
}

static void free_block(void *memory) {
#if defined(_WIN32) || defined(_WIN64)
  _aligned_free(memory);
#else
  free(memory);
#endif
}

int main(void) {
  printf("safety runtime\n");

  CASE("in bounds read", 0, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_check(block, 0, 4, READ, 10);
    mettle_safety_check(block, 60, 4, READ, 11);
    mettle_safety_unregister(block);
    free_block(block);
  });

  CASE("one past the end", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_check(block, 64, 1, READ, 20);
    mettle_safety_unregister(block);
    free_block(block);
  });

  /* The last element read at the wrong width is still an overrun. */
  CASE("straddling the end", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_check(block, 61, 4, READ, 21);
    mettle_safety_unregister(block);
    free_block(block);
  });

  CASE("negative index", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_check(block, -4, 4, READ, 22);
    mettle_safety_unregister(block);
    free_block(block);
  });

  /* The reason the check carries the base pointer rather than just the final
   * address: running off one live allocation into another is a violation, and
   * an address-only check would call it fine. */
  CASE("overrun into a neighbour", 1, {
    char *first = (char *)aligned_block(64);
    char *second = (char *)aligned_block(64);
    mettle_safety_register(first, 64);
    mettle_safety_register(second, 64);
    mettle_safety_check(first, (int64_t)((uintptr_t)second - (uintptr_t)first), 4, READ, 30);
    mettle_safety_unregister(first);
    mettle_safety_unregister(second);
    free_block(first);
    free_block(second);
  });

  CASE("use after free", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_unregister(block);
    mettle_safety_check(block, 0, 4, READ, 40);
    free_block(block);
  });

  CASE("interior pointer after free", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_unregister(block);
    mettle_safety_check(block + 32, 0, 4, READ, 41);
    free_block(block);
  });

  CASE("interior pointer while live", 0, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_check(block + 32, 0, 4, READ, 42);
    mettle_safety_check(block + 32, -32, 4, READ, 43);
    mettle_safety_unregister(block);
    free_block(block);
  });

  CASE("interior pointer past the end", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_check(block + 32, 32, 4, READ, 44);
    mettle_safety_unregister(block);
    free_block(block);
  });

  /* Memory Mettle never described is allowed through. A foreign library's
   * pointer is not something the runtime can judge, and trapping on it would
   * reject correct programs. */
  CASE("untracked memory", 0, {
    char stack_bytes[64];
    mettle_safety_check(stack_bytes, 0, 4, READ, 50);
  });

  CASE("null base", 1,
       { mettle_safety_check(NULL, 0, 4, READ, 51); });

  CASE("old pointer after realloc", 1, {
    char *old_block = (char *)aligned_block(64);
    char *new_block = (char *)aligned_block(128);
    mettle_safety_register(old_block, 64);
    mettle_safety_reregister(old_block, new_block, 128);
    mettle_safety_check(old_block, 0, 4, READ, 60);
    free_block(old_block);
    free_block(new_block);
  });

  CASE("new pointer after realloc", 0, {
    char *old_block = (char *)aligned_block(64);
    char *new_block = (char *)aligned_block(128);
    mettle_safety_register(old_block, 64);
    mettle_safety_reregister(old_block, new_block, 128);
    mettle_safety_check(new_block, 120, 8, READ, 61);
    mettle_safety_unregister(new_block);
    free_block(old_block);
    free_block(new_block);
  });

  CASE("address reuse", 0, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_unregister(block);
    mettle_safety_register(block, 64);
    mettle_safety_check(block, 60, 4, READ, 70);
    mettle_safety_unregister(block);
    free_block(block);
  });

  /* A freed allocation keeps its descriptor so a stale pointer can be named,
   * and gives it up once its memory belongs to someone else. Without that
   * second half, a program that allocates in a loop would grow the descriptor
   * table forever. Cases above that trapped jumped out before unregistering,
   * so the live count is compared against its value going in, not zero. */
  CASE("descriptor recycling stays bounded", 0, {
    uint64_t live_before = mettle_safety_live_region_count();
    uint64_t slots_before = mettle_safety_descriptor_high_water();
    char *block = (char *)aligned_block(64);
    for (int round = 0; round < 100000; round++) {
      mettle_safety_register(block, 64);
      mettle_safety_check(block, 0, 4, READ, 71);
      mettle_safety_unregister(block);
    }
    free_block(block);
    uint64_t live_after = mettle_safety_live_region_count();
    uint64_t slots_after = mettle_safety_descriptor_high_water();
    if (live_after != live_before) {
      printf("       live regions moved from %llu to %llu\n",
             (unsigned long long)live_before, (unsigned long long)live_after);
      g_failures++;
    }
    if (slots_after > slots_before + 2) {
      printf("       descriptor slots grew from %llu to %llu over 100000 "
             "cycles\n",
             (unsigned long long)slots_before, (unsigned long long)slots_after);
      g_failures++;
    }
  });

  /* A large allocation crosses many granules and several shadow tables. */
  CASE("multi megabyte allocation", 0, {
    size_t size = 4u * 1024u * 1024u;
    char *block = (char *)aligned_block(size);
    mettle_safety_register(block, size);
    mettle_safety_check(block, 0, 8, READ, 80);
    mettle_safety_check(block, (int64_t)size - 8, 8, READ, 81);
    mettle_safety_check(block + size / 2, 0, 8, READ, 82);
    mettle_safety_unregister(block);
    free_block(block);
  });

  CASE("multi megabyte overrun", 1, {
    size_t size = 4u * 1024u * 1024u;
    char *block = (char *)aligned_block(size);
    mettle_safety_register(block, size);
    mettle_safety_check(block, (int64_t)size, 1, READ, 83);
    mettle_safety_unregister(block);
    free_block(block);
  });

  /* A length that is not a multiple of the granule must refuse the bytes past
   * the requested length, not the bytes up to the granule boundary. */
  CASE("unaligned size tail", 1, {
    char *block = (char *)aligned_block(48);
    mettle_safety_register(block, 20);
    mettle_safety_check(block, 20, 1, READ, 90);
    mettle_safety_unregister(block);
    free_block(block);
  });

  CASE("failed realloc preserves old allocation", 0, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    uint64_t before = mettle_safety_live_region_count();
    mettle_safety_reregister(block, NULL, 128);
    mettle_safety_check(block, 60, 4, READ, 100);
    if (mettle_safety_span(block) != 64 ||
        mettle_safety_live_region_count() != before) g_failures++;
    mettle_safety_unregister(block);
    free_block(block);
  });

  CASE("zero size realloc retires old allocation", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_reregister(block, NULL, 0);
    mettle_safety_check(block, 0, 1, READ, 101);
  });

  CASE("in place realloc updates extent", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    mettle_safety_reregister(block, block, 20);
    mettle_safety_check(block, 19, 1, READ, 102);
    if (mettle_safety_span(block) != 20) g_failures++;
    mettle_safety_check(block, 20, 1, READ, 103);
  });

  CASE("registration address overflow", 1, {
    mettle_safety_register((void *)(UINTPTR_MAX - 15), 32);
  });
  CASE("registration size overflow", 1, {
    mettle_safety_register((void *)(uintptr_t)0x10000, UINT64_MAX);
  });

  CASE("nested allocator checks resume after leaving", 0, {
    mettle_safety_enter_allocator();
    mettle_safety_enter_allocator();
    mettle_safety_check(NULL, 0, 1, READ, 104);
    mettle_safety_leave_allocator();
    mettle_safety_check(NULL, 0, 1, READ, 105);
    mettle_safety_leave_allocator();
  });
  CASE("allocator suppression ends", 1, {
    mettle_safety_check(NULL, 0, 1, READ, 106);
  });

#if defined(_WIN32)
  g_allocator_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
  g_allocator_release = CreateEventA(NULL, TRUE, FALSE, NULL);
  HANDLE worker = CreateThread(NULL, 0, allocator_thread, NULL, 0, NULL);
  if (!g_allocator_entered || !g_allocator_release || !worker ||
      WaitForSingleObject(g_allocator_entered, 10000) != WAIT_OBJECT_0) {
    fprintf(stderr, "failed to start allocator isolation test\n");
    return 1;
  }
  CASE("another thread cannot suppress checks", 1, {
    mettle_safety_check(NULL, 0, 1, READ, 107);
  });
  SetEvent(g_allocator_release);
  if (WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0) return 1;
  CloseHandle(worker);
  CloseHandle(g_allocator_entered);
  CloseHandle(g_allocator_release);

  /* Every version permits the read. Readers must never see a half replaced
   * record, a freed record, or a zero size while registration holds the lock. */
  char *shared = (char *)aligned_block(128);
  mettle_safety_register(shared, 64);
  worker = CreateThread(NULL, 0, registry_thread, shared, 0, NULL);
  if (!worker) return 1;
  CASE("concurrent registry snapshots", 0, {
    for (int i = 0; i < 100000; i++) {
      int64_t span = mettle_safety_span(shared + 32);
      if (span != 32 && span != 96) {
        g_failures++;
        break;
      }
      mettle_safety_check(shared, 32, 32, READ, 108);
    }
  });
  if (WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0) return 1;
  CloseHandle(worker);
  mettle_safety_unregister(shared);
  free_block(shared);
#endif

  CASE("identity survives address reuse", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    uint64_t old = mettle_safety_identity(block);
    mettle_safety_unregister(block);
    mettle_safety_register(block, 64);
    uint64_t current = mettle_safety_identity(block);
    if (!old || !current || old == current) g_failures++;
    mettle_safety_check_identity(block, 0, 8, READ, 110, current);
    mettle_safety_check_identity(block, 0, 8, READ, 111, old);
  });
  CASE("identity survives derived address", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_register(block, 64);
    uint64_t identity = mettle_safety_identity(block);
    mettle_safety_check_identity((void *)((uintptr_t)block + 4096), 0, 8, READ, 112, identity);
  });
  CASE("identity survives field storage", 1, {
    char *block = (char *)aligned_block(64);
    uint64_t slot = (uint64_t)(uintptr_t)block;
    mettle_safety_register(block, 64);
    uint64_t identity = mettle_safety_identity(block);
    mettle_safety_value_store(&slot, slot, identity, 8);
    mettle_safety_reregister(block, block, 32);
    uint64_t loaded = mettle_safety_value_load(&slot, slot, 8);
    if (loaded != identity) g_failures++;
    mettle_safety_check_identity(block, 0, 8, READ, 113, loaded);
  });
  CASE("identity follows byte copies", 1, {
    char *block = (char *)aligned_block(64);
    uint64_t source = (uint64_t)(uintptr_t)block, destination = 0;
    mettle_safety_register(block, 64);
    uint64_t identity = mettle_safety_identity(block);
    mettle_safety_value_store(&source, source, identity, 8);
    for (size_t i = 0; i < 8; i++) {
      unsigned char byte = ((unsigned char *)&source)[i];
      uint64_t part = mettle_safety_value_load((char *)&source + i, byte, 1);
      ((unsigned char *)&destination)[i] = byte;
      mettle_safety_value_store((char *)&destination + i, byte, part, 1);
    }
    mettle_safety_reregister(block, block, 32);
    uint64_t loaded = mettle_safety_value_load(&destination, destination, 8);
    if (loaded != identity) g_failures++;
    mettle_safety_check_identity(block, 0, 8, READ, 114, loaded);
  });
  CASE("shared granules retain exact bounds", 1, {
    char *block = (char *)aligned_block(32);
    mettle_safety_register(block, 8);
    mettle_safety_register(block + 8, 8);
    uint64_t first = mettle_safety_identity(block);
    uint64_t second = mettle_safety_identity(block + 8);
    if (!first || !second || first == second) g_failures++;
    mettle_safety_check_identity(block, 0, 8, READ, 115, first);
    mettle_safety_check_identity(block + 8, 0, 8, READ, 116, second);
    mettle_safety_check_identity(block, 8, 1, READ, 117, first);
  });
  CASE("unknown identity fails closed", 1, {
    char *block = (char *)aligned_block(64);
    mettle_safety_check_identity(block, 0, 8, READ, 118, 0);
  });
  CASE("call identity nesting and callback isolation", 0, {
    void *callee = (void *)(uintptr_t)0x12340;
    void *other = (void *)(uintptr_t)0x45670;
    void *outer = mettle_safety_call_push(callee, 2);
    mettle_safety_call_arg(outer, 1, 77);
    if (mettle_safety_call_enter(other) != NULL) g_failures++;
    void *entry = mettle_safety_call_enter(callee);
    if (entry != outer || mettle_safety_call_param(entry, 1) != 77) g_failures++;
    if (mettle_safety_call_enter(callee) != NULL) g_failures++;
    void *inner = mettle_safety_call_push(callee, 1);
    mettle_safety_call_arg(inner, 0, 99);
    void *nested = mettle_safety_call_enter(callee);
    mettle_safety_call_return(nested, mettle_safety_call_param(nested, 0));
    if (mettle_safety_call_pop(inner) != 99) g_failures++;
    mettle_safety_call_return(entry, mettle_safety_call_param(entry, 1));
    if (mettle_safety_call_pop(outer) != 77) g_failures++;
  });

#ifdef METTLE_SAFETY_TESTING
  /* Fail the descriptor block, second level, and third level in turn. */
  for (volatile int maps = 0; maps < 3; maps++) {
    mettle_safety_reset();
    g_maps_before_failure = maps;
    CASE("metadata allocation failure traps", 1, {
      mettle_safety_register((void *)(uintptr_t)0x10000, 64);
    });
    g_maps_before_failure = -1;
    /* Also proves the failure path released the registry lock. */
    mettle_safety_reset();
  }
#endif

  printf("\n%d cases, %d failures\n", g_cases, g_failures);
  printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
