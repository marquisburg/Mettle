/* Reflection queries: the member-access surface on `Type` and `Field`.
 *
 * `typeof(T)` already yielded a TypeRef; this is what you can ask one. Every
 * query folds during const eval to an ordinary compile-time value -- an int, a
 * string, another TypeRef, or a sequence -- so nothing here has a runtime
 * representation and nothing reaches the backend un-folded.
 *
 * Three decisions are baked in here and are worth stating where they live:
 *
 *  - `.name` is module-qualified ("std/net.Point"). A bare name cannot tell two
 *    modules' `Point` apart, and since compile-time strings compare but do not
 *    concatenate, the module could never be recovered from a bare name.
 *    Builtins and structural types (pointers, arrays) answer their own
 *    unambiguous spelling.
 *
 *  - `.fields` is a real sequence value, not a special form. It is backed by an
 *    arena owned by the TypeChecker and memoized per (type, query), so the
 *    value stays trivially copyable, repeated evaluation of the same query
 *    borrows one allocation, and a constructed sequence stays possible later.
 *
 *  - `.kind` answers with `Kind`, an enum the compiler registers itself. Its
 *    variants are reachable only qualified, so reflection costs no import, no
 *    --prelude, and none of the bare global names a plain enum would claim. */
#include "type_checker_internal.h"
#include "import_resolver.h"

/* One memoized run of values, keyed by what produced it. */
typedef struct {
  uint32_t type_index;
  int query;
  uint32_t offset;
  uint32_t count;
} SequenceRun;

struct ComptimeSequenceArena {
  ComptimeValue *values;
  size_t value_count;
  size_t value_capacity;
  SequenceRun *runs;
  size_t run_count;
  size_t run_capacity;
};

enum { SEQUENCE_QUERY_FIELDS = 1 };

void type_checker_sequences_destroy(ComptimeSequenceArena *arena) {
  if (!arena) {
    return;
  }
  free(arena->values);
  free(arena->runs);
  free(arena);
}

static const SequenceRun *sequence_find(const ComptimeSequenceArena *arena,
                                        uint32_t type_index, int query) {
  for (size_t i = 0; i < arena->run_count; i++) {
    if (arena->runs[i].type_index == type_index &&
        arena->runs[i].query == query) {
      return &arena->runs[i];
    }
  }
  return NULL;
}

static int sequence_reserve(ComptimeSequenceArena *arena, size_t extra) {
  if (arena->value_count + extra <= arena->value_capacity) {
    return 1;
  }
  size_t next = arena->value_capacity ? arena->value_capacity * 2 : 32;
  while (next < arena->value_count + extra) {
    next *= 2;
  }
  ComptimeValue *grown = realloc(arena->values, next * sizeof(ComptimeValue));
  if (!grown) {
    return 0;
  }
  arena->values = grown;
  arena->value_capacity = next;
  return 1;
}

/* Values are addressed by offset, not pointer: the arena reallocs as it grows,
 * so a pointer handed out early would dangle. The ComptimeValue a caller sees
 * is built from the offset at the moment it is asked for. */
static int sequence_intern(TypeChecker *checker, uint32_t type_index, int query,
                           const ComptimeValue *items, uint32_t count,
                           ComptimeValue *out_value) {
  if (!checker->sequences) {
    checker->sequences = calloc(1, sizeof(ComptimeSequenceArena));
    if (!checker->sequences) {
      return 0;
    }
  }
  ComptimeSequenceArena *arena = checker->sequences;

  const SequenceRun *existing = sequence_find(arena, type_index, query);
  if (existing) {
    *out_value =
        comptime_sequence(arena->values + existing->offset, existing->count);
    return 1;
  }

  if (!sequence_reserve(arena, count)) {
    return 0;
  }
  if (arena->run_count == arena->run_capacity) {
    size_t next = arena->run_capacity ? arena->run_capacity * 2 : 16;
    SequenceRun *grown = realloc(arena->runs, next * sizeof(SequenceRun));
    if (!grown) {
      return 0;
    }
    arena->runs = grown;
    arena->run_capacity = next;
  }

  uint32_t offset = (uint32_t)arena->value_count;
  for (uint32_t i = 0; i < count; i++) {
    arena->values[arena->value_count++] = items[i];
  }
  arena->runs[arena->run_count].type_index = type_index;
  arena->runs[arena->run_count].query = query;
  arena->runs[arena->run_count].offset = offset;
  arena->runs[arena->run_count].count = count;
  arena->run_count++;

  *out_value = comptime_sequence(arena->values + offset, count);
  return 1;
}

/* The curated `Kind` set. Deliberately not a mirror of TypeKind: TYPE_TYPE and
 * TYPE_FIELD are compile-time only, so no value a program can reflect on ever
 * has one, and exposing them would pin user surface to a compiler-internal
 * enum. Widths stay distinct because telling int32 from int64 is exactly what a
 * wire-format generator needs. */
typedef struct {
  const char *name;
  TypeKind kind;
} KindMember;

static const KindMember g_kind_members[] = {
    {"Void", TYPE_VOID},
    {"Bool", TYPE_BOOL},
    {"Int8", TYPE_INT8},
    {"Int16", TYPE_INT16},
    {"Int32", TYPE_INT32},
    {"Int64", TYPE_INT64},
    {"Uint8", TYPE_UINT8},
    {"Uint16", TYPE_UINT16},
    {"Uint32", TYPE_UINT32},
    {"Uint64", TYPE_UINT64},
    {"Float32", TYPE_FLOAT32},
    {"Float64", TYPE_FLOAT64},
    {"Float16", TYPE_FLOAT16},
    {"Bfloat16", TYPE_BFLOAT16},
    {"String", TYPE_STRING},
    {"Pointer", TYPE_POINTER},
    {"Array", TYPE_ARRAY},
    {"Slice", TYPE_SLICE},
    {"Struct", TYPE_STRUCT},
    {"Enum", TYPE_ENUM},
    {"TaggedEnum", TYPE_TAGGED_ENUM},
    {"FunctionPointer", TYPE_FUNCTION_POINTER},
};

static const size_t g_kind_member_count =
    sizeof(g_kind_members) / sizeof(g_kind_members[0]);

/* Discriminants are the member's position here, not the internal TypeKind
 * value, so reordering TypeKind cannot silently renumber user-visible
 * constants. */
static long long kind_value_for(TypeKind kind) {
  for (size_t i = 0; i < g_kind_member_count; i++) {
    if (g_kind_members[i].kind == kind) {
      return (long long)i;
    }
  }
  return -1;
}

void type_checker_register_kind_enum(TypeChecker *checker) {
  if (!checker || checker->builtin_kind) {
    return;
  }
  Type *kind = type_create(TYPE_ENUM, "Kind");
  if (!kind) {
    return;
  }
  kind->size = 8;
  kind->alignment = 8;
  if (!type_alloc_enum_members(kind, g_kind_member_count)) {
    type_destroy(kind);
    return;
  }
  for (size_t i = 0; i < g_kind_member_count; i++) {
    type_set_enum_member(kind, i, g_kind_members[i].name, (long long)i);
  }
  type_checker_intern_type(checker, kind);
  checker->builtin_kind = kind;

  /* Only the enum name enters the namespace. A user enum also inserts each
   * variant as a bare global; doing that here would claim `Struct`, `Array`,
   * `Bool` and friends out of every program. */
  Symbol *symbol = symbol_create("Kind", SYMBOL_ENUM, kind);
  if (symbol) {
    symbol->is_initialized = 1;
    symbol_table_insert(checker->symbol_table, symbol);
  }
}

/* The module-qualified spelling reflection reports. Interned, computed once
 * per type at declaration. A type declared in the root program has no import
 * spelling, so it is qualified by the source file's stem instead -- still
 * unambiguous, and stable as long as the file keeps its name. */
void type_checker_set_qualified_name(TypeChecker *checker, Type *type,
                                     const char *filename) {
  if (!checker || !type || !type->name || type->qualified_name) {
    return;
  }

  const char *module = filename ? import_resolver_module_for_file(filename)
                                : NULL;
  char stem[256];
  if (!module && filename) {
    const char *base = filename;
    for (const char *p = filename; *p; p++) {
      if (*p == '/' || *p == '\\') {
        base = p + 1;
      }
    }
    size_t n = 0;
    while (base[n] && base[n] != '.' && n + 1 < sizeof(stem)) {
      stem[n] = base[n];
      n++;
    }
    stem[n] = '\0';
    if (n > 0) {
      module = stem;
    }
  }
  if (!module) {
    type->qualified_name = type->name;
    return;
  }

  size_t needed = strlen(module) + 1 + strlen(type->name) + 1;
  char *buffer = malloc(needed);
  if (!buffer) {
    type->qualified_name = type->name;
    return;
  }
  snprintf(buffer, needed, "%s.%s", module, type->name);
  type->qualified_name = (char *)string_intern(buffer);
  free(buffer);
}

/* What a type reports as its name. User-declared aggregates answer their
 * qualified spelling; builtins and structural types answer the spelling that
 * already names them uniquely (`int32`, `Point*`, `int32[4]`). */
static const char *type_reflected_name(const Type *type) {
  if (!type) {
    return NULL;
  }
  if (type->qualified_name) {
    return type->qualified_name;
  }
  return type->name;
}

int type_checker_type_member_exists(const char *member) {
  return member &&
         (strcmp(member, "kind") == 0 || strcmp(member, "name") == 0 ||
          strcmp(member, "size") == 0 || strcmp(member, "align") == 0 ||
          strcmp(member, "fields") == 0 || strcmp(member, "pointee") == 0 ||
          strcmp(member, "element") == 0 || strcmp(member, "len") == 0);
}

int type_checker_eval_type_member(TypeChecker *checker, ComptimeValue type_value,
                                  const char *member,
                                  ComptimeValue *out_value) {
  if (!checker || !member || !out_value ||
      type_value.kind != COMPTIME_TYPE_REF) {
    return 0;
  }
  Type *type =
      type_checker_type_from_index(checker, type_value.as.type_ref.type_index);
  if (!type) {
    return 0;
  }

  if (strcmp(member, "kind") == 0) {
    long long value = kind_value_for(type->kind);
    if (value < 0) {
      return 0;
    }
    *out_value = comptime_int(value);
    return 1;
  }
  if (strcmp(member, "name") == 0) {
    const char *name = type_reflected_name(type);
    if (!name) {
      return 0;
    }
    *out_value = comptime_string(string_intern(name));
    return out_value->as.string.value != NULL;
  }
  if (strcmp(member, "size") == 0) {
    *out_value = comptime_int((long long)type->size);
    return 1;
  }
  if (strcmp(member, "align") == 0) {
    *out_value = comptime_int((long long)type->alignment);
    return 1;
  }
  if (strcmp(member, "fields") == 0) {
    size_t count = type_field_count(type);
    if (count > UINT32_MAX) {
      return 0;
    }
    uint32_t owner = type_checker_intern_type(checker, type);
    if (owner == UINT32_MAX) {
      return 0;
    }
    if (count == 0) {
      *out_value = comptime_sequence(NULL, 0);
      return 1;
    }
    ComptimeValue *items = malloc(count * sizeof(ComptimeValue));
    if (!items) {
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      items[i] = comptime_field_ref(owner, (uint32_t)i);
    }
    int ok = sequence_intern(checker, owner, SEQUENCE_QUERY_FIELDS, items,
                             (uint32_t)count, out_value);
    free(items);
    return ok;
  }
  if (strcmp(member, "pointee") == 0) {
    Type *pointee = type_pointee(type);
    if (!pointee) {
      return 0;
    }
    uint32_t index = type_checker_intern_type(checker, pointee);
    if (index == UINT32_MAX) {
      return 0;
    }
    *out_value = comptime_type_ref(index);
    return 1;
  }
  if (strcmp(member, "element") == 0) {
    Type *element = type_element(type);
    if (!element) {
      return 0;
    }
    uint32_t index = type_checker_intern_type(checker, element);
    if (index == UINT32_MAX) {
      return 0;
    }
    *out_value = comptime_type_ref(index);
    return 1;
  }
  if (strcmp(member, "len") == 0) {
    if (!type_has_static_len(type)) {
      return 0;
    }
    *out_value = comptime_int((long long)type_len(type));
    return 1;
  }
  return 0;
}

int type_checker_field_member_exists(const char *member) {
  return member &&
         (strcmp(member, "name") == 0 || strcmp(member, "type") == 0 ||
          strcmp(member, "offset") == 0 || strcmp(member, "index") == 0);
}

int type_checker_eval_field_member(TypeChecker *checker, ComptimeValue field,
                                   const char *member,
                                   ComptimeValue *out_value) {
  if (!checker || !member || !out_value ||
      field.kind != COMPTIME_FIELD_REF) {
    return 0;
  }
  Type *owner =
      type_checker_type_from_index(checker, field.as.field_ref.type_index);
  TypeField resolved;
  if (!owner ||
      !type_field_at(owner, field.as.field_ref.field_index, &resolved)) {
    return 0;
  }

  if (strcmp(member, "index") == 0) {
    *out_value = comptime_int((long long)field.as.field_ref.field_index);
    return 1;
  }
  if (strcmp(member, "offset") == 0) {
    *out_value = comptime_int((long long)resolved.byte_offset);
    return 1;
  }
  /* A field name is not qualified: it is already unique within its type, and
   * the owning type's `.name` is where the module belongs. */
  if (strcmp(member, "name") == 0) {
    if (!resolved.name) {
      return 0;
    }
    *out_value = comptime_string(string_intern(resolved.name));
    return out_value->as.string.value != NULL;
  }
  if (strcmp(member, "type") == 0) {
    if (!resolved.type) {
      return 0;
    }
    uint32_t index = type_checker_intern_type(checker, resolved.type);
    if (index == UINT32_MAX) {
      return 0;
    }
    *out_value = comptime_type_ref(index);
    return 1;
  }
  return 0;
}

/* A column of one table row. The row is an aggregate literal and the columns
 * are its struct's fields, so a name resolves the same way a field access
 * would, and the answer is whatever constant the table wrote there. */
int type_checker_eval_row_member(TypeChecker *checker, ComptimeValue row,
                                 const char *member,
                                 ComptimeValue *out_value) {
  const AggregateLiteral *literal = NULL;
  Type *row_type = NULL;
  size_t i;

  if (!checker || !member || !out_value || row.kind != COMPTIME_ROW) {
    return 0;
  }
  literal = (const AggregateLiteral *)row.as.row.literal;
  row_type = type_checker_type_from_index(checker, row.as.row.type_index);
  if (!literal || !row_type) {
    return 0;
  }
  if (strcmp(member, "index") == 0) {
    *out_value = comptime_int((long long)row.as.row.index);
    return 1;
  }
  for (i = 0; i < literal->element_count; i++) {
    const char *written =
        literal->field_names ? literal->field_names[i] : NULL;
    if (!written || strcmp(written, member) != 0) {
      continue;
    }
    return type_checker_eval_comptime(checker, literal->elements[i],
                                      out_value);
  }
  /* A column the row left out keeps the zero the layout gives it, which is
     what the value would be at run time. */
  for (i = 0; i < row_type->field_count; i++) {
    if (row_type->field_names[i] &&
        strcmp(row_type->field_names[i], member) == 0) {
      Type *column = row_type->field_types[i];
      if (column && (column->kind == TYPE_FLOAT32 ||
                     column->kind == TYPE_FLOAT64 ||
                     column->kind == TYPE_FLOAT16 ||
                     column->kind == TYPE_BFLOAT16)) {
        *out_value = comptime_float(0.0);
      } else if (column && column->kind == TYPE_STRING) {
        *out_value = comptime_string(string_intern(""));
      } else {
        *out_value = comptime_int(0);
      }
      return 1;
    }
  }
  return 0;
}

/* Does this table row have a column by that name? */
int type_checker_row_member_exists(TypeChecker *checker, ComptimeValue row,
                                   const char *member) {
  Type *row_type = NULL;
  size_t i;
  if (!checker || !member || row.kind != COMPTIME_ROW) {
    return 0;
  }
  if (strcmp(member, "index") == 0) {
    return 1;
  }
  row_type = type_checker_type_from_index(checker, row.as.row.type_index);
  if (!row_type) {
    return 0;
  }
  for (i = 0; i < row_type->field_count; i++) {
    if (row_type->field_names[i] &&
        strcmp(row_type->field_names[i], member) == 0) {
      return 1;
    }
  }
  return 0;
}

/* `.len` and `[i]` on a sequence. Sequences answer only these two, which is
 * what makes them observable without being a container the program can hold. */
int type_checker_eval_sequence_member(TypeChecker *checker,
                                      ComptimeValue sequence,
                                      const char *member,
                                      ComptimeValue *out_value) {
  (void)checker;
  if (!member || !out_value || sequence.kind != COMPTIME_SEQUENCE) {
    return 0;
  }
  if (strcmp(member, "len") == 0) {
    *out_value = comptime_int((long long)sequence.as.sequence.count);
    return 1;
  }
  return 0;
}

/* Turn a folded query answer into a type, baking scalars into the AST as they
 * go. Baking has to happen here rather than at lowering: a query's operand may
 * be a `comptime for` binding, which leaves scope with its expansion, so by the
 * time the backend runs there is nothing left to re-derive the answer from.
 *
 * The reflection values (Type, Field, Sequence) are not baked -- they have no
 * runtime representation at all, and the escape checks reject any attempt to
 * let one reach runtime code. */
Type *type_checker_comptime_result(TypeChecker *checker, ComptimeValue value,
                                   ASTNode *expression) {
  if (!checker || !expression) {
    return NULL;
  }
  switch (value.kind) {
  case COMPTIME_INT:
    if (!ast_fold_member_access_to_int(expression, value.as.int_value)) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Out of memory folding a Type query");
      return NULL;
    }
    return checker->builtin_int64;
  case COMPTIME_STRING:
    if (!ast_fold_member_access_to_string(expression, value.as.string.value)) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Out of memory folding a name query");
      return NULL;
    }
    return checker->builtin_string;
  case COMPTIME_FLOAT:
    if (!ast_fold_member_access_to_float(expression, value.as.float_value)) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory folding a compile-time float");
      return NULL;
    }
    return checker->builtin_float64;
  case COMPTIME_TYPE_REF:
    return checker->builtin_type;
  case COMPTIME_FIELD_REF:
    return checker->builtin_field;
  case COMPTIME_SEQUENCE:
    return checker->builtin_sequence;
  default:
    return NULL;
  }
}

/* `.kind` is typed as `Kind` rather than a bare integer so it compares against
 * `Kind.Struct` and switches exhaustively, which is the whole reason it is an
 * enum and not a set of constants. */
Type *type_checker_kind_result(TypeChecker *checker, ComptimeValue value,
                               ASTNode *expression) {
  Type *folded = type_checker_comptime_result(checker, value, expression);
  if (!folded || !checker->builtin_kind) {
    return folded;
  }
  return checker->builtin_kind;
}

int type_checker_eval_sequence_index(ComptimeValue sequence, long long index,
                                     ComptimeValue *out_value) {
  if (!out_value || sequence.kind != COMPTIME_SEQUENCE) {
    return 0;
  }
  if (index < 0 || (uint64_t)index >= sequence.as.sequence.count ||
      !sequence.as.sequence.items) {
    return 0;
  }
  *out_value = sequence.as.sequence.items[index];
  return 1;
}
