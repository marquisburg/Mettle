/* mettle test / mettle trace: compile-time execution DX surfaces.
 * See ir_comptime.h. */
#include "ir_comptime.h"
#include "ir_interp.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define ICT_ISATTY _isatty
#define ICT_FILENO _fileno
#else
#include <unistd.h>
#define ICT_ISATTY isatty
#define ICT_FILENO fileno
#endif

#define ICT_TEST_FUEL 20000000LL
#define ICT_TRACE_FUEL 2000000LL

static int ict_color(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *no_color = getenv("NO_COLOR");
    cached = (!no_color || !no_color[0]) && ICT_ISATTY(ICT_FILENO(stdout));
  }
  return cached;
}

/* Strip import-mangling prefixes: "__import_<hex>_name" -> "name". */
static const char *ict_display_name(const char *name) {
  if (!name) {
    return "?";
  }
  const char *p = name;
  while (strncmp(p, "__import_", 9) == 0) {
    const char *rest = p + 9;
    while (*rest && *rest != '_') {
      rest++;
    }
    if (*rest != '_') {
      break;
    }
    p = rest + 1;
  }
  return p;
}

static void ict_format_value(const IRInterpValue *value, char *buffer,
                             size_t capacity) {
  if (value->is_float) {
    snprintf(buffer, capacity, "%g", value->f);
  } else {
    snprintf(buffer, capacity, "%lld", value->i);
  }
}

/* ---------------- mettle test ---------------- */

typedef struct {
  int passed;
  int failed;
  int skipped;
  int leaked;
} ICTTestTotals;

static void ict_report_assert_failure(ErrorReporter *reporter,
                                      IRInterpMachine *machine,
                                      const char *test_name) {
  size_t line = 0, column = 0;
  IRInterpValue left = {0, 0, 0}, right = {0, 0, 0};
  int is_eq = 0;
  ir_interp_assert_info(machine, &line, &column, &left, &right, &is_eq);

  char message[192];
  snprintf(message, sizeof(message), "assertion failed in test '%s'",
           ict_display_name(test_name));
  /* Snap from the line start: the call location points at '(' which is
   * after the name. */
  SourceSpan span = source_span_create(line, 1, is_eq ? 9 : 6);
  span = error_reporter_span_snap_to_token(reporter, span,
                                           is_eq ? "assert_eq" : "assert");
  if (span.column == 1 && column > 0) {
    span.column = column;
  }
  fflush(stdout);
  error_reporter_add_error_with_span(reporter, ERROR_SEMANTIC, span, message);
  char label[128];
  if (is_eq) {
    char a[48], b[48];
    ict_format_value(&left, a, sizeof(a));
    ict_format_value(&right, b, sizeof(b));
    snprintf(label, sizeof(label), "left: %s, right: %s", a, b);
  } else {
    snprintf(label, sizeof(label), "condition was false");
  }
  error_reporter_set_last_label(reporter, label);
  if (reporter->count > 0) {
    error_reporter_print_error(reporter, &reporter->errors[reporter->count - 1]);
  }
  fflush(stderr);
}

static void ict_report_leaks(ErrorReporter *reporter,
                             IRInterpMachine *machine, const char *test_name,
                             int *leaked_total) {
  size_t buffers = ir_interp_buffer_count(machine);
  for (size_t i = 0; i < buffers; i++) {
    size_t line = ir_interp_buffer_alloc_line(machine, i);
    if (line == 0 || ir_interp_buffer_freed(machine, i)) {
      continue;
    }
    long long size = 0;
    ir_interp_buffer_data(machine, i, &size);
    fflush(stdout);
    char message[192];
    snprintf(message, sizeof(message),
             "test '%s' leaked %lld bytes: this allocation is never freed",
             ict_display_name(test_name), size);
    SourceLocation loc = source_location_create(line, 1);
    error_reporter_add_warning_with_suggestion(
        reporter, ERROR_SEMANTIC, loc, message,
        "free it before the test returns, or `defer free(...)` right after "
        "the allocation");
    if (reporter->count > 0) {
      error_reporter_print_error(reporter,
                                 &reporter->errors[reporter->count - 1]);
    }
    fflush(stderr);
    (*leaked_total)++;
  }
}

int ir_comptime_run_tests(IRProgram *program, ErrorReporter *reporter,
                          const char *filename, const char *filter) {
  if (!program || !reporter) {
    return 1;
  }
  const char *green = ict_color() ? "\x1b[32m\x1b[1m" : "";
  const char *red = ict_color() ? "\x1b[31m\x1b[1m" : "";
  const char *yellow = ict_color() ? "\x1b[33m\x1b[1m" : "";
  const char *dim = ict_color() ? "\x1b[2m" : "";
  const char *reset = ict_color() ? "\x1b[0m" : "";

  /* Surface any warnings the front end accumulated (unused vars etc.). */
  if (reporter->count > 0) {
    error_reporter_print_errors(reporter);
  }
  fflush(stderr);

  ICTTestTotals totals = {0, 0, 0, 0};
  size_t discovered = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn || !fn->is_test) {
      continue;
    }
    if (filter && (!fn->name || !strstr(fn->name, filter))) {
      continue;
    }
    discovered++;
  }
  printf("\nrunning %zu test%s (compile-time interpreter, no codegen)\n",
         discovered, discovered == 1 ? "" : "s");

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn || !fn->is_test) {
      continue;
    }
    if (filter && (!fn->name || !strstr(fn->name, filter))) {
      continue;
    }
    const char *display = ict_display_name(fn->name);

    if (fn->parameter_count > 0) {
      printf("test %s ... %sFAILED%s\n", display, red, reset);
      fprintf(stderr,
              "error: @test function '%s' takes %zu parameters; tests must "
              "take none\n",
              display, fn->parameter_count);
      totals.failed++;
      continue;
    }

    IRInterpMachine *machine = ir_interp_create(program);
    if (!machine) {
      totals.failed++;
      continue;
    }
    IRInterpValue result = {0, 0, 0};
    IRInterpStatus status =
        ir_interp_run(machine, fn, NULL, 0, &result, ICT_TEST_FUEL);

    switch (status) {
    case IR_INTERP_OK: {
      int leaks_before = totals.leaked;
      ict_report_leaks(reporter, machine, fn->name, &totals.leaked);
      int leaked_here = totals.leaked > leaks_before;
      if (!result.is_float && result.i != 0) {
        printf("test %s ... %sFAILED%s (returned %lld; a test must return 0)\n",
               display, red, reset, result.i);
        totals.failed++;
      } else if (leaked_here) {
        printf("test %s ... %sok, but LEAKED%s\n", display, yellow, reset);
        totals.passed++;
      } else {
        printf("test %s ... %sok%s\n", display, green, reset);
        totals.passed++;
      }
      break;
    }
    case IR_INTERP_ASSERT_FAIL:
      printf("test %s ... %sFAILED%s\n", display, red, reset);
      ict_report_assert_failure(reporter, machine, fn->name);
      totals.failed++;
      break;
    case IR_INTERP_GUARD_TRAP: {
      const char *detail = ir_interp_status_detail(machine);
      if (detail && strncmp(detail, "Fatal error", 11) == 0) {
        printf("test %s ... %sFAILED%s (crashed: %s)\n", display, red, reset,
               detail);
      } else {
        printf("test %s ... %sFAILED%s (crashed: runtime guard trap - null "
               "dereference or out-of-bounds)\n",
               display, red, reset);
      }
      totals.failed++;
      break;
    }
    case IR_INTERP_TRAP:
      printf("test %s ... %sFAILED%s (trapped: %s)\n", display, red, reset,
             ir_interp_status_detail(machine));
      totals.failed++;
      break;
    case IR_INTERP_FUEL:
      printf("test %s ... %sFAILED%s (did not finish within %lld steps - "
             "infinite loop?)\n",
             display, red, reset, ICT_TEST_FUEL);
      totals.failed++;
      break;
    default:
      printf("test %s ... %sskipped%s (uses '%s'; not interpretable - run it "
             "natively via a main() + --build)\n",
             display, dim, reset, ir_interp_status_detail(machine));
      totals.skipped++;
      break;
    }
    ir_interp_destroy(machine);
  }

  printf("\n%s%d passed%s", totals.passed > 0 ? green : "", totals.passed,
         reset);
  if (totals.failed > 0) {
    printf(", %s%d failed%s", red, totals.failed, reset);
  }
  if (totals.leaked > 0) {
    printf(", %s%d leak%s%s", yellow, totals.leaked,
           totals.leaked == 1 ? "" : "s", reset);
  }
  if (totals.skipped > 0) {
    printf(", %d skipped", totals.skipped);
  }
  printf(" %s(%s)%s\n", dim, filename ? filename : "?", reset);

  if (discovered == 0) {
    printf("note: no @test functions found%s%s%s; declare one:\n"
           "    @test fn my_first_test() -> int64 {\n"
           "        assert_eq(2 + 2, 4);\n"
           "        return 0;\n"
           "    }\n",
           filter ? " matching '" : "", filter ? filter : "",
           filter ? "'" : "");
  }

  return totals.failed > 0 ? 1 : 0;
}

/* ---------------- mettle trace ---------------- */

#define ICT_TRACE_MAX_ENTRIES 256
#define ICT_TRACE_SAMPLES 4

typedef struct {
  size_t line;
  const char *name;
  /* The `comptime for` iteration that generated the instruction, or NULL for
   * written code. Part of the entry's identity: without it every expansion of
   * one written line merges into a single run of values with no way to tell
   * which iteration produced which. */
  const char *expansion_note;
  long long count;
  char samples[ICT_TRACE_SAMPLES][40];
  char last[40];
} ICTTraceEntry;

typedef struct {
  ICTTraceEntry entries[ICT_TRACE_MAX_ENTRIES];
  size_t count;
  size_t min_line;
  size_t max_line;
} ICTTraceLog;

static void ict_trace_hook(void *ctx, size_t line, const char *name,
                           IRInterpValue value, const char *expansion_note) {
  ICTTraceLog *log = (ICTTraceLog *)ctx;
  /* Compiler temps and synthesized locals are noise; show user symbols. */
  if (!name || name[0] == '.' || name[0] == '_') {
    return;
  }
  ICTTraceEntry *entry = NULL;
  for (size_t i = 0; i < log->count; i++) {
    if (log->entries[i].line == line &&
        strcmp(log->entries[i].name, name) == 0 &&
        log->entries[i].expansion_note == expansion_note) {
      entry = &log->entries[i];
      break;
    }
  }
  if (!entry) {
    if (log->count >= ICT_TRACE_MAX_ENTRIES) {
      return;
    }
    entry = &log->entries[log->count++];
    memset(entry, 0, sizeof(*entry));
    entry->line = line;
    entry->name = name;
    entry->expansion_note = expansion_note;
  }
  char text[40];
  ict_format_value(&value, text, sizeof(text));
  if (entry->count < ICT_TRACE_SAMPLES) {
    snprintf(entry->samples[entry->count], sizeof(entry->samples[0]), "%s",
             text);
  }
  snprintf(entry->last, sizeof(entry->last), "%s", text);
  entry->count++;
  if (line < log->min_line || log->min_line == 0) {
    log->min_line = line;
  }
  if (line > log->max_line) {
    log->max_line = line;
  }
}

/* Size of a by-value aggregate parameter (struct/array/tagged enum), from the
 * module symbol table; 0 for scalars, pointers, and strings. */
static long long ict_param_aggregate_size(IRProgram *program,
                                          const char *function_name,
                                          size_t index) {
  const IRModuleSymbol *sym =
      ir_program_lookup_symbol(program, function_name);
  if (!sym || !sym->param_types || index >= sym->param_count) {
    return 0;
  }
  const MtlcType *pt = sym->param_types[index];
  if (!pt || pt->size == 0 ||
      (pt->kind != MTLC_TYPE_STRUCT && pt->kind != MTLC_TYPE_ARRAY &&
       pt->kind != MTLC_TYPE_TAGGED_ENUM)) {
    return 0;
  }
  return (long long)pt->size;
}

static IRFunction *ict_find_function(IRProgram *program, const char *name) {
  size_t name_len = strlen(name);
  IRFunction *suffix_match = NULL;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (!fn || !fn->name) {
      continue;
    }
    if (strcmp(fn->name, name) == 0) {
      return fn;
    }
    size_t fn_len = strlen(fn->name);
    if (fn_len > name_len + 1 &&
        strcmp(fn->name + fn_len - name_len, name) == 0 &&
        fn->name[fn_len - name_len - 1] == '_') {
      suffix_match = fn;
    }
  }
  return suffix_match;
}

static int ict_extern_returns_void(const IRProgram *program,
                                   const char *link_name) {
  if (!program || !link_name) {
    return 0;
  }
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *sym = &program->module_symbols[i];
    const char *linked =
        sym->link_name && sym->link_name[0] ? sym->link_name : sym->name;
    if (!linked || strcmp(linked, link_name) != 0) {
      continue;
    }
    if (sym->return_type && sym->return_type->kind == MTLC_TYPE_VOID) {
      return 1;
    }
  }
  return 0;
}

int ir_comptime_trace(IRProgram *program, ErrorReporter *reporter,
                      const char *filename, const char *source,
                      const char *function_name, const char *const *args,
                      size_t arg_count) {
  (void)reporter;
  if (!program || !function_name) {
    return 1;
  }
  const char *bold = ict_color() ? "\x1b[1m" : "";
  const char *cyan = ict_color() ? "\x1b[36m" : "";
  const char *dim = ict_color() ? "\x1b[2m" : "";
  const char *reset = ict_color() ? "\x1b[0m" : "";

  IRFunction *fn = ict_find_function(program, function_name);
  if (!fn) {
    fprintf(stderr, "error: no function named '%s' in %s\n", function_name,
            filename ? filename : "?");
    return 1;
  }

  /* Build arguments: CLI values fill int/float parameters in order; pointer
   * parameters get a synthesized 33-element seeded buffer. */
  IRInterpMachine *machine = ir_interp_create(program);
  if (!machine) {
    return 1;
  }
  IRInterpValue call_args[16] = {{0}};
  char arg_display[16][48];
  size_t cli_index = 0;
  if (fn->parameter_count > 16) {
    fprintf(stderr, "error: too many parameters to trace\n");
    ir_interp_destroy(machine);
    return 1;
  }
  for (size_t p = 0; p < fn->parameter_count; p++) {
    const char *type =
        fn->parameter_types && fn->parameter_types[p] ? fn->parameter_types[p]
                                                      : "int64";
    size_t type_len = strlen(type);
    if (type_len > 0 && type[type_len - 1] == '*') {
      long long elems = 33;
      long long elem_size = 8;
      {
        static const struct { const char *name; int size; } ELEMS[] = {
            {"int8*", 1},  {"uint8*", 1},  {"bool*", 1},
            {"int16*", 2}, {"uint16*", 2},
            {"int32*", 4}, {"uint32*", 4}, {"float32*", 4},
        };
        for (size_t e = 0; e < sizeof(ELEMS) / sizeof(ELEMS[0]); e++) {
          if (strcmp(type, ELEMS[e].name) == 0) {
            elem_size = ELEMS[e].size;
            break;
          }
        }
      }
      long long bytes = elems * elem_size;
      unsigned char *init = (unsigned char *)malloc((size_t)bytes);
      if (!init) {
        ir_interp_destroy(machine);
        return 1;
      }
      unsigned int seed = 0x2545F491u + (unsigned int)p;
      if (strncmp(type, "float", 5) == 0) {
        for (long long e = 0; e < elems; e++) {
          seed = seed * 1664525u + 1013904223u;
          double v = (double)(int)(seed % 19) - 9.0;
          if (elem_size == 4) {
            float fv = (float)v;
            memcpy(init + e * 4, &fv, 4);
          } else {
            memcpy(init + e * 8, &v, 8);
          }
        }
      } else {
        for (long long b = 0; b < bytes; b++) {
          seed = seed * 1664525u + 1013904223u;
          init[b] = (unsigned char)((seed >> 16) % 10);
        }
      }
      unsigned long long addr = ir_interp_add_buffer(machine, init, bytes);
      free(init);
      call_args[p].i = (long long)addr;
      call_args[p].f = 0;
      call_args[p].is_float = 0;
      snprintf(arg_display[p], sizeof(arg_display[0]), "<buf:%lld x %.*s>",
               elems, (int)(type_len - 1), type);
    } else if (ict_param_aggregate_size(program, function_name, p) > 0) {
      /* By-value aggregate parameter: synthesize zeroed storage of the
       * type's size; the value convention passes its address. */
      long long bytes = ict_param_aggregate_size(program, function_name, p);
      unsigned long long addr = ir_interp_add_buffer(machine, NULL, bytes);
      call_args[p].i = (long long)addr;
      call_args[p].f = 0;
      call_args[p].is_float = 0;
      snprintf(arg_display[p], sizeof(arg_display[0]), "<%s: %lld zero bytes>",
               type, bytes);
    } else {
      if (cli_index >= arg_count) {
        fprintf(stderr,
                "error: '%s' needs a value for parameter %zu ('%s': %s)\n"
                "usage: mettle trace %s %s <args...>\n",
                function_name, p,
                fn->parameter_names && fn->parameter_names[p]
                    ? fn->parameter_names[p]
                    : "?",
                type, filename ? filename : "<file>", function_name);
        ir_interp_destroy(machine);
        return 1;
      }
      const char *text = args[cli_index++];
      if (strchr(text, '.') || strchr(text, 'e') || strchr(text, 'E')) {
        call_args[p].f = strtod(text, NULL);
        call_args[p].i = 0;
        call_args[p].is_float = 1;
      } else {
        call_args[p].i = strtoll(text, NULL, 0);
        call_args[p].f = 0;
        call_args[p].is_float = 0;
      }
      snprintf(arg_display[p], sizeof(arg_display[0]), "%s", text);
    }
  }

  ICTTraceLog log;
  memset(&log, 0, sizeof(log));
  ir_interp_set_value_hook(machine, ict_trace_hook, &log, fn);

  IRInterpValue result = {0, 0, 0};
  IRInterpStatus status =
      ir_interp_run(machine, fn, call_args, fn->parameter_count, &result,
                    ICT_TRACE_FUEL);

  printf("%strace%s: %s(", bold, reset, ict_display_name(fn->name));
  for (size_t p = 0; p < fn->parameter_count; p++) {
    printf("%s%s=%s", p == 0 ? "" : ", ",
           fn->parameter_names && fn->parameter_names[p]
               ? fn->parameter_names[p]
               : "?",
           arg_display[p]);
  }
  printf(")\n\n");

  /* Widen the printed range to the function's full extent when known. */
  for (size_t i = 0; i < fn->instruction_count; i++) {
    size_t line = fn->instructions[i].location.line;
    if (line == 0) {
      continue;
    }
    if (line < log.min_line || log.min_line == 0) {
      log.min_line = line;
    }
    if (line > log.max_line) {
      log.max_line = line;
    }
  }

  if (source && log.min_line > 0) {
    for (size_t line = log.min_line; line <= log.max_line; line++) {
      char *text = error_reporter_get_line_from_source(source, line);
      printf("%s%4zu |%s %-52s", dim, line, reset, text ? text : "");
      free(text);
      int printed_any = 0;
      for (size_t i = 0; i < log.count; i++) {
        const ICTTraceEntry *entry = &log.entries[i];
        if (entry->line != line) {
          continue;
        }
        printf("%s%s ", cyan, printed_any ? ";" : " <-");
        /* The diagnostic's wording is "expanded from comptime-for iteration 1
         * (field `kind`)". Inline against source that is too long, so show
         * the parenthesized subject, which is what distinguishes one
         * iteration from the next. */
        if (entry->expansion_note) {
          const char *subject = strchr(entry->expansion_note, '(');
          if (subject) {
            printf("%s ", subject);
          } else {
            printf("[%s] ", entry->expansion_note);
          }
        }
        printf("%s = ", entry->name);
        long long shown = entry->count < ICT_TRACE_SAMPLES
                              ? entry->count
                              : ICT_TRACE_SAMPLES;
        for (long long s = 0; s < shown; s++) {
          printf("%s%s", s == 0 ? "" : ", ", entry->samples[s]);
        }
        if (entry->count > ICT_TRACE_SAMPLES) {
          printf(", ..., %s", entry->last);
        }
        if (entry->count > 1) {
          printf(" (%lldx)", entry->count);
        }
        printf("%s", reset);
        printed_any = 1;
      }
      printf("\n");
    }
  }

  printf("\n");
  {
    size_t traced = ir_interp_extern_trace_count(machine);
    size_t named = 0;
    for (size_t t = 0; t < traced; t++) {
      const IRInterpExternCall *call = ir_interp_extern_trace(machine, t);
      int seen = 0;
      if (!call) {
        continue;
      }
      for (size_t earlier = 0; earlier < t && !seen; earlier++) {
        const IRInterpExternCall *prev =
            ir_interp_extern_trace(machine, earlier);
        seen = prev && strcmp(prev->name, call->name) == 0;
      }
      if (seen || call->modelled ||
          ict_extern_returns_void(program, call->name)) {
        continue;
      }
      if (named == 0) {
        printf("%snote%s: no model for %s", dim, reset, call->name);
      } else {
        printf(", %s", call->name);
      }
      named++;
    }
    if (named > 0) {
      printf(" -- each answered 0, so the values below may differ from a"
             " real run\n\n");
    }
  }
  switch (status) {
  case IR_INTERP_OK: {
    char value[48];
    ict_format_value(&result, value, sizeof(value));
    if (result.undefined || ir_interp_branched_on_undefined(machine)) {
      printf("%sno answer%s: the result depends on something this run could"
             " not know -- memory nothing had written, or a call with no"
             " model above. It computed %s; a real run would compute"
             " something else\n",
             bold, reset, value);
      ir_interp_destroy(machine);
      return 0;
    }
    printf("%sreturns %s%s\n", bold, value, reset);
    ir_interp_destroy(machine);
    return 0;
  }
  case IR_INTERP_GUARD_TRAP: {
    const char *detail = ir_interp_status_detail(machine);
    if (detail && strncmp(detail, "Fatal error", 11) == 0) {
      printf("%scrashed%s: %s\n", bold, reset, detail);
    } else {
      printf("%scrashed%s: runtime guard trap (null dereference or "
             "out-of-bounds)\n",
             bold, reset);
    }
    break;
  }
  case IR_INTERP_FUEL:
    printf("%sstopped%s: exceeded %lld steps (infinite loop?)\n", bold, reset,
           ICT_TRACE_FUEL);
    break;
  case IR_INTERP_UNSUPPORTED:
    printf("%snot traceable%s: uses '%s' (outside the interpretable subset)\n",
           bold, reset, ir_interp_status_detail(machine));
    break;
  default:
    printf("%strapped%s: %s\n", bold, reset,
           ir_interp_status_detail(machine));
    break;
  }
  ir_interp_destroy(machine);
  return 1;
}
