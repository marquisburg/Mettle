#include "ir_optimize_internal.h"
#include "common.h"
#include "../ir_explain_memory.h"
#include "../ir_explain_safety.h"
#include "../../error/diag_style.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define explain_isatty _isatty
#define explain_fileno _fileno
#else
#include <sys/ioctl.h>
#include <unistd.h>
#define explain_isatty isatty
#define explain_fileno fileno
#endif

/* --explain: the optimization report.
 *
 * Every pass that makes a user-visible decision (the loop verifier in
 * ir_optimize_simd_contract.c, the inliner in ir_optimize_inline.c, the MIR
 * eligibility gate in codegen) records a remark here instead of printing
 * directly. At the end of the optimizer pipeline the remarks are sorted into
 * source order and printed as one coherent, human-first report:
 *
 *   saxpy (loop @ line 12): vectorized -> vfmadd231ps, 8-wide float32
 *   process (loop @ line 40): NOT vectorized
 *       |_ reason: each iteration calls `scale`; ...
 *       |_ fix: mark `scale` @inline, or hoist the call out of the loop
 *
 * Remarks are limited to the main input file (the focus file) so imported
 * stdlib modules don't flood the report. */

static MTLC_THREAD_LOCAL int g_explain = 0;
static MTLC_THREAD_LOCAL const char *g_explain_focus_file = NULL;
/* Output binary path (-o): a large report is diverted to a `.explain.txt`
 * sidecar next to it instead of flooding the terminal. */
static MTLC_THREAD_LOCAL const char *g_explain_output_path = NULL;
/* Set while a fix hypothesis is being simulated on a scratch clone: the
 * re-run optimizer passes must not pollute the report with the clone's
 * remarks (the unroller, for one, records remarks from inside the stages). */
static MTLC_THREAD_LOCAL int g_explain_hypothesis = 0;

void ir_explain_set_hypothesis(int active) { g_explain_hypothesis = active; }

void ir_explain_set_output_path(const char *path) {
  g_explain_output_path = path;
}

/* ---- machine-readable report (--explain-json) -------------------------------
 * A `<output-stem>.explain.json` sidecar with the same content as the prose
 * report, for tooling (the editor panel parses this instead of prose). The
 * fragments are accumulated here as sections flush, and finalize assembles
 * the document. */

static MTLC_THREAD_LOCAL int g_explain_json = 0;
/* When set (by --annotate-asm), ir_explain_flush keeps the remark table alive
 * past optimization so the codegen annotator can join it onto emitted asm. */
static MTLC_THREAD_LOCAL int g_explain_retain_remarks = 0;
static MTLC_THREAD_LOCAL char *g_json_buf = NULL;
static MTLC_THREAD_LOCAL size_t g_json_len = 0;
static MTLC_THREAD_LOCAL size_t g_json_cap = 0;

void ir_explain_set_json(int enabled) { g_explain_json = enabled; }

static void ir_explain_json_raw(const char *fmt, ...) {
  va_list args;
  if (!g_explain_json) {
    return;
  }
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    return;
  }
  if (g_json_len + (size_t)needed + 1 > g_json_cap) {
    size_t new_cap = g_json_cap ? g_json_cap * 2 : 4096;
    while (new_cap < g_json_len + (size_t)needed + 1) {
      new_cap *= 2;
    }
    char *grown = realloc(g_json_buf, new_cap);
    if (!grown) {
      return;
    }
    g_json_buf = grown;
    g_json_cap = new_cap;
  }
  va_start(args, fmt);
  vsnprintf(g_json_buf + g_json_len, g_json_cap - g_json_len, fmt, args);
  va_end(args);
  g_json_len += (size_t)needed;
}

/* Append a JSON string literal (quoted, escaped); NULL becomes null. */
static void ir_explain_json_str(const char *s) {
  if (!g_explain_json) {
    return;
  }
  if (!s) {
    ir_explain_json_raw("null");
    return;
  }
  ir_explain_json_raw("\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
    case '"': ir_explain_json_raw("\\\""); break;
    case '\\': ir_explain_json_raw("\\\\"); break;
    case '\n': ir_explain_json_raw("\\n"); break;
    case '\r': ir_explain_json_raw("\\r"); break;
    case '\t': ir_explain_json_raw("\\t"); break;
    default:
      if (c < 0x20) {
        ir_explain_json_raw("\\u%04x", c);
      } else {
        ir_explain_json_raw("%c", c);
      }
    }
  }
  ir_explain_json_raw("\"");
}

/* ---- report buffer ----------------------------------------------------------
 * Both report sections render here first (with color codes; they're stripped
 * if the report goes to a file). Routing happens once, at finalize time, when
 * the total size is known: small reports print to stderr as before, large
 * ones are written to the sidecar with a digest on stderr. */

static MTLC_THREAD_LOCAL char *g_report_buf = NULL;
static MTLC_THREAD_LOCAL size_t g_report_len = 0;
static MTLC_THREAD_LOCAL size_t g_report_cap = 0;

static void ir_explain_emit(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    return;
  }
  if (g_report_len + (size_t)needed + 1 > g_report_cap) {
    size_t new_cap = g_report_cap ? g_report_cap * 2 : 4096;
    while (new_cap < g_report_len + (size_t)needed + 1) {
      new_cap *= 2;
    }
    char *grown = realloc(g_report_buf, new_cap);
    if (!grown) {
      return;
    }
    g_report_buf = grown;
    g_report_cap = new_cap;
  }
  va_start(args, fmt);
  vsnprintf(g_report_buf + g_report_len, g_report_cap - g_report_len, fmt,
            args);
  va_end(args);
  g_report_len += (size_t)needed;
}

/* Where the "where to start" plan goes, and whether it still owes the report a
 * block. The plan ranks codegen results that only exist after the optimization
 * report has been written, so it is rendered last and spliced back into place. */
static MTLC_THREAD_LOCAL size_t g_plan_offset = 0;
static MTLC_THREAD_LOCAL int g_plan_pending = 0;

/* Digest stats collected while the sections render, for the one-paragraph
 * stderr summary that accompanies a file-diverted report. */
static struct {
  size_t loops_vectorized;
  size_t loops_scalar;
  size_t fixes_verified;
  size_t calls_inlined;
  size_t calls_refused;
  size_t backend_ok;
  size_t backend_total;
  size_t changes_improved;
  size_t changes_regressed;
  int had_baseline;
  /* The first "where to start" entry. A diverted report shows counts, and
     counts do not tell anyone what to do; this line does. */
  char start_here[240];
  int start_here_proven;
} g_digest;

/* A named number a pass measured about one remark: an unroll factor, a trip
 * count, how many instructions a callee weighs. Free-form on purpose -- a pass
 * with something to count adds a quantity instead of a struct field, and the
 * JSON carries it without any schema surgery. */
typedef struct {
  char *name;
  long value;
} IRExplainQuantity;

#define IR_EXPLAIN_MAX_QUANTITIES 6

/* One remark: an entity ("loop", "call to `f`") in a function, a colored
 * headline, and optional reason/fix detail lines.
 *
 * The prose is for a person; `code` and the quantities are for tools. A
 * consumer keying off `code` keeps working when the wording is improved, which
 * the wording regularly is. */
typedef struct {
  char *function_name;
  char *entity;
  size_t line;
  size_t column;
  size_t end_line; /* last line of the construct; 0 = unknown */
  int positive; /* 1 = the optimizer did something good (green), 0 = declined */
  char *headline;
  char *reason;   /* may be NULL */
  char *fix;      /* may be NULL */
  char *verified; /* may be NULL: the fix was SIMULATED and proven to work */
  /* May be NULL. The fix was SIMULATED, applied cleanly, and the loop still
     did not vectorize: what this holds is the obstacle that surfaced next.
     A fix worth making that does not finish the job on its own, which is a
     different claim from both "proven" and "unknown". */
  char *partial;
  char *code;     /* stable id for the decision; may be NULL */
  size_t depth;   /* loop nest depth (1 = top level); 0 = not a loop/unknown */
  int trivial;    /* 1 = routine housekeeping a reader can collapse */
  /* 1 = the `fix` text says there is nothing to change: the loop is at its
     floor, or the gap belongs to the compiler. Worth printing, but not an
     instruction, so the triage never ranks it and the prose labels it a
     note rather than a fix. */
  int advisory;
  IRExplainQuantity quantities[IR_EXPLAIN_MAX_QUANTITIES];
  size_t quantity_count;
} IRExplainRemark;

static MTLC_THREAD_LOCAL IRExplainRemark *g_remarks = NULL;
static MTLC_THREAD_LOCAL size_t g_remark_count = 0;
static MTLC_THREAD_LOCAL size_t g_remark_capacity = 0;
/* Did the last ir_explain_remark call actually append? The detail stamps read
 * this so a filtered remark's detail never lands on an unrelated one. */
static MTLC_THREAD_LOCAL int g_last_remark_recorded = 0;

/* Backend (codegen-stage) entries: per function, did it get the
 * register-allocating MIR backend or fall back to baseline codegen? */
typedef struct {
  char *function_name;
  int ok;
  char *detail;        /* gate reason code when !ok */
  size_t instructions; /* non-nop IR size: where baseline codegen COSTS */
} IRExplainBackendEntry;

static MTLC_THREAD_LOCAL IRExplainBackendEntry *g_backend = NULL;
static MTLC_THREAD_LOCAL size_t g_backend_count = 0;
static MTLC_THREAD_LOCAL size_t g_backend_capacity = 0;

/* ---- memory diagnostics (--explain surfacing, fed by the type checker) ---- */
typedef struct {
  int severity; /* 0 = warning, 1 = error */
  size_t line;
  char *code; /* stable finding id (M0101..), may be NULL */
  char *headline;
  char *fix; /* may be NULL */
} IRExplainMemNote;

static MTLC_THREAD_LOCAL IRExplainMemNote *g_mem = NULL;
static MTLC_THREAD_LOCAL size_t g_mem_count = 0;
static MTLC_THREAD_LOCAL size_t g_mem_capacity = 0;
static MTLC_THREAD_LOCAL int g_mem_collect = 0;
static MTLC_THREAD_LOCAL char *g_mem_focus = NULL; /* basename to filter by, or NULL */

/* ---- --safe accounting (fed by the safety pass, before the optimizer) ---- */
typedef struct {
  size_t line;
  char *function_name; /* may be NULL */
  int kind;            /* IRSafetySurvivorKind */
} IRExplainSafetyNote;

/* Enough to show the shape of what is left without turning the report into a
 * listing. The totals are exact regardless; only the per-line detail stops. */
#define IR_EXPLAIN_SAFETY_MAX_NOTES 64

static MTLC_THREAD_LOCAL IRExplainSafetyNote *g_safety = NULL;
static MTLC_THREAD_LOCAL size_t g_safety_count = 0;
static MTLC_THREAD_LOCAL size_t g_safety_capacity = 0;
static MTLC_THREAD_LOCAL int g_safety_collect = 0;
static MTLC_THREAD_LOCAL int g_safety_have_totals = 0;
static MTLC_THREAD_LOCAL char *g_safety_focus = NULL;
static MTLC_THREAD_LOCAL size_t g_safety_emitted = 0;
static MTLC_THREAD_LOCAL size_t g_safety_proved = 0;
static MTLC_THREAD_LOCAL size_t g_safety_hoisted = 0;
static MTLC_THREAD_LOCAL size_t g_safety_spanned = 0;
static MTLC_THREAD_LOCAL size_t g_safety_exempt = 0;
static MTLC_THREAD_LOCAL size_t g_safety_extent_tests = 0;
static MTLC_THREAD_LOCAL size_t g_safety_region_calls = 0;

void ir_optimize_set_explain(int enabled, const char *focus_file) {
  g_explain = enabled;
  g_explain_focus_file = focus_file;
}

int ir_explain_enabled(void) { return g_explain; }

/* ---- prose filter (--explain=SELECTOR) -------------------------------------
 * A whole program's report runs to hundreds of lines, and most of it is the
 * compiler doing the right thing. The selector narrows the prose to the part
 * the reader asked about. The JSON sidecar is never filtered: a tool wants the
 * whole picture and does its own narrowing. */

static MTLC_THREAD_LOCAL const char *g_explain_filter = NULL;

void ir_explain_set_filter(const char *selector) {
  g_explain_filter = (selector && selector[0]) ? selector : NULL;
}

const char *ir_explain_filter(void) { return g_explain_filter; }

static int ir_explain_remark_selected(const IRExplainRemark *r) {
  const char *f = g_explain_filter;
  if (!f) {
    return 1;
  }
  if (strcmp(f, "missed") == 0) {
    return !r->positive;
  }
  if (strcmp(f, "fixable") == 0) {
    return !r->positive && r->fix != NULL;
  }
  if (strcmp(f, "proven") == 0) {
    return r->verified != NULL;
  }
  if (strcmp(f, "loops") == 0) {
    return r->entity && strcmp(r->entity, "loop") == 0;
  }
  if (strcmp(f, "calls") == 0) {
    return r->entity && strncmp(r->entity, "call to ", 8) == 0;
  }
  /* Otherwise a name: the function the decision was made in, or the decision
   * code itself, so both `--explain=saxpy` and `--explain=dot-shape-address`
   * do what they look like they do. */
  if (r->function_name && strcmp(r->function_name, f) == 0) {
    return 1;
  }
  return r->code && strcmp(r->code, f) == 0;
}

void ir_explain_set_retain_remarks(int enabled) {
  g_explain_retain_remarks = enabled ? 1 : 0;
}

/* --annotate-asm reads the collected remarks to enrich its codegen listing with
 * the same verified vectorization/inlining narration. These accessors expose the
 * remark table read-only without leaking the struct definition. */
size_t ir_explain_remark_count(void) { return g_remark_count; }

int ir_explain_remark_at(size_t i, const char **function_name,
                         const char **entity, size_t *line, int *positive,
                         const char **headline, const char **reason,
                         const char **fix, const char **verified,
                         size_t *depth) {
  if (i >= g_remark_count) {
    return 0;
  }
  const IRExplainRemark *r = &g_remarks[i];
  if (function_name) *function_name = r->function_name;
  if (entity) *entity = r->entity;
  if (line) *line = r->line;
  if (positive) *positive = r->positive;
  if (headline) *headline = r->headline;
  if (reason) *reason = r->reason;
  if (fix) *fix = r->fix;
  if (verified) *verified = r->verified;
  if (depth) *depth = r->depth;
  return 1;
}

static const char *ir_explain_path_basename(const char *path) {
  const char *base = path;
  for (; *path; path++) {
    if (*path == '/' || *path == '\\') {
      base = path + 1;
    }
  }
  return base;
}

void ir_explain_safety_set_collect(int enabled, const char *focus_file) {
  g_safety_collect = enabled;
  free(g_safety_focus);
  g_safety_focus = NULL;
  if (enabled && focus_file) {
    const char *base = ir_explain_path_basename(focus_file);
    if (base && *base) {
      g_safety_focus = strdup(base);
    }
  }
}

void ir_explain_safety_note(const char *file, size_t line,
                            const char *function_name,
                            IRSafetySurvivorKind kind) {
  if (!g_safety_collect || g_safety_count >= IR_EXPLAIN_SAFETY_MAX_NOTES) {
    return;
  }
  /* An access with an unknown file is kept rather than risk dropping a real
   * survivor, the same call the memory notes make. */
  if (g_safety_focus && file &&
      strcmp(ir_explain_path_basename(file), g_safety_focus) != 0) {
    return;
  }
  if (g_safety_count >= g_safety_capacity) {
    size_t new_cap = g_safety_capacity ? g_safety_capacity * 2 : 16;
    IRExplainSafetyNote *grown = realloc(g_safety, new_cap * sizeof(*grown));
    if (!grown) {
      return;
    }
    g_safety = grown;
    g_safety_capacity = new_cap;
  }
  IRExplainSafetyNote *note = &g_safety[g_safety_count++];
  note->line = line;
  note->function_name = function_name ? strdup(function_name) : NULL;
  note->kind = (int)kind;
}

typedef struct {
  size_t line;
  char *function_name;
  char *type_name;
  long long min;
  long long max;
  size_t length;
} IRExplainTypedNote;

static MTLC_THREAD_LOCAL IRExplainTypedNote *g_typed = NULL;
static MTLC_THREAD_LOCAL size_t g_typed_count = 0;
static MTLC_THREAD_LOCAL size_t g_typed_capacity = 0;
static MTLC_THREAD_LOCAL size_t g_typed_total = 0;

void ir_explain_safety_typed_note(const char *file, size_t line,
                                  const char *function_name,
                                  const char *type_name, long long min,
                                  long long max, size_t length) {
  if (!g_safety_collect) {
    return;
  }
  if (g_safety_focus && file &&
      strcmp(ir_explain_path_basename(file), g_safety_focus) != 0) {
    return;
  }
  g_typed_total++;
  if (g_typed_count >= IR_EXPLAIN_SAFETY_MAX_NOTES) {
    return;
  }
  if (g_typed_count >= g_typed_capacity) {
    size_t new_cap = g_typed_capacity ? g_typed_capacity * 2 : 16;
    IRExplainTypedNote *grown = realloc(g_typed, new_cap * sizeof(*grown));
    if (!grown) {
      return;
    }
    g_typed = grown;
    g_typed_capacity = new_cap;
  }
  IRExplainTypedNote *note = &g_typed[g_typed_count++];
  note->line = line;
  note->function_name = function_name ? strdup(function_name) : NULL;
  note->type_name = type_name ? strdup(type_name) : NULL;
  note->min = min;
  note->max = max;
  note->length = length;
}

void ir_explain_safety_totals(size_t emitted, size_t proved, size_t hoisted,
                              size_t spanned, size_t exempt,
                              size_t extent_tests, size_t region_calls) {
  if (!g_safety_collect) {
    return;
  }
  g_safety_have_totals = 1;
  g_safety_emitted = emitted;
  g_safety_proved = proved;
  g_safety_hoisted = hoisted;
  g_safety_spanned = spanned;
  g_safety_exempt = exempt;
  g_safety_extent_tests = extent_tests;
  g_safety_region_calls = region_calls;
}

void ir_explain_memory_set_collect(int enabled, const char *focus_file) {
  g_mem_collect = enabled;
  free(g_mem_focus);
  g_mem_focus = NULL;
  if (enabled && focus_file) {
    const char *base = ir_explain_path_basename(focus_file);
    if (base && *base) {
      g_mem_focus = strdup(base);
    }
  }
}

void ir_explain_memory_note(const char *file, int severity, size_t line,
                            const char *code, const char *headline,
                            const char *fix) {
  if (!g_mem_collect || !headline) {
    return;
  }
  /* Scope to the focus file when one is known (mirrors optimizer remarks). A
   * note with an unknown file is kept rather than risk dropping a real one. */
  if (g_mem_focus && file &&
      strcmp(ir_explain_path_basename(file), g_mem_focus) != 0) {
    return;
  }
  if (g_mem_count == g_mem_capacity) {
    size_t new_cap = g_mem_capacity ? g_mem_capacity * 2 : 8;
    IRExplainMemNote *grown = realloc(g_mem, new_cap * sizeof(*grown));
    if (!grown) {
      return;
    }
    g_mem = grown;
    g_mem_capacity = new_cap;
  }
  IRExplainMemNote *n = &g_mem[g_mem_count++];
  n->severity = severity ? 1 : 0;
  n->line = line;
  n->code = code ? strdup(code) : NULL;
  n->headline = strdup(headline);
  n->fix = fix ? strdup(fix) : NULL;
}

int ir_explain_file_enabled(const char *filename) {
  if (!g_explain) {
    return 0;
  }
  if (!g_explain_focus_file || !filename) {
    return 1;
  }
  return strcmp(ir_explain_path_basename(filename),
                ir_explain_path_basename(g_explain_focus_file)) == 0;
}

int ir_explain_location_enabled(const SourceLocation *location) {
  if (!g_explain) {
    return 0;
  }
  if (!location || !location->filename) {
    return g_explain_focus_file == NULL;
  }
  return ir_explain_file_enabled(location->filename);
}

static int ir_explain_use_color(void) { return diag_style_color(); }

#define EXPLAIN_GREEN "\x1b[32m"
#define EXPLAIN_RED "\x1b[31m"
#define EXPLAIN_DIM "\x1b[2m"
#define EXPLAIN_BOLD "\x1b[1m"
#define EXPLAIN_RESET "\x1b[0m"

static const char *clr(const char *code) {
  return ir_explain_use_color() ? code : "";
}

static int ir_explain_use_unicode(void) { return diag_style_unicode(); }

static const char *glyph_elbow(void) { return diag_glyphs()->elbow; }
static const char *glyph_arrow(void) { return diag_glyphs()->arrow; }

/* ---- source echo ------------------------------------------------------------
 * A verdict that says "line 38" makes the reader open the file to find out
 * which loop it means. Printing the line costs one row and answers that, the
 * way the error diagnostics have always done it.
 *
 * The focus file is read once, on the first echo, and held as an index of line
 * starts. Only the focus file: with --explain-all the remarks come from several
 * files and a remark carries no filename of its own, so the report stays quiet
 * rather than quoting the wrong file. */

/* A loop up to this many lines long is quoted whole. */
#define IR_EXPLAIN_ECHO_MAX_LINES 5

static MTLC_THREAD_LOCAL char *g_source_text = NULL;
static MTLC_THREAD_LOCAL char **g_source_lines = NULL;
static MTLC_THREAD_LOCAL size_t g_source_line_count = 0;
static MTLC_THREAD_LOCAL int g_source_tried = 0;

static void ir_explain_source_load(void) {
  if (g_source_tried) {
    return;
  }
  g_source_tried = 1;
  if (!g_explain_focus_file) {
    return;
  }
  FILE *in = fopen(g_explain_focus_file, "rb");
  if (!in) {
    return;
  }
  if (fseek(in, 0, SEEK_END) != 0) {
    fclose(in);
    return;
  }
  long size = ftell(in);
  if (size <= 0 || size > (1L << 24)) {
    fclose(in);
    return;
  }
  rewind(in);
  char *text = malloc((size_t)size + 1);
  if (!text) {
    fclose(in);
    return;
  }
  size_t read = fread(text, 1, (size_t)size, in);
  fclose(in);
  text[read] = '\0';

  size_t lines = 1;
  for (size_t i = 0; i < read; i++) {
    lines += (text[i] == '\n') ? 1 : 0;
  }
  char **index = calloc(lines + 1, sizeof(char *));
  if (!index) {
    free(text);
    return;
  }
  size_t at = 0;
  index[at++] = text;
  for (size_t i = 0; i < read; i++) {
    if (text[i] == '\n') {
      text[i] = '\0';
      if (i > 0 && text[i - 1] == '\r') {
        text[i - 1] = '\0';
      }
      if (at <= lines) {
        index[at++] = text + i + 1;
      }
    }
  }
  g_source_text = text;
  g_source_lines = index;
  g_source_line_count = at;
}

static void ir_explain_source_free(void) {
  free(g_source_lines);
  g_source_lines = NULL;
  free(g_source_text);
  g_source_text = NULL;
  g_source_line_count = 0;
  g_source_tried = 0;
}

/* Columns of leading whitespace on `line`, or (size_t)-1 when it is blank. */
static size_t ir_explain_source_indent(size_t line) {
  const char *text = g_source_lines[line - 1];
  size_t n = 0;
  while (text[n] == ' ' || text[n] == '\t') {
    n++;
  }
  return text[n] ? n : (size_t)-1;
}

static void ir_explain_echo_one(size_t line, size_t strip) {
  const char *text = g_source_lines[line - 1];
  size_t skip = 0;
  while (skip < strip && (text[skip] == ' ' || text[skip] == '\t')) {
    skip++;
  }
  text += skip;
  if (!*text) {
    return;
  }
  char cut[132];
  if (strlen(text) >= sizeof(cut) - 4) {
    memcpy(cut, text, sizeof(cut) - 5);
    memcpy(cut + sizeof(cut) - 5, " ...", 5);
    text = cut;
  }
  char painted[1024];
  diag_source_into(painted, sizeof(painted), text);
  ir_explain_emit("   %s%5zu %s%s %s%c", clr(EXPLAIN_DIM), line,
                  diag_glyphs()->v, clr(EXPLAIN_RESET), painted, 10);
}

/* Echo the source a remark is about. A short loop is quoted whole, so the
 * reader sees the accumulator or the index expression the advice refers to
 * without opening the file; anything longer shows its first line, because a
 * forty-line loop pasted into a report helps nobody. The shared indentation is
 * stripped and the relative shape kept. */
static void ir_explain_echo_source_range(size_t line, size_t end_line) {
  ir_explain_source_load();
  if (!g_source_lines || line == 0 || line > g_source_line_count) {
    return;
  }
  size_t last = line;
  if (end_line > line && end_line <= g_source_line_count &&
      end_line - line <= IR_EXPLAIN_ECHO_MAX_LINES - 1) {
    last = end_line;
  }
  size_t strip = (size_t)-1;
  for (size_t l = line; l <= last; l++) {
    size_t indent = ir_explain_source_indent(l);
    if (indent < strip) {
      strip = indent;
    }
  }
  if (strip == (size_t)-1) {
    strip = 0;
  }
  for (size_t l = line; l <= last; l++) {
    ir_explain_echo_one(l, strip);
  }
}

static void ir_explain_echo_source(size_t line) {
  ir_explain_echo_source_range(line, 0);
}

/* ---- remark store -------------------------------------------------------- */

static char *ir_explain_strdup(const char *s) {
  if (!s) {
    return NULL;
  }
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (copy) {
    memcpy(copy, s, n);
  }
  return copy;
}

/* Copy remark text, transliterating the report's known UTF-8 glyphs to ASCII
 * when the output target can't render UTF-8. Contributors (the loop verifier,
 * the inliner) embed → and — freely; this is the single choke point that keeps
 * them readable everywhere. Every replacement is no longer than the original
 * sequence, so the transliteration runs in place on the copy. */
static char *ir_explain_text_dup(const char *s) {
  char *copy = ir_explain_strdup(s);
  if (!copy || ir_explain_use_unicode()) {
    return copy;
  }
  const unsigned char *read = (const unsigned char *)copy;
  char *write = copy;
  while (*read) {
    if (read[0] == 0xE2 && read[1] == 0x86 && read[2] == 0x92) {
      *write++ = '-'; /* → */
      *write++ = '>';
      read += 3;
    } else if (read[0] == 0xE2 && read[1] == 0x80 && read[2] == 0x94) {
      *write++ = '-'; /* — */
      *write++ = '-';
      read += 3;
    } else if (read[0] == 0xE2 && read[1] == 0x94 && read[2] == 0x94) {
      *write++ = '\\'; /* └ */
      *write++ = '_';
      read += 3;
    } else if (read[0] == 0xE2 && read[1] == 0x94 && read[2] == 0x80) {
      *write++ = '-'; /* ─ */
      read += 3;
    } else if (read[0] >= 0x80) {
      *write++ = '?'; /* any other multibyte: never emit raw mojibake */
      read++;
      while (*read >= 0x80 && *read < 0xC0) {
        read++; /* skip the sequence's continuation bytes */
      }
    } else {
      *write++ = (char)*read++;
    }
  }
  *write = '\0';
  return copy;
}

/* Non-nop instruction weight: what the inliner's budgets, the backend gate and
 * this report all mean by "how big is this function". The inliner keeps a
 * private copy for its hot paths; this is the one the report uses. */
size_t ir_explain_instruction_weight(const IRFunction *function) {
  if (!function) {
    return 0;
  }
  size_t weight = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_NOP) {
      weight++;
    }
  }
  return weight;
}

/* ---- the codegen cost model ------------------------------------------------
 * Published by mir_annotate once every function is encoded. The optimizer's
 * half of the report says what it decided; this says what the decision costs,
 * which is the difference between "this loop did not vectorize" and "this loop
 * did not vectorize and runs 7.2 cycles an iteration, bottlenecked on p23". */
typedef struct {
  char *function_name;
  size_t head_line;
  size_t tail_line;
  int depth;
  int cycles_per_iter; /* centicycles */
  const char *bottleneck;
  int has_kernel;
  int estimated;
} IRExplainLoopCost;

typedef struct {
  char *function_name;
  int spills;
  int regs_used;
  int total_rthru;
  long hot_cost;
  int vec_ops;
  int estimated_spans;
} IRExplainFunctionCost;

static MTLC_THREAD_LOCAL IRExplainLoopCost *g_loop_costs = NULL;
static MTLC_THREAD_LOCAL size_t g_loop_cost_count = 0;
static MTLC_THREAD_LOCAL size_t g_loop_cost_capacity = 0;
static MTLC_THREAD_LOCAL IRExplainFunctionCost *g_function_costs = NULL;
static MTLC_THREAD_LOCAL size_t g_function_cost_count = 0;
static MTLC_THREAD_LOCAL size_t g_function_cost_capacity = 0;

void ir_explain_backend_loop(const char *function_name, const char *filename,
                             size_t head_line, size_t tail_line, int depth,
                             int cycles_per_iter, const char *bottleneck,
                             int has_kernel, int estimated) {
  if (!g_explain || !function_name || !ir_explain_file_enabled(filename)) {
    return;
  }
  if (g_loop_cost_count == g_loop_cost_capacity) {
    size_t grown_capacity = g_loop_cost_capacity ? g_loop_cost_capacity * 2 : 32;
    IRExplainLoopCost *grown =
        realloc(g_loop_costs, grown_capacity * sizeof(IRExplainLoopCost));
    if (!grown) {
      return;
    }
    g_loop_costs = grown;
    g_loop_cost_capacity = grown_capacity;
  }
  IRExplainLoopCost *cost = &g_loop_costs[g_loop_cost_count++];
  cost->function_name = ir_explain_strdup(function_name);
  cost->head_line = head_line;
  cost->tail_line = tail_line;
  cost->depth = depth;
  cost->cycles_per_iter = cycles_per_iter;
  cost->bottleneck = bottleneck; /* static port-name table; not owned */
  cost->has_kernel = has_kernel;
  cost->estimated = estimated;
}

void ir_explain_backend_cost(const char *function_name, const char *filename,
                             int spills, int regs_used, int total_rthru,
                             long hot_cost, int vec_ops, int estimated_spans) {
  if (!g_explain || !function_name || !ir_explain_file_enabled(filename)) {
    return;
  }
  if (g_function_cost_count == g_function_cost_capacity) {
    size_t grown_capacity =
        g_function_cost_capacity ? g_function_cost_capacity * 2 : 32;
    IRExplainFunctionCost *grown = realloc(
        g_function_costs, grown_capacity * sizeof(IRExplainFunctionCost));
    if (!grown) {
      return;
    }
    g_function_costs = grown;
    g_function_cost_capacity = grown_capacity;
  }
  IRExplainFunctionCost *cost = &g_function_costs[g_function_cost_count++];
  cost->function_name = ir_explain_strdup(function_name);
  cost->spills = spills;
  cost->regs_used = regs_used;
  cost->total_rthru = total_rthru;
  cost->hot_cost = hot_cost;
  cost->vec_ops = vec_ops;
  cost->estimated_spans = estimated_spans;
}

/* ---- the pass ledger -------------------------------------------------------
 * What each optimization pass actually did to this file: how often it ran, how
 * often it changed something, and the net instructions it removed. A pass that
 * runs forty times and never fires is as interesting as one that halves the
 * function -- both are invisible in a report that only lists vectorization and
 * inlining. Only collected under --explain, and only for the focus file. */
#define IR_EXPLAIN_OPCODE_LIMIT ((size_t)IR_OP_SELECT + 1)
#define IR_EXPLAIN_MAX_SITES 256
#define IR_EXPLAIN_REPORTED_SITES 12
#define IR_EXPLAIN_MAX_SHAPE_LINES 1024

/* The shape of a function: how many of each opcode, and how many instructions
 * sit on each source line. Diffing two shapes says what a pass actually did --
 * "removed 8 loads and 8 stores at lines 38 and 39" rather than "changed
 * something". Cheap enough to take twice per pass run, which is the price of
 * the ledger being worth reading. */
typedef struct {
  int *opcodes;
  size_t *lines;
  int *line_counts;
  size_t line_count;
  size_t line_capacity;
} IRExplainShape;

/* One (function, line) the pass moved instructions at. */
typedef struct {
  char *function_name;
  size_t line;
  long delta; /* positive = instructions removed here */
} IRExplainSite;

typedef struct {
  const char *name; /* static pass-name pointer; not owned */
  size_t runs;
  size_t changed_runs;
  long instructions_removed; /* negative = the pass added instructions */
  int *opcode_delta;         /* per-opcode net change, lazily allocated */
  IRExplainSite *sites;
  size_t site_count;
  size_t site_capacity;
} IRExplainPassEntry;

#define IR_EXPLAIN_MAX_PASSES 128
static MTLC_THREAD_LOCAL IRExplainPassEntry g_passes[IR_EXPLAIN_MAX_PASSES];
static MTLC_THREAD_LOCAL size_t g_pass_count = 0;

/* The two scratch shapes, reused for every pass run so the ledger allocates
 * once rather than per pass. */
static MTLC_THREAD_LOCAL IRExplainShape g_shape_before;
static MTLC_THREAD_LOCAL IRExplainShape g_shape_after;

static int ir_explain_shape_reserve(IRExplainShape *shape) {
  if (!shape->opcodes) {
    shape->opcodes = calloc(IR_EXPLAIN_OPCODE_LIMIT, sizeof(int));
    if (!shape->opcodes) {
      return 0;
    }
  }
  if (!shape->lines) {
    shape->lines = calloc(IR_EXPLAIN_MAX_SHAPE_LINES, sizeof(size_t));
    shape->line_counts = calloc(IR_EXPLAIN_MAX_SHAPE_LINES, sizeof(int));
    if (!shape->lines || !shape->line_counts) {
      return 0;
    }
    shape->line_capacity = IR_EXPLAIN_MAX_SHAPE_LINES;
  }
  return 1;
}

/* Count the function into `shape`. Instructions run roughly in line order, so
 * the last-line fast path turns the histogram into a linear scan in practice. */
static int ir_explain_shape_take(IRExplainShape *shape,
                                 const IRFunction *function) {
  if (!ir_explain_shape_reserve(shape)) {
    return 0;
  }
  memset(shape->opcodes, 0, IR_EXPLAIN_OPCODE_LIMIT * sizeof(int));
  memset(shape->line_counts, 0, shape->line_capacity * sizeof(int));
  shape->line_count = 0;

  const char *own_file = function->instructions[0].location.filename;
  size_t last = (size_t)-1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if ((size_t)instruction->op < IR_EXPLAIN_OPCODE_LIMIT) {
      shape->opcodes[instruction->op]++;
    }
    size_t line = instruction->location.line;
    if (line == 0) {
      continue;
    }
    /* An inlined callee's instructions carry ITS file's lines. Counting those
     * would report sites at line numbers that mean nothing in the file the
     * report is about, so only the function's own file contributes. */
    if (own_file && instruction->location.filename &&
        strcmp(instruction->location.filename, own_file) != 0) {
      continue;
    }
    if (last < shape->line_count && shape->lines[last] == line) {
      shape->line_counts[last]++;
      continue;
    }
    size_t slot = (size_t)-1;
    for (size_t s = 0; s < shape->line_count; s++) {
      if (shape->lines[s] == line) {
        slot = s;
        break;
      }
    }
    if (slot == (size_t)-1) {
      if (shape->line_count >= shape->line_capacity) {
        continue; /* pathologically wide function: the opcode delta still lands */
      }
      slot = shape->line_count++;
      shape->lines[slot] = line;
    }
    shape->line_counts[slot]++;
    last = slot;
  }
  return 1;
}

static void ir_explain_pass_record_site(IRExplainPassEntry *entry,
                                        const char *function_name, size_t line,
                                        long delta) {
  for (size_t i = 0; i < entry->site_count; i++) {
    if (entry->sites[i].line == line &&
        strcmp(entry->sites[i].function_name, function_name) == 0) {
      entry->sites[i].delta += delta;
      return;
    }
  }
  if (entry->site_count == entry->site_capacity) {
    if (entry->site_capacity >= IR_EXPLAIN_MAX_SITES) {
      return;
    }
    size_t grown_capacity = entry->site_capacity ? entry->site_capacity * 2 : 16;
    IRExplainSite *grown =
        realloc(entry->sites, grown_capacity * sizeof(IRExplainSite));
    if (!grown) {
      return;
    }
    entry->sites = grown;
    entry->site_capacity = grown_capacity;
  }
  IRExplainSite *site = &entry->sites[entry->site_count++];
  site->function_name = ir_explain_strdup(function_name);
  site->line = line;
  site->delta = delta;
}

/* Is this pass run worth measuring? */
static int ir_explain_pass_tracked(const IRFunction *function) {
  return g_explain && !g_explain_hypothesis && function &&
         function->instruction_count > 0 &&
         ir_explain_location_enabled(&function->instructions[0].location);
}

void ir_explain_pass_begin(const IRFunction *function) {
  if (!ir_explain_pass_tracked(function)) {
    return;
  }
  ir_explain_shape_take(&g_shape_before, function);
}

void ir_explain_pass_end(const IRFunction *function, const char *name,
                         int changed) {
  if (!name || !ir_explain_pass_tracked(function)) {
    return;
  }
  IRExplainPassEntry *entry = NULL;
  for (size_t i = 0; i < g_pass_count; i++) {
    if (g_passes[i].name == name || strcmp(g_passes[i].name, name) == 0) {
      entry = &g_passes[i];
      break;
    }
  }
  if (!entry) {
    if (g_pass_count >= IR_EXPLAIN_MAX_PASSES) {
      return;
    }
    entry = &g_passes[g_pass_count++];
    memset(entry, 0, sizeof(*entry));
    entry->name = name;
  }
  entry->runs++;
  if (!changed) {
    return; /* a clean run has nothing to describe */
  }
  entry->changed_runs++;

  if (!ir_explain_shape_take(&g_shape_after, function) ||
      !g_shape_before.opcodes) {
    return;
  }

  long removed = 0;
  if (!entry->opcode_delta) {
    entry->opcode_delta = calloc(IR_EXPLAIN_OPCODE_LIMIT, sizeof(int));
  }
  for (size_t op = 0; op < IR_EXPLAIN_OPCODE_LIMIT; op++) {
    int delta = g_shape_before.opcodes[op] - g_shape_after.opcodes[op];
    removed += delta;
    if (delta != 0 && entry->opcode_delta) {
      entry->opcode_delta[op] += delta;
    }
  }
  entry->instructions_removed += removed;

  /* Where it happened: lines whose instruction count moved. */
  for (size_t i = 0; i < g_shape_before.line_count; i++) {
    size_t line = g_shape_before.lines[i];
    int after = 0;
    for (size_t j = 0; j < g_shape_after.line_count; j++) {
      if (g_shape_after.lines[j] == line) {
        after = g_shape_after.line_counts[j];
        break;
      }
    }
    int delta = g_shape_before.line_counts[i] - after;
    if (delta != 0) {
      ir_explain_pass_record_site(entry, function->name ? function->name : "?",
                                  line, delta);
    }
  }
  for (size_t j = 0; j < g_shape_after.line_count; j++) {
    size_t line = g_shape_after.lines[j];
    int existed = 0;
    for (size_t i = 0; i < g_shape_before.line_count; i++) {
      if (g_shape_before.lines[i] == line) {
        existed = 1;
        break;
      }
    }
    if (!existed) {
      ir_explain_pass_record_site(entry, function->name ? function->name : "?",
                                  line, -g_shape_after.line_counts[j]);
    }
  }
}

/* ---- the per-function table ------------------------------------------------
 * One row per function in the focus file: where it starts, what it weighed
 * before and after optimization, and (merged in at flush time) how it fared in
 * the backend. The report's spine -- everything else hangs off a function. */
typedef struct {
  char *name;
  size_t line;
  size_t instructions_before;
  size_t instructions_after;
  /* Tallied from the remarks before they are freed, because the section is
   * written later -- once codegen has decided the backend half. */
  size_t loops;
  size_t loops_vectorized;
  size_t calls_inlined;
  size_t calls_refused;
} IRExplainFunctionEntry;

static MTLC_THREAD_LOCAL IRExplainFunctionEntry *g_functions = NULL;
static MTLC_THREAD_LOCAL size_t g_function_count = 0;
static MTLC_THREAD_LOCAL size_t g_function_capacity = 0;

static IRExplainFunctionEntry *ir_explain_function_entry(const char *name) {
  for (size_t i = 0; i < g_function_count; i++) {
    if (strcmp(g_functions[i].name, name) == 0) {
      return &g_functions[i];
    }
  }
  if (g_function_count == g_function_capacity) {
    size_t grown_capacity = g_function_capacity ? g_function_capacity * 2 : 32;
    IRExplainFunctionEntry *grown =
        realloc(g_functions, grown_capacity * sizeof(IRExplainFunctionEntry));
    if (!grown) {
      return NULL;
    }
    g_functions = grown;
    g_function_capacity = grown_capacity;
  }
  IRExplainFunctionEntry *entry = &g_functions[g_function_count++];
  entry->name = ir_explain_strdup(name);
  entry->line = 0;
  entry->instructions_before = 0;
  entry->instructions_after = 0;
  entry->loops = 0;
  entry->loops_vectorized = 0;
  entry->calls_inlined = 0;
  entry->calls_refused = 0;
  return entry;
}

void ir_explain_function_before(const IRFunction *function) {
  if (!g_explain || g_explain_hypothesis || !function || !function->name ||
      function->instruction_count == 0 ||
      !ir_explain_location_enabled(&function->instructions[0].location)) {
    return;
  }
  IRExplainFunctionEntry *entry = ir_explain_function_entry(function->name);
  if (!entry) {
    return;
  }
  entry->line = function->instructions[0].location.line;
  entry->instructions_before = ir_explain_instruction_weight(function);
  entry->instructions_after = entry->instructions_before;
}

void ir_explain_function_after(const IRFunction *function) {
  if (!g_explain || g_explain_hypothesis || !function || !function->name ||
      function->instruction_count == 0 ||
      !ir_explain_location_enabled(&function->instructions[0].location)) {
    return;
  }
  IRExplainFunctionEntry *entry = ir_explain_function_entry(function->name);
  if (entry) {
    entry->instructions_after = ir_explain_instruction_weight(function);
  }
}

void ir_explain_remark(const char *function_name, const char *entity,
                       SourceLocation location, int positive,
                       const char *headline, const char *reason,
                       const char *fix, const char *verified) {
  g_last_remark_recorded = 0;
  if (!g_explain || g_explain_hypothesis || !headline ||
      !ir_explain_location_enabled(&location)) {
    return;
  }

  /* Dedupe: an inlined callee's body can be cloned into several callers, each
   * clone carrying the callee's original source locations; report the
   * decision once. */
  for (size_t i = 0; i < g_remark_count; i++) {
    IRExplainRemark *r = &g_remarks[i];
    if (r->line == location.line && r->column == location.column &&
        r->entity && entity && strcmp(r->entity, entity) == 0 &&
        strcmp(r->headline, headline) == 0) {
      return;
    }
  }

  if (g_remark_count == g_remark_capacity) {
    size_t new_capacity = g_remark_capacity ? g_remark_capacity * 2 : 32;
    IRExplainRemark *grown =
        realloc(g_remarks, new_capacity * sizeof(IRExplainRemark));
    if (!grown) {
      return;
    }
    g_remarks = grown;
    g_remark_capacity = new_capacity;
  }

  IRExplainRemark *r = &g_remarks[g_remark_count++];
  r->function_name = ir_explain_strdup(function_name ? function_name : "?");
  r->entity = ir_explain_text_dup(entity ? entity : "loop");
  r->line = location.line;
  r->column = location.column;
  r->end_line = 0;
  r->positive = positive;
  r->headline = ir_explain_text_dup(headline);
  r->reason = ir_explain_text_dup(reason);
  r->fix = ir_explain_text_dup(fix);
  r->verified = ir_explain_text_dup(verified);
  r->partial = NULL;
  r->code = NULL;
  r->depth = 0;
  r->trivial = 0;
  r->advisory = 0;
  r->quantity_count = 0;
  g_last_remark_recorded = 1;
}

/* The remark most recently recorded, or NULL when the last ir_explain_remark
 * call was filtered out (wrong file, hypothesis run, duplicate). Every stamp
 * below goes through here, so a suppressed remark can never have its detail
 * land on the previous one. */
static IRExplainRemark *ir_explain_last_remark(void) {
  if (!g_last_remark_recorded || g_remark_count == 0) {
    return NULL;
  }
  return &g_remarks[g_remark_count - 1];
}

void ir_explain_remark_code(const char *code) {
  IRExplainRemark *r = ir_explain_last_remark();
  if (r && !r->code) {
    r->code = ir_explain_text_dup(code);
  }
}

void ir_explain_remark_extent(size_t end_line) {
  IRExplainRemark *r = ir_explain_last_remark();
  if (r && end_line >= r->line) {
    r->end_line = end_line;
  }
}

void ir_explain_remark_trivial(void) {
  IRExplainRemark *r = ir_explain_last_remark();
  if (r) {
    r->trivial = 1;
  }
}

void ir_explain_remark_advisory(void) {
  IRExplainRemark *r = ir_explain_last_remark();
  if (r) {
    r->advisory = 1;
  }
}

void ir_explain_remark_partial(const char *what_still_blocks) {
  IRExplainRemark *r = ir_explain_last_remark();
  if (r && what_still_blocks && what_still_blocks[0]) {
    free(r->partial);
    r->partial = ir_explain_text_dup(what_still_blocks);
  }
}

void ir_explain_remark_quantity(const char *name, long value) {
  IRExplainRemark *r = ir_explain_last_remark();
  if (!r || !name || r->quantity_count >= IR_EXPLAIN_MAX_QUANTITIES) {
    return;
  }
  IRExplainQuantity *q = &r->quantities[r->quantity_count++];
  q->name = ir_explain_text_dup(name);
  q->value = value;
}

/* Stamp the nest depth on the most recent loop remark at `line` (the
 * contract walker computes containment after recording). 1 = top level. */
void ir_explain_remark_loop_depth(size_t line, size_t depth) {
  for (size_t i = g_remark_count; i > 0; i--) {
    IRExplainRemark *r = &g_remarks[i - 1];
    if (r->line == line && r->entity && strcmp(r->entity, "loop") == 0) {
      r->depth = depth;
      return;
    }
  }
}

int ir_explain_has_remark_at(size_t line, const char *entity) {
  for (size_t i = 0; i < g_remark_count; i++) {
    if (g_remarks[i].line == line && g_remarks[i].entity && entity &&
        strcmp(g_remarks[i].entity, entity) == 0) {
      return 1;
    }
  }
  return 0;
}

size_t ir_explain_inlined_calls_in_range(const char *function_name,
                                         size_t first_line, size_t last_line,
                                         size_t *callee_line, char *callee_out,
                                         size_t callee_cap) {
  size_t hits = 0;
  if (callee_out && callee_cap) {
    callee_out[0] = '\0';
  }
  if (callee_line) {
    *callee_line = 0;
  }
  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    if (!r->positive || r->line < first_line || r->line > last_line) {
      continue;
    }
    if (!r->function_name || !function_name ||
        strcmp(r->function_name, function_name) != 0) {
      continue;
    }
    if (!r->code || strcmp(r->code, "inlined") != 0) {
      continue;
    }
    hits++;
    if (hits > 1) {
      continue; /* ambiguous: the caller must not name one of several */
    }
    if (callee_line) {
      *callee_line = r->line;
    }
    /* The entity reads "call to `name`"; hand back just the name. */
    const char *open = r->entity ? strchr(r->entity, '`') : NULL;
    const char *close = open ? strchr(open + 1, '`') : NULL;
    if (open && close && callee_out && callee_cap) {
      size_t n = (size_t)(close - open - 1);
      if (n >= callee_cap) {
        n = callee_cap - 1;
      }
      memcpy(callee_out, open + 1, n);
      callee_out[n] = '\0';
    }
  }
  return hits;
}

/* The bracketed decision id that follows a verdict, ready to paste into
 * `mettle explain`. Empty when the pass recorded no id (positive housekeeping
 * remarks mostly), so the line reads exactly as it did before. Two rotating
 * buffers let one emit call carry two tags. */
static const char *ir_explain_code_tag(const IRExplainRemark *r) {
  static MTLC_THREAD_LOCAL char buf[2][96];
  static MTLC_THREAD_LOCAL int slot = 0;
  if (!r->code || !r->code[0] || strcmp(r->code, "none") == 0) {
    return "";
  }
  /* On a success the headline already names what happened, and the id is
   * the same word again. It carries information only on a refusal, where it
   * names the reason and is the argument to `mettle explain`. */
  if (r->positive) {
    return "";
  }
  char *out = buf[slot];
  slot = (slot + 1) & 1;
  snprintf(out, sizeof(buf[0]), "  %s[%s]%s", clr(EXPLAIN_DIM), r->code,
           clr(EXPLAIN_RESET));
  return out;
}

static int ir_explain_remark_compare(const void *a, const void *b) {
  const IRExplainRemark *ra = a, *rb = b;
  if (ra->line != rb->line) {
    return ra->line < rb->line ? -1 : 1;
  }
  if (ra->column != rb->column) {
    return ra->column < rb->column ? -1 : 1;
  }
  return 0;
}

/* ---- repeated-refusal aggregation ------------------------------------------
 * Real-world functions (a setup-heavy main, an init routine) produce WALLS of
 * identical call refusals -- one fact ("main is over the caller budget")
 * repeated for every call site, drowning the remarks that matter. Identical
 * (caller, headline, reason, fix) call remarks are folded into one entry with
 * the line range and a deduplicated callee list. Remarks carrying a verified
 * line are never folded: each is a per-site proof. */

#define IR_EXPLAIN_DOCS_BASE "https://suidvandiewereld.github.io/Mettle"
#define IR_EXPLAIN_GROUP_MIN 4
#define IR_EXPLAIN_GROUP_LIST_MAX 6

static int ir_explain_str_eq(const char *a, const char *b) {
  if (!a || !b) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

/* A call remark eligible for folding: "call to `f`" entity with a reason
 * (the repeated-refusal shape). */
static int ir_explain_remark_foldable(const IRExplainRemark *r) {
  return r->entity && strncmp(r->entity, "call to ", 8) == 0;
}

/* Verified text participates in the key: a generic per-group claim ("with
 * @inline this will inline") folds with its group, while per-site proofs
 * that differ in wording keep their own entries. */
static int ir_explain_remarks_groupable(const IRExplainRemark *a,
                                        const IRExplainRemark *b) {
  return ir_explain_str_eq(a->function_name, b->function_name) &&
         ir_explain_str_eq(a->headline, b->headline) &&
         ir_explain_str_eq(a->reason, b->reason) &&
         ir_explain_str_eq(a->fix, b->fix) &&
         ir_explain_str_eq(a->verified, b->verified) &&
         ir_explain_str_eq(a->partial, b->partial);
}

/* The callee name inside a "call to `f`" entity; "?" when unparsable. */
static void ir_explain_entity_callee(const char *entity, char *buf,
                                     size_t cap) {
  const char *open = entity ? strchr(entity, '`') : NULL;
  const char *close = open ? strchr(open + 1, '`') : NULL;
  if (!open || !close || (size_t)(close - open) >= cap) {
    snprintf(buf, cap, "?");
    return;
  }
  size_t n = (size_t)(close - open - 1);
  memcpy(buf, open + 1, n);
  buf[n] = '\0';
}

/* Build the group's deduplicated callee list ("a, b (x9), c ... and N more")
 * into `out`. Membership is determined by the same predicate the flush loop
 * groups by, starting at the group leader `first`. */
static void ir_explain_group_callee_list(const IRExplainRemark *remarks,
                                         size_t first, char *out, size_t cap) {
  char names[64][96];
  size_t name_counts[64];
  size_t n_names = 0;

  for (size_t j = first; j < g_remark_count; j++) {
    if (!ir_explain_remark_foldable(&remarks[j]) ||
        !ir_explain_remarks_groupable(&remarks[first], &remarks[j])) {
      continue;
    }
    char callee[96];
    ir_explain_entity_callee(remarks[j].entity, callee, sizeof(callee));
    size_t k = 0;
    for (; k < n_names; k++) {
      if (strcmp(names[k], callee) == 0) {
        name_counts[k]++;
        break;
      }
    }
    if (k == n_names && n_names < 64) {
      snprintf(names[n_names], sizeof(names[0]), "%s", callee);
      name_counts[n_names] = 1;
      n_names++;
    }
  }

  size_t written = 0;
  out[0] = '\0';
  size_t shown = n_names < IR_EXPLAIN_GROUP_LIST_MAX
                     ? n_names
                     : IR_EXPLAIN_GROUP_LIST_MAX;
  for (size_t k = 0; k < shown; k++) {
    int n;
    if (name_counts[k] > 1) {
      n = snprintf(out + written, cap - written, "%s%s (x%zu)",
                   k ? ", " : "", names[k], name_counts[k]);
    } else {
      n = snprintf(out + written, cap - written, "%s%s", k ? ", " : "",
                   names[k]);
    }
    if (n < 0 || (size_t)n >= cap - written) {
      return;
    }
    written += (size_t)n;
  }
  if (n_names > shown) {
    snprintf(out + written, cap - written, " ... and %zu more",
             n_names - shown);
  }
}

/* ---- "since last build" diffing ---------------------------------------------
 * Each explain build writes a compact baseline of its loop/call outcomes to
 * `<output-stem>.explain.base`; the next build compares before rendering and
 * leads the report with what CHANGED -- newly vectorized loops, and (the part
 * benchmarks find too late) regressions. Entities are matched by (function,
 * ordinal within the function) so ordinary edits that shift line numbers do
 * not produce false alarms. */

typedef struct {
  char function_name[128];
  char callee[96]; /* calls only; empty for loops */
  size_t ordinal;
  size_t line;
  char status; /* 'V'/'S' for loops, 'I'/'R' for calls */
  char kind;   /* 'L' or 'C' */
  const char *reason; /* current-side only: points into g_remarks */
} IRExplainBaseKey;

/* Status for diffing, or 0 when the remark is not tracked. "vectorized
 * inner, scalar outer" is intentionally untracked: its own status lives on
 * the inner loop's remark. */
static char ir_explain_remark_status(const IRExplainRemark *r, char *kind) {
  if (!r->entity || !r->headline) {
    return 0;
  }
  if (strcmp(r->entity, "loop") == 0) {
    *kind = 'L';
    if (strncmp(r->headline, "vectorized inner", 16) == 0) {
      return 0;
    }
    if (strncmp(r->headline, "vectorized", 10) == 0) {
      return 'V';
    }
    if (strncmp(r->headline, "NOT vectorized", 14) == 0) {
      return 'S';
    }
    return 0;
  }
  if (strncmp(r->entity, "call to ", 8) == 0) {
    *kind = 'C';
    if (strcmp(r->headline, "inlined") == 0) {
      return 'I';
    }
    if (strcmp(r->headline, "NOT inlined") == 0) {
      return 'R';
    }
    return 0;
  }
  return 0;
}

/* Build the tracked-outcome list from the (already sorted) remarks.
 * Returns a malloc'd array; count in *count_out. */
static IRExplainBaseKey *ir_explain_build_keys(size_t *count_out) {
  IRExplainBaseKey *keys = calloc(g_remark_count ? g_remark_count : 1,
                                  sizeof(IRExplainBaseKey));
  size_t count = 0;
  if (!keys) {
    *count_out = 0;
    return NULL;
  }
  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    char kind = 0;
    char status = ir_explain_remark_status(r, &kind);
    if (!status) {
      continue;
    }
    IRExplainBaseKey *k = &keys[count];
    snprintf(k->function_name, sizeof(k->function_name), "%s",
             r->function_name ? r->function_name : "?");
    k->callee[0] = '\0';
    if (kind == 'C') {
      ir_explain_entity_callee(r->entity, k->callee, sizeof(k->callee));
    }
    k->kind = kind;
    k->status = status;
    k->line = r->line;
    k->reason = r->reason;
    /* ordinal: how many earlier tracked entries share (kind, fn, callee) */
    k->ordinal = 0;
    for (size_t j = 0; j < count; j++) {
      if (keys[j].kind == kind &&
          strcmp(keys[j].function_name, k->function_name) == 0 &&
          strcmp(keys[j].callee, k->callee) == 0) {
        k->ordinal++;
      }
    }
    count++;
  }
  *count_out = count;
  return keys;
}

/* `<dir>/<stem><suffix>` from the output path; caller frees. */
static char *ir_explain_derived_path(const char *suffix) {
  if (!g_explain_output_path) {
    return NULL;
  }
  size_t base_len = strlen(g_explain_output_path);
  const char *last_dot = NULL;
  for (const char *p = g_explain_output_path; *p; p++) {
    if (*p == '.') {
      last_dot = p;
    } else if (*p == '/' || *p == '\\') {
      last_dot = NULL;
    }
  }
  size_t stem_len = last_dot ? (size_t)(last_dot - g_explain_output_path)
                             : base_len;
  char *path = malloc(stem_len + strlen(suffix) + 1);
  if (!path) {
    return NULL;
  }
  memcpy(path, g_explain_output_path, stem_len);
  strcpy(path + stem_len, suffix);
  return path;
}

static IRExplainBaseKey *ir_explain_read_baseline(size_t *count_out) {
  *count_out = 0;
  char *path = ir_explain_derived_path(".explain.base");
  if (!path) {
    return NULL;
  }
  FILE *in = fopen(path, "rb");
  free(path);
  if (!in) {
    return NULL;
  }
  IRExplainBaseKey *keys = NULL;
  size_t count = 0, capacity = 0;
  char line[512];
  while (fgets(line, sizeof(line), in)) {
    char kind = line[0];
    if ((kind != 'L' && kind != 'C') || line[1] != '\t') {
      continue;
    }
    if (count == capacity) {
      size_t new_capacity = capacity ? capacity * 2 : 64;
      IRExplainBaseKey *grown =
          realloc(keys, new_capacity * sizeof(IRExplainBaseKey));
      if (!grown) {
        break;
      }
      keys = grown;
      capacity = new_capacity;
    }
    IRExplainBaseKey *k = &keys[count];
    memset(k, 0, sizeof(*k));
    k->kind = kind;
    /* L \t fn \t ordinal \t status \t line
     * C \t fn \t callee \t ordinal \t status \t line */
    char *cursor = line + 2;
    char *fields[5] = {0};
    int n_fields = 0;
    while (cursor && n_fields < 5) {
      fields[n_fields++] = cursor;
      cursor = strchr(cursor, '\t');
      if (cursor) {
        *cursor++ = '\0';
      }
    }
    int needed = kind == 'L' ? 4 : 5;
    if (n_fields < needed) {
      continue;
    }
    snprintf(k->function_name, sizeof(k->function_name), "%s", fields[0]);
    int field = 1;
    if (kind == 'C') {
      snprintf(k->callee, sizeof(k->callee), "%s", fields[field++]);
    }
    k->ordinal = (size_t)strtoul(fields[field++], NULL, 10);
    k->status = fields[field++][0];
    k->line = (size_t)strtoul(fields[field], NULL, 10);
    count++;
  }
  fclose(in);
  *count_out = count;
  return keys;
}

static void ir_explain_write_baseline(const IRExplainBaseKey *keys,
                                      size_t count) {
  char *path = ir_explain_derived_path(".explain.base");
  if (!path) {
    return;
  }
  FILE *out = fopen(path, "wb");
  free(path);
  if (!out) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    const IRExplainBaseKey *k = &keys[i];
    if (k->kind == 'L') {
      fprintf(out, "L\t%s\t%zu\t%c\t%zu\n", k->function_name, k->ordinal,
              k->status, k->line);
    } else {
      fprintf(out, "C\t%s\t%s\t%zu\t%c\t%zu\n", k->function_name, k->callee,
              k->ordinal, k->status, k->line);
    }
  }
  fclose(out);
}

static const IRExplainBaseKey *
ir_explain_find_key(const IRExplainBaseKey *keys, size_t count,
                    const IRExplainBaseKey *want) {
  for (size_t i = 0; i < count; i++) {
    if (keys[i].kind == want->kind && keys[i].ordinal == want->ordinal &&
        strcmp(keys[i].function_name, want->function_name) == 0 &&
        strcmp(keys[i].callee, want->callee) == 0) {
      return &keys[i];
    }
  }
  return NULL;
}

/* Compare against the previous build, render the "changes" section, emit the
 * JSON changes object, update the digest, and rewrite the baseline. Runs on
 * the SORTED remark list before the main listing renders. */
static void ir_explain_render_changes(void) {
  size_t current_count = 0, old_count = 0;
  IRExplainBaseKey *current = ir_explain_build_keys(&current_count);
  IRExplainBaseKey *old = ir_explain_read_baseline(&old_count);

  if (current) {
    if (old) {
      size_t improved = 0, regressed = 0;
      ir_explain_json_raw("\"changes\":{\"baseline\":true,\"entries\":[");
      size_t json_entries = 0;
      for (size_t i = 0; i < current_count; i++) {
        const IRExplainBaseKey *was =
            ir_explain_find_key(old, old_count, &current[i]);
        if (!was || was->status == current[i].status) {
          continue;
        }
        const char *what = current[i].kind == 'L' ? "loop" : "call";
        int now_better = current[i].status == 'V' || current[i].status == 'I';
        if ((improved + regressed) == 0) {
          ir_explain_emit("  %schanges since the last explain build:%s\n",
                          clr(EXPLAIN_BOLD), clr(EXPLAIN_RESET));
        }
        if (now_better) {
          improved++;
          ir_explain_emit(
              "    %s+ %s (%s @ line %zu): now %s%s\n", clr(EXPLAIN_GREEN),
              current[i].function_name, what, current[i].line,
              current[i].kind == 'L' ? "vectorized" : "inlined",
              clr(EXPLAIN_RESET));
        } else {
          regressed++;
          ir_explain_emit(
              "    %s%s- %s (%s @ line %zu): REGRESSED %s was %s%s\n",
              clr(EXPLAIN_BOLD), clr(EXPLAIN_RED), current[i].function_name,
              what, current[i].line, glyph_arrow(),
              current[i].kind == 'L' ? "vectorized, now scalar"
                                     : "inlined, now a real call",
              clr(EXPLAIN_RESET));
          if (current[i].reason) {
            ir_explain_emit("        %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                            glyph_elbow(), current[i].reason,
                            clr(EXPLAIN_RESET));
          }
        }
        ir_explain_json_raw("%s{\"kind\":\"%s\",\"fn\":",
                            json_entries++ ? "," : "", what);
        ir_explain_json_str(current[i].function_name);
        ir_explain_json_raw(",\"line\":%zu,\"direction\":\"%s\",\"reason\":",
                            current[i].line,
                            now_better ? "improved" : "regressed");
        ir_explain_json_str(now_better ? NULL : current[i].reason);
        ir_explain_json_raw("}");
      }
      if ((improved + regressed) == 0) {
        ir_explain_emit("  %sno optimization changes since the last explain "
                        "build%s\n",
                        clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
      }
      ir_explain_emit("\n");
      ir_explain_json_raw("]},");
      g_digest.changes_improved = improved;
      g_digest.changes_regressed = regressed;
      g_digest.had_baseline = 1;
    } else {
      ir_explain_json_raw("\"changes\":{\"baseline\":false,\"entries\":[]},");
    }
    ir_explain_write_baseline(current, current_count);
  }
  free(current);
  free(old);
}

static void ir_explain_print_header(const char *what) {
  const char *file = g_explain_focus_file
                         ? ir_explain_path_basename(g_explain_focus_file)
                         : "<input>";
  char label[512];
  snprintf(label, sizeof(label), "%s%s: %s%s", clr(EXPLAIN_BOLD), what, file,
           clr(EXPLAIN_RESET));
  char line[2048];
  diag_rule_into(line, sizeof(line), 0, label, "");
  ir_explain_emit("%c%s%c", 10, line, 10);
}

/* ---- "where to start" -------------------------------------------------------
 * The remark list is in source order, which is the right order to read a file
 * in and the wrong order to decide what to do. A report of forty findings
 * answers "what happened"; this block answers "what do I change", which is the
 * question the reader actually arrived with.
 *
 * Ranked by what the compiler can stand behind: a fix it applied to a clone
 * and re-checked outranks one it merely believes, and a fix inside a nested
 * loop outranks the same fix at top level. */

#define IR_EXPLAIN_START_MAX 5

/* Copy `text` into `out`, cut at `width` with an ellipsis. The full sentence is
 * a few lines below in the report, so this is a signpost rather than a summary.
 *
 * A fix line is usually an imperative clause followed by a parenthesised
 * example, and cutting inside the example leaves a fragment that reads as
 * damage ("... (e.g. `var row: float32* = ..."). So: drop the whole example
 * when the clause before it still carries the instruction, and otherwise fall
 * back to a word boundary. */
static void ir_explain_fit(const char *text, size_t width, char *out,
                           size_t cap) {
  if (!text) {
    out[0] = '\0';
    return;
  }
  size_t len = strlen(text);
  if (len <= width || width + 4 >= cap) {
    snprintf(out, cap, "%s", text);
    return;
  }

  /* An open parenthesis at the cut means the cut lands inside an example. */
  size_t depth = 0, opened_at = 0;
  int inside = 0;
  for (size_t i = 0; i < width; i++) {
    if (text[i] == '(') {
      if (depth++ == 0) {
        opened_at = i;
        inside = 1;
      }
    } else if (text[i] == ')' && depth > 0) {
      if (--depth == 0) {
        inside = 0;
      }
    }
  }

  size_t cut = width;
  if (inside && opened_at > width / 2) {
    cut = opened_at; /* keep the clause, drop the example */
    while (cut > 0 && text[cut - 1] == ' ') {
      cut--;
    }
  } else {
    while (cut > width / 2 && text[cut] != ' ') {
      cut--;
    }
    if (text[cut] != ' ') {
      cut = width;
    }
  }
  memcpy(out, text, cut);
  snprintf(out + cut, cap - cut, " ...");
}

/* Did the compiler identify what blocked this loop, or fall back to "no
 * recognizer claimed it"? The fallback's advice is the same checklist
 * everywhere, so it sorts below any diagnosis that names a cause. */
static int ir_explain_names_a_cause(const IRExplainRemark *r) {
  return !(r->code && strcmp(r->code, "unrecognized-shape") == 0);
}

/* Rank the findings that have a fix into `order`, heaviest first, and return
 * how many entries it holds. `missed_out` counts every missed optimization and
 * `actionable_out` the subset with a fix. With `apply_filter` the ranking is
 * over the selected slice only; without it, over the whole file. */
static size_t ir_explain_rank_fixes(int apply_filter, size_t *order,
                                    size_t *sites, size_t *missed_out,
                                    size_t *actionable_out) {
  size_t shown = 0, actionable = 0, missed = 0;
  for (size_t i = 0; i < IR_EXPLAIN_START_MAX; i++) {
    sites[i] = 1;
  }

  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    if (r->positive || (apply_filter && !ir_explain_remark_selected(r))) {
      continue;
    }
    /* The outer loop of a nest is never a missed optimization: only
     * innermost loops vectorize, so its remark is a signpost to the inner
     * loop's problem, not a second problem. Counting it turns one finding
     * into "1 of 2" and reads as though something else went wrong. */
    if (r->code && strcmp(r->code, "outer-of-nest") == 0) {
      continue;
    }
    missed++;
    if (!r->fix || r->advisory) {
      continue;
    }
    actionable++;

    /* One line per kind of work. Four sites needing the same change are one
     * decision to make and four edits to do; showing them as four entries
     * spends the whole list on one idea. The site named is the first the
     * ranking reached, and the count says how far the work spreads.
     *
     * Folding on the advice, not the code. One code can cover several
     * distinct causes with distinct fixes -- store-only-fill alone has three
     * -- and folding those together hides real work behind unrelated work,
     * including hiding a proven fix behind an unproven one. */
    if (r->code && r->fix) {
      int folded = 0;
      for (size_t j = 0; j < shown; j++) {
        const IRExplainRemark *p = &g_remarks[order[j]];
        if (p->code && p->fix && strcmp(p->code, r->code) == 0 &&
            strcmp(p->fix, r->fix) == 0) {
          sites[j]++;
          folded = 1;
          break;
        }
      }
      if (folded) {
        continue;
      }
    }

    /* Insertion sort into the top-N: proven first, then advice specific to
     * this loop, then deepest, then earliest, so the same file always
     * produces the same order.
     *
     * The specificity key matters on a real program. `unrecognized-shape` is
     * the fallback the compiler reaches when it cannot name a cause, and its
     * advice is the same checklist at every site; without this key a file
     * with many such loops fills the list with one repeated sentence and
     * buries the diagnoses that name a cause. */
    size_t at = shown;
    while (at > 0) {
      const IRExplainRemark *p = &g_remarks[order[at - 1]];
      int r_proven = r->verified ? 1 : 0, p_proven = p->verified ? 1 : 0;
      int r_named = ir_explain_names_a_cause(r), p_named =
                                                    ir_explain_names_a_cause(p);
      if (r_proven != p_proven) {
        if (r_proven < p_proven) {
          break;
        }
      } else if (r_named != p_named) {
        if (r_named < p_named) {
          break;
        }
      } else if (r->depth != p->depth) {
        if (r->depth < p->depth) {
          break;
        }
      } else {
        break;
      }
      at--;
    }
    if (at >= IR_EXPLAIN_START_MAX) {
      continue;
    }
    for (size_t j = shown < IR_EXPLAIN_START_MAX ? shown : IR_EXPLAIN_START_MAX - 1;
         j > at; j--) {
      order[j] = order[j - 1];
      sites[j] = sites[j - 1];
    }
    order[at] = i;
    sites[at] = 1;
    if (shown < IR_EXPLAIN_START_MAX) {
      shown++;
    }
  }
  *missed_out = missed;
  *actionable_out = actionable;
  return shown;
}

/* A whole-function fallback, ready to print as a line of the plan. Built after
 * codegen (that is when the eligibility gate has run), which is why the plan is
 * rendered late and spliced back into the report at the point it belongs. */
typedef struct {
  char location[160];      /* "main (252 instrs)" */
  char function_name[128]; /* the same function, for the JSON sidecar */
  /* The gate's reason, and the loop it is about. Sized to hold both whole: a
     reason is up to 256 and the loop list up to 96. */
  char why[384];
  const char *fix;
  size_t instructions_sort;
} IRExplainBackendPlan;
/* Two at most. A third repeats the same lesson and pushes out the loop work. */
#define IR_EXPLAIN_BACKEND_PLAN_MAX 2
static size_t ir_explain_collect_backend_plan(IRExplainBackendPlan *out);

static void ir_explain_render_start_here(void) {
  size_t order[IR_EXPLAIN_START_MAX];
  size_t actionable = 0, missed = 0;

  /* Whole-function fallbacks lead the plan. A loop remark is a prediction about
   * one loop; a fallback is a measurement over a whole function that already
   * happened, and it costs every value in that function a register. Ranking it
   * under a per-loop heuristic would put the smaller number first. */
  IRExplainBackendPlan backend_plan[IR_EXPLAIN_BACKEND_PLAN_MAX];
  size_t backend_shown = ir_explain_collect_backend_plan(backend_plan);

  /* The same ranking, for tools. An editor showing a "what to fix" panel
   * should not have to re-derive the order from the remark list and guess at
   * the tie-breaks. Ranked over the whole file, since --explain=SELECTOR
   * narrows the prose and leaves the sidecar alone. Always emitted, empty when
   * there is nothing to do, so the document's shape does not vary. */
  if (g_explain_json) {
    size_t all_missed = 0, all_actionable = 0;
    size_t all_sites[IR_EXPLAIN_START_MAX];
    size_t whole =
        ir_explain_rank_fixes(0, order, all_sites, &all_missed, &all_actionable);
    ir_explain_json_raw("\"startHere\":[");
    for (size_t i = 0; i < backend_shown; i++) {
      ir_explain_json_raw("%s{\"kind\":\"backend\",\"fn\":", i ? "," : "");
      ir_explain_json_str(backend_plan[i].function_name);
      ir_explain_json_raw(",\"instructions\":%zu,\"why\":",
                          backend_plan[i].instructions_sort);
      ir_explain_json_str(backend_plan[i].why);
      ir_explain_json_raw(",\"fix\":");
      ir_explain_json_str(backend_plan[i].fix);
      ir_explain_json_raw(",\"proven\":false}");
    }
    for (size_t i = 0; i < whole; i++) {
      const IRExplainRemark *r = &g_remarks[order[i]];
      ir_explain_json_raw("%s{\"kind\":\"remark\",\"fn\":",
                          (i || backend_shown) ? "," : "");
      ir_explain_json_str(r->function_name);
      ir_explain_json_raw(",\"line\":%zu,\"code\":", r->line);
      ir_explain_json_str(r->code);
      ir_explain_json_raw(",\"fix\":");
      ir_explain_json_str(r->fix);
      ir_explain_json_raw(",\"proven\":%s,\"stillBlocked\":%s,\"depth\":%zu,"
                          "\"sites\":%zu}",
                          r->verified ? "true" : "false",
                          r->partial ? "true" : "false", r->depth,
                          all_sites[i]);
    }
    ir_explain_json_raw("],");
  }

  size_t sites[IR_EXPLAIN_START_MAX];
  size_t shown = ir_explain_rank_fixes(1, order, sites, &missed, &actionable);

  if (shown == 0 && backend_shown == 0) {
    /* Silence is ambiguous: it could mean a clean file or a report that
     * forgot to say. One line, and only when there was something to miss. */
    if (missed > 0) {
      ir_explain_emit("  %s%zu missed optimization%s in this file, none with a "
                      "fix the compiler can name%s\n\n",
                      clr(EXPLAIN_DIM), missed, missed == 1 ? "" : "s",
                      clr(EXPLAIN_RESET));
    }
    return;
  }

  /* One pass to size the location column, so the fixes line up and the block
   * scans as a list rather than as ragged prose. */
  size_t location_width = 0;
  char location[IR_EXPLAIN_START_MAX][160];
  for (size_t i = 0; i < shown; i++) {
    const IRExplainRemark *r = &g_remarks[order[i]];
    snprintf(location[i], sizeof(location[0]), "%s:%zu", r->function_name,
             r->line);
    size_t len = strlen(location[i]);
    if (len > location_width) {
      location_width = len;
    }
  }
  for (size_t i = 0; i < backend_shown; i++) {
    size_t len = strlen(backend_plan[i].location);
    if (len > location_width) {
      location_width = len;
    }
  }

  ir_explain_emit("  %swhere to start%s (%zu of %zu missed optimization%s "
                  "ha%s a fix)\n",
                  clr(EXPLAIN_BOLD), clr(EXPLAIN_RESET), actionable, missed,
                  missed == 1 ? "" : "s", actionable == 1 ? "s" : "ve");
  if (backend_shown > 0) {
    snprintf(g_digest.start_here, sizeof(g_digest.start_here), "%s  %s",
             backend_plan[0].location, backend_plan[0].fix);
    g_digest.start_here_proven = 0;
  } else {
    const IRExplainRemark *lead = &g_remarks[order[0]];
    snprintf(g_digest.start_here, sizeof(g_digest.start_here), "%s:%zu  %s",
             lead->function_name, lead->line, lead->fix);
    g_digest.start_here_proven = lead->verified ? 1 : 0;
  }

  size_t rank = 0;
  int saw_proven = 0;
  int saw_partial = 0;
  for (size_t i = 0; i < backend_shown; i++) {
    char fix[200];
    ir_explain_fit(backend_plan[i].fix, 84, fix, sizeof(fix));
    ir_explain_emit("    %zu. %s%-6s%s %-*s  %s\n", ++rank, clr(EXPLAIN_RED),
                    "spills", clr(EXPLAIN_RESET), (int)location_width,
                    backend_plan[i].location, fix);
    ir_explain_emit("       %s%-6s %-*s  %s%s\n", clr(EXPLAIN_DIM), "",
                    (int)location_width, "", backend_plan[i].why,
                    clr(EXPLAIN_RESET));
  }
  for (size_t i = 0; i < shown; i++) {
    const IRExplainRemark *r = &g_remarks[order[i]];
    char fix[200];
    char spread[48] = "";
    if (sites[i] > 1) {
      snprintf(spread, sizeof(spread), " %s(+%zu more site%s)%s",
               clr(EXPLAIN_DIM), sites[i] - 1, sites[i] == 2 ? "" : "s",
               clr(EXPLAIN_RESET));
    }
    ir_explain_fit(r->fix, sites[i] > 1 ? 66 : 84, fix, sizeof(fix));
    /* The caveat has to survive the truncation that trims the fix text, or the
     * plan reads as "do this and you are done" for a fix we know is partial. */
    const char *status = r->verified ? "proven" : (r->partial ? "step 1" : "");
    saw_proven |= r->verified ? 1 : 0;
    saw_partial |= (!r->verified && r->partial) ? 1 : 0;
    ir_explain_emit("    %zu. %s%-6s%s %-*s  %s%s\n", ++rank,
                    clr(r->verified ? EXPLAIN_GREEN : EXPLAIN_DIM), status,
                    clr(EXPLAIN_RESET), (int)location_width, location[i], fix,
                    spread);
  }
  /* The lines above stand for `covered` findings, not `shown` of them, since
   * each folds its own sites. The remainder is what no line represents. */
  size_t covered = 0;
  for (size_t i = 0; i < shown; i++) {
    covered += sites[i];
  }
  if (actionable > covered) {
    ir_explain_emit("       %s... and %zu more below%s\n", clr(EXPLAIN_DIM),
                    actionable - covered, clr(EXPLAIN_RESET));
  }
  /* The badges are only worth explaining on a report that uses them. */
  if (saw_proven || saw_partial) {
    ir_explain_emit("       %s%s%s%s\n", clr(EXPLAIN_DIM),
                    saw_proven ? "proven = applied to a clone and re-checked"
                               : "",
                    saw_partial ? (saw_proven ? ";  step 1 = applied, the loop "
                                               "still needs more"
                                             : "step 1 = applied, the loop still "
                                               "needs more")
                                : "",
                    clr(EXPLAIN_RESET));
  }
  ir_explain_emit("\n");
}

/* One remark as a JSON object in the "remarks" array. `kind` is explicit so
 * consumers never re-derive it from prose. */
static void ir_explain_json_remark(const IRExplainRemark *r, const char *kind,
                                   const char *callee, size_t count,
                                   size_t line_end, const char *calls,
                                   size_t *json_count) {
  if (!g_explain_json) {
    return;
  }
  ir_explain_json_raw("%s{\"kind\":\"%s\",\"fn\":", (*json_count)++ ? "," : "",
                      kind);
  ir_explain_json_str(r->function_name);
  ir_explain_json_raw(",\"entity\":");
  ir_explain_json_str(r->entity);
  ir_explain_json_raw(",\"line\":%zu,\"positive\":%s,\"headline\":", r->line,
                      r->positive ? "true" : "false");
  ir_explain_json_str(r->headline);
  ir_explain_json_raw(",\"reason\":");
  ir_explain_json_str(r->reason);
  ir_explain_json_raw(",\"fix\":");
  ir_explain_json_str(r->fix);
  ir_explain_json_raw(",\"verified\":");
  ir_explain_json_str(r->verified);
  ir_explain_json_raw(",\"stillBlocked\":");
  ir_explain_json_str(r->partial);
  ir_explain_json_raw(",\"callee\":");
  ir_explain_json_str(callee);
  if (count > 1) {
    ir_explain_json_raw(",\"count\":%zu,\"lineEnd\":%zu,\"calls\":", count,
                        line_end);
    ir_explain_json_str(calls);
  }
  if (r->depth > 0) {
    ir_explain_json_raw(",\"depth\":%zu", r->depth);
  }
  /* Schema 2: the machine-readable half. `code` is the stable decision id,
   * `column`/`endLine` the construct's extent, `trivial` marks housekeeping a
   * reader can collapse, and `quantities` carries whatever the pass measured. */
  ir_explain_json_raw(",\"code\":");
  ir_explain_json_str(r->code);
  ir_explain_json_raw(",\"column\":%zu", r->column);
  if (r->end_line > 0) {
    ir_explain_json_raw(",\"endLine\":%zu", r->end_line);
  }
  if (r->trivial) {
    ir_explain_json_raw(",\"trivial\":true");
  }
  if (r->advisory) {
    ir_explain_json_raw(",\"advisory\":true");
  }
  if (r->quantity_count > 0) {
    ir_explain_json_raw(",\"quantities\":{");
    for (size_t q = 0; q < r->quantity_count; q++) {
      ir_explain_json_raw("%s", q ? "," : "");
      ir_explain_json_str(r->quantities[q].name);
      ir_explain_json_raw(":%ld", r->quantities[q].value);
    }
    ir_explain_json_raw("}");
  }
  ir_explain_json_raw("}");
}

/* Render what --safe did: how many accesses were checked, how many the
 * compiler proved could not fail, and where the rest are.
 *
 * The proportion is the whole point of the mode, so it leads. A survivor is
 * usually actionable: a constant-extent comparison is a couple of
 * instructions, while a runtime call means the compiler could not see how
 * large the object was, which is often a loop bound it could have been told. */
static void ir_explain_typed_flush(void) {
  if (g_explain_json) {
    ir_explain_json_raw("\"proven_by_type\":[");
    for (size_t i = 0; i < g_typed_count; i++) {
      const IRExplainTypedNote *n = &g_typed[i];
      ir_explain_json_raw("%s{\"line\":%zu,\"type\":", i ? "," : "",
                          n->line);
      ir_explain_json_str(n->type_name);
      ir_explain_json_raw(",\"min\":%lld,\"max\":%lld,\"length\":%zu}",
                          n->min, n->max, n->length);
    }
    ir_explain_json_raw("],");
  }
  if (!g_explain || g_typed_total == 0) {
    return;
  }
  ir_explain_print_header("proven by type");
  ir_explain_emit("  %zu bounds check%s not emitted: the index's declared "
                  "type already proves it in range\n",
                  g_typed_total, g_typed_total == 1 ? "" : "s");
  for (size_t i = 0; i < g_typed_count; i++) {
    const IRExplainTypedNote *n = &g_typed[i];
    ir_explain_emit("  %sline %zu%s%s%s: '%s' holds %lld..%lld, inside an "
                    "array of %zu  %s[P0002]%s\n",
                    clr(EXPLAIN_BOLD), n->line, clr(EXPLAIN_RESET),
                    n->function_name ? " in " : "",
                    n->function_name ? n->function_name : "",
                    n->type_name ? n->type_name : "?", n->min, n->max,
                    n->length, clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
    ir_explain_echo_source(n->line);
  }
  if (g_typed_total > g_typed_count) {
    ir_explain_emit("  %s(%zu more not listed)%s\n", clr(EXPLAIN_DIM),
                    g_typed_total - g_typed_count, clr(EXPLAIN_RESET));
  }
  ir_explain_emit("\n");
}

static void ir_explain_safety_flush(void) {
  if (g_explain_json) {
    ir_explain_json_raw("\"safety\":{\"enabled\":%s",
                        g_safety_have_totals ? "true" : "false");
    if (g_safety_have_totals) {
      ir_explain_json_raw(",\"accesses\":%zu,\"proved\":%zu,\"hoisted\":%zu"
                          ",\"spanned\":%zu,\"exempt\":%zu"
                          ",\"extentTests\":%zu,\"regionCalls\":%zu",
                          g_safety_emitted, g_safety_proved, g_safety_hoisted,
                          g_safety_spanned, g_safety_exempt,
                          g_safety_extent_tests, g_safety_region_calls);
    }
    ir_explain_json_raw(",\"survivors\":[");
    for (size_t i = 0; i < g_safety_count; i++) {
      const IRExplainSafetyNote *n = &g_safety[i];
      ir_explain_json_raw("%s{\"line\":%zu,\"kind\":", i ? "," : "", n->line);
      ir_explain_json_str(n->kind == IR_SAFETY_SURVIVOR_REGION  ? "runtime"
                          : n->kind == IR_SAFETY_SURVIVOR_SPAN ? "span"
                                                               : "extent");
      ir_explain_json_raw(",\"function\":");
      ir_explain_json_str(n->function_name);
      ir_explain_json_raw("}");
    }
    ir_explain_json_raw("]},");
  }

  ir_explain_typed_flush();
  if (!g_explain || !g_safety_have_totals) {
    return;
  }
  ir_explain_print_header("memory safety");
  if (g_safety_emitted == 0) {
    ir_explain_emit("  %sno memory accesses to check in this file%s\n\n",
                    clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
    return;
  }

  size_t survivors =
      g_safety_extent_tests + g_safety_region_calls + g_safety_spanned;
  size_t settled = g_safety_proved + g_safety_hoisted;
  ir_explain_emit("  %zu access%s, %zu settled at compile time (%zu%%), "
                  "%zu checked at run time\n",
                  g_safety_emitted, g_safety_emitted == 1 ? "" : "es", settled,
                  (settled * 100) / g_safety_emitted, survivors);
  ir_explain_emit("  %s%zu proved in place, %zu folded into a check covering "
                  "a whole loop%s\n",
                  clr(EXPLAIN_DIM), g_safety_proved, g_safety_hoisted,
                  clr(EXPLAIN_RESET));
  if (g_safety_exempt > 0) {
    ir_explain_emit("  %s%zu inside the allocator, which is not checked%s\n",
                    clr(EXPLAIN_DIM), g_safety_exempt, clr(EXPLAIN_RESET));
  }
  if (survivors > 0) {
    ir_explain_emit("  %zu compare against a known extent, %zu against an "
                    "allocation the loop resolves once, %zu ask the runtime "
                    "which allocation the pointer came from\n",
                    g_safety_extent_tests, g_safety_spanned,
                    g_safety_region_calls);
  }

  for (size_t i = 0; i < g_safety_count; i++) {
    const IRExplainSafetyNote *n = &g_safety[i];
    const char *why =
        n->kind == IR_SAFETY_SURVIVOR_REGION
            ? "the object's size is not known here, so the runtime is asked "
              "which allocation the pointer came from"
        : n->kind == IR_SAFETY_SURVIVOR_SPAN
            ? "nothing bounds the index, but the pointer holds still, so the "
              "loop resolves its allocation once and this compares against it"
            : "the index is not a constant and no loop bounds it, so it is "
              "compared against the object's extent";
    ir_explain_emit("  %sline %zu%s%s%s: %s\n", clr(EXPLAIN_BOLD), n->line,
                    clr(EXPLAIN_RESET), n->function_name ? " in " : "",
                    n->function_name ? n->function_name : "", why);
    ir_explain_echo_source(n->line);
  }
  if (survivors > g_safety_count) {
    ir_explain_emit("  %s(%zu more not listed)%s\n", clr(EXPLAIN_DIM),
                    survivors - g_safety_count, clr(EXPLAIN_RESET));
  }
  ir_explain_emit("\n");
}

/* Render the memory diagnostics the type checker handed us: a JSON "memory"
 * array (always emitted so the document's comma chain stays valid) and a prose
 * "memory report" section. Called at the tail of ir_explain_flush, so it lands
 * after "remarks" and before "backend" in the JSON buffer. */
static void ir_explain_memory_flush(void) {
  if (g_explain_json) {
    ir_explain_json_raw("\"memory\":[");
    for (size_t i = 0; i < g_mem_count; i++) {
      const IRExplainMemNote *n = &g_mem[i];
      ir_explain_json_raw("%s{\"severity\":", i ? "," : "");
      ir_explain_json_str(n->severity ? "error" : "warning");
      ir_explain_json_raw(",\"line\":%zu,\"code\":", n->line);
      ir_explain_json_str(n->code);
      ir_explain_json_raw(",\"headline\":");
      ir_explain_json_str(n->headline);
      ir_explain_json_raw(",\"fix\":");
      ir_explain_json_str(n->fix);
      ir_explain_json_raw("}");
    }
    ir_explain_json_raw("],");
  }

  if (!g_explain) {
    return;
  }
  /* A section whose whole content is "nothing to report" costs four lines to
   * say what its absence already says. */
  if (g_mem_count == 0) {
    return;
  }
  ir_explain_print_header("memory report");
  size_t errors = 0, warnings = 0;
  for (size_t i = 0; i < g_mem_count; i++) {
    if (g_mem[i].severity) {
      errors++;
    } else {
      warnings++;
    }
  }
  ir_explain_emit("  %zu issue%s (%zu error%s, %zu warning%s):\n", g_mem_count,
                  g_mem_count == 1 ? "" : "s", errors, errors == 1 ? "" : "s",
                  warnings, warnings == 1 ? "" : "s");
  for (size_t i = 0; i < g_mem_count; i++) {
    const IRExplainMemNote *n = &g_mem[i];
    char code_tag[32] = "";
    if (n->code) {
      snprintf(code_tag, sizeof(code_tag), "  %s[%s]%s", clr(EXPLAIN_DIM),
               n->code, clr(EXPLAIN_RESET));
    }
    ir_explain_emit("  %s%s%s (line %zu): %s%s\n",
                    clr(n->severity ? EXPLAIN_RED : EXPLAIN_BOLD),
                    n->severity ? "error" : "warning", clr(EXPLAIN_RESET),
                    n->line, n->headline, code_tag);
    ir_explain_echo_source(n->line);
    if (n->fix) {
      ir_explain_emit("      %s%s fix: %s%s\n", clr(EXPLAIN_DIM), glyph_elbow(),
                      n->fix, clr(EXPLAIN_RESET));
    }
  }
  ir_explain_emit("\n");
}

/* Fold the remarks into their functions. Called while the remarks are still
 * alive; the section itself is written after codegen. */
static void ir_explain_tally_functions(void) {
  for (size_t r = 0; r < g_remark_count; r++) {
    const IRExplainRemark *remark = &g_remarks[r];
    IRExplainFunctionEntry *f = NULL;
    for (size_t i = 0; i < g_function_count; i++) {
      if (strcmp(g_functions[i].name, remark->function_name) == 0) {
        f = &g_functions[i];
        break;
      }
    }
    if (!f || !remark->entity) {
      continue;
    }
    if (strcmp(remark->entity, "loop") == 0) {
      f->loops++;
      if (remark->positive) {
        f->loops_vectorized++;
      }
    } else if (strncmp(remark->entity, "call", 4) == 0) {
      if (remark->positive) {
        f->calls_inlined++;
      } else {
        f->calls_refused++;
      }
    }
  }
}

/* The per-function table: the weight the pipeline started and finished with,
 * the decisions recorded against it, and how it fared in the backend. */
static void ir_explain_functions_json(void) {
  if (!g_explain_json) {
    return;
  }
  ir_explain_json_raw("\"functions\":[");
  for (size_t i = 0; i < g_function_count; i++) {
    const IRExplainFunctionEntry *f = &g_functions[i];
    int backend_ok = -1;
    const char *backend_reason = NULL;
    size_t backend_instructions = 0;
    for (size_t b = 0; b < g_backend_count; b++) {
      if (g_backend[b].function_name &&
          strcmp(g_backend[b].function_name, f->name) == 0) {
        backend_ok = g_backend[b].ok;
        backend_reason = g_backend[b].detail;
        backend_instructions = g_backend[b].instructions;
        break;
      }
    }

    ir_explain_json_raw("%s{\"fn\":", i ? "," : "");
    ir_explain_json_str(f->name);
    ir_explain_json_raw(",\"line\":%zu,\"instructionsBefore\":%zu,"
                        "\"instructionsAfter\":%zu,\"loops\":%zu,"
                        "\"loopsVectorized\":%zu,\"callsInlined\":%zu,"
                        "\"callsRefused\":%zu",
                        f->line, f->instructions_before, f->instructions_after,
                        f->loops, f->loops_vectorized, f->calls_inlined,
                        f->calls_refused);
    if (backend_ok >= 0) {
      ir_explain_json_raw(",\"backendOk\":%s,\"backendInstructions\":%zu",
                          backend_ok ? "true" : "false", backend_instructions);
      ir_explain_json_raw(",\"backendReason\":");
      ir_explain_json_str(backend_ok ? NULL : backend_reason);
    }
    for (size_t c = 0; c < g_function_cost_count; c++) {
      if (strcmp(g_function_costs[c].function_name, f->name) != 0) {
        continue;
      }
      const IRExplainFunctionCost *cost = &g_function_costs[c];
      ir_explain_json_raw(",\"spills\":%d,\"regsUsed\":%d,\"throughput\":%d,"
                          "\"hotCost\":%ld,\"vectorOps\":%d,"
                          "\"estimatedSpans\":%d",
                          cost->spills, cost->regs_used, cost->total_rthru,
                          cost->hot_cost, cost->vec_ops, cost->estimated_spans);
      break;
    }
    ir_explain_json_raw("}");
  }
  ir_explain_json_raw("],");
}

/* A static hotness proxy: a loop body runs some multiple of its enclosing
 * loop's iterations, and nothing here has measured frequencies unless --pgo
 * ran. Ten per level, capped, is the same convention the codegen hot-cost
 * weighting uses -- enough to sort by, never mistaken for a measurement. */
static long ir_explain_depth_weight(int depth) {
  long weight = 1;
  for (int i = 0; i < depth && i < 3; i++) {
    weight *= 10;
  }
  return weight;
}

/* The nest depth of the loop containing `line` in `function`, or 0 when the
 * line is not inside one. Uses the loop extents the vectorizer stamped. */
static int ir_explain_enclosing_depth(const char *function, size_t line) {
  int deepest = 0;
  for (size_t i = 0; i < g_loop_cost_count; i++) {
    const IRExplainLoopCost *cost = &g_loop_costs[i];
    if (strcmp(cost->function_name, function) != 0) {
      continue;
    }
    if (line >= cost->head_line && line <= cost->tail_line &&
        cost->depth + 1 > deepest) {
      deepest = cost->depth + 1;
    }
  }
  return deepest;
}

/* Every decision with a number on it, heaviest first.
 *
 * A long report is unreadable without an order, and line order is the wrong
 * one: it puts a one-line inline in a cold path above the scalar loop that
 * costs the program its afternoon. Loops are weighted by modelled cycles times
 * nest depth; refused calls by the callee's weight times the depth of the loop
 * they sit in. */
static void ir_explain_hotspots_json(void) {
  if (!g_explain_json) {
    return;
  }
  ir_explain_json_raw("\"hotspots\":[");

  typedef struct {
    const char *function;
    size_t line;
    const char *kind;
    const char *code;
    long cost;
  } Hotspot;

  Hotspot *spots = calloc(g_loop_cost_count + g_remark_count, sizeof(Hotspot));
  if (!spots) {
    ir_explain_json_raw("],");
    return;
  }
  size_t count = 0;

  for (size_t i = 0; i < g_loop_cost_count; i++) {
    const IRExplainLoopCost *cost = &g_loop_costs[i];
    spots[count].function = cost->function_name;
    spots[count].line = cost->head_line;
    spots[count].kind = "loop";
    spots[count].code = cost->has_kernel ? "vectorized" : NULL;
    spots[count].cost =
        (long)cost->cycles_per_iter * ir_explain_depth_weight(cost->depth);
    count++;
  }
  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    if (r->positive || !r->entity || strncmp(r->entity, "call", 4) != 0) {
      continue;
    }
    long weight = 0;
    for (size_t q = 0; q < r->quantity_count; q++) {
      if (strcmp(r->quantities[q].name, "calleeInstructions") == 0) {
        weight = r->quantities[q].value;
      }
    }
    if (weight <= 0) {
      continue;
    }
    spots[count].function = r->function_name;
    spots[count].line = r->line;
    spots[count].kind = "call";
    spots[count].code = r->code;
    spots[count].cost =
        weight * ir_explain_depth_weight(
                     ir_explain_enclosing_depth(r->function_name, r->line));
    count++;
  }

  /* Selection sort: the list is short and this keeps the output stable. */
  for (size_t i = 0; i < count; i++) {
    size_t best = i;
    for (size_t j = i + 1; j < count; j++) {
      if (spots[j].cost > spots[best].cost) {
        best = j;
      }
    }
    Hotspot swap = spots[i];
    spots[i] = spots[best];
    spots[best] = swap;

    ir_explain_json_raw("%s{\"fn\":", i ? "," : "");
    ir_explain_json_str(spots[i].function);
    ir_explain_json_raw(",\"line\":%zu,\"kind\":", spots[i].line);
    ir_explain_json_str(spots[i].kind);
    ir_explain_json_raw(",\"code\":");
    ir_explain_json_str(spots[i].code);
    ir_explain_json_raw(",\"cost\":%ld}", spots[i].cost);
  }
  free(spots);
  ir_explain_json_raw("],");
}

/* Who called whom, and what became of it: one edge per caller/callee pair with
 * the sites inlined, the sites refused, and the callee's weight. Answers "where
 * did this function actually go" without re-deriving it from the remark list. */
static void ir_explain_call_graph_json(void) {
  if (!g_explain_json) {
    return;
  }
  ir_explain_json_raw("\"callGraph\":[");

  typedef struct {
    const char *caller;
    char callee[96];
    size_t inlined;
    size_t refused;
    long callee_instructions;
  } Edge;

  Edge *edges = calloc(g_remark_count ? g_remark_count : 1, sizeof(Edge));
  if (!edges) {
    ir_explain_json_raw("],");
    return;
  }
  size_t edge_count = 0;

  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    if (!r->entity || strncmp(r->entity, "call", 4) != 0) {
      continue;
    }
    char callee[96];
    ir_explain_entity_callee(r->entity, callee, sizeof(callee));
    if (!callee[0]) {
      continue;
    }
    Edge *edge = NULL;
    for (size_t e = 0; e < edge_count; e++) {
      if (strcmp(edges[e].caller, r->function_name) == 0 &&
          strcmp(edges[e].callee, callee) == 0) {
        edge = &edges[e];
        break;
      }
    }
    if (!edge) {
      edge = &edges[edge_count++];
      edge->caller = r->function_name;
      snprintf(edge->callee, sizeof(edge->callee), "%s", callee);
      edge->inlined = 0;
      edge->refused = 0;
      edge->callee_instructions = 0;
    }
    if (r->positive) {
      edge->inlined++;
    } else {
      edge->refused++;
    }
    for (size_t q = 0; q < r->quantity_count; q++) {
      if (strcmp(r->quantities[q].name, "calleeInstructions") == 0) {
        edge->callee_instructions = r->quantities[q].value;
      }
    }
  }

  for (size_t e = 0; e < edge_count; e++) {
    ir_explain_json_raw("%s{\"caller\":", e ? "," : "");
    ir_explain_json_str(edges[e].caller);
    ir_explain_json_raw(",\"callee\":");
    ir_explain_json_str(edges[e].callee);
    ir_explain_json_raw(",\"inlined\":%zu,\"refused\":%zu,"
                        "\"calleeInstructions\":%ld}",
                        edges[e].inlined, edges[e].refused,
                        edges[e].callee_instructions);
  }
  free(edges);
  ir_explain_json_raw("],");
}

/* Every loop the backend measured, whether or not the optimizer had anything
 * to say about it. Keyed by function and head line so a consumer can join it
 * onto the remarks. Cycles are centicycles: 720 is 7.2 cycles an iteration. */
static void ir_explain_loop_costs_json(void) {
  if (!g_explain_json) {
    return;
  }
  ir_explain_json_raw("\"loops\":[");
  for (size_t i = 0; i < g_loop_cost_count; i++) {
    const IRExplainLoopCost *cost = &g_loop_costs[i];
    ir_explain_json_raw("%s{\"fn\":", i ? "," : "");
    ir_explain_json_str(cost->function_name);
    ir_explain_json_raw(",\"line\":%zu,\"endLine\":%zu,\"depth\":%d,"
                        "\"cyclesPerIter\":%d,\"bottleneck\":",
                        cost->head_line, cost->tail_line, cost->depth,
                        cost->cycles_per_iter);
    ir_explain_json_str(cost->bottleneck);
    ir_explain_json_raw(",\"hasKernel\":%s,\"estimated\":%s}",
                        cost->has_kernel ? "true" : "false",
                        cost->estimated ? "true" : "false");
  }
  ir_explain_json_raw("],");
}

/* The pass ledger, heaviest first: a reader wants the passes that moved the
 * most instructions, not the alphabetical list. */
static void ir_explain_passes_json(void) {
  if (!g_explain_json) {
    return;
  }
  ir_explain_json_raw("\"passes\":[");
  size_t emitted = 0;
  for (;;) {
    size_t best = (size_t)-1;
    long best_removed = 0;
    size_t best_changed = 0;
    for (size_t i = 0; i < g_pass_count; i++) {
      if (g_passes[i].runs == 0) {
        continue; /* already emitted: runs is zeroed as we go */
      }
      long removed = g_passes[i].instructions_removed;
      if (best == (size_t)-1 || removed > best_removed ||
          (removed == best_removed && g_passes[i].changed_runs > best_changed)) {
        best = i;
        best_removed = removed;
        best_changed = g_passes[i].changed_runs;
      }
    }
    if (best == (size_t)-1) {
      break;
    }
    IRExplainPassEntry *entry = &g_passes[best];
    ir_explain_json_raw("%s{\"pass\":", emitted++ ? "," : "");
    ir_explain_json_str(entry->name);
    ir_explain_json_raw(",\"runs\":%zu,\"changedRuns\":%zu,"
                        "\"instructionsRemoved\":%ld",
                        entry->runs, entry->changed_runs,
                        entry->instructions_removed);

    /* What it did: the opcodes it removed (positive) or introduced (negative).
     * "-8 load, -8 store, +4 assign" is a pass description; "changed" is not. */
    if (entry->opcode_delta) {
      ir_explain_json_raw(",\"effects\":{");
      size_t effects = 0;
      for (size_t op = 0; op < IR_EXPLAIN_OPCODE_LIMIT; op++) {
        if (entry->opcode_delta[op] == 0) {
          continue;
        }
        ir_explain_json_raw("%s", effects++ ? "," : "");
        ir_explain_json_str(ir_opcode_name((IROpcode)op));
        ir_explain_json_raw(":%d", entry->opcode_delta[op]);
      }
      ir_explain_json_raw("}");
    }

    /* Where it happened: the lines it moved the most instructions at. */
    if (entry->site_count > 0) {
      ir_explain_json_raw(",\"sites\":[");
      size_t shown = 0;
      for (size_t round = 0; round < IR_EXPLAIN_REPORTED_SITES; round++) {
        size_t pick = (size_t)-1;
        long best_delta = 0;
        for (size_t s = 0; s < entry->site_count; s++) {
          long magnitude = entry->sites[s].delta < 0 ? -entry->sites[s].delta
                                                     : entry->sites[s].delta;
          if (magnitude > best_delta) {
            best_delta = magnitude;
            pick = s;
          }
        }
        if (pick == (size_t)-1) {
          break;
        }
        ir_explain_json_raw("%s{\"fn\":", shown++ ? "," : "");
        ir_explain_json_str(entry->sites[pick].function_name);
        ir_explain_json_raw(",\"line\":%zu,\"delta\":%ld}",
                            entry->sites[pick].line, entry->sites[pick].delta);
        entry->sites[pick].delta = 0; /* consumed */
      }
      ir_explain_json_raw("]");
    }
    ir_explain_json_raw("}");

    for (size_t s = 0; s < entry->site_count; s++) {
      free(entry->sites[s].function_name);
    }
    free(entry->sites);
    entry->sites = NULL;
    entry->site_count = 0;
    entry->site_capacity = 0;
    free(entry->opcode_delta);
    entry->opcode_delta = NULL;
    entry->runs = 0; /* consumed */
  }
  ir_explain_json_raw("],");
  g_pass_count = 0;
}

void ir_explain_flush(void) {
  if (!g_explain) {
    return;
  }

  ir_explain_print_header("optimization report");

  if (g_remark_count > 0) {
    qsort(g_remarks, g_remark_count, sizeof(IRExplainRemark),
          ir_explain_remark_compare);
  }
  ir_explain_render_changes();
  /* The plan belongs here, and half of what it ranks does not exist yet: the
   * eligibility gate runs during codegen, after this flush. Remember the spot
   * and fill it in once the whole report is assembled. */
  g_plan_offset = g_report_len;
  g_plan_pending = 1;

  size_t json_remark_count = 0;
  ir_explain_json_raw("\"remarks\":[");

  size_t shown_remarks = 0;
  if (g_remark_count == 0) {
    ir_explain_emit("  (no loops or calls to report)\n\n");
  } else {
    char *suppressed = calloc(g_remark_count, 1);
    for (size_t i = 0; i < g_remark_count; i++) {
      const IRExplainRemark *r = &g_remarks[i];
      if (suppressed && suppressed[i]) {
        continue;
      }
      /* The selector hides prose only. The JSON sidecar and the digest
       * tallies below stay whole-file, so a filtered run and an unfiltered
       * one produce the same machine-readable document. */
      int show = ir_explain_remark_selected(r);
      shown_remarks += show ? 1 : 0;

      /* Fold a run of identical call refusals into one entry. */
      if (suppressed && ir_explain_remark_foldable(r)) {
        size_t group_count = 0;
        size_t last_line = r->line;
        for (size_t j = i; j < g_remark_count; j++) {
          if (ir_explain_remark_foldable(&g_remarks[j]) &&
              ir_explain_remarks_groupable(r, &g_remarks[j])) {
            group_count++;
            last_line = g_remarks[j].line;
          }
        }
        if (group_count >= IR_EXPLAIN_GROUP_MIN) {
          char callees[512];
          ir_explain_group_callee_list(g_remarks, i, callees,
                                       sizeof(callees));
          if (show) {
            ir_explain_emit("  %s%s%s (%zu calls, lines %zu-%zu): %s%s%s%s\n",
                            clr(EXPLAIN_BOLD), r->function_name,
                            clr(EXPLAIN_RESET), group_count, r->line, last_line,
                            clr(r->positive ? EXPLAIN_GREEN : EXPLAIN_RED),
                            r->headline, clr(EXPLAIN_RESET),
                            ir_explain_code_tag(r));
            if (r->reason) {
              ir_explain_emit("      %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                              glyph_elbow(), r->reason, clr(EXPLAIN_RESET));
            }
            if (r->fix) {
              ir_explain_emit("      %s%s fix: %s%s\n", clr(EXPLAIN_DIM),
                              glyph_elbow(), r->fix, clr(EXPLAIN_RESET));
            }
            if (r->verified) {
              ir_explain_emit("      %s%s verified: %s%s%s\n", clr(EXPLAIN_DIM),
                              glyph_elbow(), clr(EXPLAIN_GREEN), r->verified,
                              clr(EXPLAIN_RESET));
            }
            if (r->partial) {
              ir_explain_emit("      %s%s still blocked: %s%s\n",
                              clr(EXPLAIN_DIM), glyph_elbow(), r->partial,
                              clr(EXPLAIN_RESET));
            }
            ir_explain_emit("      %s%s calls: %s%s\n", clr(EXPLAIN_DIM),
                            glyph_elbow(), callees, clr(EXPLAIN_RESET));
          }
          if (strcmp(r->headline, "NOT inlined") == 0) {
            g_digest.calls_refused += group_count;
          } else if (strcmp(r->headline, "inlined") == 0) {
            g_digest.calls_inlined += group_count;
          }
          ir_explain_json_remark(r, "calls-folded", NULL, group_count,
                                 last_line, callees, &json_remark_count);
          for (size_t j = i; j < g_remark_count; j++) {
            if (ir_explain_remark_foldable(&g_remarks[j]) &&
                ir_explain_remarks_groupable(r, &g_remarks[j])) {
              suppressed[j] = 1;
            }
          }
          continue;
        }
      }

      if (show) {
        ir_explain_emit("  %s%s%s (%s @ line %zu): %s%s%s%s\n",
                        clr(EXPLAIN_BOLD), r->function_name, clr(EXPLAIN_RESET),
                        r->entity, r->line,
                        clr(r->positive ? EXPLAIN_GREEN : EXPLAIN_RED),
                        r->headline, clr(EXPLAIN_RESET),
                        ir_explain_code_tag(r));
        /* The line itself, but only where there is something to act on:
         * quoting the source under every successful inline would treble the
         * report and say nothing. */
        if (r->reason) {
          ir_explain_echo_source_range(r->line, r->end_line);
          ir_explain_emit("      %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), r->reason, clr(EXPLAIN_RESET));
        }
        if (r->fix) {
          /* "fix: nothing to change here" is a contradiction. Where the
           * advice describes the loop instead of instructing anyone, the
           * label says so. */
          ir_explain_emit("      %s%s %s: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), r->advisory ? "note" : "fix", r->fix,
                          clr(EXPLAIN_RESET));
        }
        if (r->verified) {
          ir_explain_emit("      %s%s verified: %s%s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), clr(EXPLAIN_GREEN), r->verified,
                          clr(EXPLAIN_RESET));
        }
        if (r->partial) {
          ir_explain_emit("      %s%s still blocked: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), r->partial, clr(EXPLAIN_RESET));
        }
      }
      if (r->verified) {
        g_digest.fixes_verified++;
      }
      /* Digest tallies (loop/call outcomes by entity + headline). */
      if (r->entity && strcmp(r->entity, "loop") == 0) {
        if (strncmp(r->headline, "vectorized", 10) == 0) {
          g_digest.loops_vectorized++;
        } else if (strncmp(r->headline, "NOT vectorized", 14) == 0) {
          g_digest.loops_scalar++;
        }
        ir_explain_json_remark(r, "loop", NULL, 1, 0, NULL,
                               &json_remark_count);
      } else if (r->entity && strncmp(r->entity, "call to ", 8) == 0) {
        if (strcmp(r->headline, "inlined") == 0) {
          g_digest.calls_inlined++;
        } else if (strcmp(r->headline, "NOT inlined") == 0) {
          g_digest.calls_refused++;
        }
        char callee[96];
        ir_explain_entity_callee(r->entity, callee, sizeof(callee));
        ir_explain_json_remark(r, "call", callee, 1, 0, NULL,
                               &json_remark_count);
      } else {
        ir_explain_json_remark(
            r, r->entity && strcmp(r->entity, "function") == 0 ? "function"
                                                               : "other",
            NULL, 1, 0, NULL, &json_remark_count);
      }
    }
    free(suppressed);

    /* A selector that matched nothing is a question the report failed to
     * answer, so say what the selector accepts rather than printing a blank
     * section and letting the reader guess. */
    if (g_explain_filter && shown_remarks == 0) {
      ir_explain_emit("  nothing matches --explain=%s (%zu findings in this "
                      "file)\n",
                      g_explain_filter, g_remark_count);
      ir_explain_emit("  %sselectors: missed, fixable, proven, loops, calls, "
                      "a function name, or a decision code%s\n",
                      clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
    } else if (g_explain_filter && shown_remarks < g_remark_count) {
      ir_explain_emit("  %s%zu of %zu findings hidden by --explain=%s%s\n",
                      clr(EXPLAIN_DIM), g_remark_count - shown_remarks,
                      g_remark_count, g_explain_filter, clr(EXPLAIN_RESET));
    }

    /* Close the loop between the one-line verdict and the paragraph behind it.
     * The example names a code the reader can actually see above, preferring a
     * refusal, since that is the line they want explained. */
    const char *sample = NULL;
    for (size_t i = 0; i < g_remark_count && !sample && shown_remarks; i++) {
      if (!g_remarks[i].positive && g_remarks[i].code &&
          g_remarks[i].code[0] && ir_explain_remark_selected(&g_remarks[i])) {
        sample = g_remarks[i].code;
      }
    }
    for (size_t i = 0; i < g_remark_count && !sample && shown_remarks; i++) {
      if (g_remarks[i].code && g_remarks[i].code[0] &&
          ir_explain_remark_selected(&g_remarks[i])) {
        sample = g_remarks[i].code;
      }
    }
    if (sample) {
      ir_explain_emit("  %sthe bracketed id after a verdict has a longer "
                      "explanation: mettle explain %s%s\n",
                      clr(EXPLAIN_DIM), sample, clr(EXPLAIN_RESET));
      ir_explain_emit("  %s%s/explain/%s.html%s\n", clr(EXPLAIN_DIM),
                      IR_EXPLAIN_DOCS_BASE, sample, clr(EXPLAIN_RESET));
    }
    ir_explain_emit("\n");
  }
  ir_explain_json_raw("],");

  ir_explain_tally_functions();

  /* Memory diagnostics land after "remarks" and before "backend". */
  ir_explain_safety_flush();
  ir_explain_memory_flush();
}

/* The remarks outlive this flush: --annotate-asm reads them during codegen, and
 * the backend section (also after codegen) needs them to name the loop that
 * made a function ineligible. Released once the report is written. */
static void ir_explain_release_remarks(void) {
  for (size_t i = 0; i < g_remark_count; i++) {
    free(g_remarks[i].function_name);
    free(g_remarks[i].entity);
    free(g_remarks[i].headline);
    free(g_remarks[i].reason);
    free(g_remarks[i].fix);
    free(g_remarks[i].verified);
    free(g_remarks[i].partial);
    free(g_remarks[i].code);
    for (size_t q = 0; q < g_remarks[i].quantity_count; q++) {
      free(g_remarks[i].quantities[q].name);
    }
  }
  free(g_remarks);
  g_remarks = NULL;
  g_remark_count = 0;
  g_remark_capacity = 0;
}

/* ---- backend (codegen) section ------------------------------------------- */

void ir_explain_backend_function(const char *function_name,
                                 const char *filename, int ok,
                                 const char *detail, size_t instructions) {
  if (!g_explain || !function_name || !ir_explain_file_enabled(filename)) {
    return;
  }
  for (size_t i = 0; i < g_backend_count; i++) {
    if (strcmp(g_backend[i].function_name, function_name) == 0) {
      return; /* first decision wins; the gate can be probed more than once */
    }
  }
  if (g_backend_count == g_backend_capacity) {
    size_t new_capacity = g_backend_capacity ? g_backend_capacity * 2 : 16;
    IRExplainBackendEntry *grown =
        realloc(g_backend, new_capacity * sizeof(IRExplainBackendEntry));
    if (!grown) {
      return;
    }
    g_backend = grown;
    g_backend_capacity = new_capacity;
  }
  IRExplainBackendEntry *e = &g_backend[g_backend_count++];
  e->function_name = ir_explain_strdup(function_name);
  e->ok = ok;
  e->detail = ir_explain_strdup(detail);
  e->instructions = instructions;
}

/* ---- report routing ---------------------------------------------------------
 * Small reports print to stderr exactly as before. Past a line threshold (a
 * real application produces hundreds of remarks) the full report is written
 * to `<output-stem>.explain.txt` next to the output binary, and stderr gets a
 * one-paragraph digest with the path. */

#define IR_EXPLAIN_STDERR_MAX_LINES 200

static size_t ir_explain_report_lines(void) {
  size_t lines = 0;
  for (size_t i = 0; i < g_report_len; i++) {
    lines += (g_report_buf[i] == '\n') ? 1 : 0;
  }
  return lines;
}

/* `<dir>/<stem>.explain.txt` from the output path; caller frees. */
static char *ir_explain_sidecar_path(void) {
  return ir_explain_derived_path(".explain.txt");
}

/* ---- wrapping ---------------------------------------------------------------
 * The report is built one line per fact, and a reason can run past 300
 * columns. A terminal folds that at column 0, so the `\_ reason:` tree the
 * report is shaped around dissolves into a wall.
 *
 * Wrapping happens here, at the moment of writing to a terminal, and nowhere
 * else. A redirected run, a pipe into grep and the `.explain.txt` sidecar all
 * keep the one-line-per-fact form, so a pattern that matches a whole reason
 * keeps matching one. Only the interactive reader gets the typeset version,
 * which is the only reader whose width we know. */

static int ir_explain_terminal_columns(void) {
  return (int)diag_style_columns();
}

/* Visible columns in `line` (ANSI escape sequences occupy none), counting a
 * UTF-8 sequence as the one column its glyph takes. */
static size_t ir_explain_visible_width(const char *line, size_t len) {
  size_t width = 0;
  for (size_t i = 0; i < len; i++) {
    if (line[i] == '\x1b') {
      while (i < len && line[i] != 'm') {
        i++;
      }
      continue;
    }
    if (((unsigned char)line[i] & 0xC0) != 0x80) {
      width++;
    }
  }
  return width;
}

/* Fold one already-rendered line to `columns`, breaking on spaces and
 * indenting continuations three columns inside the line's own indent, so a
 * wrapped detail still reads as subordinate to its verdict. Colors survive
 * the break: the sequence that opened the line has not been reset yet. */
static void ir_explain_write_wrapped_line(FILE *out, const char *line,
                                          size_t len, size_t columns) {
  size_t indent = 0;
  while (indent < len && line[indent] == ' ') {
    indent++;
  }
  size_t hang = indent + 3;
  if (hang + 24 > columns) {
    hang = indent; /* a narrow terminal needs the width more than the shape */
  }

  size_t start = 0;
  size_t budget = columns;
  while (start < len) {
    if (ir_explain_visible_width(line + start, len - start) <= budget) {
      fwrite(line + start, 1, len - start, out);
      break;
    }
    /* Walk forward to the last space that still fits. */
    size_t width = 0, i = start, last_space = 0;
    while (i < len && width <= budget) {
      if (line[i] == '\x1b') {
        while (i < len && line[i] != 'm') {
          i++;
        }
        i++;
        continue;
      }
      if (line[i] == ' ' && i > start) {
        last_space = i;
      }
      if (((unsigned char)line[i] & 0xC0) != 0x80) {
        width++;
      }
      i++;
    }
    if (last_space <= start) {
      /* One unbreakable run (a path, a long identifier): let it overflow
         rather than cutting it in half. */
      fwrite(line + start, 1, len - start, out);
      break;
    }
    fwrite(line + start, 1, last_space - start, out);
    fputc('\n', out);
    for (size_t s = 0; s < hang; s++) {
      fputc(' ', out);
    }
    start = last_space + 1;
    budget = columns > hang ? columns - hang : columns;
  }
  fputc('\n', out);
}

static void ir_explain_write_wrapped(FILE *out, size_t columns) {
  size_t start = 0;
  for (size_t i = 0; i <= g_report_len; i++) {
    if (i == g_report_len || g_report_buf[i] == '\n') {
      ir_explain_write_wrapped_line(out, g_report_buf + start, i - start,
                                    columns);
      start = i + 1;
    }
  }
}

/* Write the buffer with ANSI color sequences stripped (the report renders
 * with stderr in mind; a file must stay plain). */
static int ir_explain_write_plain(FILE *out) {
  for (size_t i = 0; i < g_report_len; i++) {
    if (g_report_buf[i] == '\x1b' && i + 1 < g_report_len &&
        g_report_buf[i + 1] == '[') {
      i += 2;
      while (i < g_report_len && g_report_buf[i] != 'm') {
        i++;
      }
      continue;
    }
    if (fputc(g_report_buf[i], out) == EOF) {
      return 0;
    }
  }
  return 1;
}

/* Render the deferred plan and put it back where it belongs: after the changes
 * block, before the remarks. Splicing beats printing it at the end -- the plan
 * is the first thing a reader should meet, and a report that opens with the
 * detail and closes with the summary gets read in the wrong order. */
static void ir_explain_splice_plan(void) {
  if (!g_plan_pending) {
    return;
  }
  g_plan_pending = 0;
  if (g_plan_offset > g_report_len) {
    return;
  }
  size_t tail_len = g_report_len - g_plan_offset;
  char *tail = tail_len ? malloc(tail_len) : NULL;
  if (tail_len && !tail) {
    ir_explain_render_start_here(); /* at the end beats not at all */
    return;
  }
  if (tail) {
    memcpy(tail, g_report_buf + g_plan_offset, tail_len);
  }
  g_report_len = g_plan_offset;
  ir_explain_render_start_here();
  if (tail) {
    ir_explain_emit("%.*s", (int)tail_len, tail);
    free(tail);
  }
}

void ir_explain_finalize(int force_stderr) {
  if (!g_explain || !g_report_buf || g_report_len == 0) {
    return;
  }
  ir_explain_splice_plan();

  size_t threshold = IR_EXPLAIN_STDERR_MAX_LINES;
  const char *env = getenv("METTLE_EXPLAIN_REPORT_LINES");
  if (env && env[0]) {
    long v = atol(env);
    threshold = (v <= 0) ? (size_t)-1 : (size_t)v;
  }

  char *sidecar = NULL;
  int diverted = 0;
  if (!force_stderr && ir_explain_report_lines() > threshold &&
      (sidecar = ir_explain_sidecar_path()) != NULL) {
    FILE *out = fopen(sidecar, "wb");
    if (out) {
      diverted = ir_explain_write_plain(out);
      fclose(out);
    }
  }

  /* The machine-readable sidecar, independent of where the prose went. */
  if (g_explain_json && g_json_buf) {
    char *json_path = ir_explain_derived_path(".explain.json");
    if (json_path) {
      FILE *out = fopen(json_path, "wb");
      if (out) {
        const char *source = g_explain_focus_file
                                 ? ir_explain_path_basename(g_explain_focus_file)
                                 : "";
        /* Schema 2 is additive: every schema 1 key still means what it did,
         * so a consumer written against 1 keeps working unchanged. */
        fprintf(out, "{\"schema\":2,\"source\":\"%s\",", source);
        fwrite(g_json_buf, 1, g_json_len, out);
        fprintf(out,
                "\"stats\":{\"loopsVectorized\":%zu,\"loopsScalar\":%zu,"
                "\"fixesVerified\":%zu,\"callsInlined\":%zu,"
                "\"callsRefused\":%zu,\"changesImproved\":%zu,"
                "\"changesRegressed\":%zu,\"hadBaseline\":%s}}\n",
                g_digest.loops_vectorized, g_digest.loops_scalar,
                g_digest.fixes_verified, g_digest.calls_inlined,
                g_digest.calls_refused, g_digest.changes_improved,
                g_digest.changes_regressed,
                g_digest.had_baseline ? "true" : "false");
        fclose(out);
      }
      free(json_path);
    }
  }
  free(g_json_buf);
  g_json_buf = NULL;
  g_json_len = 0;
  g_json_cap = 0;

  for (size_t i = 0; i < g_mem_count; i++) {
    free(g_mem[i].code);
    free(g_mem[i].headline);
    free(g_mem[i].fix);
  }
  free(g_mem);
  g_mem = NULL;
  g_mem_count = 0;
  g_mem_capacity = 0;
  free(g_mem_focus);
  g_mem_focus = NULL;
  ir_explain_source_free();

  if (!diverted) {
    /* Only a terminal gets the wrapped form, and only when it is narrower
     * than the report. Everything else stays one line per fact.
     *
     * METTLE_EXPLAIN_COLUMNS forces a width, which is how the wrapping is
     * tested (a test harness never has a terminal) and how someone whose
     * terminal misreports its size can pin one. */
    int fd = explain_fileno(stderr);
    int columns = (fd >= 0 && explain_isatty(fd)) ? ir_explain_terminal_columns()
                                                  : 0;
    const char *forced = getenv("METTLE_EXPLAIN_COLUMNS");
    if (forced && forced[0]) {
      columns = atoi(forced);
    }
    diag_style_output_begin();
    if (columns >= 40) {
      ir_explain_write_wrapped(stderr, (size_t)columns);
    } else {
      fwrite(g_report_buf, 1, g_report_len, stderr);
    }
    diag_style_output_end();
  } else {
    /* The digest: the report's conclusions in five lines, plus the path.
     * Regressions lead -- they must never hide inside a sidecar. */
    diag_style_output_begin();
    {
      char label[64];
      snprintf(label, sizeof(label), "%soptimization report%s",
               clr(EXPLAIN_BOLD), clr(EXPLAIN_RESET));
      fputc(10, stderr);
      diag_rule(stderr, 0, label, "");
    }
    if (g_digest.changes_regressed > 0) {
      fprintf(stderr,
              "  %s%s%zu optimization%s REGRESSED since the last build%s "
              "(see the changes section of the report)\n",
              clr(EXPLAIN_BOLD), clr(EXPLAIN_RED), g_digest.changes_regressed,
              g_digest.changes_regressed == 1 ? "" : "s", clr(EXPLAIN_RESET));
    } else if (g_digest.changes_improved > 0) {
      fprintf(stderr, "  %s%zu optimization%s improved since the last build%s\n",
              clr(EXPLAIN_GREEN), g_digest.changes_improved,
              g_digest.changes_improved == 1 ? "" : "s", clr(EXPLAIN_RESET));
    }
    fprintf(stderr,
            "  loops: %s%zu vectorized%s, %zu scalar; %s%zu fix suggestions "
            "verified by simulation%s\n",
            clr(EXPLAIN_GREEN), g_digest.loops_vectorized, clr(EXPLAIN_RESET),
            g_digest.loops_scalar, clr(EXPLAIN_GREEN), g_digest.fixes_verified,
            clr(EXPLAIN_RESET));
    fprintf(stderr, "  calls: %zu inlined, %zu kept as real calls\n",
            g_digest.calls_inlined, g_digest.calls_refused);
    if (g_digest.backend_total > 0) {
      fprintf(stderr,
              "  backend: %zu/%zu functions register-allocated\n",
              g_digest.backend_ok, g_digest.backend_total);
    }
    /* Counts say how the build went; this says what to do about it. */
    if (g_digest.start_here[0]) {
      char lead[200];
      ir_explain_fit(g_digest.start_here, 96, lead, sizeof(lead));
      fprintf(stderr, "  start with: %s%s%s%s\n",
              g_digest.start_here_proven ? clr(EXPLAIN_GREEN) : "",
              g_digest.start_here_proven ? "[proven] " : "", lead,
              clr(EXPLAIN_RESET));
    }
    fprintf(stderr, "  full report (%zu lines): %s%s%s\n\n",
            ir_explain_report_lines(), clr(EXPLAIN_BOLD), sidecar,
            clr(EXPLAIN_RESET));
    diag_style_output_end();
  }

  free(sidecar);
  free(g_report_buf);
  g_report_buf = NULL;
  g_report_len = 0;
  g_report_cap = 0;
  memset(&g_digest, 0, sizeof(g_digest));
  if (!g_explain_retain_remarks) {
    ir_explain_release_remarks();
  }
}

/* Past this many optimized IR instructions, a whole-function fallback stops
 * being a curiosity and becomes the biggest single cost in the report: every
 * value in a function this size goes through the stack. Below it, splitting the
 * function out costs the reader more than the spills do. */
#define IR_EXPLAIN_BACKEND_LARGE_INSTRUCTIONS 64

/* True when `callee` had a loop of its own vectorized. */
static int ir_explain_function_vectorized(const char *callee) {
  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    if (r->code && strcmp(r->code, "vectorized") == 0 && r->function_name &&
        callee && strcmp(r->function_name, callee) == 0) {
      return 1;
    }
  }
  return 0;
}

/* The calls this function inlined that brought a vectorized loop in with them.
 * Writes "kernel inlined from `imap` @ line 19" (or a list) into `buf` and
 * returns the first such line, or 0 when there are none. */
static size_t ir_explain_inlined_kernel_calls(const char *function_name,
                                              char *buf, size_t cap) {
  size_t first = 0, listed = 0, w = 0;
  buf[0] = '\0';
  for (size_t i = 0; i < g_remark_count && listed < 3; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    if (!r->code || strcmp(r->code, "inlined") != 0 || !r->function_name ||
        !function_name || strcmp(r->function_name, function_name) != 0) {
      continue;
    }
    char callee[128];
    ir_explain_entity_callee(r->entity, callee, sizeof(callee));
    if (!ir_explain_function_vectorized(callee)) {
      continue;
    }
    if (!listed) {
      w = (size_t)snprintf(buf, cap, "kernel inlined from ");
      first = r->line;
    }
    int n = snprintf(buf + w, cap > w ? cap - w : 0, "%s`%s` @ line %zu",
                     listed ? ", " : "", callee, r->line);
    if (n < 0) {
      break;
    }
    w += (size_t)n;
    listed++;
  }
  if (!listed) {
    buf[0] = '\0';
  }
  return first;
}

/* Where this function's vectorized loops are, when a SIMD kernel is what made
 * it ineligible. Falling back is always ABOUT a construct in the source, and
 * "contains simd_vloop_i32" is not something a reader can act on without being
 * told which loop that is. Writes "loop @ line 29" or "loops @ lines 12, 20,
 * 29" into `buf` and returns the first line, or returns 0 and leaves `buf`
 * empty when no vectorized loop was recorded for the function. A function that
 * inlined several kernels gets all of them listed: picking one would be a
 * guess, and the list is short enough to read. */
#define IR_EXPLAIN_KERNEL_LINES_MAX 4
static size_t ir_explain_kernel_loop_lines(const char *function_name, char *buf,
                                           size_t cap) {
  size_t lines[IR_EXPLAIN_KERNEL_LINES_MAX];
  size_t found = 0, total = 0;
  if (buf && cap) {
    buf[0] = '\0';
  }
  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    if (!r->code || strcmp(r->code, "vectorized") != 0) {
      continue;
    }
    if (!r->function_name || !function_name ||
        strcmp(r->function_name, function_name) != 0) {
      continue;
    }
    total++;
    if (found < IR_EXPLAIN_KERNEL_LINES_MAX) {
      lines[found++] = r->line;
    }
  }
  if (found == 0) {
    /* No kernel of its own. A function reaches this gate with a kernel it
     * never wrote all the time: inlining brings the callee's vectorized loop
     * in with it. Pointing at the call is what lets the reader act -- the
     * loop to move out is on the other side of it. */
    return buf && cap ? ir_explain_inlined_kernel_calls(function_name, buf, cap)
                      : 0;
  }
  if (!buf || !cap) {
    return lines[0];
  }
  size_t w = (size_t)snprintf(buf, cap, found == 1 ? "loop @ line " :
                                                     "loops @ lines ");
  for (size_t i = 0; i < found && w < cap; i++) {
    int n = snprintf(buf + w, cap - w, "%s%zu", i ? ", " : "", lines[i]);
    if (n < 0) {
      break;
    }
    w += (size_t)n;
  }
  if (total > found && w < cap) {
    snprintf(buf + w, cap - w, " and %zu more", total - found);
  }
  return lines[0];
}

/* True when the gate declined because of a SIMD kernel it cannot pass through,
 * as opposed to something about the function itself. The two need opposite
 * advice, and only the kernel families have a loop to point the reader at. */
static int ir_explain_backend_detail_is_kernel(const char *detail) {
  if (!detail) {
    return 0;
  }
  if (strncmp(detail, "op:", 3) == 0) {
    int op = atoi(detail + 3);
    return op >= (int)IR_OP_COUNT_WORD_STARTS &&
           op <= (int)IR_OP_SIMD_OUTER_LANE_F64;
  }
  return strncmp(detail, "simd_fill:", 10) == 0 ||
         strncmp(detail, "affine_map:", 11) == 0 ||
         strncmp(detail, "slp_mac:", 8) == 0 ||
         strncmp(detail, "silu:", 5) == 0 ||
         strncmp(detail, "kernel:", 7) == 0 ||
         strncmp(detail, "vloop:", 6) == 0;
}

/* Translate the MIR gate's terse reason codes ("op:37", "vloop:width", ...)
 * into a sentence, plus what falling back actually COSTS and what the user can
 * do about it.
 *
 * Two families, and they need opposite advice. A SIMD kernel the allocator
 * cannot pass through still runs at full vector speed; only the scalar code
 * around it spills, so on a small function there is nothing worth doing. Every
 * other cause spills the whole function for a construct the reader chose. Both
 * turn actionable once the function is large, which is why size decides whether
 * the advice is an instruction or a note. */
static void ir_explain_backend_reason(const IRExplainBackendEntry *e, char *buf,
                                      size_t cap, const char **consequence,
                                      const char **fix, int *advisory) {
  *consequence = NULL;
  *fix = NULL;
  /* The same distinction the loop remarks draw: advice that says nothing
     needs doing is a note, not an instruction. */
  *advisory = 0;
  if (!e->detail) {
    snprintf(buf, cap, "declined by the eligibility gate");
    return;
  }
  int large = e->instructions >= IR_EXPLAIN_BACKEND_LARGE_INSTRUCTIONS;
  const char *kernel_consequence =
      "the kernel itself runs at full vector speed; only the scalar code "
      "around it keeps values on the stack";
  const char *whole_consequence =
      "every value in the function is kept on the stack instead of in "
      "registers";

  /* Shared closing for every SIMD-kernel cause: same consequence, same fix,
   * and the fix names the loop to move when there is one to name. */
  const char *kernel_family = NULL;
  char kernel_desc[192];
  kernel_desc[0] = '\0';

  if (strncmp(e->detail, "op:", 3) == 0) {
    int op = atoi(e->detail + 3);
    if (op >= (int)IR_OP_COUNT_WORD_STARTS &&
        op <= (int)IR_OP_SIMD_OUTER_LANE_F64) {
      snprintf(kernel_desc, sizeof(kernel_desc),
               "contains the SIMD kernel `%s`, which the register allocator "
               "doesn't cover yet",
               ir_opcode_name((IROpcode)op));
      kernel_family = kernel_desc;
    } else {
      snprintf(buf, cap,
               "contains `%s`, which the register allocator doesn't cover yet",
               ir_opcode_name((IROpcode)op));
      *consequence = whole_consequence;
      return;
    }
  } else if (strncmp(e->detail, "simd_fill:", 10) == 0 ||
             strncmp(e->detail, "affine_map:", 11) == 0 ||
             strncmp(e->detail, "slp_mac:", 8) == 0 ||
             strncmp(e->detail, "silu:", 5) == 0 ||
             strncmp(e->detail, "kernel:", 7) == 0) {
    /* A kernel whose inline-passthrough subset doesn't cover this loop's exact
     * shape (a mode-2 fill, an affine map with a runtime coefficient). */
    const char *kernel = "a SIMD kernel";
    if (strncmp(e->detail, "simd_fill:", 10) == 0) {
      kernel = "the fill kernel `simd_fill`";
    } else if (strncmp(e->detail, "affine_map:", 11) == 0) {
      kernel = "the affine-map kernel `simd_affine_map`";
    } else if (strncmp(e->detail, "slp_mac:", 8) == 0) {
      kernel = "the multiply-accumulate kernel `simd_slp_mac`";
    } else if (strncmp(e->detail, "silu:", 5) == 0) {
      kernel = "the SiLU kernel `simd_silu`";
    }
    snprintf(kernel_desc, sizeof(kernel_desc),
             "contains %s in a form the register allocator's inline "
             "passthrough doesn't cover yet",
             kernel);
    kernel_family = kernel_desc;
  } else if (strcmp(e->detail, "vloop:reduce") == 0) {
    snprintf(kernel_desc, sizeof(kernel_desc),
             "contains a vectorized '+' reduction (`s = s + expr`); the "
             "allocator's inline passthrough covers element-wise maps only");
    kernel_family = kernel_desc;
  } else if (strcmp(e->detail, "vloop:width") == 0) {
    snprintf(kernel_desc, sizeof(kernel_desc),
             "contains a general-vectorized loop over int32 or float32 lanes; "
             "the allocator's inline passthrough covers float64 lanes only");
    kernel_family = kernel_desc;
  } else if (strncmp(e->detail, "vloop:", 6) == 0) {
    snprintf(kernel_desc, sizeof(kernel_desc),
             "contains a general-vectorized loop whose operands the allocator "
             "cannot marshal (%s)",
             e->detail + 6);
    kernel_family = kernel_desc;
  } else if (strcmp(e->detail, "call_unsupported") == 0) {
    snprintf(buf, cap, "contains a call form the register allocator doesn't "
                       "support yet");
    *consequence = whole_consequence;
    return;
  } else if (strcmp(e->detail, "call_indirect_unsupported") == 0) {
    snprintf(buf, cap, "calls through a function pointer, which the register "
                       "allocator doesn't cover yet");
    *consequence = whole_consequence;
    *fix = "call the target directly where it is known at compile time";
    return;
  } else if (strcmp(e->detail, "sig:float_stack_param") == 0) {
    snprintf(buf, cap,
             "takes a float argument past the register-argument slots, which "
             "the allocator's calling convention doesn't cover yet");
    *consequence = whole_consequence;
    *fix = "pass the floats in a struct, or reorder the parameters so the "
           "floats land in the first four";
    return;
  } else if (strncmp(e->detail, "sig:params", 10) == 0) {
    snprintf(buf, cap,
             "takes more arguments than the allocator's calling convention "
             "covers");
    *consequence = whole_consequence;
    *fix = "group the arguments into a struct and pass a pointer to it";
    return;
  } else if (strcmp(e->detail, "sig:param_nonscalar") == 0 ||
             strcmp(e->detail, "sig:return_nonscalar") == 0 ||
             strcmp(e->detail, "sig:arg_layout") == 0) {
    snprintf(buf, cap,
             "passes or returns an aggregate by value, which the allocator's "
             "calling convention doesn't cover yet");
    *consequence = whole_consequence;
    *fix = "pass a pointer to the aggregate instead of the value";
    return;
  } else if (strcmp(e->detail, "global_access") == 0) {
    snprintf(buf, cap, "reads or writes a global, which the register allocator "
                       "doesn't cover yet");
    *consequence = whole_consequence;
    *fix = "read the global once into a local at the top of the function";
    return;
  } else if (strcmp(e->detail, "declare_local:nonscalar") == 0) {
    snprintf(buf, cap,
             "declares a struct or array local, which the register allocator "
             "doesn't cover yet");
    *consequence = whole_consequence;
    return;
  } else {
    snprintf(buf, cap,
             "declined by the eligibility gate; the gate's reason code is `%s` "
             "and no plain-language translation exists for it yet",
             e->detail);
    *consequence = whole_consequence;
    *fix = "nothing to change in your code: this is a gap in the compiler's "
           "reporting as well as in the register allocator";
    *advisory = 1;
    return;
  }

  /* One SIMD-kernel ending for all of the above. */
  snprintf(buf, cap, "%s", kernel_family);
  *consequence = kernel_consequence;
  if (large) {
    *fix = "move the vectorized loop into a function of its own: the kernel "
           "keeps its speed there, and the scalar code left behind gets the "
           "register allocator back";
  } else {
    *fix = "nothing worth doing at this size: the spills are confined to the "
           "scalar code around a kernel that already runs at full speed";
    *advisory = 1;
  }
}

static size_t ir_explain_collect_backend_plan(IRExplainBackendPlan *out) {
  size_t found = 0;
  for (size_t i = 0; i < g_backend_count; i++) {
    const IRExplainBackendEntry *e = &g_backend[i];
    if (e->ok || e->instructions < IR_EXPLAIN_BACKEND_LARGE_INSTRUCTIONS) {
      continue;
    }
    /* --explain=SELECTOR narrows the prose, and a fallback answers to the
     * function it happened in: name that function and it belongs, otherwise
     * the reader asked about something else. */
    if (g_explain_filter && (!e->function_name ||
                             strcmp(g_explain_filter, e->function_name) != 0)) {
      continue;
    }
    char reason[256];
    const char *consequence = NULL, *fix = NULL;
    int advisory = 0;
    ir_explain_backend_reason(e, reason, sizeof(reason), &consequence, &fix,
                              &advisory);
    if (advisory || !fix) {
      continue; /* nothing to do about it is not a plan entry */
    }
    /* Insert by size: the largest fallback is the most expensive one. */
    size_t at = found;
    while (at > 0 && e->instructions > out[at - 1].instructions_sort) {
      at--;
    }
    if (at >= IR_EXPLAIN_BACKEND_PLAN_MAX) {
      continue;
    }
    for (size_t j = found < IR_EXPLAIN_BACKEND_PLAN_MAX
                         ? found
                         : IR_EXPLAIN_BACKEND_PLAN_MAX - 1;
         j > at; j--) {
      out[j] = out[j - 1];
    }
    snprintf(out[at].function_name, sizeof(out[at].function_name), "%s",
             e->function_name ? e->function_name : "?");
    snprintf(out[at].location, sizeof(out[at].location), "%s (%zu instrs)",
             out[at].function_name, e->instructions);
    char lines[96];
    lines[0] = '\0';
    if (ir_explain_backend_detail_is_kernel(e->detail)) {
      ir_explain_kernel_loop_lines(e->function_name, lines, sizeof(lines));
    }
    snprintf(out[at].why, sizeof(out[at].why), "%s%s%s", reason,
             lines[0] ? ": " : "", lines[0] ? lines : "");
    out[at].fix = fix;
    out[at].instructions_sort = e->instructions;
    if (found < IR_EXPLAIN_BACKEND_PLAN_MAX) {
      found++;
    }
  }
  return found;
}

/* Sort helper: biggest functions first -- size is where baseline codegen
 * costs, so the list reads as a priority queue. */
static int ir_explain_backend_size_compare(const void *a, const void *b) {
  const IRExplainBackendEntry *ea = a, *eb = b;
  if (ea->instructions != eb->instructions) {
    return ea->instructions > eb->instructions ? -1 : 1;
  }
  return strcmp(ea->function_name ? ea->function_name : "",
                eb->function_name ? eb->function_name : "");
}

void ir_explain_backend_flush(void) {
  if (!g_explain) {
    return;
  }

  size_t ok_count = 0;
  size_t total_instructions = 0;
  size_t ok_instructions = 0;
  for (size_t i = 0; i < g_backend_count; i++) {
    ok_count += g_backend[i].ok ? 1 : 0;
    total_instructions += g_backend[i].instructions;
    ok_instructions += g_backend[i].ok ? g_backend[i].instructions : 0;
  }
  g_digest.backend_ok = ok_count;
  g_digest.backend_total = g_backend_count;

  ir_explain_json_raw(
      "\"backend\":{\"ok\":%zu,\"total\":%zu,\"instructions\":%zu,"
      "\"okInstructions\":%zu,\"groups\":[",
      ok_count, g_backend_count, total_instructions, ok_instructions);
  size_t json_group_count = 0;

  ir_explain_print_header("backend report");
  if (g_backend_count == 0) {
    ir_explain_emit("  (no functions reached native codegen)\n\n");
  } else {
    ir_explain_emit(
        "  %zu/%zu functions reaching codegen (after inlining) compiled with "
        "the register-allocating backend\n",
        ok_count, g_backend_count);
    if (total_instructions > 0) {
      ir_explain_emit(
          "  %.1f%% of the program's %zu optimized IR instructions are in "
          "register-allocated code\n",
          100.0 * (double)ok_instructions / (double)total_instructions,
          total_instructions);
    }

    if (ok_count < g_backend_count) {
      ir_explain_emit(
          "\n  %zu function%s use%s baseline (spill-everything) codegen, "
          "grouped by cause, largest first:\n",
          g_backend_count - ok_count,
          g_backend_count - ok_count == 1 ? "" : "s",
          g_backend_count - ok_count == 1 ? "s" : "");

      /* Group the bailed entries by their rendered reason sentence, ordered
       * by the group's total instruction count (where the cost actually
       * is). Entries were sorted by size already, so each group's function
       * list reads largest-first. */
      qsort(g_backend, g_backend_count, sizeof(IRExplainBackendEntry),
            ir_explain_backend_size_compare);
      char *grouped = calloc(g_backend_count, 1);
      for (;;) {
        /* Pick the ungrouped reason with the largest remaining total. */
        char best_reason[256];
        const char *best_consequence = NULL, *best_fix = NULL;
        int best_advisory = 0;
        size_t best_total = 0, best_first = (size_t)-1;
        for (size_t i = 0; i < g_backend_count; i++) {
          if (g_backend[i].ok || (grouped && grouped[i])) {
            continue;
          }
          char reason_i[256];
          const char *cons_i, *fix_i;
          int advisory_i = 0;
          ir_explain_backend_reason(&g_backend[i], reason_i, sizeof(reason_i),
                                    &cons_i, &fix_i, &advisory_i);
          size_t total_i = 0;
          for (size_t j = i; j < g_backend_count; j++) {
            if (g_backend[j].ok || (grouped && grouped[j])) {
              continue;
            }
            char reason_j[256];
            const char *cj, *fj;
            int aj = 0;
            ir_explain_backend_reason(&g_backend[j], reason_j,
                                      sizeof(reason_j), &cj, &fj, &aj);
            if (strcmp(reason_i, reason_j) == 0) {
              total_i += g_backend[j].instructions;
            }
          }
          if (best_first == (size_t)-1 || total_i > best_total) {
            snprintf(best_reason, sizeof(best_reason), "%s", reason_i);
            best_consequence = cons_i;
            best_fix = fix_i;
            best_advisory = advisory_i;
            best_total = total_i;
            best_first = i;
          }
        }
        if (best_first == (size_t)-1) {
          break;
        }

        /* Render the group: header, consequence/fix once, then the largest
         * members with sizes. */
        size_t members = 0;
        for (size_t j = best_first; j < g_backend_count; j++) {
          if (g_backend[j].ok || (grouped && grouped[j])) {
            continue;
          }
          char reason_j[256];
          const char *cj, *fj;
          int aj = 0;
          ir_explain_backend_reason(&g_backend[j], reason_j, sizeof(reason_j),
                                    &cj, &fj, &aj);
          if (strcmp(best_reason, reason_j) == 0) {
            members++;
          }
        }
        ir_explain_emit("\n  %s%s%s (%zu function%s, %zu instructions):\n",
                        clr(EXPLAIN_BOLD), best_reason, clr(EXPLAIN_RESET),
                        members, members == 1 ? "" : "s", best_total);
        if (best_consequence) {
          ir_explain_emit("      %s%s consequence: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), best_consequence,
                          clr(EXPLAIN_RESET));
        }
        if (best_fix) {
          ir_explain_emit("      %s%s %s: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), best_advisory ? "note" : "fix",
                          best_fix, clr(EXPLAIN_RESET));
        }
        ir_explain_json_raw("%s{\"reason\":", json_group_count++ ? "," : "");
        ir_explain_json_str(best_reason);
        ir_explain_json_raw(",\"functions\":%zu,\"instructions\":%zu,"
                            "\"consequence\":",
                            members, best_total);
        ir_explain_json_str(best_consequence);
        ir_explain_json_raw(",\"fix\":");
        ir_explain_json_str(best_fix);
        if (best_advisory) {
          ir_explain_json_raw(",\"advisory\":true");
        }
        ir_explain_json_raw(",\"members\":[");
        size_t shown = 0;
        char list[512];
        size_t written = 0;
        list[0] = '\0';
        for (size_t j = best_first; j < g_backend_count && shown < 6; j++) {
          if (g_backend[j].ok || (grouped && grouped[j])) {
            continue;
          }
          char reason_j[256];
          const char *cj, *fj;
          int aj = 0;
          ir_explain_backend_reason(&g_backend[j], reason_j, sizeof(reason_j),
                                    &cj, &fj, &aj);
          if (strcmp(best_reason, reason_j) != 0) {
            continue;
          }
          /* Name the loop per member, not in the shared fix: one group can
           * hold functions whose kernels sit on different lines, and a single
           * line quoted above them would be wrong for all but one. */
          char lines[96];
          lines[0] = '\0';
          size_t kernel_line =
              ir_explain_backend_detail_is_kernel(g_backend[j].detail)
                  ? ir_explain_kernel_loop_lines(g_backend[j].function_name,
                                                 lines, sizeof(lines))
                  : 0;
          char where[112];
          where[0] = '\0';
          if (kernel_line) {
            snprintf(where, sizeof(where), ", %s", lines);
          }
          int n = snprintf(list + written, sizeof(list) - written,
                           "%s%s (%zu%s)", shown ? ", " : "",
                           g_backend[j].function_name,
                           g_backend[j].instructions, where);
          if (n < 0 || (size_t)n >= sizeof(list) - written) {
            break;
          }
          written += (size_t)n;
          ir_explain_json_raw("%s{\"fn\":", shown ? "," : "");
          ir_explain_json_str(g_backend[j].function_name);
          ir_explain_json_raw(",\"instructions\":%zu", g_backend[j].instructions);
          if (kernel_line) {
            ir_explain_json_raw(",\"kernelLine\":%zu", kernel_line);
          }
          ir_explain_json_raw("}");
          shown++;
        }
        ir_explain_json_raw("]}");
        ir_explain_emit("      %s%s %s%s%s%s\n", clr(EXPLAIN_DIM),
                        glyph_elbow(),
                        members > shown ? "largest: " : "", list,
                        members > shown ? " ..." : "", clr(EXPLAIN_RESET));
        if (members > shown) {
          ir_explain_emit("      %s%s   ... and %zu more%s\n",
                          clr(EXPLAIN_DIM), glyph_elbow(), members - shown,
                          clr(EXPLAIN_RESET));
        }
        for (size_t j = best_first; j < g_backend_count; j++) {
          if (g_backend[j].ok || (grouped && grouped[j])) {
            continue;
          }
          char reason_j[256];
          const char *cj, *fj;
          int aj = 0;
          ir_explain_backend_reason(&g_backend[j], reason_j, sizeof(reason_j),
                                    &cj, &fj, &aj);
          if (grouped && strcmp(best_reason, reason_j) == 0) {
            grouped[j] = 1;
          }
        }
        if (!grouped) {
          break; /* allocation failed: rendered the largest group, stop */
        }
      }
      free(grouped);
    }
    ir_explain_emit("\n");
  }

  ir_explain_json_raw("]},");

  /* Both need the backend decisions, so they land here rather than in the
   * optimization-stage flush. */
  ir_explain_functions_json();
  ir_explain_loop_costs_json();
  ir_explain_call_graph_json();
  ir_explain_hotspots_json();
  ir_explain_passes_json();

  /* Every section the plan ranks now exists. Render it before the tables it
   * reads are released. */
  ir_explain_splice_plan();

  for (size_t i = 0; i < g_loop_cost_count; i++) {
    free(g_loop_costs[i].function_name);
  }
  free(g_loop_costs);
  g_loop_costs = NULL;
  g_loop_cost_count = 0;
  g_loop_cost_capacity = 0;
  for (size_t i = 0; i < g_function_cost_count; i++) {
    free(g_function_costs[i].function_name);
  }
  free(g_function_costs);
  g_function_costs = NULL;
  g_function_cost_count = 0;
  g_function_cost_capacity = 0;

  for (size_t i = 0; i < g_function_count; i++) {
    free(g_functions[i].name);
  }
  free(g_functions);
  g_functions = NULL;
  g_function_count = 0;
  g_function_capacity = 0;

  for (size_t i = 0; i < g_backend_count; i++) {
    free(g_backend[i].function_name);
    free(g_backend[i].detail);
  }
  free(g_backend);
  g_backend = NULL;
  g_backend_count = 0;
  g_backend_capacity = 0;

  ir_explain_finalize(0);
}

void ir_explain_target_flush(const char *target_name) {
  if (!g_explain) {
    return;
  }

  const char *target = target_name && *target_name ? target_name : "non-native";
  g_digest.backend_ok = 0;
  g_digest.backend_total = 0;
  ir_explain_json_raw(
      "\"backend\":{\"ok\":0,\"total\":0,\"instructions\":0,"
      "\"okInstructions\":0,\"groups\":[],\"target\":");
  ir_explain_json_str(target);
  ir_explain_json_raw("},");

  ir_explain_print_header("backend report");
  ir_explain_emit(
      "  target-neutral optimized IR emitted through the %s backend; "
      "native MIR eligibility does not apply\n\n",
      target);
  ir_explain_finalize(0);
}

/* ---- hypothesis clone ------------------------------------------------------
 * A scratch deep copy of a function for simulating a suggested fix: the
 * caller mutates the clone, re-runs the vectorization stages on it, inspects
 * the result, and destroys it. Parameter names/types are copied because the
 * recognizers consult them (e.g. the uint8* gate on the byte-sum kernel). */

IRFunction *ir_explain_clone_function(const IRFunction *src) {
  if (!src) {
    return NULL;
  }
  IRFunction *clone = ir_function_create(src->name ? src->name : "?");
  if (!clone) {
    return NULL;
  }
  if (src->parameter_count > 0 &&
      !ir_function_set_parameters(clone,
                                  (const char **)src->parameter_names,
                                  (const char **)src->parameter_types,
                                  src->parameter_count)) {
    ir_function_destroy(clone);
    return NULL;
  }
  clone->is_inline = src->is_inline;
  clone->is_noinline = src->is_noinline;
  clone->is_pure = src->is_pure;
  clone->is_kernel = src->is_kernel;
  for (size_t i = 0; i < src->instruction_count; i++) {
    if (!ir_function_append_instruction(clone, &src->instructions[i])) {
      ir_function_destroy(clone);
      return NULL;
    }
  }
  return clone;
}

/* ---- kernel descriptions --------------------------------------------------
 * What a vectorized loop actually became, in instruction-level terms a
 * performance programmer recognizes. */

void ir_explain_kernel_desc(const IRInstruction *ins, char *buf, size_t cap) {
  if (!ins) {
    snprintf(buf, cap, "a SIMD kernel");
    return;
  }
  switch (ins->op) {
  case IR_OP_COUNT_WORD_STARTS:
    snprintf(buf, cap, "SSE2 word-start scan, 16 bytes/iteration");
    return;
  case IR_OP_MEMCPY_INLINE:
    snprintf(buf, cap, "inline memcpy (constant size)");
    return;
  case IR_OP_SIMD_SUM_I32:
    snprintf(buf, cap, "vpaddd, 8-wide int32 sum (AVX2)");
    return;
  case IR_OP_SIMD_SUM_U8:
    snprintf(buf, cap, "vpsadbw, 32-wide byte sum (AVX2)");
    return;
  case IR_OP_SIMD_BYTE_MAP:
    snprintf(buf, cap, "32-wide byte map (AVX2)");
    return;
  case IR_OP_SIMD_FILL:
    snprintf(buf, cap, "16-byte splat stores (vectorized fill/memset)");
    return;
  case IR_OP_SIMD_DOT_I32:
    snprintf(buf, cap, "vpmulld + vpaddd, 8-wide int32 dot product (AVX2)");
    return;
  case IR_OP_SIMD_DOT_I8:
    snprintf(buf, cap, "vpmaddwd, 16-wide int8 dot product (AVX2)");
    return;
  case IR_OP_SIMD_SLP_MAC_I32:
    snprintf(buf, cap, "SLP multiply-accumulate, %lld int32 lanes (AVX2)",
             ins->argument_count > 0 ? ins->arguments[0].int_value : 4LL);
    return;
  case IR_OP_SIMD_SLP_MAC_I8:
    snprintf(buf, cap, "SLP int8 multiply-accumulate tile (AVX2)");
    return;
  case IR_OP_SIMD_SCALE_I32:
    snprintf(buf, cap, "8-wide int32 scale map (AVX2)");
    return;
  case IR_OP_SIMD_CLAMP_I32:
    snprintf(buf, cap, "vpminsd/vpmaxsd, 8-wide int32 clamp (AVX2)");
    return;
  case IR_OP_SIMD_REVERSE_COPY_I32:
    snprintf(buf, cap, "8-wide int32 reverse copy (AVX2)");
    return;
  case IR_OP_LOWER_BOUND_I32:
    snprintf(buf, cap, "branchless lower-bound search");
    return;
  case IR_OP_PREFIX_SUM_I32:
    snprintf(buf, cap, "vectorized int32 prefix sum");
    return;
  case IR_OP_SIMD_MINMAX_I32:
    snprintf(buf, cap, "vpminsd/vpmaxsd, 8-wide int32 min/max scan (AVX2)");
    return;
  case IR_OP_SIMD_SUM_F64:
    snprintf(buf, cap, "vaddpd, 4-wide float64 sum, 2 accumulators (AVX)");
    return;
  case IR_OP_SIMD_SUM_F32:
    snprintf(buf, cap, "vaddps, 8-wide float32 sum, 2 accumulators (AVX)");
    return;
  case IR_OP_SIMD_DOT_F64:
    snprintf(buf, cap, "vfmadd231pd, 4-wide float64 FMA dot product");
    return;
  case IR_OP_SIMD_DOT_F32:
    snprintf(buf, cap, "vfmadd231ps, 8-wide float32 FMA dot product");
    return;
  case IR_OP_SIMD_AFFINE_MAP_F64:
    snprintf(buf, cap, "vfmadd231pd, 4-wide float64 affine map");
    return;
  case IR_OP_SIMD_AFFINE_MAP_F32:
    snprintf(buf, cap, "vfmadd231ps, 8-wide float32 affine map");
    return;
  case IR_OP_SIMD_EXP_F32:
    snprintf(buf, cap, "8-wide float32 exp (Cephes polynomial, AVX2)");
    return;
  case IR_OP_SIMD_SILU_F32:
    snprintf(buf, cap, "8-wide float32 SiLU/SwiGLU gate (AVX2 exp poly)");
    return;
  case IR_OP_SIMD_I2F_REDUCE_F64:
    snprintf(buf, cap, "4-wide float64 counter reduction (AVX2)");
    return;
  case IR_OP_SIMD_VLOOP_F64: {
    int f32 = ins->float_bits == 32;
    long long r = ins->argument_count > 0 ? ins->arguments[0].int_value : 0;
    snprintf(buf, cap, "%s-wide %s %s (AVX2 general vectorizer)",
             f32 ? "8" : "4", f32 ? "float32" : "float64",
             r == 1   ? "'+' reduction"
             : r == 2 ? (f32 ? "vmaxps max reduction" : "vmaxpd max reduction")
             : r == 3 ? (f32 ? "vminps min reduction" : "vminpd min reduction")
                      : "element-wise map");
    return;
  }
  case IR_OP_SIMD_VLOOP_I32: {
    long long r = ins->argument_count > 0 ? ins->arguments[0].int_value : 0;
    if (ins->float_bits == 8) {
      snprintf(buf, cap,
               "8-wide %s byte map widened to int32 lanes (AVX2 general "
               "vectorizer, bit-exact)",
               ins->is_unsigned ? "uint8" : "int8");
      return;
    }
    snprintf(buf, cap, "8-wide int32 %s (AVX2 general vectorizer, bit-exact)",
             r == 1   ? "'+' reduction"
             : r == 2 ? "vpmaxsd max reduction"
             : r == 3 ? "vpminsd min reduction"
                      : "element-wise map");
    return;
  }
  case IR_OP_SIMD_FIND: {
    int u8 = ins->argument_count > 1 && ins->arguments[1].int_value == 1;
    int pred = ins->argument_count > 0 ? (int)ins->arguments[0].int_value : -1;
    snprintf(buf, cap,
             "%s-wide %s search skip-ahead (%s; the scalar loop replays the "
             "stop iteration, exits exact)",
             pred == 6 ? "16" : (u8 ? "32" : "8"),
             pred == 6 ? "ASCII identifier" : (u8 ? "byte" : "int32"),
             pred == 6 ? "SSE4.2 ranges" : "AVX2 compare+movemask");
    return;
  }
  case IR_OP_SIMD_OUTER_LANE_F64:
    snprintf(buf, cap, "vdivpd, 4 outer iterations in 4-wide float64 lockstep "
                       "(hides the inner recurrence's latency)");
    return;
  case IR_OP_SIMD_MATMUL_N32:
    snprintf(buf, cap, "32x32 int32 matrix-multiply kernel");
    return;
  case IR_OP_SIMD_INSERTION_SORT_I32:
    snprintf(buf, cap, "accelerated int32 insertion sort");
    return;
  default:
    snprintf(buf, cap, "%s (SIMD kernel)", ir_opcode_name(ins->op));
    return;
  }
}

/* Render the --ml-opt report from the native pass's TSV (fn, gidx, kind, before,
 * after, saved, line, file), styled like the main report. */
static const char *glyph_ellipsis(void) {
  return ir_explain_use_unicode() ? "\xE2\x80\xA6" : "..";
}

static void ml_fit(const char *s, int width, char *out, size_t cap) {
  int len = (int)strlen(s);
  if (len <= width) {
    snprintf(out, cap, "%-*s", width, s);
    return;
  }
  int keep = width - 2;
  if (keep < 1) keep = 1;
  snprintf(out, cap, "%.*s%s", keep, s, glyph_ellipsis());
}

void ir_explain_ml_opt(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    return;
  }
  {
    char label[96];
    snprintf(label, sizeof(label), "%sml-opt: model-driven IR optimizations%s",
             clr(EXPLAIN_BOLD), clr(EXPLAIN_RESET));
    diag_style_output_begin();
    fputc(10, stderr);
    diag_rule(stderr, 0, label, "");
    fputc(10, stderr);
    diag_style_output_end();
  }

  char ln[1024];
  char cur_fn[256] = "";
  int total = 0, funcs = 0, saved_total = 0;
  int n_validated = 0, n_proven = 0, n_rejected = 0;
  while (fgets(ln, sizeof ln, f)) {
    char *fn = strtok(ln, "\t");
    char *gi = fn ? strtok(NULL, "\t") : NULL;
    char *kind = gi ? strtok(NULL, "\t") : NULL;
    char *before = kind ? strtok(NULL, "\t") : NULL;
    char *after = before ? strtok(NULL, "\t") : NULL;
    char *saved = after ? strtok(NULL, "\t") : NULL;
    char *line = saved ? strtok(NULL, "\t") : NULL;
    char *file = line ? strtok(NULL, "\t\n") : NULL;
    char *verdict = file ? strtok(NULL, "\t\n") : NULL;
    if (!after) {
      continue;
    }
    int rejected = verdict && strcmp(verdict, "rejected") == 0;
    int skipped = verdict && strcmp(verdict, "skipped") == 0;
    int proven = verdict && strcmp(verdict, "proven") == 0;
    if (skipped) {
      continue; /* declined applier or unverifiable speculative: never stood */
    }
    int sv = saved ? atoi(saved) : 0;
    long src_line = line ? atol(line) : 0;
    if (strcmp(fn, cur_fn) != 0) {
      snprintf(cur_fn, sizeof cur_fn, "%s", fn);
      funcs++;
      if (file && file[0]) {
        fprintf(stderr, "  %sfunction %s%s%s  %s%s%s\n", clr(EXPLAIN_DIM),
                clr(EXPLAIN_RESET), clr(EXPLAIN_BOLD), fn, clr(EXPLAIN_DIM),
                file, clr(EXPLAIN_RESET));
      } else {
        fprintf(stderr, "  %sfunction %s%s%s%s\n", clr(EXPLAIN_DIM),
                clr(EXPLAIN_RESET), clr(EXPLAIN_BOLD), fn, clr(EXPLAIN_RESET));
      }
    }
    char kbuf[28], bbuf[34], loc[24];
    ml_fit(kind, 22, kbuf, sizeof kbuf);
    ml_fit(before, 30, bbuf, sizeof bbuf);
    if (src_line > 0) snprintf(loc, sizeof loc, "line %ld", src_line);
    else snprintf(loc, sizeof loc, "ir#%s", gi ? gi : "?");
    const char *aft = (after[0] == '@') ? after + 1 : after;   /* drop sigil */
    fprintf(stderr, "    %s%s %-9s%s %s  %s  %s %s%s%s",
            clr(EXPLAIN_DIM), glyph_elbow(), loc, clr(EXPLAIN_RESET), kbuf, bbuf,
            glyph_arrow(), rejected ? clr(EXPLAIN_RED) : clr(EXPLAIN_GREEN), aft,
            clr(EXPLAIN_RESET));
    if (rejected) {
      fprintf(stderr, "  %sREJECTED: counterexample found%s", clr(EXPLAIN_RED),
              clr(EXPLAIN_RESET));
      n_rejected++;
    } else {
      if (sv > 0) {
        fprintf(stderr, "  %s-%d op%s%s", clr(EXPLAIN_DIM), sv,
                sv == 1 ? "" : "s", clr(EXPLAIN_RESET));
      }
      if (proven) {
        fprintf(stderr, "  %s(proven)%s", clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
        n_proven++;
      } else {
        n_validated++;
      }
      total++;
      saved_total += sv;
    }
    fprintf(stderr, "\n");
  }
  fclose(f);
  if (total || n_rejected) {
    const char *dot = ir_explain_use_unicode() ? "\xC2\xB7" : "*";
    fprintf(stderr,
            "\n  %s%d rewrite%s in %d function%s %s %d IR op%s removed %s ",
            clr(EXPLAIN_DIM), total, total == 1 ? "" : "s", funcs,
            funcs == 1 ? "" : "s", dot, saved_total,
            saved_total == 1 ? "" : "s", dot);
    if (n_proven == 0) {
      fprintf(stderr, "all validated equivalent by the interpreter");
    } else {
      fprintf(stderr, "%d validated equivalent, %d proven by construction",
              n_validated, n_proven);
    }
    if (n_rejected > 0) {
      fprintf(stderr, "%s %s%d proposal%s rejected with a counterexample",
              clr(EXPLAIN_RESET), clr(EXPLAIN_RED), n_rejected,
              n_rejected == 1 ? "" : "s");
    }
    fprintf(stderr, "%s\n", clr(EXPLAIN_RESET));
  }
}
