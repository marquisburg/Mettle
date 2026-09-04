#include "ir_machine.h"
#include <stdlib.h>
#include <string.h>

/* The snapshot owns its strings: this file is archived into libmtlc, and the
   frontend's interner is not on that side of the line. */
static char *machine_dup(const char *text) {
  size_t length;
  char *copy;
  if (!text) {
    return NULL;
  }
  length = strlen(text);
  copy = malloc(length + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, length + 1);
  return copy;
}

static int g_collect;
static IRMachineFunction *g_functions;
static size_t g_function_count;
static size_t g_function_capacity;

void ir_machine_set_collect(int on) { g_collect = on; }

int ir_machine_collecting(void) { return g_collect; }

size_t ir_machine_function_count(void) { return g_function_count; }

const IRMachineFunction *ir_machine_function_at(size_t index) {
  return index < g_function_count ? &g_functions[index] : NULL;
}

void ir_machine_reset(void) {
  for (size_t i = 0; i < g_function_count; i++) {
    for (size_t l = 0; l < g_functions[i].loop_count; l++) {
      free((void *)g_functions[i].loops[l].kind);
    }
    free(g_functions[i].loops);
    free((void *)g_functions[i].name);
    free((void *)g_functions[i].file);
  }
  free(g_functions);
  g_functions = NULL;
  g_function_count = 0;
  g_function_capacity = 0;
}

static IRMachineFunction *machine_find(const char *name, const char *file) {
  IRMachineFunction *entry;
  if (!g_collect || !name) {
    return NULL;
  }
  for (size_t i = 0; i < g_function_count; i++) {
    if (g_functions[i].name && strcmp(g_functions[i].name, name) == 0) {
      if (file && !g_functions[i].file) {
        g_functions[i].file = machine_dup(file);
      }
      return &g_functions[i];
    }
  }
  if (g_function_count == g_function_capacity) {
    size_t grown = g_function_capacity ? g_function_capacity * 2 : 16;
    IRMachineFunction *table =
        realloc(g_functions, grown * sizeof(IRMachineFunction));
    if (!table) {
      return NULL;
    }
    g_functions = table;
    g_function_capacity = grown;
  }
  entry = &g_functions[g_function_count++];
  memset(entry, 0, sizeof(*entry));
  entry->name = machine_dup(name);
  entry->file = file ? machine_dup(file) : NULL;
  return entry;
}

void ir_machine_note_frame(const char *name, long long frame_bytes,
                           long long spills) {
  IRMachineFunction *entry = machine_find(name, NULL);
  if (!entry) {
    return;
  }
  entry->frame_bytes = frame_bytes;
  entry->spills = spills;
}

void ir_machine_note_backend(const char *name, const char *file,
                             long long instructions, int register_allocated) {
  IRMachineFunction *entry = machine_find(name, file);
  if (!entry) {
    return;
  }
  entry->instructions = instructions;
  entry->register_allocated = register_allocated;
}

void ir_machine_note_loop(const char *function, const char *file, size_t line,
                          size_t column, int vectorized, const char *kind) {
  IRMachineFunction *entry = machine_find(function, file);
  if (!entry) {
    return;
  }
  for (size_t i = 0; i < entry->loop_count; i++) {
    if (entry->loops[i].line == line) {
      entry->loops[i].vectorized = vectorized;
      free((void *)entry->loops[i].kind);
      entry->loops[i].kind = kind ? machine_dup(kind) : NULL;
      return;
    }
  }
  if (entry->loop_count == entry->loop_capacity) {
    size_t grown = entry->loop_capacity ? entry->loop_capacity * 2 : 4;
    IRMachineLoop *table =
        realloc(entry->loops, grown * sizeof(IRMachineLoop));
    if (!table) {
      return;
    }
    entry->loops = table;
    entry->loop_capacity = grown;
  }
  entry->loops[entry->loop_count].line = line;
  entry->loops[entry->loop_count].column = column;
  entry->loops[entry->loop_count].vectorized = vectorized;
  entry->loops[entry->loop_count].kind = kind ? machine_dup(kind) : NULL;
  entry->loop_count++;
}

void ir_machine_note_call(const char *function, const char *file, int inlined) {
  IRMachineFunction *entry = machine_find(function, file);
  if (!entry) {
    return;
  }
  if (inlined) {
    entry->inlined_calls++;
  } else {
    entry->calls_left++;
  }
}
