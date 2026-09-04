#include "ir_trace.h"
#include <stdlib.h>
#include <string.h>

#define IR_TRACE_MAX_EVENTS 200000

static int g_collect;
static IRTraceEvent *g_events;
static size_t g_event_count;
static size_t g_event_capacity;
static char *g_test_name;

static char *trace_dup(const char *text) {
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

void ir_trace_set_collect(int on) { g_collect = on; }

int ir_trace_collecting(void) { return g_collect; }

const char *ir_trace_test_name(void) { return g_test_name ? g_test_name : ""; }

size_t ir_trace_event_count(void) { return g_event_count; }

const IRTraceEvent *ir_trace_event_at(size_t index) {
  return index < g_event_count ? &g_events[index] : NULL;
}

void ir_trace_reset(void) {
  for (size_t i = 0; i < g_event_count; i++) {
    free((void *)g_events[i].kind);
    free((void *)g_events[i].name);
    free((void *)g_events[i].file);
  }
  free(g_events);
  g_events = NULL;
  g_event_count = 0;
  g_event_capacity = 0;
  free(g_test_name);
  g_test_name = NULL;
}

void ir_trace_begin(const char *test_name) {
  ir_trace_reset();
  g_test_name = trace_dup(test_name);
}

void ir_trace_record(const char *kind, const char *name, const char *file,
                     size_t line, size_t column, long long value) {
  IRTraceEvent *event;
  if (!g_collect || !kind || g_event_count >= IR_TRACE_MAX_EVENTS) {
    return;
  }
  if (g_event_count == g_event_capacity) {
    size_t grown = g_event_capacity ? g_event_capacity * 2 : 256;
    IRTraceEvent *table = realloc(g_events, grown * sizeof(IRTraceEvent));
    if (!table) {
      return;
    }
    g_events = table;
    g_event_capacity = grown;
  }
  event = &g_events[g_event_count++];
  event->kind = trace_dup(kind);
  event->name = trace_dup(name ? name : "");
  event->file = trace_dup(file ? file : "");
  event->line = line;
  event->column = column;
  event->value = value;
}
