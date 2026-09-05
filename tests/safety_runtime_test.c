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

/* safety.c asks the runtime where this thread's stack tops out. The
 * freestanding runtime that answers it is not part of this harness. */
void *mettle_thread_stack_high(void) { return NULL; }

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
    mettle_safety_check(first, (int64_t)(second - first), 4, READ, 30);
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

  printf("\n%d cases, %d failures\n", g_cases, g_failures);
  printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
