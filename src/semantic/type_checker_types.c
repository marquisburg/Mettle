// Type checker: type construction, builtins, numeric promotion, conversions.
#include "type_checker_internal.h"
#include "string_intern.h"

/* A shared non-NULL marker used as closure_env for a boundary closure type
 * (`Fn(...)->R`), where the specific environment layout is opaque. Call dispatch
 * only checks closure_env for non-NULL; the concrete env is known to the callee. */
Type *type_checker_closure_env_sentinel(void) {
  static Type *sentinel = NULL;
  if (!sentinel) {
    sentinel = type_create(TYPE_STRUCT, "__closure_env");
  }
  return sentinel;
}

/* The bracket group that gives an array type its OUTER dimension, and the
 * matching ']'. Dimensions read left to right, so `int32[3][4]` is three rows
 * of four and the first group is the one this array measures; everything after
 * it belongs to the element. Answers 0 when the name is not an array.
 *
 * The search starts from the END so a bracket inside the element type is not
 * mistaken for a dimension: the element of `(fn(int32[4]) -> int32)[2]` is the
 * whole parenthesised function type. It then steps left, group by group, to
 * reach the first. A single-dimension name never enters that loop. */
static int type_checker_array_outer_group(const char *name,
                                          const char **lbracket_out,
                                          const char **rbracket_out) {
  size_t length = name ? strlen(name) : 0;
  const char *lbracket = NULL;
  const char *rbracket = NULL;
  const char *scan;
  int depth = 0;

  if (length == 0 || name[length - 1] != ']') {
    return 0;
  }
  rbracket = name + length - 1;
  for (scan = rbracket; scan >= name; scan--) {
    if (*scan == ']') {
      depth++;
    } else if (*scan == '[') {
      depth--;
      if (depth == 0) {
        lbracket = scan;
        break;
      }
    }
  }
  if (!lbracket) {
    return 0;
  }

  for (;;) {
    const char *end = lbracket - 1;
    const char *found = NULL;
    depth = 0;
    if (end <= name || *end != ']') {
      break;
    }
    for (scan = end; scan > name; scan--) {
      if (*scan == ']') {
        depth++;
      } else if (*scan == '[') {
        depth--;
        if (depth == 0) {
          found = scan;
          break;
        }
      }
    }
    if (!found || found == name) {
      break;
    }
    lbracket = found;
  }

  depth = 0;
  for (scan = lbracket; *scan; scan++) {
    if (*scan == '[') {
      depth++;
    } else if (*scan == ']') {
      depth--;
      if (depth == 0) {
        rbracket = scan;
        break;
      }
    }
  }
  if (!rbracket || rbracket < lbracket) {
    return 0;
  }
  *lbracket_out = lbracket;
  *rbracket_out = rbracket;
  return 1;
}

Type *type_checker_parse_array_type(TypeChecker *checker,
                                           const char *name) {
  const char *lbracket = NULL;
  const char *rbracket = NULL;

  if (!checker || !name) {
    return NULL;
  }
  if (!type_checker_array_outer_group(name, &lbracket, &rbracket)) {
    return NULL;
  }

  size_t base_len = (size_t)(lbracket - name);
  if (base_len == 0) {
    return NULL;
  }

  /* The element is the base plus whatever dimensions follow this one, so
   * `int32[3][4]` resolves its element as `int32[4]` and recurses. With one
   * dimension the tail is empty and this is the base name on its own. */
  size_t tail_len = strlen(rbracket + 1);
  char *base_name = malloc(base_len + tail_len + 1);
  if (!base_name) {
    return NULL;
  }
  memcpy(base_name, name, base_len);
  memcpy(base_name + base_len, rbracket + 1, tail_len);
  base_name[base_len + tail_len] = '\0';

  Type *base_type = type_checker_get_type_by_name(checker, base_name);
  free(base_name);
  if (!base_type) {
    return NULL;
  }

  const char *size_start = lbracket + 1;
  if (size_start == rbracket) {
    return NULL;
  }

  /* Scanned here rather than through strtoull: the owned runtime's strtoull
   * wraps modulo 2^64 and never reports ERANGE, so `int64[2^64 + 1]` came back
   * as an array of one element. A digit past the range is the type not naming
   * an array size at all. */
  unsigned long long array_size_ull = 0;
  int size_is_literal = size_start < rbracket;
  for (const char *digit = size_start; digit < rbracket; digit++) {
    if (*digit < '0' || *digit > '9') {
      size_is_literal = 0;
      break;
    }
    if (array_size_ull > ULLONG_MAX / 10ull) {
      return NULL;
    }
    array_size_ull *= 10ull;
    if (array_size_ull > ULLONG_MAX - (unsigned long long)(*digit - '0')) {
      return NULL;
    }
    array_size_ull += (unsigned long long)(*digit - '0');
  }
  if (!size_is_literal) {
    size_t size_name_len = (size_t)(rbracket - size_start);
    char *size_name = malloc(size_name_len + 1);
    if (!size_name) {
      return NULL;
    }
    memcpy(size_name, size_start, size_name_len);
    size_name[size_name_len] = '\0';
    Symbol *size_symbol = symbol_table_lookup(checker->symbol_table,
                                              size_name);
    free(size_name);
    if (!size_symbol ||
        (!size_symbol->has_constant_value &&
         size_symbol->kind != SYMBOL_CONSTANT) ||
        size_symbol->constant_is_float ||
        !type_checker_is_integer_type(size_symbol->type)) {
      return NULL;
    }
    long long constant_size = size_symbol->has_constant_value
                                  ? size_symbol->constant_integer_value
                                  : size_symbol->data.constant.value;
    if (constant_size <= 0) {
      return NULL;
    }
    array_size_ull = (unsigned long long)constant_size;
  }
  if (array_size_ull == 0 || array_size_ull > SIZE_MAX) {
    return NULL;
  }

  size_t array_size = (size_t)array_size_ull;
  /* SIZE_MAX is not a bound any object can actually reach: the backend keeps
   * frame offsets and local storage sizes in `int`, so an array whose bytes
   * pass INT_MAX arrived there as a negative size and was reported as an
   * internal compiler error. int64[1152921504606846976] fit under SIZE_MAX/8
   * and did exactly that. */
  if (base_type->size > 0 &&
      array_size > (size_t)INT_MAX / base_type->size) {
    return NULL;
  }

  Type *array_type = type_create(TYPE_ARRAY, name);
  if (!array_type) {
    return NULL;
  }

  array_type->base_type = base_type;
  array_type->array_size = array_size;
  if (!type_compute_layout(array_type)) {
    type_destroy(array_type);
    return NULL;
  }
  return type_checker_canon_type(checker, array_type);
}

int type_checker_ensure_multi_return_type(TypeChecker *checker,
                                          FunctionDeclaration *function,
                                          SourceLocation location) {
  if (!checker || !function || function->return_type_count < 2 ||
      !function->return_type) {
    return 0;
  }

  Type *existing = type_checker_get_type_by_name(checker, function->return_type);
  if (existing) {
    return existing->kind == TYPE_STRUCT &&
           existing->field_count == function->return_type_count;
  }

  Type **field_types = calloc(function->return_type_count, sizeof(Type *));
  char **field_names = calloc(function->return_type_count, sizeof(char *));
  if (!field_types || !field_names) {
    free(field_types);
    free(field_names);
    return 0;
  }

  for (size_t i = 0; i < function->return_type_count; i++) {
    field_types[i] = type_checker_get_type_by_name(
        checker, function->return_types[i]);
    /* Every other type is copied whole into the tuple and back out again. An
     * array is not: it would decay to its first element's address and the
     * caller would read a slot that has already been reused. Reject it here,
     * where the function's own signature is what the message can point at. */
    if (field_types[i] && field_types[i]->kind == TYPE_ARRAY) {
      type_checker_set_error_at_location(
          checker, location,
          "Function '%s' returns an array as value %zu of %zu; return a "
          "pointer to it, or wrap it in a struct",
          function->name ? function->name : "?", i + 1,
          function->return_type_count);
      for (size_t j = 0; j <= i; j++) {
        free(field_names[j]);
      }
      free(field_names);
      free(field_types);
      return 0;
    }
    field_names[i] = malloc(32);
    if (!field_types[i] || !field_names[i]) {
      for (size_t j = 0; j <= i; j++) {
        free(field_names[j]);
      }
      free(field_names);
      free(field_types);
      return 0;
    }
    snprintf(field_names[i], 32, "_%zu", i);
  }

  Type *tuple_type = type_create_struct(function->return_type, field_names,
                                        field_types, function->return_type_count);
  for (size_t i = 0; i < function->return_type_count; i++) {
    free(field_names[i]);
  }
  free(field_names);
  free(field_types);
  if (!tuple_type) {
    return 0;
  }

  Symbol *tuple_symbol = symbol_create(function->return_type, SYMBOL_STRUCT,
                                        tuple_type);
  if (!tuple_symbol ||
      !symbol_table_declare(checker->symbol_table, tuple_symbol)) {
    symbol_destroy(tuple_symbol);
    type_destroy(tuple_type);
    return 0;
  }
  return 1;
}

/* A slice of `element`: the fat pointer `{ data, length }` that carries its own
 * extent. It is spelled `T[]`, it is what a `T[N]` becomes when it is handed to
 * something that does not know N, and it is what `new T[n]` produces. The two
 * fields are ordinary ones, so `.length` and `.data` read the way any struct's
 * fields read, and the value copies and passes the way a 16-byte struct does. */
Type *type_checker_device_slice_of(TypeChecker *checker, Type *element,
                                   unsigned char space, size_t align,
                                   const char *qualifiers) {
  const char *element_name = NULL;
  size_t name_length = 0;
  char *name = NULL;
  Type *slice = NULL;
  Type *data = NULL;

  if (!checker || !element) {
    return NULL;
  }
  if (!qualifiers) {
    qualifiers = "";
  }
  element_name = element->name ? element->name : "?";
  name_length = strlen(element_name) + strlen(qualifiers) + 3;
  name = malloc(name_length);
  if (!name) {
    return NULL;
  }
  snprintf(name, name_length, "%s%s[]", element_name, qualifiers);

  slice = type_create(TYPE_SLICE, name);
  free(name);
  if (!slice) {
    return NULL;
  }
  data = type_checker_device_pointer_to(checker, element, space, align,
                                       qualifiers);
  if (!data || !type_alloc_fields(slice, 2)) {
    type_destroy(slice);
    return NULL;
  }
  slice->base_type = element;
  slice->device_space = space;
  slice->declared_align = align;
  slice->size = 16;
  slice->alignment = 8;
  type_set_field(slice, 0, "data", data, 0);
  type_set_field(slice, 1, "length", checker->builtin_int64, 0);
  slice->field_offsets[0] = 0;
  slice->field_offsets[1] = 8;
  return type_checker_canon_type(checker, slice);
}

Type *type_checker_slice_of(TypeChecker *checker, Type *element) {
  return type_checker_device_slice_of(checker, element, DEVICE_SPACE_NONE, 0,
                                      "");
}

Type *type_checker_device_view_of(TypeChecker *checker, Type *element,
                                  size_t rank, unsigned char space,
                                  size_t align, const char *qualifiers) {
  const char *element_name = NULL;
  size_t name_length = 0;
  char *name = NULL;
  char extent_name[32];
  Type *view = NULL;
  Type *data = NULL;
  Type *dims = NULL;
  Type *lead = NULL;

  if (!checker || !element) {
    return NULL;
  }
  if (!qualifiers) {
    qualifiers = "";
  }
  if (rank <= 1) {
    return type_checker_device_slice_of(checker, element, space, align,
                                        qualifiers);
  }
  element_name = element->name ? element->name : "?";
  name_length = strlen(element_name) + strlen(qualifiers) + rank + 2;
  name = malloc(name_length);
  if (!name) {
    return NULL;
  }
  snprintf(name, name_length, "%s%s[", element_name, qualifiers);
  for (size_t i = 1; i < rank; i++) {
    strcat(name, ",");
  }
  strcat(name, "]");

  view = type_create(TYPE_SLICE, name);
  free(name);
  if (!view) {
    return NULL;
  }
  data = type_checker_device_pointer_to(checker, element, space, align,
                                       qualifiers);
  snprintf(extent_name, sizeof(extent_name), "int64[%zu]", rank);
  dims = type_checker_get_type_by_name(checker, extent_name);
  snprintf(extent_name, sizeof(extent_name), "int64[%zu]", rank - 1);
  lead = type_checker_get_type_by_name(checker, extent_name);
  if (!data || !dims || !lead || !type_alloc_fields(view, 3)) {
    type_destroy(view);
    return NULL;
  }
  view->base_type = element;
  view->view_rank = rank;
  view->device_space = space;
  view->declared_align = align;
  view->size = 16 * rank;
  view->alignment = 8;
  type_set_field(view, 0, "data", data, 0);
  type_set_field(view, 1, "dims", dims, 0);
  type_set_field(view, 2, "lead", lead, 0);
  view->field_offsets[0] = 0;
  view->field_offsets[1] = 8;
  view->field_offsets[2] = 8 + 8 * rank;
  return type_checker_canon_type(checker, view);
}

/* Pointer to an arbitrary type, built from the type rather than from its
 * spelling. Address-of used to mangle "<name>*" and look the result up, which
 * works while the name is a plain identifier and fails the moment it is not:
 * `&slot` on a `fn(int32) -> int32` global asked for a type named
 * "fn(int32) -> int32*", which nothing registers. */
/* A view whose extents are in its type: one pointer, and a shape nobody has to
   carry. The element count is the product of the extents, so an allocation of
   one is a fixed size and every index into one is bounded by the declaration. */
Type *type_checker_static_view_of(TypeChecker *checker, Type *element,
                                  const char *name, const size_t *extents,
                                  size_t rank) {
  Type *view;
  if (!checker || !element || !name || !extents || rank < 2 || rank > 4) {
    return NULL;
  }
  view = type_create(TYPE_SLICE, name);
  if (!view) {
    return NULL;
  }
  view->base_type = element;
  view->view_rank = rank;
  view->size = 8;
  view->alignment = 8;
  for (size_t i = 0; i < rank; i++) {
    view->view_extents[i] = extents[i];
  }
  if (!type_alloc_fields(view, 1)) {
    type_destroy(view);
    return NULL;
  }
  type_set_field(view, 0, "data", type_checker_pointer_to(checker, element), 0);
  view->field_offsets[0] = 0;
  return type_checker_canon_type(checker, view);
}

Type *type_checker_view_of(TypeChecker *checker, Type *element, size_t rank) {
  return type_checker_device_view_of(checker, element, rank, DEVICE_SPACE_NONE,
                                     0, "");
}

Type *type_checker_device_pointer_to(TypeChecker *checker, Type *base,
                                     unsigned char space, size_t align,
                                     const char *qualifiers) {
  if (!checker || !base) {
    return NULL;
  }
  if (!qualifiers) {
    qualifiers = "";
  }
  const char *base_name = base->name ? base->name : "ptr";
  size_t pointer_name_len = strlen(base_name) + strlen(qualifiers) + 2;
  char *pointer_name = malloc(pointer_name_len);
  if (!pointer_name) {
    return NULL;
  }
  snprintf(pointer_name, pointer_name_len, "%s%s*", base_name, qualifiers);

  Type *existing = type_checker_get_type_by_name(checker, pointer_name);
  if (existing) {
    free(pointer_name);
    return existing;
  }

  Type *pointer_type = type_create(TYPE_POINTER, pointer_name);
  free(pointer_name);
  if (!pointer_type) {
    return NULL;
  }
  pointer_type->base_type = base;
  pointer_type->device_space = space;
  pointer_type->declared_align = align;
  if (!type_compute_layout(pointer_type)) {
    type_destroy(pointer_type);
    return NULL;
  }
  return type_checker_canon_type(checker, pointer_type);
}

Type *type_checker_pointer_to(TypeChecker *checker, Type *base) {
  return type_checker_device_pointer_to(checker, base, DEVICE_SPACE_NONE, 0,
                                        "");
}

Type *type_checker_volatile_of(TypeChecker *checker, Type *base) {
  if (!checker || !base) {
    return NULL;
  }
  if (base->is_volatile) {
    return base;
  }
  {
    const char *base_name = base->name ? base->name : "value";
    size_t length = strlen(base_name) + 10;
    char *qualified_name = malloc(length);
    Type *qualified = NULL;
    size_t i;
    if (!qualified_name) {
      return NULL;
    }
    snprintf(qualified_name, length, "volatile %s", base_name);
    for (i = 0; i < checker->type_table_count; i++) {
      Type *existing = checker->type_table[i];
      if (existing && existing->is_volatile && existing->name &&
          strcmp(existing->name, qualified_name) == 0) {
        free(qualified_name);
        return existing;
      }
    }
    qualified = type_create(base->kind, qualified_name);
    free(qualified_name);
    if (!qualified) {
      return NULL;
    }
    qualified->is_volatile = 1;
    qualified->base_type = base->base_type;
    qualified->array_size = base->array_size;
    qualified->view_rank = base->view_rank;
    qualified->fn_param_types = base->fn_param_types;
    qualified->fn_param_count = base->fn_param_count;
    qualified->fn_return_type = base->fn_return_type;
    qualified->closure_env = base->closure_env;
    qualified->fn_effects_closed = base->fn_effects_closed;
    qualified->fn_effect_signature = base->fn_effect_signature;
    qualified->field_names = base->field_names;
    qualified->field_types = base->field_types;
    qualified->field_offsets = base->field_offsets;
    qualified->field_bit_offsets = base->field_bit_offsets;
    qualified->field_bit_widths = base->field_bit_widths;
    qualified->field_count = base->field_count;
    qualified->tagged_variant_names = base->tagged_variant_names;
    qualified->tagged_variant_tags = base->tagged_variant_tags;
    qualified->tagged_variant_payloads = base->tagged_variant_payloads;
    qualified->tagged_variant_count = base->tagged_variant_count;
    qualified->enum_member_names = base->enum_member_names;
    qualified->enum_member_values = base->enum_member_values;
    qualified->enum_member_count = base->enum_member_count;
    qualified->tagged_data_offset = base->tagged_data_offset;
    qualified->tagged_data_size = base->tagged_data_size;
    qualified->size = base->size;
    qualified->alignment = base->alignment;
    if (type_checker_intern_type(checker, qualified) == UINT32_MAX) {
      return base;
    }
    return qualified;
  }
}

/* Peel `global`, `shared`, `constant`, `local` and `align(N)` off the head of
   a type spelling. The parser writes them in that order and always directly in
   front of the pointer, slice or view suffix they qualify, so the tail of the
   head is the whole search. Returns the length of what is left. */
/* `layout row`, `layout swizzle128`, `layout interleave(4)` off the tail of a
   type spelling. The names are data: std/warp declares one constant per form,
   and the type refers to it by the same word. */
static const struct {
  const char *word;
  unsigned char layout;
  int takes_parameter;
} g_view_layouts[] = {
    {"row", VIEW_LAYOUT_ROW, 0},
    {"col", VIEW_LAYOUT_COL, 0},
    {"swizzle32", VIEW_LAYOUT_SWIZZLE32, 0},
    {"swizzle64", VIEW_LAYOUT_SWIZZLE64, 0},
    {"swizzle128", VIEW_LAYOUT_SWIZZLE128, 0},
    {"interleave", VIEW_LAYOUT_INTERLEAVE, 1},
    {"fragment_a", VIEW_LAYOUT_FRAGMENT_A, 0},
    {"fragment_b", VIEW_LAYOUT_FRAGMENT_B, 0},
    {"fragment_c", VIEW_LAYOUT_FRAGMENT_C, 0}};

size_t type_checker_split_view_layout(const char *name, size_t length,
                                      unsigned char *layout,
                                      unsigned short *parameter) {
  const char *marker = " layout ";
  size_t marker_length = 8;
  size_t at;
  if (layout) {
    *layout = VIEW_LAYOUT_NONE;
  }
  if (parameter) {
    *parameter = 0;
  }
  if (!name || length < marker_length + 1) {
    return length;
  }
  for (at = length - marker_length; at > 0; at--) {
    if (strncmp(name + at, marker, marker_length) == 0) {
      break;
    }
  }
  if (at == 0 && strncmp(name, marker, marker_length) != 0) {
    return length;
  }
  {
    const char *word = name + at + marker_length;
    size_t word_length = length - (at + marker_length);
    unsigned long value = 0;
    const char *open = memchr(word, '(', word_length);
    if (open) {
      value = strtoul(open + 1, NULL, 10);
      word_length = (size_t)(open - word);
    }
    for (size_t i = 0; i < sizeof(g_view_layouts) / sizeof(g_view_layouts[0]);
         i++) {
      if (strlen(g_view_layouts[i].word) == word_length &&
          strncmp(word, g_view_layouts[i].word, word_length) == 0) {
        if (g_view_layouts[i].takes_parameter && value == 0) {
          return length;
        }
        if (layout) {
          *layout = g_view_layouts[i].layout;
        }
        if (parameter) {
          *parameter = (unsigned short)value;
        }
        return at;
      }
    }
  }
  return length;
}

size_t type_checker_split_device_qualifiers(const char *name, size_t length,
                                            unsigned char *space,
                                            size_t *align) {
  static const struct {
    const char *word;
    unsigned char space;
  } spaces[] = {{" global", DEVICE_SPACE_GLOBAL},
                {" shared", DEVICE_SPACE_SHARED},
                {" constant", DEVICE_SPACE_CONSTANT},
                {" local", DEVICE_SPACE_LOCAL}};
  size_t i;
  if (space) {
    *space = DEVICE_SPACE_NONE;
  }
  if (align) {
    *align = 0;
  }
  if (!name) {
    return 0;
  }
  if (length > 8 && name[length - 1] == ')') {
    size_t open = length - 1;
    while (open > 0 && name[open] != '(') {
      open--;
    }
    if (open >= 6 && strncmp(name + open - 5, "align(", 6) == 0 &&
        name[open - 6] == ' ') {
      long long value = strtoll(name + open + 1, NULL, 10);
      if (value > 0 && align) {
        *align = (size_t)value;
      }
      length = open - 6;
    }
  }
  for (i = 0; i < sizeof(spaces) / sizeof(spaces[0]); i++) {
    size_t word_length = strlen(spaces[i].word);
    if (length > word_length &&
        strncmp(name + length - word_length, spaces[i].word, word_length) == 0) {
      if (space) {
        *space = spaces[i].space;
      }
      length -= word_length;
      break;
    }
  }
  return length;
}

/* The spelling a qualified head had, minus the head itself: ` global align(16)`
   out of `float32 global align(16)*`. The pointer type's own name is built back
   from it so a diagnostic reads the way the source did. */
static char *type_checker_qualifier_text(const char *name, size_t head_length,
                                         size_t plain_length) {
  size_t extra = head_length - plain_length;
  char *text = malloc(extra + 1);
  if (!text) {
    return NULL;
  }
  memcpy(text, name + plain_length, extra);
  text[extra] = '\0';
  return text;
}

Type *type_checker_parse_pointer_type(TypeChecker *checker,
                                             const char *name) {
  if (!checker || !name) {
    return NULL;
  }

  size_t name_len = strlen(name);
  size_t pointer_depth = 0;
  unsigned char space = DEVICE_SPACE_NONE;
  size_t align = 0;
  size_t plain_len;
  char *qualifiers = NULL;
  while (name_len > 0 && name[name_len - 1] == '*') {
    pointer_depth++;
    name_len--;
  }

  if (pointer_depth == 0 || name_len == 0) {
    return NULL;
  }

  plain_len = type_checker_split_device_qualifiers(name, name_len, &space,
                                                   &align);
  if (plain_len == 0) {
    return NULL;
  }
  if (plain_len != name_len) {
    qualifiers = type_checker_qualifier_text(name, name_len, plain_len);
    if (!qualifiers) {
      return NULL;
    }
  }

  char *base_name = malloc(plain_len + 1);
  if (!base_name) {
    free(qualifiers);
    return NULL;
  }
  memcpy(base_name, name, plain_len);
  base_name[plain_len] = '\0';

  Type *base_type = type_checker_get_type_by_name(checker, base_name);
  free(base_name);
  if (!base_type) {
    free(qualifiers);
    return NULL;
  }

  Type *current = base_type;
  for (size_t i = 0; i < pointer_depth; i++) {
    const char *current_name = current && current->name ? current->name : "ptr";
    const char *qualifier_text = (i == 0 && qualifiers) ? qualifiers : "";
    size_t pointer_name_len =
        strlen(current_name) + strlen(qualifier_text) + 2;
    char *pointer_name = malloc(pointer_name_len);
    if (!pointer_name) {
      free(qualifiers);
      return NULL;
    }
    snprintf(pointer_name, pointer_name_len, "%s%s*", current_name,
             qualifier_text);

    Type *pointer_type = type_create(TYPE_POINTER, pointer_name);
    free(pointer_name);
    if (!pointer_type) {
      free(qualifiers);
      return NULL;
    }

    pointer_type->base_type = current;
    if (i == 0) {
      pointer_type->device_space = space;
      pointer_type->declared_align = align;
    }
    if (!type_compute_layout(pointer_type)) {
      type_destroy(pointer_type);
      free(qualifiers);
      return NULL;
    }
    current = type_checker_canon_type(checker, pointer_type);
  }

  free(qualifiers);
  return current;
}

Type *type_checker_parse_function_pointer_type(TypeChecker *checker,
                                                      const char *name) {
  if (!checker || !name) {
    return NULL;
  }

  // Check if it's a function pointer type: fn(param1,param2)->returntype (thin)
  // or Fn(...)->returntype (a stateful closure type). Both prefixes are 3 chars.
  int is_closure_type = 0;
  if (strlen(name) < 4 || strncmp(name, "fn(", 3) != 0) {
    if (strlen(name) >= 4 && strncmp(name, "Fn(", 3) == 0) {
      is_closure_type = 1;
    } else {
      return NULL;
    }
  }

  size_t close_index = 0;
  int paren_depth = 0;
  int found_close = 0;
  for (size_t i = 2; name[i] != '\0'; i++) {
    if (name[i] == '(') {
      paren_depth++;
    } else if (name[i] == ')') {
      paren_depth--;
      if (paren_depth < 0) {
        return NULL;
      }
      if (paren_depth == 0) {
        close_index = i;
        found_close = 1;
        break;
      }
    }
  }

  if (!found_close || name[close_index + 1] != '-' ||
      name[close_index + 2] != '>') {
    return NULL;
  }

  // Parse parameter types
  const char *params_start = name + 3; // skip "fn("
  const char *params_end = name + close_index;
  size_t params_len = params_end - params_start;

  Type **param_types = NULL;
  size_t param_count = 0;
  char *params_copy = NULL;

  if (params_len > 0) {
    // Parse comma-separated parameter types, splitting only on top-level commas.
    params_copy = malloc(params_len + 1);
    if (!params_copy) {
      return NULL;
    }
    memcpy(params_copy, params_start, params_len);
    params_copy[params_len] = '\0';

    // Count top-level parameters.
    param_count = 1;
    int angle_depth = 0;
    int bracket_depth = 0;
    paren_depth = 0;
    for (size_t i = 0; i < params_len; i++) {
      if (params_copy[i] == '<') {
        angle_depth++;
      } else if (params_copy[i] == '>') {
        if (angle_depth > 0) {
          angle_depth--;
        }
      } else if (params_copy[i] == '[') {
        bracket_depth++;
      } else if (params_copy[i] == ']') {
        bracket_depth--;
      } else if (params_copy[i] == '(') {
        paren_depth++;
      } else if (params_copy[i] == ')') {
        paren_depth--;
      } else if (params_copy[i] == ',' && angle_depth == 0 &&
                 bracket_depth == 0 && paren_depth == 0) {
        param_count++;
      }

      if (angle_depth < 0 || bracket_depth < 0 || paren_depth < 0) {
        free(params_copy);
        return NULL;
      }
    }

    param_types = calloc(param_count, sizeof(Type *));
    if (!param_types) {
      free(params_copy);
      return NULL;
    }

    // Parse each top-level parameter type.
    size_t param_start = 0;
    size_t param_idx = 0;
    angle_depth = 0;
    bracket_depth = 0;
    paren_depth = 0;
    for (size_t i = 0; i <= params_len; i++) {
      char ch = params_copy[i];
      int is_end = (ch == '\0');

      if (!is_end) {
        if (ch == '<') {
          angle_depth++;
        } else if (ch == '>') {
          if (angle_depth > 0) {
            angle_depth--;
          }
        } else if (ch == '[') {
          bracket_depth++;
        } else if (ch == ']') {
          bracket_depth--;
        } else if (ch == '(') {
          paren_depth++;
        } else if (ch == ')') {
          paren_depth--;
        }
      }

      if (angle_depth < 0 || bracket_depth < 0 || paren_depth < 0) {
        free(params_copy);
        free(param_types);
        return NULL;
      }

      int is_separator =
          is_end || (ch == ',' && angle_depth == 0 && bracket_depth == 0 &&
                     paren_depth == 0);
      if (!is_separator) {
        continue;
      }

      size_t start = param_start;
      size_t end = i;
      while (start < end && isspace((unsigned char)params_copy[start])) {
        start++;
      }
      while (end > start && isspace((unsigned char)params_copy[end - 1])) {
        end--;
      }
      if (end <= start) {
        free(params_copy);
        free(param_types);
        return NULL;
      }

      char saved = params_copy[end];
      params_copy[end] = '\0';
      Type *param_type =
          type_checker_get_type_by_name(checker, params_copy + start);
      params_copy[end] = saved;
        if (!param_type) {
          free(params_copy);
          free(param_types);
          return NULL;
        }
        if (param_idx < param_count) {
          param_types[param_idx++] = param_type;
        }
        param_start = i + 1;
      }

    if (param_idx != param_count) {
      free(params_copy);
      free(param_types);
      return NULL;
    }
  }

  // Parse return type
  const char *return_type_start = name + close_index + 3; // skip ")->"
  if (*return_type_start == '\0') {
    free(params_copy);
    free(param_types);
    return NULL;
  }
  const char *effect_clauses =
      type_checker_fn_type_effect_clause_start(return_type_start);
  char *return_copy = strdup(return_type_start);
  if (!return_copy) {
    free(params_copy);
    free(param_types);
    return NULL;
  }
  size_t return_start = 0;
  size_t return_end = effect_clauses
                          ? (size_t)(effect_clauses - return_type_start)
                          : strlen(return_copy);
  while (return_start < return_end &&
         isspace((unsigned char)return_copy[return_start])) {
    return_start++;
  }
  while (return_end > return_start &&
         isspace((unsigned char)return_copy[return_end - 1])) {
    return_end--;
  }
  if (return_end <= return_start) {
    free(params_copy);
    free(param_types);
    free(return_copy);
    return NULL;
  }
  return_copy[return_end] = '\0';

  Type *return_type =
      type_checker_get_type_by_name(checker, return_copy + return_start);
  if (!return_type) {
    free(params_copy);
    free(param_types);
    free(return_copy);
    return NULL;
  }

  Type *fp_type =
      type_create_function_pointer(param_types, param_count, return_type);
  free(params_copy);
  free(param_types);
  free(return_copy);
  if (!fp_type) {
    return NULL;
  }
  if (effect_clauses &&
      !type_checker_fn_type_apply_effect_clauses(checker, fp_type,
                                                 effect_clauses)) {
    type_destroy(fp_type);
    return NULL;
  }
  if (is_closure_type) {
    /* Name it with the resolvable `Fn(...)->R` string so an inferred closure
     * local is sized as an 8-byte pointer by the backend, and mark it a
     * closure so calls dispatch through the environment. */
    fp_type->name = (char *)string_intern(name);
    fp_type->closure_env = type_checker_closure_env_sentinel();
  }

  return fp_type;
}


Type *type_checker_refinement_base(Type *type) {
  while (type && type->refined_base) {
    type = type->refined_base;
  }
  return type;
}

int type_checker_types_equal(const Type *lhs, const Type *rhs) {
  if (lhs == rhs) {
    return 1;
  }
  if (!lhs || !rhs) {
    return 0;
  }
  if (lhs->refined_base || rhs->refined_base) {
    return lhs->refined_base && rhs->refined_base && lhs->name && rhs->name &&
           strcmp(lhs->name, rhs->name) == 0 &&
           (lhs->qualified_name == rhs->qualified_name ||
            (lhs->qualified_name && rhs->qualified_name &&
             strcmp(lhs->qualified_name, rhs->qualified_name) == 0));
  }
  if (lhs->kind != rhs->kind) {
    return 0;
  }

  switch (lhs->kind) {
  case TYPE_POINTER:
    return lhs->device_space == rhs->device_space &&
           lhs->declared_align == rhs->declared_align &&
           type_checker_types_equal(lhs->base_type, rhs->base_type);
  case TYPE_SLICE:
    return type_view_rank(lhs) == type_view_rank(rhs) &&
           lhs->device_space == rhs->device_space &&
           lhs->declared_align == rhs->declared_align &&
           lhs->view_layout == rhs->view_layout &&
           lhs->view_layout_param == rhs->view_layout_param &&
           lhs->view_extents[0] == rhs->view_extents[0] &&
           lhs->view_extents[1] == rhs->view_extents[1] &&
           lhs->view_extents[2] == rhs->view_extents[2] &&
           lhs->view_extents[3] == rhs->view_extents[3] &&
           type_checker_types_equal(lhs->base_type, rhs->base_type);
  case TYPE_ARRAY:
    return lhs->array_size == rhs->array_size &&
           type_checker_types_equal(lhs->base_type, rhs->base_type);
  case TYPE_STRUCT:
  case TYPE_ENUM:
  case TYPE_TAGGED_ENUM:
    if (lhs->name && rhs->name) {
      return strcmp(lhs->name, rhs->name) == 0;
    }
    return lhs->name == rhs->name;
  case TYPE_FUNCTION_POINTER:
    return type_checker_fn_signatures_equal(lhs, rhs) &&
           type_checker_fn_effect_sets_equal(lhs, rhs);
  default:
    return 1;
  }
}

int type_checker_fn_signatures_equal(const Type *lhs, const Type *rhs) {
  if (!lhs || !rhs || lhs->kind != TYPE_FUNCTION_POINTER ||
      rhs->kind != TYPE_FUNCTION_POINTER) {
    return 0;
  }
  if (lhs->fn_param_count != rhs->fn_param_count) {
    return 0;
  }
  if (!type_checker_types_equal(lhs->fn_return_type, rhs->fn_return_type)) {
    return 0;
  }
  for (size_t i = 0; i < lhs->fn_param_count; i++) {
    if (!type_checker_types_equal(lhs->fn_param_types[i],
                                  rhs->fn_param_types[i])) {
      return 0;
    }
  }
  return 1;
}

int type_checker_is_cstring_type(const Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "cstring") == 0;
}

int type_checker_is_rawptr_type(const Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "rawptr") == 0;
}

// Built-in type system functions implementation

void type_checker_init_builtin_types(TypeChecker *checker) {
  if (!checker)
    return;

  // Create built-in integer types
  checker->builtin_int8 = type_create(TYPE_INT8, "int8");
  checker->builtin_int16 = type_create(TYPE_INT16, "int16");
  checker->builtin_int32 = type_create(TYPE_INT32, "int32");
  checker->builtin_int64 = type_create(TYPE_INT64, "int64");

  // Create built-in unsigned integer types
  checker->builtin_uint8 = type_create(TYPE_UINT8, "uint8");
  checker->builtin_uint16 = type_create(TYPE_UINT16, "uint16");
  checker->builtin_uint32 = type_create(TYPE_UINT32, "uint32");
  checker->builtin_uint64 = type_create(TYPE_UINT64, "uint64");

  // Create first-class bool type (1-byte integer, distinct from uint8)
  checker->builtin_bool = type_create(TYPE_BOOL, "bool");
  if (checker->builtin_bool) {
    checker->builtin_bool->size = 1;
    checker->builtin_bool->alignment = 1;
  }

  // Create first-class char type (1-byte character, distinct from uint8)
  checker->builtin_char = type_create(TYPE_CHAR, "char");
  if (checker->builtin_char) {
    checker->builtin_char->size = 1;
    checker->builtin_char->alignment = 1;
  }

  // Create built-in floating-point types
  checker->builtin_float32 = type_create(TYPE_FLOAT32, "float32");
  checker->builtin_float64 = type_create(TYPE_FLOAT64, "float64");
  checker->builtin_float16 = type_create(TYPE_FLOAT16, "float16");
  checker->builtin_bfloat16 = type_create(TYPE_BFLOAT16, "bfloat16");

  // C interop alias: cstring -> uint8*
  checker->builtin_cstring = type_create(TYPE_POINTER, "cstring");
  if (checker->builtin_cstring) {
    checker->builtin_cstring->base_type = checker->builtin_uint8;
    type_compute_layout(checker->builtin_cstring);
  }

  // Create built-in string type backed by a uint8* and length
  checker->builtin_string = type_create(TYPE_STRING, "string");
  if (checker->builtin_string) {
    checker->builtin_string->size = 16;
    checker->builtin_string->alignment = 8;

    if (!type_alloc_fields(checker->builtin_string, 2)) {
      return;
    }
    Type *chars = type_create(TYPE_POINTER, "uint8*");
    if (chars) {
      chars->base_type = checker->builtin_uint8;
      type_compute_layout(chars);
      chars = type_checker_canon_type(checker, chars);
    }
    type_set_field(checker->builtin_string, 0, "chars", chars, 0);
    type_set_field(checker->builtin_string, 1, "length", checker->builtin_uint64,
                   0);
    type_compute_layout(checker->builtin_string);
  }

  // Create built-in void type
  checker->builtin_void = type_create(TYPE_VOID, "void");
  if (checker->builtin_void) {
    checker->builtin_void->size = 0;
    checker->builtin_void->alignment = 1;
  }

  /* An address with no element type. The allocator hands one out and the
   * deallocator takes one, so releasing an int32 buffer no longer requires
   * claiming it holds characters. It converts to and from every pointer type,
   * and only to them: with no element size there is nothing to index or offset
   * by, and the checker's pointer arithmetic refuses it on those grounds. */
  checker->builtin_rawptr = type_create(TYPE_POINTER, "rawptr");
  if (checker->builtin_rawptr) {
    checker->builtin_rawptr->base_type = checker->builtin_void;
    checker->builtin_rawptr->size = 8;
    checker->builtin_rawptr->alignment = 8;
  }

  /* Type and Field are comptime-only: size 0, no backend kind. */
  checker->builtin_type = type_create(TYPE_TYPE, "Type");
  if (checker->builtin_type) {
    checker->builtin_type->size = 0;
    checker->builtin_type->alignment = 0;
  }
  checker->builtin_field = type_create(TYPE_FIELD, "Field");
  if (checker->builtin_field) {
    checker->builtin_field->size = 0;
    checker->builtin_field->alignment = 0;
  }
  checker->builtin_row = type_create(TYPE_FIELD, "Row");
  if (checker->builtin_row) {
    checker->builtin_row->size = 0;
    checker->builtin_row->alignment = 0;
  }
  checker->builtin_sequence = type_create(TYPE_SEQUENCE, "Sequence");
  if (checker->builtin_sequence) {
    checker->builtin_sequence->size = 0;
    checker->builtin_sequence->alignment = 0;
  }

  type_checker_intern_type(checker, checker->builtin_int8);
  type_checker_intern_type(checker, checker->builtin_int16);
  type_checker_intern_type(checker, checker->builtin_int32);
  type_checker_intern_type(checker, checker->builtin_int64);
  type_checker_intern_type(checker, checker->builtin_uint8);
  type_checker_intern_type(checker, checker->builtin_uint16);
  type_checker_intern_type(checker, checker->builtin_uint32);
  type_checker_intern_type(checker, checker->builtin_uint64);
  type_checker_intern_type(checker, checker->builtin_bool);
  type_checker_intern_type(checker, checker->builtin_float32);
  type_checker_intern_type(checker, checker->builtin_float64);
  type_checker_intern_type(checker, checker->builtin_float16);
  type_checker_intern_type(checker, checker->builtin_bfloat16);
  type_checker_intern_type(checker, checker->builtin_string);
  type_checker_intern_type(checker, checker->builtin_cstring);
  type_checker_intern_type(checker, checker->builtin_rawptr);
  type_checker_intern_type(checker, checker->builtin_void);
  type_checker_intern_type(checker, checker->builtin_type);
  type_checker_intern_type(checker, checker->builtin_field);
  type_checker_intern_type(checker, checker->builtin_sequence);

  // Register 'true' and 'false' as global bool constants so user code can
  // reference them as plain identifiers without any extra keyword machinery.
  if (checker->builtin_bool && checker->symbol_table) {
    Symbol *true_sym =
        symbol_create("true", SYMBOL_CONSTANT, checker->builtin_bool);
    if (true_sym) {
      true_sym->data.constant.value = 1;
      true_sym->is_initialized = 1;
      symbol_table_insert(checker->symbol_table, true_sym);
    }
    Symbol *false_sym =
        symbol_create("false", SYMBOL_CONSTANT, checker->builtin_bool);
    if (false_sym) {
      false_sym->data.constant.value = 0;
      false_sym->is_initialized = 1;
      symbol_table_insert(checker->symbol_table, false_sym);
    }
  }
}

static int type_checker_builtin_by_name(TypeChecker *checker,
                                        const char *name, Type **out) {
  if (strcmp(name, "bool") == 0) {
    *out = checker->builtin_bool;
    return 1;
  }
  if (strcmp(name, "char") == 0) {
    *out = checker->builtin_char;
    return 1;
  }
  if (strcmp(name, "int8") == 0) {
    *out = checker->builtin_int8;
    return 1;
  }
  if (strcmp(name, "int16") == 0) {
    *out = checker->builtin_int16;
    return 1;
  }
  if (strcmp(name, "int32") == 0) {
    *out = checker->builtin_int32;
    return 1;
  }
  if (strcmp(name, "int64") == 0) {
    *out = checker->builtin_int64;
    return 1;
  }
  if (strcmp(name, "uint8") == 0) {
    *out = checker->builtin_uint8;
    return 1;
  }
  if (strcmp(name, "uint16") == 0) {
    *out = checker->builtin_uint16;
    return 1;
  }
  if (strcmp(name, "uint32") == 0) {
    *out = checker->builtin_uint32;
    return 1;
  }
  if (strcmp(name, "uint64") == 0) {
    *out = checker->builtin_uint64;
    return 1;
  }
  if (strcmp(name, "float32") == 0) {
    *out = checker->builtin_float32;
    return 1;
  }
  if (strcmp(name, "float64") == 0) {
    *out = checker->builtin_float64;
    return 1;
  }
  if (strcmp(name, "float16") == 0) {
    *out = checker->builtin_float16;
    return 1;
  }
  if (strcmp(name, "bfloat16") == 0) {
    *out = checker->builtin_bfloat16;
    return 1;
  }
  if (strcmp(name, "string") == 0) {
    *out = checker->builtin_string;
    return 1;
  }
  if (strcmp(name, "cstring") == 0) {
    *out = checker->builtin_cstring;
    return 1;
  }
  if (strcmp(name, "rawptr") == 0) {
    *out = checker->builtin_rawptr;
    return 1;
  }
  if (strcmp(name, "void") == 0) {
    *out = checker->builtin_void;
    return 1;
  }
  if (strcmp(name, "Type") == 0) {
    *out = checker->builtin_type;
    return 1;
  }
  if (strcmp(name, "Field") == 0) {
    *out = checker->builtin_field;
    return 1;
  }
  if (strcmp(name, "Kind") == 0) {
    type_checker_register_kind_enum(checker);
    *out = checker->builtin_kind;
    return 1;
  }
  return 0;
}

static int type_checker_parse_view_layout(TypeChecker *checker, const char *name, Type **out) {
  unsigned char layout = VIEW_LAYOUT_NONE;
  unsigned short parameter = 0;
  size_t length = strlen(name);
  size_t head = type_checker_split_view_layout(name, length, &layout,
                                               &parameter);
  if (head != length) {
    char *plain = malloc(head + 1);
    Type *base;
    Type *laid_out;
    if (!plain) {
      *out = NULL;
      return 1;
    }
    memcpy(plain, name, head);
    plain[head] = '\0';
    base = type_checker_get_type_by_name(checker, plain);
    free(plain);
    if (!base || base->kind != TYPE_SLICE) {
      *out = NULL;
      return 1;
    }
    for (size_t i = 0; i < checker->type_table_count; i++) {
      Type *existing = checker->type_table[i];
      if (existing && existing->name &&
          strcmp(existing->name, name) == 0) {
        *out = existing;
        return 1;
      }
    }
    laid_out = type_create(TYPE_SLICE, name);
    if (!laid_out) {
      *out = NULL;
      return 1;
    }
    /* The layout is the only difference, and the field arrays are the
       base's. Interning directly rather than canonicalizing is what keeps
       them the base's: a canonicalization that found an equal type would
       destroy this one and take those arrays with it. */
    *laid_out = *base;
    laid_out->name = (char *)string_intern(name);
    laid_out->type_table_index = UINT32_MAX;
    laid_out->view_layout = layout;
    laid_out->view_layout_param = parameter;
    if (type_checker_intern_type(checker, laid_out) == UINT32_MAX) {
      *out = base;
      return 1;
    }
    *out = laid_out;
    return 1;
  }
  return 0;
}

static int type_checker_parse_extent_view(TypeChecker *checker, const char *name, Type **out) {
  size_t length = strlen(name);
  if (length > 3 && name[length - 1] == ']' && strchr(name, ',')) {
    size_t open = length - 1;
    size_t extents[4];
    size_t rank = 0;
    int all_numeric = 1;
    while (open > 0 && name[open] != '[') {
      open--;
    }
    if (name[open] == '[' && open > 0) {
      const char *scan = name + open + 1;
      while (scan < name + length - 1 && rank < 4) {
        char *end = NULL;
        unsigned long value = strtoul(scan, &end, 10);
        if (end == scan || value == 0) {
          all_numeric = 0;
          break;
        }
        extents[rank++] = (size_t)value;
        scan = end;
        if (*scan == ',') {
          scan++;
        } else {
          break;
        }
      }
      if (all_numeric && rank >= 2 && scan == name + length - 1) {
        unsigned char space = DEVICE_SPACE_NONE;
        size_t align = 0;
        size_t plain = type_checker_split_device_qualifiers(name, open,
                                                            &space, &align);
        char *element_name = malloc(plain + 1);
        Type *element;
        Type *view;
        if (!element_name || plain == 0) {
          free(element_name);
          *out = NULL;
          return 1;
        }
        memcpy(element_name, name, plain);
        element_name[plain] = '\0';
        element = type_checker_get_type_by_name(checker, element_name);
        free(element_name);
        if (!element) {
          *out = NULL;
          return 1;
        }
        view = type_checker_static_view_of(checker, element, name, extents,
                                           rank);
        if (view) {
          view->device_space = space;
          view->declared_align = align;
          *out = view;
          return 1;
        }
      }
    }
  }
  return 0;
}

static int type_checker_parse_slice(TypeChecker *checker, const char *name, Type **out) {
  size_t length = strlen(name);
  if (length > 4 && strcmp(name + length - 4, "[..]") == 0) {
    char *element_name = malloc(length - 3);
    Type *element = NULL;
    if (!element_name) {
      *out = NULL;
      return 1;
    }
    memcpy(element_name, name, length - 4);
    element_name[length - 4] = '\0';
    element = type_checker_get_type_by_name(checker, element_name);
    free(element_name);
    *out = element ? type_checker_slice_of(checker, element) : NULL;
    return 1;
  }
  if (length > 2 && name[length - 2] == '[' && name[length - 1] == ']') {
    size_t head = length - 2;
    unsigned char space = DEVICE_SPACE_NONE;
    size_t align = 0;
    size_t plain = type_checker_split_device_qualifiers(name, head, &space,
                                                        &align);
    char *element_name = malloc(plain + 1);
    char *qualifiers = NULL;
    Type *element = NULL;
    Type *slice = NULL;
    if (!element_name || plain == 0) {
      free(element_name);
      *out = NULL;
      return 1;
    }
    memcpy(element_name, name, plain);
    element_name[plain] = '\0';
    if (plain != head) {
      qualifiers = type_checker_qualifier_text(name, head, plain);
      if (!qualifiers) {
        free(element_name);
        *out = NULL;
        return 1;
      }
    }
    element = type_checker_get_type_by_name(checker, element_name);
    free(element_name);
    slice = element ? type_checker_device_slice_of(checker, element, space,
                                                   align, qualifiers)
                    : NULL;
    free(qualifiers);
    *out = slice;
    return 1;
  }
  if (length > 3 && name[length - 1] == ']' && name[length - 2] == ',') {
    size_t open = length - 2;
    while (open > 0 && name[open] == ',') {
      open--;
    }
    if (name[open] == '[' && open > 0) {
      size_t rank = length - 1 - open;
      unsigned char space = DEVICE_SPACE_NONE;
      size_t align = 0;
      size_t plain = type_checker_split_device_qualifiers(name, open, &space,
                                                          &align);
      char *element_name = malloc(plain + 1);
      char *qualifiers = NULL;
      Type *element = NULL;
      Type *view = NULL;
      if (!element_name || plain == 0) {
        free(element_name);
        *out = NULL;
        return 1;
      }
      memcpy(element_name, name, plain);
      element_name[plain] = '\0';
      if (plain != open) {
        qualifiers = type_checker_qualifier_text(name, open, plain);
        if (!qualifiers) {
          free(element_name);
          *out = NULL;
          return 1;
        }
      }
      element = type_checker_get_type_by_name(checker, element_name);
      free(element_name);
      view = element ? type_checker_device_view_of(checker, element, rank,
                                                   space, align, qualifiers)
                     : NULL;
      free(qualifiers);
      *out = view;
      return 1;
    }
  }
  return 0;
}

Type *type_checker_get_type_by_name(TypeChecker *checker, const char *name) {
  Type *named = NULL;
  if (!checker || !name)
    return NULL;

  // Check built-in types by name
  {
    Type *builtin = NULL;
    if (type_checker_builtin_by_name(checker, name, &builtin)) {
      return builtin;
    }
  }

  /* A layout is part of the type, so it is peeled first and stamped on what is
     left. `float16[128,64] layout swizzle128` is a swizzled view of the same
     shape as the row-major one, and neither flows into the other. */
  if (type_checker_parse_view_layout(checker, name, &named)) {
    return named;
  }

  /* `T[128,64]`: a view whose extents are in its type. It is a pointer and a
     shape, so nothing travels beside the data and every index into one is
     bounded by the declaration. */
  if (type_checker_parse_extent_view(checker, name, &named)) {
    return named;
  }

  /* `T[]`: a slice, which is `T*` and a length in one value. The brackets are
   * empty because the length is not part of the type. `T[..]` is the same type
   * written where a parameter gathers its arguments. */
  if (type_checker_parse_slice(checker, name, &named)) {
    return named;
  }

  /* A parenthesised type. Bare, it is the type inside; suffixed, the array and
   * pointer branches below strip the suffix and land back here on the head. */
  if (name[0] == '(') {
    const char *scan;
    const char *close = NULL;
    int depth = 0;
    for (scan = name; *scan; scan++) {
      if (*scan == '(') {
        depth++;
      } else if (*scan == ')') {
        depth--;
        if (depth == 0) {
          close = scan;
          break;
        }
      }
    }
    if (close && close[1] == '\0') {
      size_t inner_len = (size_t)(close - name) - 1;
      char *inner = malloc(inner_len + 1);
      Type *inner_type;
      if (!inner) {
        return NULL;
      }
      memcpy(inner, name + 1, inner_len);
      inner[inner_len] = '\0';
      inner_type = type_checker_get_type_by_name(checker, inner);
      free(inner);
      return inner_type;
    }
  }

  // Check for function pointer types: fn(...)->R (thin) or Fn(...)->R (closure).
  if (strncmp(name, "fn(", 3) == 0 || strncmp(name, "Fn(", 3) == 0) {
    Type *fp_type = type_checker_parse_function_pointer_type(checker, name);
    if (fp_type) {
      return fp_type;
    }
  }

  if (strchr(name, '[') && strchr(name, ']')) {
    Type *array_type = type_checker_parse_array_type(checker, name);
    if (array_type) {
      return array_type;
    }
  }

  if (strchr(name, '*')) {
    Type *pointer_type = type_checker_parse_pointer_type(checker, name);
    if (pointer_type) {
      return pointer_type;
    }
  }

  /* `volatile T`. The qualifier binds to the value being accessed, so
   * `volatile uint16*` is a pointer to volatile uint16: the pointer branch
   * above strips the `*` first and lands back here on the element. */
  if (strncmp(name, "volatile ", 9) == 0) {
    Type *base = type_checker_get_type_by_name(checker, name + 9);
    if (base) {
      return type_checker_volatile_of(checker, base);
    }
    return NULL;
  }

  // Check for user-defined types in symbol table
  Symbol *struct_symbol = symbol_table_lookup(checker->symbol_table, name);
  if (struct_symbol && (struct_symbol->kind == SYMBOL_STRUCT ||
                        struct_symbol->kind == SYMBOL_ENUM)) {
    return struct_symbol->type;
  }

  // Check for generic enum instantiation: "Option<int32>", "Result<int64,string>"
  // Syntax stored by the parser as "Name<arg>" or "Name<arg1,arg2>"
  const char *lt = strchr(name, '<');
  if (lt && name[strlen(name) - 1] == '>') {
    size_t base_len = (size_t)(lt - name);
    char *base_name = malloc(base_len + 1);
    if (base_name) {
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      const char *arg_start = lt + 1;
      const char *arg_end = name + strlen(name) - 1;
      size_t arg_len = (size_t)(arg_end - arg_start);
      char *arg_str = malloc(arg_len + 1);
      if (arg_str) {
        memcpy(arg_str, arg_start, arg_len);
        arg_str[arg_len] = '\0';
        Type *result =
            type_checker_instantiate_declared_type(checker, base_name, arg_str);
        if (!result) {
          result =
              type_checker_instantiate_generic_enum(checker, base_name, arg_str);
        }
        free(arg_str);
        free(base_name);
        if (result)
          return result;
      } else {
        free(base_name);
      }
    }
  }

  return NULL;
}

int type_checker_is_integer_type(Type *type) {
  if (!type)
    return 0;

  switch (type->kind) {
  case TYPE_INT8:
  case TYPE_INT16:
  case TYPE_INT32:
  case TYPE_INT64:
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
  case TYPE_BOOL:
  case TYPE_CHAR:
    return 1;
  default:
    return 0;
  }
}

int type_checker_is_discrete_type(Type *type) {
  return type_checker_is_integer_type(type) ||
         (type && type->kind == TYPE_ENUM);
}

int type_checker_is_floating_type(Type *type) {
  if (!type)
    return 0;

  switch (type->kind) {
  case TYPE_FLOAT32:
  case TYPE_FLOAT64:
  case TYPE_FLOAT16:
  case TYPE_BFLOAT16:
    return 1;
  default:
    return 0;
  }
}

int type_checker_is_numeric_type(Type *type) {
  return type_checker_is_integer_type(type) ||
         type_checker_is_floating_type(type);
}

// Type inference and promotion functions implementation

static Type *type_checker_promote_base_types(TypeChecker *checker, Type *left,
                                             Type *right,
                                             const char *operator);

Type *type_checker_promote_types(TypeChecker *checker, Type *left, Type *right,
                                 const char *operator) {
  Type *result;
  Type *unit = NULL;
  if (!checker || !left || !right || !operator)
    return NULL;
  if (left->refined_base || right->refined_base) {
    int left_refined = left->refined_base != NULL;
    int right_refined = right->refined_base != NULL;
    if (left_refined && right_refined &&
        !type_checker_types_equal(left, right) &&
        (!left->refinement || !right->refinement)) {
      return NULL;
    }
    if (left_refined && !left->refinement) {
      unit = left;
    } else if (right_refined && !right->refinement) {
      unit = right;
    }
    result = type_checker_promote_base_types(
        checker, type_checker_refinement_base(left),
        type_checker_refinement_base(right), operator);
    if (unit && result &&
        type_checker_types_equal(result, type_checker_refinement_base(unit)) &&
        (strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0 ||
         strcmp(operator, "*") == 0 || strcmp(operator, "/") == 0)) {
      if (left_refined && right_refined) {
        return strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0
                   ? unit
                   : result;
      }
      if (strcmp(operator, "/") == 0 && unit == right) {
        return result;
      }
      return unit;
    }
    return result;
  }
  return type_checker_promote_base_types(checker, left, right, operator);
}

static Type *type_checker_promote_base_types(TypeChecker *checker, Type *left,
                                             Type *right,
                                             const char *operator) {
  if (!checker || !left || !right || !operator)
    return NULL;

  // For comparison operators, result is always int32 (boolean represented as
  // int)
  if (strcmp(operator, "==") == 0 || strcmp(operator, "!=") == 0 ||
      strcmp(operator, "<") == 0 || strcmp(operator, "<=") == 0 ||
      strcmp(operator, ">") == 0 || strcmp(operator, ">=") == 0) {
    return checker->builtin_int32;
  }

  /* Character arithmetic promotes to int32, the way C promotes a char. `c -
   * 'a'` is an index and `c + 1` is the next code point; neither is a
   * character, and leaving them as one would print the answer as text.
   * Comparison is unaffected: it returned above, and `c == 'h'` still asks
   * whether two characters match. */
  if (left->kind == TYPE_CHAR) {
    left = checker->builtin_int32;
  }
  if (right->kind == TYPE_CHAR) {
    right = checker->builtin_int32;
  }

  /* A bool used as a bit pattern promotes the same way, because the answer is
   * a number and not a yes. `{flag | 2}` printed "true" where the value it
   * held was 3, and the shift lowered at 64 bits rather than the width it
   * reads as, so `flag << -11` answered 0 where `(int32)flag << -11` answered
   * 2097152. Arithmetic on a bool already promotes through the larger-type
   * rule below; only the bitwise operators, which have no case there, fell
   * through to "the left type" and kept it. `&&` and `||` are unaffected: they
   * return bool of their own accord, and a comparison returned above. */
  if (strcmp(operator, "&") == 0 || strcmp(operator, "|") == 0 ||
      strcmp(operator, "^") == 0 || strcmp(operator, "<<") == 0 ||
      strcmp(operator, ">>") == 0) {
    if (left->kind == TYPE_BOOL) {
      left = checker->builtin_int32;
    }
    if (right->kind == TYPE_BOOL) {
      right = checker->builtin_int32;
    }
  }

  // For arithmetic operators, promote to larger type
  if (strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0 ||
      strcmp(operator, "*") == 0 || strcmp(operator, "/") == 0 ||
      strcmp(operator, "%") == 0) {

    // If either operand is floating-point, result is floating-point
    if (type_checker_is_floating_type(left) ||
        type_checker_is_floating_type(right)) {
      int left_is_small = left->kind == TYPE_FLOAT16 || left->kind == TYPE_BFLOAT16;
      int right_is_small = right->kind == TYPE_FLOAT16 || right->kind == TYPE_BFLOAT16;
      if ((left_is_small || right_is_small) &&
          left->kind != TYPE_FLOAT64 && right->kind != TYPE_FLOAT64) {
        return checker->builtin_float32;
      }
      if (left_is_small && right_is_small) {
        return checker->builtin_float32;
      }
      return type_checker_get_larger_type(checker, left, right);
    }

    // Both are integers, promote to larger integer type
    if (type_checker_is_integer_type(left) &&
        type_checker_is_integer_type(right)) {
      return type_checker_get_larger_type(checker, left, right);
    }
  }

  // For logical operators, result is int32 (boolean)
  if (strcmp(operator, "&&") == 0 || strcmp(operator, "||") == 0) {
    return checker->builtin_int32;
  }

  // Default: return left type
  return left;
}

Type *type_checker_get_larger_type(TypeChecker *checker, Type *type1,
                                   Type *type2) {
  if (!checker || !type1 || !type2)
    return NULL;

  int rank1 = type_checker_get_type_rank(type1);
  int rank2 = type_checker_get_type_rank(type2);

  // Return the type with higher rank
  return (rank1 >= rank2) ? type1 : type2;
}

int type_checker_get_type_rank(Type *type) {
  if (!type)
    return -1;

  // Type promotion ranking (higher number = higher rank)
  switch (type->kind) {
  case TYPE_INT8:
  case TYPE_UINT8:
  case TYPE_CHAR:
    return 1;
  case TYPE_INT16:
  case TYPE_UINT16:
    return 2;
  case TYPE_INT32:
  case TYPE_UINT32:
    return 3;
  case TYPE_FLOAT16:
  case TYPE_BFLOAT16:
  case TYPE_FLOAT32:
    return 4;
  case TYPE_INT64:
  case TYPE_UINT64:
    return 5;
  case TYPE_FLOAT64:
    return 6;
  case TYPE_STRING:
    return 10; // Special case - strings don't promote with numbers
  default:
    return 0;
  }
}

// Type compatibility and conversion functions implementation

int type_checker_is_cast_valid(Type *from, Type *to) {
  if (!from || !to)
    return 0;
  if (from->refined_base || to->refined_base) {
    if (type_checker_types_equal(from, to)) {
      return 1;
    }
    return type_checker_is_cast_valid(type_checker_refinement_base(from),
                                      type_checker_refinement_base(to));
  }

  /* Reflection types have no runtime representation, so they cannot be
   * cast to or from anything, including each other. */
  if (type_is_comptime_only(from) || type_is_comptime_only(to))
    return type_checker_types_equal(from, to);

  if (type_checker_types_equal(from, to))
    return 1;

  // Numeric <-> numeric
  if (type_checker_is_numeric_type(from) && type_checker_is_numeric_type(to))
    return 1;

  if ((from->kind == TYPE_ENUM && type_checker_is_integer_type(to)) ||
      (type_checker_is_integer_type(from) && to->kind == TYPE_ENUM)) {
    return 1;
  }

  /* A `string` reaches a pointer or an integer as its characters, which is
   * exactly what the implicit coercion at a `cstring` binding already does. A
   * cast must never be more restrictive than the conversion it spells out:
   * while it was, `(int64)"main"` was refused although "main" passed to a
   * cstring parameter and cast there was fine, so a one-line identity wrapper
   * defeated the rule. A restriction a wrapper defeats is in the wrong place. */
  if (from->kind == TYPE_STRING &&
      (to->kind == TYPE_POINTER || to->kind == TYPE_FUNCTION_POINTER ||
       type_checker_is_integer_type(to))) {
    return 1;
  }

  // Pointer <-> pointer
  if (from->kind == TYPE_POINTER && to->kind == TYPE_POINTER)
    return 1;

  // Integer <-> pointer
  if ((type_checker_is_integer_type(from) && to->kind == TYPE_POINTER) ||
      (from->kind == TYPE_POINTER && type_checker_is_integer_type(to))) {
    return 1;
  }

  // Pointer <-> function pointer
  if ((from->kind == TYPE_POINTER && to->kind == TYPE_FUNCTION_POINTER) ||
      (from->kind == TYPE_FUNCTION_POINTER && to->kind == TYPE_POINTER)) {
    return 1;
  }

  // Integer <-> function pointer
  if ((type_checker_is_integer_type(from) &&
       to->kind == TYPE_FUNCTION_POINTER) ||
      (from->kind == TYPE_FUNCTION_POINTER &&
       type_checker_is_integer_type(to))) {
    return 1;
  }

  // Function pointer <-> function pointer
  if (from->kind == TYPE_FUNCTION_POINTER &&
      to->kind == TYPE_FUNCTION_POINTER) {
    return 1;
  }

  return 0;
}

// Type compatibility and conversion functions implementation

static int type_checker_pointer_conversion_allowed(Type *dest_type,
                                                   Type *src_type) {
  if (type_checker_is_cstring_type(dest_type) &&
      src_type->kind == TYPE_STRING) {
    return 1;
  }

  /* A fixed array becomes a slice of the same element: the length the type
     carried becomes the length the value carries. Nothing is lost, and it is
     the conversion that lets a function be written once for any extent. */
  if (dest_type->kind == TYPE_SLICE && src_type->kind == TYPE_ARRAY &&
      dest_type->base_type &&
      /* An array says nothing about which device memory it sits in, so it
         cannot become a view that claims one. Whichever space the array's
         binding has is the one to write. */
      dest_type->device_space == DEVICE_SPACE_NONE &&
      dest_type->declared_align == 0) {
    Type *inner = src_type;
    size_t rank = type_view_rank(dest_type);
    for (size_t level = 0; level < rank; level++) {
      if (!inner || inner->kind != TYPE_ARRAY) {
        inner = NULL;
        break;
      }
      inner = inner->base_type;
    }
    if (inner && type_checker_types_equal(dest_type->base_type, inner)) {
      return 1;
    }
  }

  /* A rawptr is an address with no element type, so it converts to and from
   * every pointer in both directions. That is the whole of the opaque-pointer
   * contract, and it is what lets `var a: int32* = malloc(n);` be written
   * without a cast and `free(a)` without pretending the bytes are characters.
   * An array decays to it the same way it decays to a typed pointer, and a
   * string's bytes are an address like any other -- every rawptr consumer
   * takes an explicit length, so no terminator is implied the way a cstring
   * implies one. */
  if (type_checker_is_rawptr_type(dest_type) &&
      (src_type->kind == TYPE_POINTER || src_type->kind == TYPE_ARRAY ||
       src_type->kind == TYPE_FUNCTION_POINTER ||
       src_type->kind == TYPE_STRING)) {
    return 1;
  }
  if (type_checker_is_rawptr_type(src_type) &&
      (dest_type->kind == TYPE_POINTER ||
       dest_type->kind == TYPE_FUNCTION_POINTER)) {
    return 1;
  }

  /* Allow int8* (e.g. from &array[0] for int8[]) to cstring (uint8*) for C interop */
  if (dest_type->kind == TYPE_POINTER && src_type->kind == TYPE_POINTER &&
      dest_type->name && strcmp(dest_type->name, "cstring") == 0 &&
      src_type->base_type && src_type->base_type->name &&
      strcmp(src_type->base_type->name, "int8") == 0) {
    return 1;
  }

  /* Allow array to pointer decay (T[N] to T*) for function arguments */
  if (dest_type->kind == TYPE_POINTER && src_type->kind == TYPE_ARRAY &&
      dest_type->device_space == DEVICE_SPACE_NONE &&
      dest_type->declared_align == 0 && dest_type->base_type &&
      src_type->base_type &&
      type_checker_types_equal(dest_type->base_type, src_type->base_type)) {
    return 1;
  }
  return 0;
}

int type_checker_is_assignable(TypeChecker *checker, Type *dest_type,
                               Type *src_type) {
  if (type_is_comptime_only(dest_type) || type_is_comptime_only(src_type)) {
    return dest_type && src_type &&
           type_checker_types_equal(dest_type, src_type);
  }
  if (!checker || !dest_type || !src_type)
    return 0;
  if (dest_type->refined_base) {
    return type_checker_types_equal(dest_type, src_type);
  }
  if (src_type->refined_base) {
    src_type = type_checker_refinement_base(src_type);
  }

  /* A closure (function-pointer type carrying an environment) and a thin
   * function pointer are not interchangeable: a thin call site dispatches
   * without the environment, and a closure call site reads a code pointer the
   * thin value does not carry. Closures cross boundaries only as `Fn(...)->R`. */
  {
    int src_is_closure = src_type->kind == TYPE_FUNCTION_POINTER &&
                         src_type->closure_env;
    int dst_is_closure = dest_type->kind == TYPE_FUNCTION_POINTER &&
                         dest_type->closure_env;
    int src_is_thin_fn =
        src_type->kind == TYPE_FUNCTION_POINTER && !src_type->closure_env;
    int dst_is_thin_fn =
        dest_type->kind == TYPE_FUNCTION_POINTER && !dest_type->closure_env;
    if ((src_is_closure && dst_is_thin_fn) ||
        (dst_is_closure && src_is_thin_fn)) {
      return 0;
    }
    if (src_type->kind == TYPE_FUNCTION_POINTER &&
        dest_type->kind == TYPE_FUNCTION_POINTER &&
        type_checker_fn_signatures_equal(dest_type, src_type)) {
      return type_checker_fn_effects_flow(checker, dest_type, src_type);
    }
  }

  if (type_checker_types_equal(dest_type, src_type)) {
    return 1;
  }

  /* A device pointer forgets where its data lives for free: `T global*` flows
     into `T*`, which claims nothing. The other direction is a claim nobody
     proved, so it needs the explicit cast the interpreter re-checks, and one
     space where another is wanted is two different memories. An alignment
     claim travels the same way: a stronger one satisfies a weaker one. */
  if (dest_type->kind == TYPE_POINTER && src_type->kind == TYPE_POINTER &&
      (dest_type->device_space || src_type->device_space ||
       dest_type->declared_align || src_type->declared_align) &&
      type_checker_types_equal(dest_type->base_type, src_type->base_type)) {
    int space_ok = dest_type->device_space == src_type->device_space ||
                   dest_type->device_space == DEVICE_SPACE_NONE ||
                   dest_type->device_space == DEVICE_SPACE_GENERIC;
    int align_ok = dest_type->declared_align == 0 ||
                   (src_type->declared_align &&
                    src_type->declared_align % dest_type->declared_align == 0);
    return space_ok && align_ok;
  }

  /* A Mettle string can flow to a cstring by exposing its chars pointer. */
  if (type_checker_pointer_conversion_allowed(dest_type, src_type)) {
    return 1;
  }

  if (dest_type->kind == TYPE_POINTER || src_type->kind == TYPE_POINTER ||
      dest_type->kind == TYPE_ARRAY || src_type->kind == TYPE_ARRAY ||
      dest_type->kind == TYPE_STRUCT || src_type->kind == TYPE_STRUCT) {
    return 0;
  }

  // Check for safe implicit conversions
  return type_checker_is_implicitly_convertible(src_type, dest_type);
}

/* Operators whose low N bits are decided by the operands' low N bits alone, so
 * computing them in a wider type and truncating gives what the narrow type
 * would have given. `/` and `%` are not here: they read the whole value. `>>`
 * is not here either: it feeds high bits downward. */
static int type_checker_op_keeps_low_bits(const char *op) {
  if (!op) {
    return 0;
  }
  return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
         strcmp(op, "*") == 0 || strcmp(op, "&") == 0 ||
         strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
         strcmp(op, "<<") == 0;
}

/* Is this expression already an expression OF the destination type, spelled
 * with a literal that a narrower type cannot hold on its own?
 *
 * `var n: int8 = s - 1;` where s is an int8 is that shape. The literal 1 has no
 * type of its own until something gives it one, so the subtraction typed as
 * int32 and the store was reported as a narrowing -- a cast that said nothing
 * the destination had not already said. Reading the literal at the destination
 * type instead, the arithmetic is int8 arithmetic and the result is the same
 * value the cast produced.
 *
 * Every leaf must fit the destination and every operator must be one whose
 * result's low bits come only from its operands' low bits, so nothing is lost
 * that the destination would have kept. Anything reaching a genuinely wider
 * value -- an int64 variable, a call result, a divide -- still narrows loudly. */
static int type_checker_expression_is_destination_width(TypeChecker *checker,
                                                        Type *dest_type,
                                                        ASTNode *expression,
                                                        int depth) {
  if (!checker || !dest_type || !expression || depth > 32) {
    return 0;
  }

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    long long value = 0;
    if (!literal || literal->is_float) {
      return 0;
    }
    if (!type_checker_eval_integer_constant_with_checker(checker, expression,
                                                         &value)) {
      return 0;
    }
    return type_checker_constant_fits_type(dest_type, expression->resolved_type,
                                           value);
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    if (!binary || !type_checker_op_keeps_low_bits(binary->operator)) {
      return 0;
    }
    return type_checker_expression_is_destination_width(checker, dest_type,
                                                        binary->left,
                                                        depth + 1) &&
           type_checker_expression_is_destination_width(checker, dest_type,
                                                        binary->right,
                                                        depth + 1);
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    if (!unary || !unary->operator|| !unary->operand) {
      return 0;
    }
    if (strcmp(unary->operator, "-") != 0 && strcmp(unary->operator, "+") != 0 &&
        strcmp(unary->operator, "~") != 0) {
      return 0;
    }
    return type_checker_expression_is_destination_width(checker, dest_type,
                                                        unary->operand,
                                                        depth + 1);
  }

  default:
    break;
  }

  /* Any other leaf: a variable, a field, a call. It counts only when its own
   * type already reaches the destination without losing anything. */
  return expression->resolved_type &&
         type_checker_is_integer_type(expression->resolved_type) &&
         expression->resolved_type->kind != TYPE_ENUM &&
         type_checker_int_conversion_is_value_preserving(
             expression->resolved_type, dest_type);
}

static int assignable_widens_effects(TypeChecker *checker, Type *dest_type,
                                     Type *src_type, ASTNode *src_expr) {
  return checker && dest_type && src_type && src_expr &&
         dest_type->kind == TYPE_FUNCTION_POINTER &&
         src_type->kind == TYPE_FUNCTION_POINTER &&
         !src_type->fn_effects_closed && src_type->fn_require_count == 0 &&
         (dest_type->closure_env != NULL) ==
             (src_type->closure_env != NULL) &&
         type_checker_fn_signatures_equal(dest_type, src_type);
}

static int assignable_device_view(TypeChecker *checker, Type *dest_type,
                                  Type *src_type, ASTNode *src_expr) {
  return checker && dest_type && src_type && src_expr &&
         dest_type->kind == TYPE_SLICE && src_type->kind == TYPE_SLICE &&
         dest_type->view_extents[0] > 0 &&
         dest_type->device_space != DEVICE_SPACE_NONE &&
         src_type->device_space == DEVICE_SPACE_NONE &&
         dest_type->view_layout == src_type->view_layout &&
         dest_type->view_layout_param == src_type->view_layout_param &&
         type_view_rank(dest_type) == type_view_rank(src_type) &&
         dest_type->view_extents[0] == src_type->view_extents[0] &&
         dest_type->view_extents[1] == src_type->view_extents[1] &&
         dest_type->view_extents[2] == src_type->view_extents[2] &&
         dest_type->view_extents[3] == src_type->view_extents[3] &&
         type_checker_types_equal(dest_type->base_type, src_type->base_type) &&
         type_checker_lvalue_device_space(checker, src_expr) ==
             dest_type->device_space;
}

static int assignable_device_array(TypeChecker *checker, Type *dest_type,
                                   Type *src_type, ASTNode *src_expr) {
  return checker && dest_type && src_type && src_expr &&
         src_type->kind == TYPE_ARRAY && dest_type->declared_align == 0 &&
         (dest_type->kind == TYPE_POINTER ||
          (dest_type->kind == TYPE_SLICE && type_view_rank(dest_type) == 1)) &&
         dest_type->device_space != DEVICE_SPACE_NONE &&
         dest_type->base_type && src_type->base_type &&
         type_checker_types_equal(dest_type->base_type, src_type->base_type) &&
         type_checker_lvalue_device_space(checker, src_expr) ==
             dest_type->device_space;
}

static int assignable_is_literal(const ASTNode *src_expr) {
  return src_expr->type == AST_NUMBER_LITERAL ||
         src_expr->type == AST_STRING_LITERAL ||
         (src_expr->type == AST_UNARY_EXPRESSION &&
          ((UnaryExpression *)src_expr->data)->operand &&
          ((UnaryExpression *)src_expr->data)->operand->type ==
              AST_NUMBER_LITERAL);
}

static int assignable_refuse(TypeChecker *checker, const char *message) {
  free(checker->refine_failure);
  checker->refine_failure = strdup(message);
  return 0;
}

static int assignable_to_refinement(TypeChecker *checker, Type *dest_type,
                                    Type *src_type, ASTNode *src_expr) {
  Type *dest_base = type_checker_refinement_base(dest_type);
  char message[320];

  if (!type_checker_is_assignable_from(checker, dest_base, src_type,
                                       src_expr) ||
      !src_expr) {
    return 0;
  }
  if (dest_type->refinement) {
    return type_checker_prove_refinement(checker, dest_type, src_expr);
  }
  if (src_type->refined_base) {
    snprintf(message, sizeof(message),
             "'%s' and '%s' are different declared types and do not mix; "
             "convert one of them where the meaning is decided: (%s)value",
             src_type->name ? src_type->name : "?",
             dest_type->name ? dest_type->name : "?",
             dest_type->name ? dest_type->name : "?");
    return assignable_refuse(checker, message);
  }
  if (!assignable_is_literal(src_expr)) {
    snprintf(message, sizeof(message),
             "'%s' is a declared type; a plain '%s' becomes one where the "
             "meaning is decided: (%s)value",
             dest_type->name ? dest_type->name : "?",
             src_type->name ? src_type->name : "?",
             dest_type->name ? dest_type->name : "?");
    return assignable_refuse(checker, message);
  }
  src_expr->proven_refinement = dest_type;
  return 1;
}

static int assignable_narrows_float(Type *dest_type, ASTNode *src_expr) {
  NumberLiteral *literal;

  if (src_expr->type != AST_NUMBER_LITERAL || !src_expr->data) {
    return 0;
  }
  literal = (NumberLiteral *)src_expr->data;
  if (!literal->is_float) {
    return 0;
  }
  return dest_type->kind == TYPE_FLOAT16
             ? mettle_f64_is_exact_f16(literal->float_value)
             : mettle_f64_is_exact_bf16(literal->float_value);
}

int type_checker_is_assignable_from(TypeChecker *checker, Type *dest_type,
                                    Type *src_type, ASTNode *src_expr) {
  long long folded = 0;

  if (assignable_widens_effects(checker, dest_type, src_type, src_expr)) {
    const char *named = type_checker_function_value_name(checker, src_expr);
    if (named) {
      return type_checker_add_effect_obligation(
          checker, named,
          dest_type->fn_effect_signature ? dest_type->fn_effect_signature
                                         : "open",
          src_expr->location);
    }
  }
  if (type_checker_is_assignable(checker, dest_type, src_type) ||
      assignable_device_view(checker, dest_type, src_type, src_expr) ||
      assignable_device_array(checker, dest_type, src_type, src_expr)) {
    return 1;
  }
  if (checker && dest_type && src_type && dest_type->refined_base) {
    return assignable_to_refinement(checker, dest_type, src_type, src_expr);
  }
  if (src_type && src_type->refined_base) {
    src_type = type_checker_refinement_base(src_type);
    if (type_checker_is_assignable(checker, dest_type, src_type)) {
      return 1;
    }
  }
  if (src_expr && checker && dest_type && src_type &&
      (dest_type->kind == TYPE_FLOAT16 || dest_type->kind == TYPE_BFLOAT16) &&
      (src_type->kind == TYPE_FLOAT32 || src_type->kind == TYPE_FLOAT64)) {
    return assignable_narrows_float(dest_type, src_expr);
  }
  if (!src_expr || !checker || !dest_type || !src_type ||
      !type_checker_is_integer_type(dest_type) ||
      !type_checker_is_integer_type(src_type) ||
      dest_type->kind == TYPE_ENUM || src_type->kind == TYPE_ENUM) {
    return 0;
  }
  if (!type_checker_eval_integer_constant_with_checker(checker, src_expr,
                                                       &folded)) {
    return type_checker_expression_is_destination_width(checker, dest_type,
                                                        src_expr, 0);
  }
  return type_checker_constant_fits_type(dest_type, src_type, folded);
}

int type_checker_integer_bounds(const Type *type, long long *out_min,
                                unsigned long long *out_max) {
  long long min = 0;
  unsigned long long max = 0;

  if (!type) {
    return 0;
  }
  switch (type->kind) {
  /* A bool holds 0 or 1, so it widens into every integer type. */
  case TYPE_BOOL:   min = 0;         max = 1ULL;       break;
  case TYPE_INT8:   min = INT8_MIN;  max = INT8_MAX;   break;
  case TYPE_INT16:  min = INT16_MIN; max = INT16_MAX;  break;
  case TYPE_INT32:  min = INT32_MIN; max = INT32_MAX;  break;
  case TYPE_INT64:  min = INT64_MIN; max = INT64_MAX;  break;
  case TYPE_UINT8:  min = 0;         max = UINT8_MAX;  break;
  case TYPE_CHAR:   min = 0;         max = UINT8_MAX;  break;
  case TYPE_UINT16: min = 0;         max = UINT16_MAX; break;
  case TYPE_UINT32: min = 0;         max = UINT32_MAX; break;
  case TYPE_UINT64: min = 0;         max = UINT64_MAX; break;
  default:
    return 0;
  }
  if (out_min) {
    *out_min = min;
  }
  if (out_max) {
    *out_max = max;
  }
  return 1;
}

int type_checker_int_conversion_is_value_preserving(const Type *from,
                                                    const Type *to) {
  long long from_min = 0, to_min = 0;
  unsigned long long from_max = 0, to_max = 0;

  if (!from || !to) {
    return 0;
  }
  if (!type_checker_integer_bounds(to, &to_min, &to_max)) {
    return 0;
  }

  /* An enum's value set is written down, so containment is decidable exactly
   * rather than approximated by width: the conversion is value-preserving when
   * every declared member fits. */
  if (from->kind == TYPE_ENUM) {
    if (from->enum_member_count == 0 || !from->enum_member_values) {
      return 0;
    }
    for (size_t i = 0; i < from->enum_member_count; i++) {
      long long value = from->enum_member_values[i];
      if (value < to_min) {
        return 0;
      }
      if (value >= 0 && (unsigned long long)value > to_max) {
        return 0;
      }
    }
    return 1;
  }

  if (!type_checker_integer_bounds(from, &from_min, &from_max)) {
    return 0;
  }
  return to_min <= from_min && to_max >= from_max;
}

int type_checker_constant_fits_type(const Type *dest_type, const Type *src_type,
                                    long long value) {
  long long dest_min = 0;
  unsigned long long dest_max = 0;

  if (!type_checker_integer_bounds(dest_type, &dest_min, &dest_max)) {
    return 0;
  }
  /* The folder carries every constant in a long long, so a value typed
   * unsigned above INT64_MAX arrives as a negative bit pattern. Read it back
   * with the signedness the source was given, not the container's. */
  if (src_type && (src_type->kind == TYPE_UINT8 ||
                   src_type->kind == TYPE_UINT16 ||
                   src_type->kind == TYPE_UINT32 ||
                   src_type->kind == TYPE_UINT64)) {
    return (unsigned long long)value <= dest_max;
  }
  if (value < 0) {
    return dest_min <= value;
  }
  return (unsigned long long)value <= dest_max;
}

int type_checker_is_implicitly_convertible(Type *from_type, Type *to_type) {
  if (!from_type || !to_type)
    return 0;

  // Same type is always convertible
  if (from_type->kind == to_type->kind) {
    return type_checker_types_equal(from_type, to_type);
  }

  /* Integer to integer: widen silently, narrow loudly. A conversion that can
   * change the value is written at the site, where a reader can see it; one
   * that cannot is not worth writing. Two destinations sit outside the rule
   * because they are not integer range conversions at all: `bool` is a truth
   * coercion (a comparison's result is an int32 that every `var b: bool = x >
   * y;` stores), and an enum names a set rather than a range. */
  if (type_checker_is_integer_type(from_type) &&
      type_checker_is_integer_type(to_type)) {
    if (to_type->kind == TYPE_BOOL) {
      return 1;
    }
    return type_checker_int_conversion_is_value_preserving(from_type, to_type);
  }

  // Integer to floating point conversions
  if (type_checker_is_integer_type(from_type) &&
      type_checker_is_floating_type(to_type)) {
    return 1; // Generally safe
  }

  if (type_checker_is_floating_type(from_type) &&
      type_checker_is_floating_type(to_type)) {
    int from_small = from_type->kind == TYPE_FLOAT16 || from_type->kind == TYPE_BFLOAT16;
    int to_small = to_type->kind == TYPE_FLOAT16 || to_type->kind == TYPE_BFLOAT16;
    if (to_small) {
      return 0;
    }
    if (from_small) {
      return 1;
    }
    return 1;
  }

  // No other implicit conversions are allowed
  return 0;
}

int type_checker_are_compatible(Type *type1, Type *type2) {
  if (!type1 || !type2)
    return 0;

  if (type_checker_types_equal(type1, type2)) {
    return 1;
  }

  /* Comparison and match-arm unification, not assignment. The narrowing rule
   * governs where a value is stored; `i < len` stores nothing, so both sides
   * are read at their own width and every integer stays comparable with every
   * other. */
  if (type_checker_is_integer_type(type1) &&
      type_checker_is_integer_type(type2)) {
    return 1;
  }

  if (type1->kind == TYPE_POINTER || type2->kind == TYPE_POINTER ||
      type1->kind == TYPE_ARRAY || type2->kind == TYPE_ARRAY ||
      type1->kind == TYPE_STRUCT || type2->kind == TYPE_STRUCT) {
    return 0;
  }

  // Check for implicit numeric conversions
  return type_checker_is_implicitly_convertible(type1, type2) ||
         type_checker_is_implicitly_convertible(type2, type1);
}

Type *type_checker_default_integer_literal_type(TypeChecker *checker,
                                                     NumberLiteral *literal) {
  if (!checker || !literal || literal->is_float) {
    return checker ? checker->builtin_int32 : NULL;
  }

  unsigned long long u_bitpat = (unsigned long long)literal->int_value;
  unsigned char radix = literal->int_radix;
  if (radix != 2u && radix != 16u) {
    radix = 10u;
  }

  /*
   * Decimal defaults follow signed widening so large magnitudes usable with
   * unary minus (-2147483648 via -(int64)...). Hex/binary infer uint32 in the
   * (INT32_MAX, UINT32_MAX] range so 0xFFFFFFFF and similar stay uint32-ish.
   */
  if (radix == 10u) {
    /* A literal is never negative in source -- a leading '-' lexes as unary
     * minus -- so a negative bit pattern here is a decimal past LLONG_MAX that
     * the parser re-read unsigned. It is a uint64, and typing it int32 by its
     * bit pattern (18446744073709551615 reading as -1) is how it used to reach
     * codegen as the right bits for the wrong reason. */
    if (literal->int_value < 0) {
      return checker->builtin_uint64;
    }
    if (literal->int_value >= INT32_MIN && literal->int_value <= INT32_MAX) {
      return checker->builtin_int32;
    }
    if (u_bitpat <= (unsigned long long)INT64_MAX) {
      return checker->builtin_int64;
    }
    return checker->builtin_uint64;
  }

  if (u_bitpat <= (unsigned long long)INT32_MAX) {
    return checker->builtin_int32;
  }
  if (u_bitpat <= UINT32_MAX) {
    return checker->builtin_uint32;
  }
  if (u_bitpat <= (unsigned long long)INT64_MAX) {
    return checker->builtin_int64;
  }
  return checker->builtin_uint64;
}

Type *type_checker_canon_type(TypeChecker *checker, Type *type) {
  if (!checker || !type) {
    return type;
  }
  if (type->type_table_index != UINT32_MAX) {
    return type;
  }
  for (size_t i = 0; i < checker->type_table_count; i++) {
    Type *existing = checker->type_table[i];
    if (!existing || existing == type ||
        !type_checker_types_equal(existing, type)) {
      continue;
    }
    /* cstring and uint8* are both pointer-to-uint8 but are distinct types. */
    if (existing->name && type->name &&
        strcmp(existing->name, type->name) != 0) {
      continue;
    }
    type_destroy(type);
    return existing;
  }
  if (type_checker_intern_type(checker, type) == UINT32_MAX) {
    return type;
  }
  return type;
}

uint32_t type_checker_intern_type(TypeChecker *checker, Type *type) {
  if (!checker || !type) {
    return UINT32_MAX;
  }
  if (type->type_table_index != UINT32_MAX) {
    return type->type_table_index;
  }
  if (checker->type_table_count == checker->type_table_capacity) {
    size_t next = checker->type_table_capacity
                      ? checker->type_table_capacity * 2
                      : 32;
    Type **grown = realloc(checker->type_table, next * sizeof(Type *));
    if (!grown) {
      return UINT32_MAX;
    }
    checker->type_table = grown;
    checker->type_table_capacity = next;
  }
  if (checker->type_table_count > UINT32_MAX) {
    return UINT32_MAX;
  }
  uint32_t index = (uint32_t)checker->type_table_count;
  checker->type_table[checker->type_table_count++] = type;
  type->type_table_index = index;
  return index;
}

Type *type_checker_type_from_index(const TypeChecker *checker, uint32_t index) {
  if (!checker || index == UINT32_MAX ||
      (size_t)index >= checker->type_table_count) {
    return NULL;
  }
  return checker->type_table[index];
}

/* A type graph has cycles: `struct ArenaChunk { next: ArenaChunk* }` reaches
 * itself through its own field. Aggregates are recorded on the way down so a
 * cycle is walked once rather than forever. Only aggregates need recording --
 * pointer, array, slice and function types can only cycle by passing through
 * one. */
typedef struct {
  const Type **types;
  size_t count;
  size_t capacity;
} TypeVisitSet;

static int type_visit_set_enter(TypeVisitSet *seen, const Type *type) {
  for (size_t i = 0; i < seen->count; i++) {
    if (seen->types[i] == type) {
      return 0;
    }
  }
  if (seen->count == seen->capacity) {
    size_t next = seen->capacity ? seen->capacity * 2 : 16;
    const Type **grown = realloc(seen->types, next * sizeof(const Type *));
    if (!grown) {
      return 0; /* treat as already seen: stop descending rather than crash */
    }
    seen->types = grown;
    seen->capacity = next;
  }
  seen->types[seen->count++] = type;
  return 1;
}

static int type_contains_comptime_only_seen(const Type *type,
                                            TypeVisitSet *seen) {
  if (!type) {
    return 0;
  }
  if (type_is_comptime_only(type)) {
    return 1;
  }
  if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY ||
      type->kind == TYPE_SLICE) {
    return type_contains_comptime_only_seen(type->base_type, seen);
  }
  if (type->kind == TYPE_FUNCTION_POINTER) {
    if (type_contains_comptime_only_seen(type->fn_return_type, seen)) {
      return 1;
    }
    for (size_t i = 0; i < type->fn_param_count; i++) {
      if (type_contains_comptime_only_seen(type->fn_param_types[i], seen)) {
        return 1;
      }
    }
    return 0;
  }
  if (type->kind == TYPE_STRUCT) {
    if (!type_visit_set_enter(seen, type)) {
      return 0;
    }
    for (size_t i = 0; i < type->field_count; i++) {
      if (type_contains_comptime_only_seen(type->field_types[i], seen)) {
        return 1;
      }
    }
  }
  if (type->kind == TYPE_TAGGED_ENUM) {
    if (!type_visit_set_enter(seen, type)) {
      return 0;
    }
    for (size_t i = 0; i < type->tagged_variant_count; i++) {
      if (type_contains_comptime_only_seen(type->tagged_variant_payloads[i],
                                           seen)) {
        return 1;
      }
    }
  }
  return 0;
}

int type_contains_comptime_only(const Type *type) {
  TypeVisitSet seen = {NULL, 0, 0};
  int result = type_contains_comptime_only_seen(type, &seen);
  free(seen.types);
  return result;
}

Type *type_checker_type_value(TypeChecker *checker, Type *referred,
                              ASTNode *expression) {
  if (!checker || !referred || !checker->builtin_type) {
    return NULL;
  }
  uint32_t index = type_checker_intern_type(checker, referred);
  if (index == UINT32_MAX) {
    if (expression) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory while interning type '%s'",
          referred->name ? referred->name : "<anonymous>");
    }
    return NULL;
  }
  return checker->builtin_type;
}

Type *type_checker_field_value(TypeChecker *checker, Type *owner,
                               uint32_t field_index, ASTNode *expression) {
  if (!checker || !owner || !checker->builtin_field) {
    return NULL;
  }
  uint32_t index = type_checker_intern_type(checker, owner);
  if (index == UINT32_MAX) {
    if (expression) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory while interning type '%s'",
          owner->name ? owner->name : "<anonymous>");
    }
    return NULL;
  }
  (void)field_index;
  return checker->builtin_field;
}

/* True for the decimal literal `9223372036854775808`, the magnitude of int64's
 * minimum. A literal is never negative in source, so that number alone is past
 * LLONG_MAX and types uint64; negating it was then refused against a range the
 * diagnostic itself printed as containing the answer, which made int64's
 * minimum the one value nobody could write. Under a unary minus the number is
 * an int64 and the minus belongs to it. */
int type_checker_is_int64_min_magnitude(const ASTNode *operand) {
  const NumberLiteral *literal;
  if (!operand || operand->type != AST_NUMBER_LITERAL) {
    return 0;
  }
  literal = (const NumberLiteral *)operand->data;
  return literal && !literal->is_float && !literal->is_char &&
         literal->int_radix == 10 &&
         (unsigned long long)literal->int_value ==
             (unsigned long long)INT64_MAX + 1ull;
}
