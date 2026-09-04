/* Runtime half of `--record-trace`. See trace.h for what it is for. */

#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define METTLE_TRACE_BUFFER 65536
#define METTLE_TRACE_DEFAULT_MAX 200000

static char g_buffer[METTLE_TRACE_BUFFER];
static size_t g_used;
static FILE *g_file;
static long long g_depth;
static long long g_written;
static long long g_dropped;
static long long g_limit;
static int g_stopped;

static void trace_open(void) {
  const char *path = NULL;
  if (g_file || g_stopped) {
    return;
  }
  path = getenv("METTLE_TRACE");
  if (!path || !path[0]) {
    path = "mettle-trace.txt";
  }
  g_file = fopen(path, "wb");
  if (!g_file) {
    g_stopped = 1;
  }
}

static void trace_write_out(void) {
  trace_open();
  if (!g_file || g_used == 0) {
    g_used = 0;
    return;
  }
  fwrite(g_buffer, 1, g_used, g_file);
  g_used = 0;
}

static void trace_put(const char *text, size_t length) {
  if (length + 1 > METTLE_TRACE_BUFFER) {
    return;
  }
  if (g_used + length > METTLE_TRACE_BUFFER) {
    trace_write_out();
  }
  memcpy(g_buffer + g_used, text, length);
  g_used += length;
}

static void trace_put_text(const char *text) {
  /* A field is one column of a line, so a separator or a newline inside one
   * would make the line say something else. Both are dropped rather than
   * escaped: a name or a path carrying either is not one this reads back. */
  size_t at = 0;
  char clean[512];
  size_t length = 0;
  if (!text) {
    text = "";
  }
  while (text[at] && length + 1 < sizeof(clean)) {
    char c = text[at++];
    if (c == '|' || c == '\n' || c == '\r') {
      c = ' ';
    }
    clean[length++] = c;
  }
  clean[length] = '\0';
  trace_put(clean, length);
}

static void trace_put_number(long long value) {
  char digits[24];
  size_t length = 0;
  unsigned long long magnitude;
  if (value < 0) {
    trace_put("-", 1);
    magnitude = (unsigned long long)(-(value + 1)) + 1ull;
  } else {
    magnitude = (unsigned long long)value;
  }
  if (magnitude == 0) {
    trace_put("0", 1);
    return;
  }
  while (magnitude > 0 && length < sizeof(digits)) {
    digits[length++] = (char)('0' + (magnitude % 10ull));
    magnitude /= 10ull;
  }
  while (length > 0) {
    char one = digits[--length];
    trace_put(&one, 1);
  }
}

static long long trace_limit(void) {
  if (g_limit == 0) {
    const char *text = getenv("METTLE_TRACE_MAX");
    long long parsed = 0;
    if (text) {
      while (*text >= '0' && *text <= '9') {
        parsed = parsed * 10 + (*text++ - '0');
      }
    }
    g_limit = parsed > 0 ? parsed : METTLE_TRACE_DEFAULT_MAX;
  }
  return g_limit;
}

void mettle_trace_event(const char *kind, const char *name, const char *file,
                        int64_t line, int64_t column, int64_t value) {
  if (g_stopped) {
    return;
  }
  if (g_written >= trace_limit()) {
    g_dropped++;
    return;
  }
  g_written++;
  trace_put_text(kind);
  trace_put("|", 1);
  trace_put_text(name);
  trace_put("|", 1);
  trace_put_text(file);
  trace_put("|", 1);
  trace_put_number((long long)line);
  trace_put("|", 1);
  trace_put_number((long long)column);
  trace_put("|", 1);
  trace_put_number((long long)value);
  trace_put("\n", 1);
}

void mettle_trace_enter(const char *name, const char *file, int64_t line,
                        int64_t column) {
  g_depth++;
  mettle_trace_event("call", name, file, line, column, g_depth);
}

void mettle_trace_leave(void) {
  if (g_depth > 0) {
    g_depth--;
  }
}

void mettle_trace_flush(void) {
  if (g_dropped > 0) {
    long long dropped = g_dropped;
    g_dropped = 0;
    g_written = 0;
    mettle_trace_event("dropped", "trace", "", 0, 0, dropped);
    g_written = trace_limit();
  }
  trace_write_out();
  if (g_file) {
    fflush(g_file);
  }
}
