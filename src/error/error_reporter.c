#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "error_reporter.h"
#include "../common.h"
#include "diag_style.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_ERROR_CAPACITY 16
#define MAX_ERRORS_DEFAULT 100
/* Maximum source-line width in the snippet before truncation with "..." */
#define SNIPPET_MAX_COLS 120

/* All diagnostic output goes to stderr so it doesn't pollute stdout pipelines */
#define DIAG_STREAM stderr

/* The code a diagnostic reports under: its own, when an analysis stamped a
   finer one (the memory/range checks' M0101..M0119), otherwise its type's. */
static const char *error_report_code(const ErrorReport *error);

/* Return the short error-code string for an ErrorType */
static const char *error_type_code(ErrorType type) {
  switch (type) {
  case ERROR_LEXICAL:  return ERROR_CODE_LEXICAL;
  case ERROR_SYNTAX:   return ERROR_CODE_SYNTAX;
  case ERROR_SEMANTIC: return ERROR_CODE_SEMANTIC;
  case ERROR_TYPE:     return ERROR_CODE_TYPE;
  case ERROR_SCOPE:    return ERROR_CODE_SCOPE;
  case ERROR_IO:       return ERROR_CODE_IO;
  case ERROR_INTERNAL: return ERROR_CODE_INTERNAL;
  default:             return "E????";
  }
}

static const char *error_report_code(const ErrorReport *error) {
  if (error->code_override && error->code_override[0]) {
    return error->code_override;
  }
  return error_type_code(error->type);
}

static size_t error_reporter_count_digits(size_t n) {
  size_t digits = 1;
  while (n >= 10) {
    n /= 10;
    digits++;
  }
  return digits;
}

ErrorReporter *error_reporter_create(const char *filename,
                                     const char *source_code) {
  ErrorReporter *reporter = malloc(sizeof(ErrorReporter));
  if (!reporter)
    return NULL;

  reporter->errors = malloc(sizeof(ErrorReport) * INITIAL_ERROR_CAPACITY);
  if (!reporter->errors) {
    free(reporter);
    return NULL;
  }

  reporter->count = 0;
  reporter->capacity = INITIAL_ERROR_CAPACITY;
  reporter->max_errors = MAX_ERRORS_DEFAULT;
  reporter->source_code = source_code;
  reporter->filename = filename ? mettle_strdup(filename) : NULL;
  reporter->sources = NULL;
  reporter->source_count = 0;
  reporter->source_capacity = 0;
  reporter->current_filename = reporter->filename;
  reporter->current_source_code = reporter->source_code;
  reporter->last_add_suppressed = 0;
  reporter->note_frames = NULL;
  reporter->note_frame_count = 0;
  reporter->note_frame_capacity = 0;
  reporter->emitting_note_frames = 0;
  error_reporter_register_source(reporter, filename, source_code);

  return reporter;
}

SourceSpan source_span_create(size_t line, size_t column, size_t length) {
  SourceSpan span;
  span.line = line;
  span.column = column;
  span.length = length;
  span.filename = NULL;
  return span;
}

SourceSpan source_span_from_location(SourceLocation location, size_t length) {
  SourceSpan span = source_span_create(location.line, location.column, length);
  span.filename = location.filename;
  return span;
}

void error_reporter_destroy(ErrorReporter *reporter) {
  if (!reporter)
    return;

  for (size_t i = 0; i < reporter->count; i++) {
    free(reporter->errors[i].filename);
    free(reporter->errors[i].message);
    free(reporter->errors[i].suggestion);
    free(reporter->errors[i].code_snippet);
    free(reporter->errors[i].span_label);
    free(reporter->errors[i].code_override);
  }

  for (size_t i = 0; i < reporter->source_count; i++) {
    free(reporter->sources[i].filename);
    free(reporter->sources[i].source_code);
  }
  for (size_t i = 0; i < reporter->note_frame_count; i++) {
    free(reporter->note_frames[i].message);
  }
  free(reporter->note_frames);
  free(reporter->sources);
  free(reporter->errors);
  free((char *)reporter->filename);
  free(reporter);
}

static const ErrorReporterSource *
error_reporter_find_source(ErrorReporter *reporter, const char *filename) {
  if (!reporter || !filename) {
    return NULL;
  }

  for (size_t i = 0; i < reporter->source_count; i++) {
    if (reporter->sources[i].filename &&
        strcmp(reporter->sources[i].filename, filename) == 0) {
      return &reporter->sources[i];
    }
  }

  return NULL;
}

int error_reporter_register_source(ErrorReporter *reporter,
                                   const char *filename,
                                   const char *source_code) {
  if (!reporter || !filename || !source_code) {
    return 0;
  }

  ErrorReporterSource *existing =
      (ErrorReporterSource *)error_reporter_find_source(reporter, filename);
  if (existing) {
    char *source_copy = mettle_strdup(source_code);
    if (!source_copy) {
      return 0;
    }
    free(existing->source_code);
    existing->source_code = source_copy;
    return 1;
  }

  if (reporter->source_count >= reporter->source_capacity) {
    size_t new_capacity =
        reporter->source_capacity == 0 ? 8 : reporter->source_capacity * 2;
    ErrorReporterSource *grown =
        realloc(reporter->sources, new_capacity * sizeof(ErrorReporterSource));
    if (!grown) {
      return 0;
    }
    reporter->sources = grown;
    reporter->source_capacity = new_capacity;
  }

  ErrorReporterSource *entry = &reporter->sources[reporter->source_count];
  entry->filename = mettle_strdup(filename);
  entry->source_code = mettle_strdup(source_code);
  if (!entry->filename || !entry->source_code) {
    free(entry->filename);
    free(entry->source_code);
    entry->filename = NULL;
    entry->source_code = NULL;
    return 0;
  }

  reporter->source_count++;
  return 1;
}

int error_reporter_set_source_context(ErrorReporter *reporter,
                                      const char *filename,
                                      const char *source_code) {
  if (!reporter) {
    return 0;
  }

  if (filename && source_code &&
      !error_reporter_register_source(reporter, filename, source_code)) {
    return 0;
  }

  const ErrorReporterSource *entry =
      filename ? error_reporter_find_source(reporter, filename) : NULL;
  reporter->current_filename =
      entry ? entry->filename : (filename ? filename : reporter->filename);
  reporter->current_source_code =
      entry ? entry->source_code
            : (source_code ? source_code : reporter->source_code);
  return 1;
}

const char *error_reporter_current_filename(ErrorReporter *reporter) {
  return reporter ? reporter->current_filename : NULL;
}

const char *error_reporter_current_source_code(ErrorReporter *reporter) {
  return reporter ? reporter->current_source_code : NULL;
}

static int error_reporter_expand_capacity(ErrorReporter *reporter) {
  if (reporter->count >= reporter->capacity) {
    size_t new_capacity = reporter->capacity * 2;
    ErrorReport *new_errors =
        realloc(reporter->errors, sizeof(ErrorReport) * new_capacity);
    if (!new_errors)
      return 0;

    reporter->errors = new_errors;
    reporter->capacity = new_capacity;
  }
  return 1;
}

void error_reporter_add_error(ErrorReporter *reporter, ErrorType type,
                              SourceLocation location, const char *message) {
  error_reporter_add_error_with_suggestion(reporter, type, location, message,
                                           NULL);
}

void error_reporter_add_error_with_suggestion(ErrorReporter *reporter,
                                              ErrorType type,
                                              SourceLocation location,
                                              const char *message,
                                              const char *suggestion) {
  SourceSpan span = source_span_from_location(location, 1);
  error_reporter_add_error_with_span_and_suggestion(reporter, type, span,
                                                    message, suggestion);
}

/* A later diagnostic at the same spot is almost always a cascade of the
   first (parser retries, re-checks after recovery). Suppress: exact
   duplicates everywhere, and any second syntax error at one location. */
static int error_reporter_is_cascade(ErrorReporter *reporter, ErrorSeverity severity,
                                     const SourceSpan *span, ErrorType type,
                                     const char *filename, const char *message) {
  if (severity == DIAG_SEVERITY_NOTE_OF)
    return 0;
  for (size_t i = 0; i < reporter->count; i++) {
    const ErrorReport *e = &reporter->errors[i];
    if (e->severity == DIAG_SEVERITY_NOTE_OF)
      continue;
    if (e->location.line != span->line || e->location.column != span->column)
      continue;
    if ((e->filename && filename && strcmp(e->filename, filename) != 0) ||
        (!e->filename != !filename))
      continue;
    if (e->message && message && strcmp(e->message, message) == 0)
      return 1;
    if (type == ERROR_SYNTAX && e->type == ERROR_SYNTAX &&
        e->severity == DIAG_SEVERITY_ERROR && severity == DIAG_SEVERITY_ERROR)
      return 1;
  }
  return 0;
}

static void error_reporter_add_report(ErrorReporter *reporter,
                                      ErrorSeverity severity, SourceSpan span,
                                      ErrorType type, const char *message,
                                      const char *suggestion) {
  if (!reporter)
    return;
  reporter->last_add_suppressed = 1;
  if (reporter->count >= reporter->max_errors)
    return;

  if (!error_reporter_expand_capacity(reporter))
    return;

  if (span.length == 0)
    span.length = 1;

  const char *filename =
      span.filename ? span.filename : reporter->current_filename;
  const ErrorReporterSource *source_entry =
      filename ? error_reporter_find_source(reporter, filename) : NULL;
  const char *source_code =
      source_entry ? source_entry->source_code : reporter->current_source_code;

  if (error_reporter_is_cascade(reporter, severity, &span, type, filename,
                                message))
    return;
  reporter->last_add_suppressed = 0;

  ErrorReport *error = &reporter->errors[reporter->count];
  error->type = type;
  error->severity = severity;
  error->location = source_location_create(span.line, span.column);
  error->location.filename = filename;
  error->span = span;
  error->span.filename = filename;
  error->filename = filename ? mettle_strdup(filename) : NULL;
  error->source_code = source_code;
  error->message = mettle_strdup(message);
  error->suggestion = suggestion ? mettle_strdup(suggestion) : NULL;
  error->code_snippet =
      error_reporter_get_line_from_source(source_code, span.line);
  error->span_label = NULL;
  error->code_override = NULL;

  reporter->count++;

  /* Attribute the diagnostic to the expansion that produced it, now, while the
     chain that produced it is still live. Notes are emitted outermost first so
     the reader walks in from the code they wrote. */
  if (reporter->note_frame_count > 0 && !reporter->emitting_note_frames &&
      (severity == DIAG_SEVERITY_ERROR || severity == DIAG_SEVERITY_WARNING)) {
    reporter->emitting_note_frames = 1;
    for (size_t i = 0; i < reporter->note_frame_count; i++) {
      const ErrorNoteFrame *frame = &reporter->note_frames[i];
      if (!frame->message)
        continue;
      error_reporter_add_report(reporter, DIAG_SEVERITY_NOTE_OF, frame->span,
                                ERROR_SEMANTIC, frame->message, NULL);
    }
    reporter->emitting_note_frames = 0;
    reporter->last_add_suppressed = 0;
  }
}

/* The most recent diagnostic that is not one of its own attached notes. The
   `_last` helpers below refine a diagnostic, so they have to skip past any
   notes an expansion frame appended after it. */
static ErrorReport *error_reporter_last_primary(ErrorReporter *reporter) {
  for (size_t i = reporter->count; i > 0; i--) {
    ErrorReport *candidate = &reporter->errors[i - 1];
    if (candidate->severity != DIAG_SEVERITY_NOTE_OF)
      return candidate;
  }
  return NULL;
}

int error_reporter_push_note_frame(ErrorReporter *reporter, SourceSpan span,
                                   const char *message) {
  if (!reporter || !message)
    return 0;
  if (reporter->note_frame_count == reporter->note_frame_capacity) {
    size_t next =
        reporter->note_frame_capacity ? reporter->note_frame_capacity * 2 : 4;
    ErrorNoteFrame *grown =
        realloc(reporter->note_frames, next * sizeof(ErrorNoteFrame));
    if (!grown)
      return 0;
    reporter->note_frames = grown;
    reporter->note_frame_capacity = next;
  }
  char *copy = mettle_strdup(message);
  if (!copy)
    return 0;
  if (span.length == 0)
    span.length = 1;
  reporter->note_frames[reporter->note_frame_count].span = span;
  reporter->note_frames[reporter->note_frame_count].message = copy;
  reporter->note_frame_count++;
  return 1;
}

void error_reporter_pop_note_frame(ErrorReporter *reporter) {
  if (!reporter || reporter->note_frame_count == 0)
    return;
  reporter->note_frame_count--;
  free(reporter->note_frames[reporter->note_frame_count].message);
  reporter->note_frames[reporter->note_frame_count].message = NULL;
}

size_t error_reporter_note_frame_depth(const ErrorReporter *reporter) {
  return reporter ? reporter->note_frame_count : 0;
}

static int er_ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

SourceSpan error_reporter_span_snap_to_token(ErrorReporter *reporter,
                                             SourceSpan span,
                                             const char *token) {
  if (!reporter || !token || token[0] == '\0' || span.line == 0)
    return span;
  const char *filename = span.filename ? span.filename : reporter->current_filename;
  const ErrorReporterSource *entry =
      filename ? error_reporter_find_source(reporter, filename) : NULL;
  const char *source =
      entry ? entry->source_code : reporter->current_source_code;
  char *line = error_reporter_get_line_from_source(source, span.line);
  if (!line)
    return span;
  size_t token_len = strlen(token);
  size_t line_len = strlen(line);
  size_t start = (span.column > 0) ? span.column - 1 : 0;
  if (start > line_len)
    start = 0;
  const char *p = line + start;
  while ((p = strstr(p, token)) != NULL) {
    int left_ok = (p == line) || !er_ident_char(p[-1]);
    int right_ok = !er_ident_char(p[token_len]);
    if (left_ok && right_ok) {
      span.column = (size_t)(p - line) + 1;
      span.length = token_len;
      break;
    }
    p += 1;
  }
  free(line);
  return span;
}

void error_reporter_refine_last(ErrorReporter *reporter, const char *message) {
  if (!reporter || !message || reporter->count == 0 ||
      reporter->last_add_suppressed)
    return;
  ErrorReport *last = error_reporter_last_primary(reporter);
  if (!last || last->severity != DIAG_SEVERITY_ERROR)
    return;
  char *copy = mettle_strdup(message);
  if (!copy)
    return;
  free(last->message);
  last->message = copy;
}

void error_reporter_set_last_label(ErrorReporter *reporter, const char *label) {
  if (!reporter || !label || reporter->count == 0 ||
      reporter->last_add_suppressed)
    return;
  ErrorReport *last = error_reporter_last_primary(reporter);
  if (!last)
    return;
  free(last->span_label);
  last->span_label = mettle_strdup(label);
}

void error_reporter_set_last_code(ErrorReporter *reporter, const char *code) {
  if (!reporter || !code || reporter->count == 0 ||
      reporter->last_add_suppressed)
    return;
  ErrorReport *last = error_reporter_last_primary(reporter);
  if (!last)
    return;
  free(last->code_override);
  last->code_override = mettle_strdup(code);
}

void error_reporter_add_note_of_span(ErrorReporter *reporter, SourceSpan span,
                                     const char *message) {
  if (!reporter || !message || reporter->count == 0 ||
      reporter->last_add_suppressed)
    return;
  int saved = reporter->last_add_suppressed;
  error_reporter_add_report(reporter, DIAG_SEVERITY_NOTE_OF, span,
                            ERROR_SEMANTIC, message, NULL);
  reporter->last_add_suppressed = saved;
}

void error_reporter_add_error_with_span(ErrorReporter *reporter, ErrorType type,
                                        SourceSpan span, const char *message) {
  error_reporter_add_error_with_span_and_suggestion(reporter, type, span,
                                                    message, NULL);
}

void error_reporter_add_error_with_span_and_suggestion(
    ErrorReporter *reporter, ErrorType type, SourceSpan span, const char *message,
    const char *suggestion) {
  error_reporter_add_report(reporter, DIAG_SEVERITY_ERROR, span, type, message,
                            suggestion);
}

void error_reporter_add_warning(ErrorReporter *reporter, ErrorType type,
                                SourceLocation location, const char *message) {
  SourceSpan span = source_span_from_location(location, 1);
  error_reporter_add_warning_with_span(reporter, type, span, message);
}

void error_reporter_add_warning_with_span(ErrorReporter *reporter, ErrorType type,
                                          SourceSpan span, const char *message) {
  error_reporter_add_report(reporter, DIAG_SEVERITY_WARNING, span, type,
                            message, NULL);
}

void error_reporter_add_warning_with_suggestion(ErrorReporter *reporter,
                                                ErrorType type,
                                                SourceLocation location,
                                                const char *message,
                                                const char *suggestion) {
  SourceSpan span = source_span_from_location(location, 1);
  error_reporter_add_report(reporter, DIAG_SEVERITY_WARNING, span, type,
                            message, suggestion);
}

static int g_error_format_json = 0;

void error_reporter_set_format_json(int enabled) {
  g_error_format_json = enabled;
}

int error_reporter_format_json(void) { return g_error_format_json; }

/* Emit a JSON string literal (with escaping) to the diagnostic stream. */
static void diag_json_string(const char *s) {
  fputc('"', DIAG_STREAM);
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    switch (*p) {
    case '"':  fputs("\\\"", DIAG_STREAM); break;
    case '\\': fputs("\\\\", DIAG_STREAM); break;
    case '\n': fputs("\\n", DIAG_STREAM); break;
    case '\r': fputs("\\r", DIAG_STREAM); break;
    case '\t': fputs("\\t", DIAG_STREAM); break;
    default:
      if (*p < 0x20)
        fprintf(DIAG_STREAM, "\\u%04x", *p);
      else
        fputc(*p, DIAG_STREAM);
    }
  }
  fputc('"', DIAG_STREAM);
}

static const char *severity_json_name(ErrorSeverity s) {
  switch (s) {
  case DIAG_SEVERITY_ERROR:   return "error";
  case DIAG_SEVERITY_WARNING: return "warning";
  default:                    return "note";
  }
}

/* One diagnostic per line (NDJSON): easy for editors/CI to stream-parse. */
static void error_reporter_print_errors_json(ErrorReporter *reporter) {
  for (size_t i = 0; i < reporter->count; i++) {
    const ErrorReport *e = &reporter->errors[i];
    if (e->severity == DIAG_SEVERITY_NOTE_OF)
      continue;
    fprintf(DIAG_STREAM, "{\"severity\":\"%s\",\"code\":\"%s\",\"message\":",
            severity_json_name(e->severity), error_report_code(e));
    diag_json_string(e->message);
    fprintf(DIAG_STREAM, ",\"file\":");
    diag_json_string(e->filename ? e->filename : "");
    fprintf(DIAG_STREAM, ",\"line\":%zu,\"column\":%zu,\"length\":%zu",
            e->location.line, e->location.column,
            e->span.length > 0 ? e->span.length : 1);
    if (e->span_label) {
      fprintf(DIAG_STREAM, ",\"label\":");
      diag_json_string(e->span_label);
    }
    if (e->suggestion) {
      fprintf(DIAG_STREAM, ",\"help\":");
      diag_json_string(e->suggestion);
    }
    fprintf(DIAG_STREAM, ",\"notes\":[");
    int first_note = 1;
    for (size_t j = i + 1;
         j < reporter->count &&
         reporter->errors[j].severity == DIAG_SEVERITY_NOTE_OF;
         j++) {
      const ErrorReport *n = &reporter->errors[j];
      if (!first_note)
        fputc(',', DIAG_STREAM);
      first_note = 0;
      fprintf(DIAG_STREAM, "{\"message\":");
      diag_json_string(n->message);
      fprintf(DIAG_STREAM, ",\"file\":");
      diag_json_string(n->filename ? n->filename : "");
      fprintf(DIAG_STREAM, ",\"line\":%zu,\"column\":%zu,\"length\":%zu}",
              n->location.line, n->location.column,
              n->span.length > 0 ? n->span.length : 1);
    }
    fprintf(DIAG_STREAM, "]}\n");
  }
}

int error_reporter_get_warning_count(ErrorReporter *reporter) {
  if (!reporter)
    return 0;
  int count = 0;
  for (size_t i = 0; i < reporter->count; i++) {
    if (reporter->errors[i].severity == DIAG_SEVERITY_WARNING)
      count++;
  }
  return count;
}

void error_reporter_add_warning_span_suggestion(ErrorReporter *reporter,
                                                ErrorType type, SourceSpan span,
                                                const char *message,
                                                const char *suggestion) {
  error_reporter_add_report(reporter, DIAG_SEVERITY_WARNING, span, type,
                            message, suggestion);
}


static char *snippet_truncate(const char *line);

#define DIAG_MARGIN 2
#define DIAG_TAB_WIDTH 4
#define DIAG_GUTTER_MIN 3
#define DIAG_LINE_BUFFER (SNIPPET_MAX_COLS * DIAG_TAB_WIDTH + 8)

static size_t diag_append(char *buf, size_t cap, size_t at, const char *fmt,
                          ...) {
  if (at + 1 >= cap) {
    return at;
  }
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buf + at, cap - at, fmt, args);
  va_end(args);
  if (written < 0) {
    return at;
  }
  at += (size_t)written;
  return (at + 1 >= cap) ? cap - 1 : at;
}

static const char *error_severity_sgr(ErrorSeverity severity) {
  switch (severity) {
  case DIAG_SEVERITY_ERROR:
    return diag_sgr_error();
  case DIAG_SEVERITY_WARNING:
    return diag_sgr_warning();
  default:
    return diag_sgr_note();
  }
}

static const char *error_severity_text(ErrorSeverity severity) {
  switch (severity) {
  case DIAG_SEVERITY_ERROR:
    return "error";
  case DIAG_SEVERITY_WARNING:
    return "warning";
  case DIAG_SEVERITY_NOTE:
  case DIAG_SEVERITY_NOTE_OF:
    return "note";
  default:
    return "unknown";
  }
}

static void diag_gutter_lead(FILE *out, size_t gutter) {
  for (size_t i = 0; i < DIAG_MARGIN + gutter + 1; i++) {
    fputc(' ', out);
  }
}

static void diag_row_source(FILE *out, size_t gutter, size_t line,
                            const char *text) {
  const DiagGlyphs *g = diag_glyphs();
  fprintf(out, "%*s%s%*zu %s%s  ", DIAG_MARGIN, "", diag_sgr_dim(),
          (int)gutter, line, g->v, diag_sgr_reset());
  diag_write_source(out, text ? text : "");
  fputc('\n', out);
}

static void diag_row_text(FILE *out, size_t gutter, const char *text) {
  const DiagGlyphs *g = diag_glyphs();
  diag_gutter_lead(out, gutter);
  fprintf(out, "%s%s%s  %s\n", diag_sgr_dim(), g->v, diag_sgr_reset(),
          text ? text : "");
}

static void diag_row_caret(FILE *out, size_t gutter, const char *caret,
                           const char *label, const char *sgr) {
  const DiagGlyphs *g = diag_glyphs();
  diag_gutter_lead(out, gutter);
  fprintf(out, "%s%s%s  %s%s", diag_sgr_dim(), g->dotted, diag_sgr_reset(), sgr,
          caret);
  if (label && label[0]) {
    fprintf(out, " %s", label);
  }
  fprintf(out, "%s\n", diag_sgr_reset());
}

static size_t error_report_max_line(const ErrorReport *e) {
  return e->code_snippet ? e->location.line + 1 : e->location.line;
}

static void diag_render_section(ErrorReporter *reporter, const ErrorReport *e,
                                size_t gutter, int with_context,
                                const char *primary_file) {
  FILE *out = DIAG_STREAM;
  const char *sev_sgr = error_severity_sgr(e->severity);
  const char *filename = e->filename ? e->filename : reporter->filename;

  if (e->severity == DIAG_SEVERITY_NOTE ||
      e->severity == DIAG_SEVERITY_NOTE_OF) {
    char caption[1024];
    size_t at = 0;
    at = diag_append(caption, sizeof(caption), at, "%snote%s  %s", sev_sgr,
                     diag_sgr_reset(), e->message ? e->message : "");
    if (filename && (!primary_file || strcmp(filename, primary_file) != 0)) {
      at = diag_append(caption, sizeof(caption), at, "  %s%s:%zu:%zu%s",
                       diag_sgr_dim(), filename, e->location.line,
                       e->location.column, diag_sgr_reset());
    } else if (!e->code_snippet) {
      diag_append(caption, sizeof(caption), at, "  %sline %zu%s",
                  diag_sgr_dim(), e->location.line, diag_sgr_reset());
    }
    diag_row_text(out, gutter, caption);
  }

  if (!e->code_snippet) {
    return;
  }

  const char *source_code = e->source_code ? e->source_code
                                           : reporter->source_code;
  const size_t line = e->location.line;

  char *prev = NULL;
  char *next = NULL;
  if (with_context) {
    char *prev_raw = (line > 1) ? error_reporter_get_line_from_source(
                                      source_code, line - 1)
                                : NULL;
    char *next_raw = error_reporter_get_line_from_source(source_code, line + 1);
    prev = snippet_truncate(prev_raw);
    next = snippet_truncate(next_raw);
    free(prev_raw);
    free(next_raw);
  }
  char *snippet = snippet_truncate(e->code_snippet);

  char expanded[DIAG_LINE_BUFFER];
  if (prev) {
    diag_expand_tabs(prev, expanded, sizeof(expanded), DIAG_TAB_WIDTH);
    diag_row_source(out, gutter, line - 1, expanded);
  }

  diag_expand_tabs(snippet ? snippet : "", expanded, sizeof(expanded),
                   DIAG_TAB_WIDTH);
  diag_row_source(out, gutter, line, expanded);

  size_t caret_len = (e->span.length > 0) ? e->span.length : 1;
  size_t caret_column = e->location.column;
  /* Clamp the caret to the snippet, which was truncated at SNIPPET_MAX_COLS.
   * A column past that point used to underflow `SNIPPET_MAX_COLS - lead`
   * into a length near SIZE_MAX, which wrapped the caret line's size back
   * down to something small and left the leading-spaces loop writing past
   * it. An error 149 columns into a long line was enough. */
  if (caret_column > SNIPPET_MAX_COLS) {
    caret_column = SNIPPET_MAX_COLS + 1;
    caret_len = 1;
  } else if (caret_column > 0 &&
             caret_column - 1 + caret_len > SNIPPET_MAX_COLS) {
    caret_len = SNIPPET_MAX_COLS - (caret_column - 1);
  }
  caret_column = diag_expanded_column(snippet ? snippet : "", caret_column,
                                      DIAG_TAB_WIDTH);

  char *caret_line = error_reporter_create_caret_line(caret_column, caret_len);
  if (caret_line) {
    diag_row_caret(out, gutter, caret_line, e->span_label, sev_sgr);
    free(caret_line);
  }

  if (next) {
    diag_expand_tabs(next, expanded, sizeof(expanded), DIAG_TAB_WIDTH);
    diag_row_source(out, gutter, line + 1, expanded);
  }

  free(prev);
  free(snippet);
  free(next);
}

static void diag_render_group(ErrorReporter *reporter, const ErrorReport *e,
                              const ErrorReport *const *notes,
                              size_t note_count) {
  FILE *out = DIAG_STREAM;
  const DiagGlyphs *g = diag_glyphs();
  const char *sev_sgr = error_severity_sgr(e->severity);
  const char *reset = diag_sgr_reset();
  const char *filename = e->filename ? e->filename : reporter->filename;

  char where[1024];
  if (filename) {
    snprintf(where, sizeof(where), "%s%s%s%s:%zu:%zu%s", diag_sgr_bold(),
             filename, reset, diag_sgr_dim(), e->location.line,
             e->location.column, reset);
  } else {
    snprintf(where, sizeof(where), "%sline %zu, column %zu%s", diag_sgr_dim(),
             e->location.line, e->location.column, reset);
  }
  diag_rule(out, 0, where, "");

  char lead[128];
  size_t lead_width;
  if (e->severity == DIAG_SEVERITY_NOTE ||
      e->severity == DIAG_SEVERITY_NOTE_OF) {
    snprintf(lead, sizeof(lead), "%s%s%s: ", sev_sgr,
             error_severity_text(e->severity), reset);
    lead_width = strlen(error_severity_text(e->severity)) + 2;
  } else {
    snprintf(lead, sizeof(lead), "%s%s%s%s[%s]%s: ", sev_sgr,
             error_severity_text(e->severity), reset, diag_sgr_dim(),
             error_report_code(e), reset);
    lead_width = strlen(error_severity_text(e->severity)) +
                 strlen(error_report_code(e)) + 4;
  }
  diag_wrap(out, lead, lead_width, e->message ? e->message : "", NULL);

  size_t max_line = error_report_max_line(e);
  for (size_t i = 0; i < note_count; i++) {
    size_t candidate = error_report_max_line(notes[i]);
    if (candidate > max_line) {
      max_line = candidate;
    }
  }
  size_t gutter = error_reporter_count_digits(max_line);
  if (gutter < DIAG_GUTTER_MIN) {
    gutter = DIAG_GUTTER_MIN;
  }

  int framed = e->code_snippet != NULL;
  for (size_t i = 0; !framed && i < note_count; i++) {
    framed = notes[i]->code_snippet != NULL || notes[i]->message != NULL;
  }
  if (framed) {
    fputc('\n', out);
    diag_rule_junction(out, DIAG_MARGIN, gutter, g->tee_down);
    diag_render_section(reporter, e, gutter, 1, filename);
    for (size_t i = 0; i < note_count; i++) {
      diag_rule_junction(out, DIAG_MARGIN, gutter, g->tee_cross);
      diag_render_section(reporter, notes[i], gutter, 0, filename);
    }
    diag_rule_junction(out, DIAG_MARGIN, gutter, g->tee_up);
  }

  const char *help = e->suggestion;
  for (size_t i = 0; !help && i < note_count; i++) {
    help = notes[i]->suggestion;
  }
  if (help) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%*s%shelp%s: ", DIAG_MARGIN, "",
             diag_sgr_cyan(), reset);
    fputc('\n', out);
    diag_wrap(out, prefix, DIAG_MARGIN + 6, help, NULL);
  }
}

void error_reporter_print_errors(ErrorReporter *reporter) {
  if (!reporter)
    return;

  if (g_error_format_json) {
    error_reporter_print_errors_json(reporter);
    return;
  }

  diag_style_output_begin();

  for (size_t i = 0; i < reporter->count; i++) {
    const ErrorReport *e = &reporter->errors[i];
    /* NOTE_OF entries are drawn inside their parent's frame, skip here */
    if (e->severity == DIAG_SEVERITY_NOTE_OF)
      continue;

    const ErrorReport *notes[16];
    size_t note_count = 0;
    for (size_t j = i + 1;
         j < reporter->count &&
         reporter->errors[j].severity == DIAG_SEVERITY_NOTE_OF &&
         note_count < sizeof(notes) / sizeof(notes[0]);
         j++) {
      notes[note_count++] = &reporter->errors[j];
    }

    diag_render_group(reporter, e, notes, note_count);
    fprintf(DIAG_STREAM, "\n");
  }

  int error_count = error_reporter_get_error_count(reporter);
  if (error_count > 0) {
    const char *cyan = diag_sgr_cyan();
    const char *reset = diag_sgr_reset();
    int warning_count = error_reporter_get_warning_count(reporter);
    char summary[512];
    size_t at = 0;
    at = diag_append(summary, sizeof(summary), at, "could not compile");
    if (reporter->filename)
      at = diag_append(summary, sizeof(summary), at, " `%s`",
                       reporter->filename);
    at = diag_append(summary, sizeof(summary), at,
                     " due to %d previous error%s", error_count,
                     error_count == 1 ? "" : "s");
    if (warning_count > 0)
      diag_append(summary, sizeof(summary), at, "; %d warning%s emitted",
                  warning_count, warning_count == 1 ? "" : "s");

    diag_rule(DIAG_STREAM, 0, NULL, NULL);
    fprintf(DIAG_STREAM, "%*s%serror%s: %s\n", DIAG_MARGIN, "",
            diag_sgr_error(), reset, summary);

    /* Point at the first error's code for the explain command */
    for (size_t i = 0; i < reporter->count; i++) {
      if (reporter->errors[i].severity == DIAG_SEVERITY_ERROR) {
        fprintf(DIAG_STREAM,
                "%*s%shelp%s: for more about this error, run `mettle explain "
                "%s`\n",
                DIAG_MARGIN, "", cyan, reset,
                error_report_code(&reporter->errors[i]));
        break;
      }
    }
  }

  diag_style_output_end();
}

/* The space a pointer type's spelling names, or "generic" when it names none. */
static const char *type_name_device_space(const char *name) {
  static const char *const words[] = {"global", "shared", "constant", "local"};
  size_t i;
  if (!name) {
    return "generic";
  }
  for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    const char *found = strstr(name, words[i]);
    if (found && found > name && found[-1] == ' ') {
      return words[i];
    }
  }
  return "generic";
}

/* Truncate a source line for display if it exceeds SNIPPET_MAX_COLS.
   Returns a heap-allocated string; caller must free. */
static char *snippet_truncate(const char *line) {
  if (!line)
    return NULL;
  size_t len = strlen(line);
  if (len <= SNIPPET_MAX_COLS) {
    char *copy = malloc(len + 1);
    if (!copy)
      return NULL;
    memcpy(copy, line, len + 1);
    return copy;
  }
  /* Keep first SNIPPET_MAX_COLS chars and append UTF-8 ellipsis */
  char *buf = malloc(SNIPPET_MAX_COLS + 4); /* 3 bytes for "..." + NUL */
  if (!buf)
    return NULL;
  memcpy(buf, line, SNIPPET_MAX_COLS);
  buf[SNIPPET_MAX_COLS]     = '.';
  buf[SNIPPET_MAX_COLS + 1] = '.';
  buf[SNIPPET_MAX_COLS + 2] = '.';
  buf[SNIPPET_MAX_COLS + 3] = '\0';
  return buf;
}

void error_reporter_print_error(ErrorReporter *reporter,
                                const ErrorReport *error) {
  if (!reporter || !error)
    return;
  diag_style_output_begin();
  diag_render_group(reporter, error, NULL, 0);
  diag_style_output_end();
}

int error_reporter_has_errors(ErrorReporter *reporter) {
  if (!reporter)
    return 0;

  for (size_t i = 0; i < reporter->count; i++) {
    if (reporter->errors[i].severity == DIAG_SEVERITY_ERROR) {
      return 1;
    }
  }
  return 0;
}

int error_reporter_get_error_count(ErrorReporter *reporter) {
  if (!reporter)
    return 0;

  int count = 0;
  for (size_t i = 0; i < reporter->count; i++) {
    if (reporter->errors[i].severity == DIAG_SEVERITY_ERROR) {
      count++;
    }
  }
  return count;
}

SourceLocation source_location_create(size_t line, size_t column) {
  SourceLocation location;
  location.line = line;
  location.column = column;
  location.filename = NULL;
  return location;
}

char *error_reporter_get_line_from_source(const char *source,
                                          size_t line_number) {
  if (!source || line_number == 0)
    return NULL;

  size_t current_line = 1;
  const char *line_start = source;
  const char *line_end = source;

  while (*line_end && current_line < line_number) {
    if (*line_end == '\n') {
      current_line++;
      line_start = line_end + 1;
    }
    line_end++;
  }

  if (current_line != line_number)
    return NULL;

  while (*line_end && *line_end != '\n') {
    line_end++;
  }

  size_t line_length = line_end - line_start;
  char *line = malloc(line_length + 1);
  if (!line)
    return NULL;

  strncpy(line, line_start, line_length);
  line[line_length] = '\0';

  return line;
}

char *error_reporter_create_caret_line(size_t column, size_t length) {
  if (column == 0)
    return NULL;

  size_t lead = column - 1;
  size_t marks = length > 0 ? length : 1;
  /* A span reaching past the end of the address space is a bad span, not a
   * caret line. Answering NULL beats wrapping the sum and allocating less
   * than the loops below go on to write. */
  if (marks > SIZE_MAX - lead - 1) {
    return NULL;
  }
  size_t caret_length = lead + marks;
  char *caret_line = malloc(caret_length + 1);
  if (!caret_line)
    return NULL;

  for (size_t i = 0; i < column - 1; i++) {
    caret_line[i] = ' ';
  }

  for (size_t i = column - 1; i < column - 1 + (length > 0 ? length : 1); i++) {
    caret_line[i] = '^';
  }

  caret_line[caret_length] = '\0';
  return caret_line;
}

const char *error_reporter_suggest_for_token(const char *token) {
  if (!token)
    return NULL;

  if (strcmp(token, "fucntion") == 0)
    return "did you mean 'function'?";
  if (strcmp(token, "retrun") == 0)
    return "did you mean 'return'?";
  if (strcmp(token, "strcut") == 0)
    return "did you mean 'struct'?";
  if (strcmp(token, "methdo") == 0)
    return "did you mean 'method'?";
  if (strcmp(token, "whiel") == 0)
    return "did you mean 'while'?";
  if (strcmp(token, "fi") == 0)
    return "did you mean 'if'?";
  if (strcmp(token, "itn32") == 0)
    return "did you mean 'int32'?";
  if (strcmp(token, "int23") == 0)
    return "did you mean 'int32'?";
  if (strcmp(token, "stirng") == 0)
    return "did you mean 'string'?";
  if (strcmp(token, "flaot32") == 0)
    return "did you mean 'float32'?";
  if (strcmp(token, "flaot64") == 0)
    return "did you mean 'float64'?";

  return NULL;
}

static char er_lower(char c) {
  if (c >= 'A' && c <= 'Z')
    return (char)(c - 'A' + 'a');
  return c;
}

/* Standard iterative Levenshtein distance (insert/delete/substitute = 1),
   computed case-insensitively so "Print"/"print" count as distance 0.
   Returns SIZE_MAX on allocation failure so callers treat it as "no match". */
size_t error_reporter_edit_distance(const char *a, const char *b) {
  if (!a || !b)
    return (size_t)-1;

  size_t la = strlen(a);
  size_t lb = strlen(b);
  if (la == 0)
    return lb;
  if (lb == 0)
    return la;

  /* Single rolling row of size lb+1. */
  size_t *row = malloc((lb + 1) * sizeof(size_t));
  if (!row)
    return (size_t)-1;

  for (size_t j = 0; j <= lb; j++)
    row[j] = j;

  for (size_t i = 1; i <= la; i++) {
    size_t prev_diag = row[0];
    row[0] = i;
    for (size_t j = 1; j <= lb; j++) {
      size_t prev = row[j];
      size_t cost = (er_lower(a[i - 1]) == er_lower(b[j - 1])) ? 0 : 1;
      size_t del = row[j] + 1;
      size_t ins = row[j - 1] + 1;
      size_t sub = prev_diag + cost;
      size_t best = del < ins ? del : ins;
      if (sub < best)
        best = sub;
      row[j] = best;
      prev_diag = prev;
    }
  }

  size_t result = row[lb];
  free(row);
  return result;
}

char *error_reporter_closest_candidate(const char *name,
                                       const char *const *candidates,
                                       size_t count) {
  if (!name || !candidates || count == 0)
    return NULL;

  size_t name_len = strlen(name);
  if (name_len == 0)
    return NULL;

  /* Tolerate roughly one third of the name being wrong, with a floor of 1
     (catches single-character typos in short names like "i"/"j") and a cap
     so unrelated long names don't get spuriously matched. */
  size_t threshold = name_len / 3;
  if (threshold < 1)
    threshold = 1;
  if (threshold > 3)
    threshold = 3;

  const char *best = NULL;
  size_t best_distance = (size_t)-1;

  for (size_t i = 0; i < count; i++) {
    const char *cand = candidates[i];
    if (!cand || cand[0] == '\0')
      continue;
    if (strcmp(cand, name) == 0)
      continue; /* exact match isn't a useful "did you mean" */

    size_t d = error_reporter_edit_distance(name, cand);
    if (d <= threshold && d < best_distance) {
      best_distance = d;
      best = cand;
    }
  }

  return best ? mettle_strdup(best) : NULL;
}

/* Returns a heap-allocated suggestion string the caller must free, or NULL. */
/* ---- type-mismatch suggestions ---------------------------------------------
 * A message that names two types tells the reader what is wrong. The help
 * line has to tell them what to type instead. Mettle converts nothing
 * implicitly, so nearly every mismatch has a concrete answer: a cast, an `&`,
 * a `*`, a comparison. */

static int type_name_is_pointer(const char *name) {
  size_t n = name ? strlen(name) : 0;
  return n > 0 && name[n - 1] == '*';
}

/* Name with one level of pointer removed: "int32*" -> "int32". */
static void type_name_pointee(const char *name, char *out, size_t cap) {
  size_t n = strlen(name);
  if (n > 0 && name[n - 1] == '*') {
    n--;
  }
  while (n > 0 && name[n - 1] == ' ') {
    n--;
  }
  if (n >= cap) {
    n = cap - 1;
  }
  memcpy(out, name, n);
  out[n] = '\0';
}

static int type_name_is_integer(const char *name) {
  return strncmp(name, "int", 3) == 0 || strncmp(name, "uint", 4) == 0 ||
         strcmp(name, "char") == 0;
}

static int type_name_is_float(const char *name) {
  return strcmp(name, "float32") == 0 || strcmp(name, "float64") == 0;
}

static int type_name_is_numeric(const char *name) {
  return !type_name_is_pointer(name) &&
         (type_name_is_integer(name) || type_name_is_float(name));
}

char *error_reporter_suggest_for_type_mismatch(const char *expected,
                                               const char *actual) {
  if (!expected || !actual || strcmp(expected, actual) == 0)
    return NULL;

  char buf[256];

  /* A string literal where a number belongs, and the reverse. These are
     typos rather than conversions, so they get their own wording. */
  if (type_name_is_numeric(expected) && strcmp(actual, "string") == 0) {
    return mettle_strdup("drop the quotes: a numeric literal is 42, not \"42\"");
  }
  if (strcmp(expected, "string") == 0 && type_name_is_numeric(actual)) {
    return mettle_strdup("quote it: a string literal is \"42\", not 42");
  }

  /* string and cstring differ in representation, not in what they hold. */
  if ((strcmp(expected, "cstring") == 0 && strcmp(actual, "string") == 0) ||
      (strcmp(expected, "string") == 0 && strcmp(actual, "cstring") == 0)) {
    snprintf(buf, sizeof(buf), "cast between the two representations: (%s)value",
             expected);
    return mettle_strdup(buf);
  }

  /* Numeric to numeric: always a cast, since Mettle converts nothing on its
     own. Say which direction loses information, because that is the part a
     reader wants to think about before adding the cast. */
  if (type_name_is_numeric(expected) && type_name_is_numeric(actual)) {
    if (type_name_is_float(actual) && type_name_is_integer(expected)) {
      snprintf(buf, sizeof(buf),
               "cast explicitly: (%s)value. The fraction is discarded, not "
               "rounded",
               expected);
    } else if (type_name_is_integer(actual) && type_name_is_float(expected)) {
      snprintf(buf, sizeof(buf), "cast explicitly: (%s)value", expected);
    } else {
      snprintf(buf, sizeof(buf),
               "cast explicitly: (%s)value. Check the value fits, since a "
               "narrowing cast wraps rather than trapping",
               expected);
    }
    return mettle_strdup(buf);
  }

  /* bool against a number: Mettle has no truthiness, so say what to compare. */
  if (strcmp(expected, "bool") == 0 && type_name_is_numeric(actual)) {
    return mettle_strdup("compare explicitly: `value != 0`");
  }
  if (type_name_is_numeric(expected) && strcmp(actual, "bool") == 0) {
    snprintf(buf, sizeof(buf), "cast the boolean: (%s)value, which is 0 or 1",
             expected);
    return mettle_strdup(buf);
  }

  /* Pointer against value: one `&` or one `*` apart is the common slip, so
     check for it before falling back to the generic advice. */
  if (type_name_is_pointer(expected) && !type_name_is_pointer(actual)) {
    char pointee[128];
    type_name_pointee(expected, pointee, sizeof(pointee));
    if (strcmp(pointee, actual) == 0) {
      return mettle_strdup("take the address: `&value`");
    }
    snprintf(buf, sizeof(buf),
             "a '%s' is a pointer; pass an address (`&value`) or a value "
             "already of that type",
             expected);
    return mettle_strdup(buf);
  }
  if (!type_name_is_pointer(expected) && type_name_is_pointer(actual)) {
    char pointee[128];
    type_name_pointee(actual, pointee, sizeof(pointee));
    if (strcmp(pointee, expected) == 0) {
      return mettle_strdup("read through the pointer: `*value` or `value[0]`");
    }
    snprintf(buf, sizeof(buf),
             "'%s' is a pointer and '%s' is not; dereference it, or change the "
             "declared type to '%s'",
             actual, expected, actual);
    return mettle_strdup(buf);
  }

  /* Two device pointers that differ only in where their data lives. Naming
     both spaces is the whole diagnosis: shared memory and global memory are
     two different memories, and a pointer into one is not a pointer into the
     other however the arithmetic that produced it looked. */
  {
    const char *expected_space = type_name_device_space(expected);
    const char *actual_space = type_name_device_space(actual);
    if (type_name_is_pointer(expected) && type_name_is_pointer(actual) &&
        strcmp(expected_space, actual_space) != 0) {
      if (strcmp(actual_space, "generic") == 0) {
        snprintf(buf, sizeof(buf),
                 "this address is %s and %s memory is wanted; say where it "
                 "came from in its type, or claim it with (%s)value, which "
                 "`mettle test` re-checks",
                 actual_space, expected_space, expected);
      } else {
        snprintf(buf, sizeof(buf),
                 "%s memory is wanted and this address is in %s memory; the "
                 "two are different memories, so no cast makes one the other",
                 expected_space, actual_space);
      }
      return mettle_strdup(buf);
    }
  }

  /* Two pointers to different things: a cast is possible but rarely what the
     writer meant, so lead with the likelier cause. */
  if (type_name_is_pointer(expected) && type_name_is_pointer(actual)) {
    snprintf(buf, sizeof(buf),
             "these point at different types; check the declaration, or cast "
             "deliberately with (%s)value",
             expected);
    return mettle_strdup(buf);
  }

  return NULL;
}
