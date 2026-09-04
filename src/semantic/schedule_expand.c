#include "schedule_expand.h"
#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHEDULE_MAX_PHASES 64

typedef struct {
  const char *phase;
  const char *effect;
  const char *entry;
  long long thread;
  int barrier;
  SourceLocation location;
} SchedulePhase;

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} SourceBuffer;

static int buffer_add(SourceBuffer *buffer, const char *text) {
  size_t added = strlen(text);
  if (buffer->length + added + 1 > buffer->capacity) {
    size_t grown = buffer->capacity ? buffer->capacity : 512;
    char *data = NULL;
    while (grown < buffer->length + added + 1) {
      grown *= 2;
    }
    data = realloc(buffer->data, grown);
    if (!data) {
      return 0;
    }
    buffer->data = data;
    buffer->capacity = grown;
  }
  memcpy(buffer->data + buffer->length, text, added + 1);
  buffer->length += added;
  return 1;
}

static int buffer_add_index(SourceBuffer *buffer, long long value) {
  char text[32];
  snprintf(text, sizeof(text), "%lld", value);
  return buffer_add(buffer, text);
}

static void schedule_error(ErrorReporter *reporter, SourceLocation location,
                           const char *code, const char *suggestion,
                           const char *format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (!reporter) {
    fprintf(stderr, "error[%s]: %s\n", code, message);
    return;
  }
  error_reporter_add_error_with_suggestion(reporter, ERROR_SEMANTIC, location,
                                           message, suggestion);
  error_reporter_set_last_code(reporter, code);
}

static const char *literal_string(ASTNode *node) {
  StringLiteral *literal = NULL;
  if (!node || node->type != AST_STRING_LITERAL) {
    return NULL;
  }
  literal = (StringLiteral *)node->data;
  return literal ? literal->value : NULL;
}

static int literal_bool(ASTNode *node, int *out) {
  long long number = 0;
  const char *name = NULL;
  if (node && node->type == AST_NUMBER_LITERAL && node->data &&
      !((const NumberLiteral *)node->data)->is_float) {
    *out = ((const NumberLiteral *)node->data)->int_value != 0;
    return 1;
  }
  if (!node || node->type != AST_IDENTIFIER || !node->data) {
    return 0;
  }
  name = ((const Identifier *)node->data)->name;
  if (name && strcmp(name, "true") == 0) {
    *out = 1;
    return 1;
  }
  if (name && strcmp(name, "false") == 0) {
    *out = 0;
    return 1;
  }
  (void)number;
  return 0;
}

static int literal_integer(ASTNode *node, long long *out) {
  NumberLiteral *literal = NULL;
  if (!node || node->type != AST_NUMBER_LITERAL) {
    return 0;
  }
  literal = (NumberLiteral *)node->data;
  if (!literal || literal->is_float) {
    return 0;
  }
  *out = literal->int_value;
  return 1;
}

static const char *base_type_name(const char *type_name, char *storage,
                                  size_t capacity) {
  const char *bracket = NULL;
  size_t length = 0;
  if (!type_name) {
    return NULL;
  }
  bracket = strchr(type_name, '[');
  length = bracket ? (size_t)(bracket - type_name) : strlen(type_name);
  if (length == 0 || length + 1 > capacity) {
    return NULL;
  }
  memcpy(storage, type_name, length);
  storage[length] = '\0';
  return storage;
}

static int names_schedule(const char *type_name) {
  char storage[128];
  const char *base = base_type_name(type_name, storage, sizeof(storage));
  size_t length = 0;
  if (!base) {
    return 0;
  }
  if (strcmp(base, "Schedule") == 0) {
    return 1;
  }
  length = strlen(base);
  return length > 9 && strcmp(base + length - 9, "_Schedule") == 0;
}

static int module_declares_effect(const Program *program, const char *name) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *declaration = program->declarations[i];
    EffectDeclaration *effect = NULL;
    if (!declaration || declaration->type != AST_EFFECT_DECLARATION) {
      continue;
    }
    effect = (EffectDeclaration *)declaration->data;
    if (effect && effect->name && strcmp(effect->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int module_declares_function(const Program *program, const char *name) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *declaration = program->declarations[i];
    FunctionDeclaration *function = NULL;
    if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    function = (FunctionDeclaration *)declaration->data;
    if (function && function->name && strcmp(function->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int read_phase(ErrorReporter *reporter, ASTNode *row,
                      SchedulePhase *out) {
  AggregateLiteral *literal =
      row && row->type == AST_AGGREGATE_LITERAL ? (AggregateLiteral *)row->data
                                                : NULL;
  memset(out, 0, sizeof(*out));
  out->thread = -1;
  out->location = row ? row->location : (SourceLocation){0};
  if (!literal || !literal->is_struct || !literal->field_names) {
    schedule_error(reporter, out->location, "H0001",
                   "write each phase as `{ phase: \"name\", effect: \"Effect\", "
                   "entry: \"fn_name\", thread: 0 }`",
                   "a schedule's rows are phases, and this row is not written "
                   "as one");
    return 0;
  }
  for (size_t i = 0; i < literal->element_count; i++) {
    const char *field = literal->field_names[i];
    ASTNode *value = literal->elements[i];
    if (!field) {
      continue;
    }
    if (strcmp(field, "phase") == 0) {
      out->phase = literal_string(value);
    } else if (strcmp(field, "effect") == 0) {
      out->effect = literal_string(value);
    } else if (strcmp(field, "entry") == 0) {
      out->entry = literal_string(value);
    } else if (strcmp(field, "thread") == 0) {
      literal_integer(value, &out->thread);
    } else if (strcmp(field, "joins") == 0) {
      if (!literal_bool(value, &out->barrier)) {
        schedule_error(reporter, out->location, "H0001",
                       "a joining phase is written `joins: true`",
                       "the 'joins' of a phase is not true or false");
        return 0;
      }
    }
  }
  if (!out->phase || !*out->phase || !out->effect || !*out->effect ||
      !out->entry || !*out->entry) {
    schedule_error(reporter, out->location, "H0001",
                   "every phase names itself, the effect that holds while it "
                   "runs, and the function it runs",
                   "a phase needs `phase`, `effect` and `entry`, and this one "
                   "leaves one of them empty");
    return 0;
  }
  if (out->thread < 0) {
    schedule_error(reporter, out->location, "H0001",
                   "number the threads from zero",
                   "phase '%s' runs on thread %lld, and a thread is numbered "
                   "from zero",
                   out->phase, out->thread);
    return 0;
  }
  return 1;
}

static int check_phases(ErrorReporter *reporter, const Program *program,
                        const SchedulePhase *phases, size_t count,
                        const char *schedule) {
  int ok = 1;
  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < i; j++) {
      if (strcmp(phases[i].phase, phases[j].phase) == 0) {
        schedule_error(reporter, phases[i].location, "H0002",
                       "give each phase its own name",
                       "'%s' names the phase '%s' twice", schedule,
                       phases[i].phase);
        ok = 0;
      }
      if (strcmp(phases[i].effect, phases[j].effect) == 0) {
        schedule_error(reporter, phases[i].location, "H0002",
                       "one effect holds in one phase, which is what makes a "
                       "call across a phase boundary something the compiler "
                       "can refuse",
                       "'%s' gives the phases '%s' and '%s' the same effect "
                       "'%s'",
                       schedule, phases[j].phase, phases[i].phase,
                       phases[i].effect);
        ok = 0;
      }
    }
    if (!module_declares_effect(program, phases[i].effect)) {
      schedule_error(reporter, phases[i].location, "H0003",
                     "declare it beside the schedule: `effect <name>;`",
                     "phase '%s' runs under the effect '%s', and nothing "
                     "declares it",
                     phases[i].phase, phases[i].effect);
      ok = 0;
    }
    if (phases[i].barrier &&
        !module_declares_function(program, "atomic_inc_i32")) {
      schedule_error(reporter, phases[i].location, "H0007",
                     "add `import \"std/thread\";` beside the schedule",
                     "phase '%s' ends where the threads join, and a join is built "
                     "out of std/thread's atomics, which this module does not "
                     "import",
                     phases[i].phase);
      ok = 0;
    }
    if (!module_declares_function(program, phases[i].entry)) {
      schedule_error(reporter, phases[i].location, "H0004",
                     "the entry is a function of this module, named as it was "
                     "written",
                     "phase '%s' runs '%s', and this module declares no such "
                     "function",
                     phases[i].phase, phases[i].entry);
      ok = 0;
    }
  }
  return ok;
}

/* A phase that ends at a barrier gets a counter and a wait. Every thread
 * calls the wait, whether or not it runs that phase, because a barrier the
 * threads that skip the phase walk past is not one. The counter only ever
 * rises: a thread arriving for frame g leaves the count at g times the number
 * of threads, so there is no reset to race over and no sense bit to flip. */
static int build_barrier(SourceBuffer *buffer, const char *schedule,
                         const SchedulePhase *phase, long long threads,
                         size_t *generated) {
  if (!buffer_add(buffer, "var ") || !buffer_add(buffer, schedule) ||
      !buffer_add(buffer, "_arrived_") || !buffer_add(buffer, phase->phase) ||
      !buffer_add(buffer, ": volatile int32 = 0;\n")) {
    return 0;
  }
  if (!buffer_add(buffer, "fn ") || !buffer_add(buffer, schedule) ||
      !buffer_add(buffer, "_wait_") || !buffer_add(buffer, phase->phase) ||
      !buffer_add(buffer, "(generation: int32) {\n  atomic_inc_i32(&") ||
      !buffer_add(buffer, schedule) || !buffer_add(buffer, "_arrived_") ||
      !buffer_add(buffer, phase->phase) ||
      !buffer_add(buffer, ");\n  while (") || !buffer_add(buffer, schedule) ||
      !buffer_add(buffer, "_arrived_") || !buffer_add(buffer, phase->phase) ||
      !buffer_add(buffer, " < generation * ") ||
      !buffer_add_index(buffer, threads) ||
      !buffer_add(buffer, ") { }\n}\n")) {
    return 0;
  }
  (*generated)++;
  return 1;
}

static int build_source(SourceBuffer *buffer, const char *schedule,
                        const SchedulePhase *phases, size_t count,
                        long long threads, size_t *generated) {
  for (size_t i = 0; i < count; i++) {
    if (!buffer_add(buffer, "fn ") || !buffer_add(buffer, schedule) ||
        !buffer_add(buffer, "_phase_") || !buffer_add(buffer, phases[i].phase) ||
        !buffer_add(buffer, "() provides ") ||
        !buffer_add(buffer, phases[i].effect) || !buffer_add(buffer, " { ") ||
        !buffer_add(buffer, phases[i].entry) || !buffer_add(buffer, "(); }\n")) {
      return 0;
    }
    (*generated)++;
    if (phases[i].barrier &&
        !build_barrier(buffer, schedule, &phases[i], threads, generated)) {
      return 0;
    }
  }
  for (long long thread = 0; thread < threads; thread++) {
    if (!buffer_add(buffer, "fn ") || !buffer_add(buffer, schedule) ||
        !buffer_add(buffer, "_thread_") || !buffer_add_index(buffer, thread) ||
        !buffer_add(buffer, "(arg: cstring) -> uint32 {\n") ||
        !buffer_add(buffer, "  var frames: int32 = (int32)(int64)arg;\n") ||
        !buffer_add(buffer, "  var frame: int32 = 0;\n") ||
        !buffer_add(buffer, "  while (frame < frames) {\n")) {
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      if (phases[i].thread == thread) {
        if (!buffer_add(buffer, "    ") || !buffer_add(buffer, schedule) ||
            !buffer_add(buffer, "_phase_") ||
            !buffer_add(buffer, phases[i].phase) ||
            !buffer_add(buffer, "();\n    quiesce;\n")) {
          return 0;
        }
      }
      if (phases[i].barrier) {
        if (!buffer_add(buffer, "    ") || !buffer_add(buffer, schedule) ||
            !buffer_add(buffer, "_wait_") ||
            !buffer_add(buffer, phases[i].phase) ||
            !buffer_add(buffer, "(frame + 1);\n")) {
          return 0;
        }
      }
    }
    if (!buffer_add(buffer, "    frame = frame + 1;\n  }\n  return 0;\n}\n")) {
      return 0;
    }
    (*generated)++;
  }
  return 1;
}

static void retarget(ASTNode *node, SourceLocation location) {
  if (!node) {
    return;
  }
  node->location = location;
  for (size_t i = 0; i < node->child_count; i++) {
    retarget(node->children[i], location);
  }
}

static int append_declarations(ASTNode *program_node, ASTNode *generated,
                               const SchedulePhase *phases, size_t count,
                               ASTNode *anchor) {
  Program *program = (Program *)program_node->data;
  Program *source = generated ? (Program *)generated->data : NULL;
  ASTNode **grown = NULL;
  ASTNode **children = NULL;
  size_t total = 0;
  if (!source || source->declaration_count == 0) {
    return 1;
  }
  total = program->declaration_count + source->declaration_count;
  grown = realloc(program->declarations, total * sizeof(ASTNode *));
  if (!grown) {
    return 0;
  }
  program->declarations = grown;
  children = realloc(program_node->children, total * sizeof(ASTNode *));
  if (!children) {
    return 0;
  }
  program_node->children = children;
  for (size_t i = 0; i < source->declaration_count; i++) {
    ASTNode *declaration = source->declarations[i];
    retarget(declaration,
             i < count ? phases[i].location : anchor->location);
    program->declarations[program->declaration_count++] = declaration;
  }
  memcpy(program_node->children, program->declarations,
         program->declaration_count * sizeof(ASTNode *));
  program_node->child_count = program->declaration_count;
  source->declaration_count = 0;
  generated->child_count = 0;
  return 1;
}

static int expand_one(ASTNode *program_node, ASTNode *declaration,
                      ErrorReporter *reporter, ScheduleStats *stats) {
  Program *program = (Program *)program_node->data;
  VarDeclaration *variable = (VarDeclaration *)declaration->data;
  AggregateLiteral *rows = NULL;
  SchedulePhase phases[SCHEDULE_MAX_PHASES];
  size_t count = 0;
  long long threads = 0;
  SourceBuffer buffer = {0};
  Lexer *lexer = NULL;
  Parser *parser = NULL;
  ASTNode *generated = NULL;
  int ok = 1;

  if (!variable->is_const) {
    schedule_error(reporter, declaration->location, "H0005",
                   "write it as `const`",
                   "'%s' is a schedule, and a schedule is read while "
                   "compiling, so it cannot be a `var`",
                   variable->name ? variable->name : "?");
    return 0;
  }
  rows = variable->initializer &&
                 variable->initializer->type == AST_AGGREGATE_LITERAL
             ? (AggregateLiteral *)variable->initializer->data
             : NULL;
  if (!rows || rows->is_struct) {
    schedule_error(reporter, declaration->location, "H0001",
                   "write the phases as an array literal, one row each",
                   "'%s' is a schedule, and a schedule is written as an array "
                   "of phases",
                   variable->name ? variable->name : "?");
    return 0;
  }
  if (rows->element_count == 0 || rows->element_count > SCHEDULE_MAX_PHASES) {
    schedule_error(reporter, declaration->location, "H0001",
                   "a schedule runs between one and 64 phases",
                   "'%s' has %zu phases", variable->name ? variable->name : "?",
                   rows->element_count);
    return 0;
  }
  for (size_t i = 0; i < rows->element_count; i++) {
    if (!read_phase(reporter, rows->elements[i], &phases[count])) {
      ok = 0;
      continue;
    }
    if (phases[count].thread + 1 > threads) {
      threads = phases[count].thread + 1;
    }
    count++;
  }
  if (!ok) {
    return 0;
  }
  if (!check_phases(reporter, program, phases, count, variable->name)) {
    return 0;
  }
  stats->schedules++;
  stats->phases += count;
  if (!build_source(&buffer, variable->name, phases, count, threads,
                    &stats->generated)) {
    free(buffer.data);
    return 0;
  }
  lexer = buffer.data ? lexer_create(buffer.data) : NULL;
  parser = lexer ? parser_create(lexer) : NULL;
  generated = parser ? parser_parse_program(parser) : NULL;
  if (!generated || generated->type != AST_PROGRAM ||
      !append_declarations(program_node, generated, phases, count,
                           declaration)) {
    schedule_error(reporter, declaration->location, "H0006", NULL,
                   "the dispatcher generated from '%s' did not parse",
                   variable->name ? variable->name : "?");
    ok = 0;
  }
  if (generated) {
    ast_destroy_node(generated);
  }
  if (parser) {
    parser_destroy(parser);
  }
  if (lexer) {
    lexer_destroy(lexer);
  }
  free(buffer.data);
  return ok;
}

int schedule_expand(ASTNode *program_node, ErrorReporter *reporter,
                    ScheduleStats *stats) {
  Program *program = NULL;
  ScheduleStats local = {0};
  size_t written = 0;
  int ok = 1;
  if (!program_node || program_node->type != AST_PROGRAM) {
    return 1;
  }
  program = (Program *)program_node->data;
  if (!program) {
    return 1;
  }
  if (!stats) {
    stats = &local;
  }
  written = program->declaration_count;
  for (size_t i = 0; i < written; i++) {
    ASTNode *declaration = program->declarations[i];
    VarDeclaration *variable = NULL;
    if (!declaration || declaration->type != AST_VAR_DECLARATION) {
      continue;
    }
    variable = (VarDeclaration *)declaration->data;
    if (!variable || !variable->name || !names_schedule(variable->type_name)) {
      continue;
    }
    if (!expand_one(program_node, declaration, reporter, stats)) {
      ok = 0;
    }
  }
  return ok;
}
