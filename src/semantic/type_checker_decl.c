// Type checker: struct / enum / declaration processing.
#include "type_checker_internal.h"
#include "codegen/target.h"

static Type *type_checker_narrow_to_target_word(TypeChecker *checker,
                                               Type *type) {
  int bits = mtlc_target()->code_bits;
  size_t word;
  if (!checker || !type || bits >= 64) {
    return NULL;
  }
  word = (size_t)(bits / 8);
  if (type->size <= word) {
    return NULL;
  }
  switch (type->kind) {
  case TYPE_INT16:
  case TYPE_INT32:
  case TYPE_INT64:
    return type_checker_get_type_by_name(checker,
                                         bits == 16 ? "int16" : "int32");
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
    return type_checker_get_type_by_name(checker,
                                         bits == 16 ? "uint16" : "uint32");
  default:
    return NULL;
  }
}


/* The kernel ABI is intentionally explicit: a parameter is a POD scalar, a
 * pointer, or a record built from those. Rejecting strings, closures, and
 * function pointers here produces a source diagnostic instead of a late
 * target-emitter failure or, worse, an ABI mismatch. */
static int gpu_kernel_scalar_type(const Type *type) {
  if (!type) {
    return 0;
  }
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
  case TYPE_FLOAT32:
  case TYPE_FLOAT64:
  case TYPE_FLOAT16:
  case TYPE_BFLOAT16:
    return 1;
  default:
    return 0;
  }
}

static Symbol *find_enclosing_parameter(TypeChecker *checker,
                                        const char *name) {
  if (!checker || !checker->symbol_table || !name) {
    return NULL;
  }

  Scope *function_scope =
      symbol_table_get_current_scope(checker->symbol_table);
  while (function_scope && function_scope->type != SCOPE_FUNCTION) {
    function_scope = function_scope->parent;
  }
  if (!function_scope) {
    return NULL;
  }

  for (size_t i = 0; i < function_scope->symbol_count; i++) {
    Symbol *symbol = function_scope->symbols[i];
    if (symbol && symbol->kind == SYMBOL_PARAMETER && symbol->name &&
        strcmp(symbol->name, name) == 0) {
      return symbol;
    }
  }
  return NULL;
}

/* Can this expression become bytes in the object file's data section?
 *
 * A global has no initializer to run: its value is laid out at compile time, so
 * the initializer must be one of the shapes module lowering can fold. This
 * mirrors eval_numeric in src/frontend/mtlc_lower_module.c structurally (it does
 * not evaluate - folding still happens there, and a shape accepted here that
 * turns out not to fold is still caught downstream). Keep the two in step: a
 * shape added to eval_numeric belongs here too, or it gets rejected with a
 * misleading diagnostic. */
static int layoutable_global_initializer(TypeChecker *checker,
                                         const Type *declared,
                                         const ASTNode *expression,
                                         int top_level) {
  if (!expression) {
    return 1; /* no initializer: nothing to lay out */
  }

  /* A string global stores a pointer to one string literal's storage. There is
   * nowhere to run a concatenation, so nothing else qualifies. */
  if (declared && declared->kind == TYPE_STRING) {
    return expression->type == AST_STRING_LITERAL;
  }

  switch (expression->type) {
  case AST_NUMBER_LITERAL:
  case AST_STRING_LITERAL:
    return 1;
  /* Another global's folded value, a global const, or an enum member. An
   * extern's storage lives in another object file, so its value is not ours to
   * read at layout time. */
  case AST_IDENTIFIER: {
    const Identifier *identifier = (const Identifier *)expression->data;
    const Symbol *symbol =
        (checker && identifier && identifier->name)
            ? type_checker_resolve_identifier(checker, (Identifier *)identifier)
            : NULL;
    return !symbol || !symbol->is_extern;
  }
  case AST_UNARY_EXPRESSION: {
    const UnaryExpression *unary = (const UnaryExpression *)expression->data;
    if (!unary || !unary->operator|| !unary->operand) {
      return 0;
    }
    /* `&name` becomes a relocation filling the whole slot, so it is the entire
     * initializer or nothing: there is no addend for `&name + 1`. */
    if (strcmp(unary->operator, "&") == 0) {
      return top_level && unary->operand->type == AST_IDENTIFIER;
    }
    if (strcmp(unary->operator, "+") == 0 ||
        strcmp(unary->operator, "-") == 0 ||
        strcmp(unary->operator, "!") == 0 ||
        strcmp(unary->operator, "~") == 0) {
      return layoutable_global_initializer(checker, declared, unary->operand, 0);
    }
    return 0;
  }
  case AST_BINARY_EXPRESSION: {
    const BinaryExpression *binary = (const BinaryExpression *)expression->data;
    return binary && binary->left && binary->right &&
           layoutable_global_initializer(checker, declared, binary->left, 0) &&
           layoutable_global_initializer(checker, declared, binary->right, 0);
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *cast = (const CastExpression *)expression->data;
    return cast && cast->operand &&
           layoutable_global_initializer(checker, declared, cast->operand, 0);
  }
  /* `sizeof(T)` is a compile-time integer; every other call needs to run. */
  case AST_FUNCTION_CALL: {
    const CallExpression *call = (const CallExpression *)expression->data;
    return call && call->function_name &&
           strcmp(call->function_name, "sizeof") == 0;
  }
  default:
    return 0;
  }
}

static int gpu_kernel_value_type(const Type *type, int depth);

/* A record crosses the launch boundary as its own bytes, so every field has to
 * mean the same thing on the device as it does on the host. Scalars, fixed
 * arrays, pointers, and nested records do; a string, a closure, or a function
 * pointer is a host address with no device meaning. The depth bound keeps a
 * self-referential type from walking forever. */
int type_checker_gpu_abi_type(const Type *type) {
  return gpu_kernel_value_type(type, 0);
}

static int gpu_kernel_value_type(const Type *type, int depth) {
  if (!type || depth > 8) {
    return 0;
  }
  if (gpu_kernel_scalar_type(type)) {
    return 1;
  }
  switch (type->kind) {
  case TYPE_POINTER:
    return type->base_type && gpu_kernel_value_type(type->base_type, depth + 1);
  case TYPE_ARRAY:
    return type->base_type && gpu_kernel_value_type(type->base_type, depth + 1);
  case TYPE_STRUCT:
  case TYPE_SLICE:
    if (!type->field_count) {
      return 0;
    }
    for (size_t i = 0; i < type->field_count; i++) {
      if (!type->field_types ||
          !gpu_kernel_value_type(type->field_types[i], depth + 1)) {
        return 0;
      }
    }
    return 1;
  default:
    return 0;
  }
}

static int gpu_kernel_parameter_type(const Type *type) {
  return gpu_kernel_value_type(type, 0);
}

// Struct type processing functions

static int type_checker_claim_struct_placeholder(TypeChecker *checker,
                                                 const Type *type) {
  size_t i;
  if (!checker || !type) {
    return 0;
  }
  for (i = 0; i < checker->struct_placeholder_count; i++) {
    if (checker->struct_placeholders[i] == type) {
      checker->struct_placeholders[i] =
          checker->struct_placeholders[checker->struct_placeholder_count - 1];
      checker->struct_placeholder_count--;
      return 1;
    }
  }
  return 0;
}

int type_checker_declare_struct_placeholder(TypeChecker *checker,
                                            ASTNode *struct_decl) {
  StructDeclaration *decl;
  Type *struct_type;
  Symbol *struct_symbol;
  Type **grown;

  if (!checker || !struct_decl ||
      struct_decl->type != AST_STRUCT_DECLARATION) {
    return 0;
  }
  decl = (StructDeclaration *)struct_decl->data;
  if (!decl || !decl->name || decl->type_param_count > 0) {
    return 1;
  }
  if (symbol_table_lookup_current_scope(checker->symbol_table, decl->name)) {
    return 1;
  }

  struct_type = type_create(TYPE_STRUCT, decl->name);
  if (!struct_type) {
    return 0;
  }
  struct_symbol = symbol_create(decl->name, SYMBOL_STRUCT, struct_type);
  if (!struct_symbol) {
    type_destroy(struct_type);
    return 0;
  }
  if (!symbol_table_declare(checker->symbol_table, struct_symbol)) {
    symbol_destroy(struct_symbol);
    return 0;
  }

  if (checker->struct_placeholder_count ==
      checker->struct_placeholder_capacity) {
    size_t capacity = checker->struct_placeholder_capacity
                          ? checker->struct_placeholder_capacity * 2
                          : 8;
    grown = realloc(checker->struct_placeholders, capacity * sizeof(Type *));
    if (!grown) {
      return 0;
    }
    checker->struct_placeholders = grown;
    checker->struct_placeholder_capacity = capacity;
  }
  checker->struct_placeholders[checker->struct_placeholder_count++] =
      struct_type;
  return 1;
}

int type_checker_process_struct_declaration(TypeChecker *checker,
                                            ASTNode *struct_decl) {
  if (!checker || !struct_decl || struct_decl->type != AST_STRUCT_DECLARATION) {
    return 0;
  }

  StructDeclaration *decl = (StructDeclaration *)struct_decl->data;
  if (!decl || !decl->name) {
    return 0;
  }
  if (strcmp(decl->name, "Type") == 0 || strcmp(decl->name, "Field") == 0) {
    type_checker_set_error_at_location(
        checker, struct_decl->location,
        "'%s' is a reserved compile-time type name", decl->name);
    return 0;
  }

  // Check if struct already exists
  Symbol *existing =
      symbol_table_lookup_current_scope(checker->symbol_table, decl->name);
  Type *placeholder = NULL;
  if (existing) {
    if (type_checker_claim_struct_placeholder(checker, existing->type)) {
      placeholder = existing->type;
    } else {
      type_checker_report_duplicate_declaration(checker, struct_decl->location,
                                                decl->name);
      return 0;
    }
  }

  /* Self-referential structs (e.g. `next: Foo*` inside `struct Foo`) need
   * `Foo` resolvable as a base type while its own fields are being processed.
   * Register an empty placeholder struct type + symbol first; the pointer-type
   * parser only requires the base Type pointer to exist, not for its fields
   * to be populated. We fill in the field information in place once the
   * field types have all resolved. */
  Type *struct_type = placeholder;
  if (!struct_type) {
    Symbol *struct_symbol;
    struct_type = type_create(TYPE_STRUCT, decl->name);
    if (!struct_type) {
      return 0;
    }
    struct_symbol = symbol_create(decl->name, SYMBOL_STRUCT, struct_type);
    if (!struct_symbol) {
      type_destroy(struct_type);
      return 0;
    }
    if (!symbol_table_declare(checker->symbol_table, struct_symbol)) {
      symbol_destroy(struct_symbol);
      return 0;
    }
  }

  // Resolve field types now that the placeholder is visible.
  Type **field_types = malloc(decl->field_count * sizeof(Type *));
  if (!field_types) {
    return 0;
  }

  for (size_t i = 0; i < decl->field_count; i++) {
    field_types[i] =
        type_checker_get_type_by_name(checker, decl->field_types[i]);
    if (!field_types[i]) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Unknown type '%s' in struct field", decl->field_types[i]);
      type_checker_set_error_at_location(checker, struct_decl->location,
                                         error_msg);
      free(field_types);
      return 0;
    }
    if (type_checker_reject_no_runtime_repr(checker, struct_decl->location,
                                            field_types[i])) {
      free(field_types);
      return 0;
    }
    /* The placeholder registered above is what makes `next: Foo*` work, and it
     * also makes `a: Foo` resolve, to a type whose size is still 0. Layout then
     * gave the field no storage: `struct S { a: S; }` reached the backend at
     * size 0 as an internal compiler error, and `struct S { a: S; v: int64; }`
     * compiled with `a` silently overlapping `v`. */
    const Type *field_base = field_types[i];
    while (field_base && field_base->kind == TYPE_ARRAY) {
      field_base = field_base->base_type;
    }
    if (field_base == struct_type) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Struct '%s' contains itself by value in field '%s'. A struct "
               "cannot hold a copy of itself; store a pointer '%s*'",
               decl->name, decl->field_names[i], decl->name);
      type_checker_set_error_at_location(checker, struct_decl->location,
                                         error_msg);
      free(field_types);
      return 0;
    }
  }

  /* Populate the placeholder in place so pointers captured during field
   * resolution stay valid. Layout is computed here, in the frontend. */
  if (!type_alloc_fields(struct_type, decl->field_count)) {
    free(field_types);
    return 0;
  }
  for (size_t i = 0; i < decl->field_count; i++) {
    if (!type_set_field(struct_type, i, decl->field_names[i], field_types[i],
                        0)) {
      free(field_types);
      return 0;
    }
  }
  if (!type_compute_layout(struct_type)) {
    free(field_types);
    type_checker_set_error_at_location(
        checker, struct_decl->location,
        "Failed to compute layout for struct '%s'", decl->name);
    return 0;
  }
  /* Each field can sit within the single-object bound while the struct does
   * not. The backend keeps frame offsets and local storage in `int`, so three
   * 800 MB arrays in one struct arrived as a negative size and were reported
   * as an internal compiler error. */
  if (struct_type->size > (size_t)INT_MAX) {
    free(field_types);
    type_checker_set_error_at_location(
        checker, struct_decl->location,
        "Struct '%s' needs %zu bytes, past the %d-byte limit on one object",
        decl->name, struct_type->size, INT_MAX);
    return 0;
  }
  free(field_types);
  type_checker_intern_type(checker, struct_type);
  type_checker_set_qualified_name(checker, struct_type,
                                  struct_decl->location.filename);
  return 1;
}

// ---------------------------------------------------------------------------
// Helper: build a concrete TYPE_TAGGED_ENUM type and register its constructors.
// Called for plain (non-generic) tagged enum declarations and from the
// generic-instantiation path when we monomorphize "Option<int32>" on demand.
//
// The memory layout is:
//   offset 0              : int32 _tag  (4 bytes)
//   offset data_offset    : payload union (largest payload, alignment-padded)
// ---------------------------------------------------------------------------
static int type_checker_payload_is_self_pointer(const char *payload_type,
                                                const char *type_name) {
  size_t base_length;

  if (!payload_type || !type_name)
    return 0;
  base_length = strlen(payload_type);
  if (base_length == 0 || payload_type[base_length - 1] != '*')
    return 0;
  base_length--;
  return base_length == strlen(type_name) &&
         strncmp(payload_type, type_name, base_length) == 0;
}

Type *type_checker_build_tagged_enum_type(TypeChecker *checker,
                                                  const char *type_name,
                                                  EnumDeclaration *enum_decl) {
  if (!checker || !type_name || !enum_decl)
    return NULL;

  // Determine the max payload size and alignment
  size_t max_payload_size = 0;
  size_t max_payload_align = 1;
  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    const char *pt = enum_decl->variants[i].payload_type;
    if (!pt)
      continue;
    Type *payload_ty = type_checker_get_type_by_name(checker, pt);
    if (!payload_ty) {
      if (type_checker_payload_is_self_pointer(pt, type_name)) {
        if (sizeof(void *) > max_payload_size)
          max_payload_size = sizeof(void *);
        if (sizeof(void *) > max_payload_align)
          max_payload_align = sizeof(void *);
      }
      continue;
    }
    if (payload_ty->size > max_payload_size)
      max_payload_size = payload_ty->size;
    if (payload_ty->alignment > max_payload_align)
      max_payload_align = payload_ty->alignment;
  }

  // data starts at first offset >= 4 that satisfies alignment of payload
  size_t data_align = max_payload_align < 4 ? 4 : max_payload_align;
  // align_up(4, data_align) - tag is 4 bytes, then pad to data_align
  size_t data_offset = (4 + data_align - 1) & ~(data_align - 1);
  size_t total_size = max_payload_size > 0 ? data_offset + max_payload_size
                                           : data_offset;
  // Round up total to alignment
  total_size = (total_size + data_align - 1) & ~(data_align - 1);
  if (total_size < 8) total_size = 8; // at least 8 bytes

  Type *te = type_create(TYPE_TAGGED_ENUM, type_name);
  if (!te)
    return NULL;

  te->size = total_size;
  te->alignment = data_align;
  te->tagged_variant_count = enum_decl->variant_count;
  te->tagged_data_offset = data_offset;
  te->tagged_data_size = max_payload_size;

  te->tagged_variant_names =
      malloc(enum_decl->variant_count * sizeof(char *));
  te->tagged_variant_tags =
      malloc(enum_decl->variant_count * sizeof(int));
  te->tagged_variant_payloads =
      malloc(enum_decl->variant_count * sizeof(Type *));

  if (!te->tagged_variant_names || !te->tagged_variant_tags ||
      !te->tagged_variant_payloads) {
    type_destroy(te);
    return NULL;
  }

  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    te->tagged_variant_names[i] =
        (char *)string_intern(enum_decl->variants[i].name);
    te->tagged_variant_tags[i] = (int)i;
    const char *pt = enum_decl->variants[i].payload_type;
    te->tagged_variant_payloads[i] =
        pt ? type_checker_get_type_by_name(checker, pt) : NULL;
    if (pt && !te->tagged_variant_payloads[i] &&
        type_checker_payload_is_self_pointer(pt, type_name)) {
      te->tagged_variant_payloads[i] = type_checker_pointer_to(checker, te);
    }
  }

  type_checker_intern_type(checker, te);
  return te;
}

int type_checker_process_tagged_enum(TypeChecker *checker,
                                            ASTNode *enum_decl_node) {
  EnumDeclaration *enum_decl = (EnumDeclaration *)enum_decl_node->data;

  Type *te =
      type_checker_build_tagged_enum_type(checker, enum_decl->name, enum_decl);
  type_checker_set_qualified_name(checker, te,
                                  enum_decl_node->location.filename);
  if (te && enum_decl->type_param_count == 0) {
    for (size_t i = 0; i < enum_decl->variant_count; i++) {
      if (enum_decl->variants[i].payload_type &&
          !te->tagged_variant_payloads[i]) {
        type_checker_set_error_at_location(
            checker, enum_decl_node->location,
            "Variant '%s' of enum '%s' carries a payload of unknown type '%s'",
            enum_decl->variants[i].name, enum_decl->name,
            enum_decl->variants[i].payload_type);
        return 0;
      }
    }
  }
  if (!te) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Failed to create tagged enum type '%s'",
                                       enum_decl->name);
    return 0;
  }

  Symbol *enum_sym =
      symbol_create(enum_decl->name, SYMBOL_ENUM, te);
  if (!enum_sym) {
    type_destroy(te);
    return 0;
  }
  if (!symbol_table_declare(checker->symbol_table, enum_sym)) {
    symbol_destroy(enum_sym);
    return 0;
  }

  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    if (!type_checker_register_variant_constructor(
            checker, te, enum_decl->name, enum_decl->variants[i].name, i)) {
      return 0;
    }
  }

  return 1;
}

static Symbol *type_checker_make_variant_constructor(Type *te,
                                                     const char *name,
                                                     size_t index) {
  Symbol *ctor = symbol_create(name, SYMBOL_TAGGED_ENUM_CONSTRUCTOR, te);
  if (!ctor) {
    return NULL;
  }
  ctor->data.constructor.enum_type = te;
  ctor->data.constructor.tag_value = (int)index;
  ctor->data.constructor.payload_type = te->tagged_variant_payloads[index];
  ctor->is_initialized = 1;
  return ctor;
}

/* Every variant gets two constructor symbols. `Enum__Variant` is unique to
 * its enum and is what a qualified `Enum.Variant(...)` resolves to. The bare
 * `Variant` is a convenience that belongs to whichever enum declared it
 * first, so a second enum reusing the name keeps its own qualified symbol and
 * leaves the bare one alone. */
int type_checker_register_variant_constructor(TypeChecker *checker, Type *te,
                                              const char *enum_name,
                                              const char *variant_name,
                                              size_t index) {
  size_t qualified_len = strlen(enum_name) + 2 + strlen(variant_name) + 1;
  char *qualified = malloc(qualified_len);
  if (!qualified) {
    return 0;
  }
  snprintf(qualified, qualified_len, "%s__%s", enum_name, variant_name);
  if (!symbol_table_lookup(checker->symbol_table, qualified)) {
    Symbol *ctor = type_checker_make_variant_constructor(te, qualified, index);
    if (!ctor) {
      free(qualified);
      return 0;
    }
    symbol_table_insert(checker->symbol_table, ctor);
  }
  free(qualified);
  if (!symbol_table_lookup(checker->symbol_table, variant_name)) {
    Symbol *ctor =
        type_checker_make_variant_constructor(te, variant_name, index);
    if (!ctor) {
      return 0;
    }
    symbol_table_insert(checker->symbol_table, ctor);
  }
  return 1;
}

static size_t type_checker_split_type_args(const char *text, char ***out) {
  size_t count = 0;
  size_t depth = 0;
  const char *start = text;
  char **args = NULL;
  for (const char *p = text;; p++) {
    if (*p == '<' || *p == '[') {
      depth++;
    } else if (*p == '>' || *p == ']') {
      if (depth > 0) depth--;
    } else if ((*p == ',' && depth == 0) || *p == '\0') {
      const char *a = start;
      const char *b = p;
      while (a < b && (*a == ' ' || *a == '\t')) a++;
      while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
      char **grown = realloc(args, (count + 1) * sizeof(char *));
      if (!grown) {
        for (size_t i = 0; i < count; i++) free(args[i]);
        free(args);
        return 0;
      }
      args = grown;
      args[count] = malloc((size_t)(b - a) + 1);
      if (!args[count]) {
        for (size_t i = 0; i < count; i++) free(args[i]);
        free(args);
        return 0;
      }
      memcpy(args[count], a, (size_t)(b - a));
      args[count][b - a] = '\0';
      count++;
      start = p + 1;
      if (*p == '\0') break;
    }
  }
  *out = args;
  return count;
}

static char *type_checker_mangle_generic_enum_name(const char *base,
                                                   char **args,
                                                   size_t count) {
  size_t len = strlen(base) + 1;
  for (size_t i = 0; i < count; i++) len += 2 + strlen(args[i]) * 4;
  char *result = malloc(len);
  if (!result) return NULL;
  size_t pos = strlen(base);
  memcpy(result, base, pos);
  for (size_t i = 0; i < count; i++) {
    result[pos++] = '_';
    result[pos++] = '_';
    for (const char *p = args[i]; *p; p++) {
      if (*p == '*') {
        memcpy(result + pos, "_ptr", 4);
        pos += 4;
      } else if (*p == '<' || *p == '>' || *p == ',' || *p == ' ') {
        result[pos++] = '_';
      } else {
        result[pos++] = *p;
      }
    }
  }
  result[pos] = '\0';
  return result;
}

Type *type_checker_instantiate_generic_enum(TypeChecker *checker,
                                                    const char *generic_name,
                                                    const char *type_arg_str) {
  if (!checker || !generic_name || !type_arg_str)
    return NULL;

  char **args = NULL;
  size_t arg_count = type_checker_split_type_args(type_arg_str, &args);
  if (arg_count == 0)
    return NULL;

  char *mangled =
      type_checker_mangle_generic_enum_name(generic_name, args, arg_count);
  if (!mangled) {
    for (size_t i = 0; i < arg_count; i++) free(args[i]);
    free(args);
    return NULL;
  }

  Type *existing = type_checker_get_type_by_name(checker, mangled);
  if (existing) {
    for (size_t i = 0; i < arg_count; i++) free(args[i]);
    free(args);
    free(mangled);
    return existing;
  }

  ASTNode *template_node = NULL;
  for (size_t i = 0; i < checker->generic_enum_template_count; i++) {
    ASTNode *n = checker->generic_enum_templates[i];
    if (!n) continue;
    EnumDeclaration *ed = (EnumDeclaration *)n->data;
    if (ed && ed->name && strcmp(ed->name, generic_name) == 0) {
      template_node = n;
      break;
    }
  }
  EnumDeclaration *tmpl =
      template_node ? (EnumDeclaration *)template_node->data : NULL;
  if (!tmpl || tmpl->type_param_count != arg_count) {
    for (size_t i = 0; i < arg_count; i++) free(args[i]);
    free(args);
    free(mangled);
    return NULL;
  }

  for (size_t i = 0; i < arg_count; i++) {
    if (!type_checker_get_type_by_name(checker, args[i])) {
      for (size_t j = 0; j < arg_count; j++) free(args[j]);
      free(args);
      free(mangled);
      return NULL;
    }
  }

  EnumVariant *concrete_variants =
      malloc(tmpl->variant_count * sizeof(EnumVariant));
  if (!concrete_variants) {
    for (size_t i = 0; i < arg_count; i++) free(args[i]);
    free(args);
    free(mangled);
    return NULL;
  }
  for (size_t i = 0; i < tmpl->variant_count; i++) {
    concrete_variants[i].name = tmpl->variants[i].name;
    concrete_variants[i].value = NULL;
    const char *orig_pt = tmpl->variants[i].payload_type;
    concrete_variants[i].payload_type = (char *)orig_pt;
    for (size_t k = 0; orig_pt && k < arg_count; k++) {
      if (strcmp(orig_pt, tmpl->type_params[k]) == 0) {
        concrete_variants[i].payload_type = args[k];
        break;
      }
    }
  }

  EnumDeclaration concrete_decl;
  concrete_decl.name = mangled;
  concrete_decl.variants = concrete_variants;
  concrete_decl.variant_count = tmpl->variant_count;
  concrete_decl.is_exported = 0;
  concrete_decl.type_params = NULL;
  concrete_decl.type_param_count = 0;

  Type *te =
      type_checker_build_tagged_enum_type(checker, mangled, &concrete_decl);
  free(concrete_variants);
  for (size_t i = 0; i < arg_count; i++) free(args[i]);
  free(args);

  if (!te) {
    free(mangled);
    return NULL;
  }

  // Register type + constructors in symbol table
  Symbol *enum_sym = symbol_create(mangled, SYMBOL_ENUM, te);
  free(mangled);
  if (!enum_sym) {
    type_destroy(te);
    return NULL;
  }
  symbol_table_insert(checker->symbol_table, enum_sym);

  for (size_t i = 0; i < tmpl->variant_count; i++) {
    if (!type_checker_register_variant_constructor(
            checker, te, te->name, tmpl->variants[i].name, i)) {
      type_destroy(te);
      return NULL;
    }
  }

  return te;
}

int type_checker_process_type_declaration(TypeChecker *checker,
                                          ASTNode *type_decl_node) {
  TypeDeclaration *decl =
      type_decl_node ? (TypeDeclaration *)type_decl_node->data : NULL;
  Type *base;
  Type *refined;
  Symbol *symbol;
  if (!checker || !decl || !decl->name || !decl->base_type) {
    return 0;
  }
  if (type_checker_get_type_by_name(checker, decl->name)) {
    type_checker_set_error_at_location(checker, type_decl_node->location,
                                       "Type '%s' already declared",
                                       decl->name);
    return 0;
  }
  base = type_checker_get_type_by_name(checker, decl->base_type);
  if (!base) {
    type_checker_set_error_at_location(checker, type_decl_node->location,
                                       "Unknown base type '%s' for type '%s'",
                                       decl->base_type, decl->name);
    return 0;
  }
  if (base->kind == TYPE_VOID || base->kind == TYPE_ARRAY ||
      base->kind == TYPE_STRUCT || base->kind == TYPE_ENUM ||
      base->kind == TYPE_TAGGED_ENUM || base->kind == TYPE_FUNCTION_POINTER ||
      type_is_comptime_only(base)) {
    type_checker_set_error_at_location(
        checker, type_decl_node->location,
        "a declared type refines a number, a bool, a char, a string, a "
        "pointer or a slice; '%s' is none of those",
        decl->base_type);
    return 0;
  }
  refined = type_create(base->kind, decl->name);
  if (!refined) {
    type_checker_set_error_at_location(checker, type_decl_node->location,
                                       "Failed to create type '%s'",
                                       decl->name);
    return 0;
  }
  refined->size = base->size;
  refined->alignment = base->alignment;
  refined->base_type = base->base_type;
  refined->array_size = base->array_size;
  refined->view_rank = base->view_rank;
  refined->is_volatile = base->is_volatile;
  refined->refined_base = base;
  refined->refinement = decl->predicate;
  refined->refine_binding =
      decl->binding ? string_intern(decl->binding) : "value";
  type_checker_intern_type(checker, refined);
  type_checker_set_qualified_name(checker, refined,
                                  type_decl_node->location.filename);
  symbol = symbol_create(decl->name, SYMBOL_STRUCT, refined);
  if (!symbol) {
    return 0;
  }
  symbol->decl_line = type_decl_node->location.line;
  symbol->decl_column = type_decl_node->location.column;
  symbol->decl_file = type_decl_node->location.filename;
  if (!symbol_table_declare(checker->symbol_table, symbol)) {
    symbol_destroy(symbol);
    type_checker_set_error_at_location(checker, type_decl_node->location,
                                       "Type '%s' already declared",
                                       decl->name);
    return 0;
  }
  return 1;
}

int type_checker_check_type_predicate(TypeChecker *checker,
                                      ASTNode *type_decl_node) {
  TypeDeclaration *decl =
      type_decl_node ? (TypeDeclaration *)type_decl_node->data : NULL;
  Type *refined;
  Type *base;
  if (!checker || !decl || !decl->name) {
    return 0;
  }
  refined = type_checker_get_type_by_name(checker, decl->name);
  if (!refined || !refined->refined_base) {
    return 1;
  }
  base = refined->refined_base;
  if (decl->predicate) {
    Type *predicate_type;
    Symbol *value_symbol;
    if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
      return 0;
    }
    value_symbol = symbol_create(
        refined->refine_binding ? refined->refine_binding : "value",
        SYMBOL_PARAMETER, base);
    if (!value_symbol ||
        !symbol_table_declare(checker->symbol_table, value_symbol)) {
      if (value_symbol) {
        symbol_destroy(value_symbol);
      }
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
    value_symbol->is_initialized = 1;
    value_symbol->is_used = 1;
    predicate_type = type_checker_infer_type(checker, decl->predicate);
    symbol_table_exit_scope(checker->symbol_table);
    if (!predicate_type) {
      return 0;
    }
    if (!type_checker_is_numeric_type(predicate_type)) {
      type_checker_set_error_at_location(
          checker, decl->predicate->location,
          "the predicate of type '%s' must be a condition over `value`, "
          "and this one is a '%s'",
          decl->name, predicate_type->name ? predicate_type->name : "?");
      return 0;
    }
  }
  type_checker_compute_refinement_range(checker, refined);
  return 1;
}

int type_checker_process_enum_declaration(TypeChecker *checker,
                                          ASTNode *enum_decl_node) {
  if (!checker || !enum_decl_node ||
      enum_decl_node->type != AST_ENUM_DECLARATION) {
    return 0;
  }

  EnumDeclaration *enum_decl = (EnumDeclaration *)enum_decl_node->data;
  if (!enum_decl || !enum_decl->name) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Invalid enum declaration");
    return 0;
  }

  // If this enum has type parameters it's a generic template , store the AST
  // node for later monomorphization and do not register a concrete type now.
  if (enum_decl->type_param_count > 0) {
    ASTNode **new_tmpl = realloc(
        checker->generic_enum_templates,
        (checker->generic_enum_template_count + 1) * sizeof(ASTNode *));
    if (!new_tmpl)
      return 0;
    checker->generic_enum_templates = new_tmpl;
    checker->generic_enum_templates[checker->generic_enum_template_count++] =
        enum_decl_node;
    return 1;
  }

  // Check whether any variant carries a payload , if so, it's a tagged enum.
  int is_tagged = 0;
  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    if (enum_decl->variants[i].payload_type) {
      is_tagged = 1;
      break;
    }
  }

  // Check for duplicate type declaration
  if (type_checker_get_type_by_name(checker, enum_decl->name)) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Type '%s' already declared",
                                       enum_decl->name);
    return 0;
  }

  if (is_tagged) {
    return type_checker_process_tagged_enum(checker, enum_decl_node);
  }

  // Plain (integer-valued) enum.
  Type *new_enum_type = type_create(TYPE_ENUM, enum_decl->name);
  if (!new_enum_type) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Failed to create enum type");
    return 0;
  }
  new_enum_type->size = 8;
  new_enum_type->alignment = 8;
  if (!type_alloc_enum_members(new_enum_type, enum_decl->variant_count)) {
    type_destroy(new_enum_type);
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Out of memory recording enum members");
    return 0;
  }
  type_checker_intern_type(checker, new_enum_type);
  type_checker_set_qualified_name(checker, new_enum_type,
                                  enum_decl_node->location.filename);

  Symbol *enum_symbol =
      symbol_create(enum_decl->name, SYMBOL_ENUM, new_enum_type);
  if (!enum_symbol) {
    type_destroy(new_enum_type);
    return 0;
  }
  if (!symbol_table_declare(checker->symbol_table, enum_symbol)) {
    symbol_destroy(enum_symbol);
    return 0;
  }

  long long current_val = 0;
  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    EnumVariant *variant = &enum_decl->variants[i];

    if (variant->value) {
      ASTNode *val_node = variant->value;
      if (val_node->type == AST_NUMBER_LITERAL) {
        current_val = ((NumberLiteral *)val_node->data)->int_value;
      } else if (val_node->type == AST_UNARY_EXPRESSION &&
                 ((UnaryExpression *)val_node->data)->operand->type ==
                     AST_NUMBER_LITERAL &&
                 strcmp(((UnaryExpression *)val_node->data)->operator, "-") ==
                     0) {
        current_val = -((NumberLiteral *)((UnaryExpression *)val_node->data)
                            ->operand->data)
                           ->int_value;
      } else {
        type_checker_set_error_at_location(
            checker, val_node->location,
            "Enum variant initializer must be a constant integer");
        return 0;
      }
    }

    if (symbol_table_lookup_current_scope(checker->symbol_table,
                                          variant->name)) {
      type_checker_report_duplicate_declaration(
          checker, enum_decl_node->location, variant->name);
      return 0;
    }

    Symbol *sym = symbol_create(variant->name, SYMBOL_CONSTANT, new_enum_type);
    if (!sym)
      return 0;
    sym->data.constant.value = current_val;
    sym->is_initialized = 1;
    symbol_table_insert(checker->symbol_table, sym);
    type_set_enum_member(new_enum_type, i, variant->name, current_val);
    current_val++;
  }

  return 1;
}

static int type_checker_process_deferred(TypeChecker *checker,
                                   ASTNode *declaration,
                                   int *handled) {
  *handled = 1;
  switch (declaration->type) {
  case AST_DEFER_STATEMENT:
    type_checker_set_error_at_location(checker, declaration->location,
                                       "Defer statement outside of a function");
    return 0;

  case AST_ERRDEFER_STATEMENT:
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Errdefer statement outside of a function");
    return 0;

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)declaration->data;
    if (call && call->function_name &&
        strcmp(call->function_name, "static_assert") == 0) {
      return type_checker_validate_static_assert(checker, call,
                                                 declaration->location);
    }
    type_checker_set_error_at_location(
        checker, declaration->location,
        "A call to '%s' cannot stand at file scope. File scope holds "
        "declarations; move the call into a function body",
        call && call->function_name ? call->function_name : "a function");
    if (checker->error_reporter)
      error_reporter_set_last_label(checker->error_reporter,
                                    "this call needs a function to run in");
    return 0;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int type_checker_check_address_space(TypeChecker *checker,
                                            ASTNode *declaration,
                                            VarDeclaration *var_decl,
                                            Scope *current_scope,
                                            Type **var_type_io) {
  Type *var_type = *var_type_io;
  if (var_decl->address_space != AST_ADDRESS_SPACE_DEFAULT) {
    FunctionDeclaration *owner =
        checker->current_function_decl &&
                checker->current_function_decl->type == AST_FUNCTION_DECLARATION
            ? (FunctionDeclaration *)checker->current_function_decl->data
            : NULL;
    if (!owner || !owner->is_kernel || !current_scope ||
        current_scope->type == SCOPE_GLOBAL) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "%s storage is only legal inside a GPU kernel",
          var_decl->address_space == AST_ADDRESS_SPACE_WORKGROUP ? "workgroup"
                                                                 : "private");
      return 0;
    }
    if (var_decl->is_const || var_decl->is_extern || var_decl->is_exported) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "GPU address-space storage must be a local 'var' binding");
      return 0;
    }
    if (var_decl->initializer) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "%s storage cannot have a declaration initializer; initialize "
          "elements explicitly",
          var_decl->address_space == AST_ADDRESS_SPACE_WORKGROUP ? "workgroup"
                                                                 : "private");
      return 0;
    }
    int is_static_storage =
        var_type && var_type->kind == TYPE_ARRAY && var_type->base_type &&
        var_type->array_size > 0 && var_type->array_size <= UINT32_MAX;
    int is_dynamic_workgroup_view =
        var_type && var_type->kind == TYPE_POINTER && var_type->base_type &&
        var_decl->address_space == AST_ADDRESS_SPACE_WORKGROUP;
    if (!is_static_storage && !is_dynamic_workgroup_view) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "GPU address-space storage requires a statically sized array type "
          "with at most %u elements, or a pointer type for a dynamic "
          "workgroup view",
          UINT32_MAX);
      return 0;
    }
    Type *element_type = var_type->base_type;
    switch (element_type->kind) {
    case TYPE_INT8:
    case TYPE_INT16:
    case TYPE_INT32:
    case TYPE_INT64:
    case TYPE_UINT8:
    case TYPE_UINT16:
    case TYPE_UINT32:
    case TYPE_UINT64:
    case TYPE_BOOL:
    case TYPE_FLOAT32:
    case TYPE_FLOAT64:
    case TYPE_FLOAT16:
    case TYPE_BFLOAT16:
      break;
    default:
      type_checker_set_error_at_location(
          checker, declaration->location,
          "GPU address-space binding '%s' must have a scalar numeric element "
          "type",
          var_decl->name);
      return 0;
    }
  }
  *var_type_io = var_type;
  return 1;
}

static int type_checker_check_variable_initializer(
    TypeChecker *checker, ASTNode *declaration,
    VarDeclaration *var_decl, Scope *current_scope, Type **var_type_io,
    int *poisoned_io) {
  Type *var_type = *var_type_io;
  // If there's an initializer, validate it. When validation fails but the
  // declared type is known, the variable is still registered with that type
  // ("poisoned") so later uses don't cascade into bogus undefined-variable
  // errors; the declaration itself still fails.
  int poisoned = *poisoned_io;
  if (var_decl->initializer) {
    size_t reports_before =
        checker->error_reporter ? checker->error_reporter->count : 0;
    /* An aggregate literal has no type of its own, so hand it the declared
     * type. Without one there is nothing to check it against, and the
     * literal reports that itself. */
    checker->aggregate_target_type =
        var_decl->initializer->type == AST_AGGREGATE_LITERAL ||
                var_decl->initializer->type == AST_FUNCTION_CALL ||
                var_decl->initializer->type == AST_IDENTIFIER
            ? var_type
            : NULL;
    /* A `const` and a module-scope `var` are laid out in the object file, so
       an element of theirs has to be known while compiling. A local is
       initialized by code, which can compute one. */
    checker->aggregate_requires_constant =
        var_decl->is_const ||
        (current_scope && current_scope->type == SCOPE_GLOBAL);
    Type *init_type = type_checker_infer_type(checker, var_decl->initializer);
    checker->aggregate_target_type = NULL;
    checker->aggregate_requires_constant = 0;
    if (!init_type) {
      int already_reported =
          checker->error_reporter
              ? checker->error_reporter->count > reports_before
              : checker->has_error;
      if (!already_reported) {
        type_checker_set_error_at_location(
            checker, var_decl->initializer->location,
            "Cannot infer type of initializer for variable '%s'",
            var_decl->name);
      }
      checker->has_error = 1;
      if (!var_type)
        return 0;
      poisoned = 1;
    }
    if (!poisoned && var_type) {
      /* A capturing closure carries a heap environment and cannot be stored in
       * a plain function-pointer type; it needs a closure type `Fn(...)`. */
      if (init_type && init_type->kind == TYPE_FUNCTION_POINTER &&
          init_type->closure_env &&
          !(var_type->kind == TYPE_FUNCTION_POINTER && var_type->closure_env)) {
        type_checker_set_error_at_location(
            checker, var_decl->initializer->location,
            "a capturing closure cannot be stored in a plain function-pointer "
            "type '%s'; declare '%s' with a closure type 'Fn(...)' instead",
            var_type->name, var_decl->name);
        poisoned = 1;
      }
      // Type specified: validate assignment compatibility
      else if (type_is_comptime_only(init_type) &&
               !type_is_comptime_only(var_type)) {
        type_checker_reject_comptime_escape(
            checker, var_decl->initializer->location, init_type);
        poisoned = 1;
      } else if (!(type_checker_type_accepts_null_pointer(var_type) &&
            type_checker_is_null_pointer_constant(var_decl->initializer)) &&
          !type_checker_is_assignable_from(checker, var_type, init_type,
                                           var_decl->initializer)) {
        type_checker_report_assign_mismatch(
            checker, var_decl->initializer,
            var_decl->initializer->location, var_type, init_type);
        poisoned = 1;
      }
    } else if (poisoned) {
      /* Initializer failed but declared type is known: register anyway. */
    } else if (var_decl->structural_type ||
               (var_decl->is_const &&
                (!current_scope || current_scope->type == SCOPE_GLOBAL))) {
      // Exempt: a compiler-synthesized binding whose type is structural (e.g.
      // a range-`for` counter), or a global `const` (integer-only and folded
      // at each use, so its type is exactly its literal value's type). Take
      // the initializer type.
      var_type = init_type;
      if (var_decl->structural_type) {
        Type *narrowed = type_checker_narrow_to_target_word(checker, var_type);
        if (narrowed && narrowed->name) {
          char *narrowed_name = strdup(narrowed->name);
          if (!narrowed_name) {
            type_checker_set_error_at_location(
                checker, declaration->location,
                "Out of memory narrowing '%s' to the target's word",
                var_decl->name);
            return 0;
          }
          free(var_decl->type_name);
          var_decl->type_name = narrowed_name;
          var_type = narrowed;
        }
      }
    } else {
      // Mettle requires an explicit type on every user `var` and local
      // `const` binding; nothing is inferred from an arbitrary initializer.
      type_checker_set_error_at_location(
          checker, declaration->location,
          "%s '%s' requires an explicit type: write '%s %s: <type> = ...' "
          "(Mettle does not infer binding types)",
          var_decl->is_const ? "constant" : "variable", var_decl->name,
          var_decl->is_const ? "const" : "var", var_decl->name);
      return 0;
    }
  } else if (!var_type) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Variable '%s' must have either a type annotation or an initializer",
        var_decl->name);
    return 0;
  }
  *var_type_io = var_type;
  *poisoned_io = poisoned;
  return 1;
}

static int type_checker_declare_comptime_const(
    TypeChecker *checker, ASTNode *declaration,
    VarDeclaration *var_decl, Type *var_type, int poisoned,
    int *handled) {
  *handled = 1;
  if (var_type && type_contains_comptime_only(var_type)) {
    if (!var_decl->is_const || !type_is_comptime_only(var_type)) {
      type_checker_reject_no_runtime_repr(checker, declaration->location,
                                          var_type);
      return 0;
    }
    if (!var_decl->initializer) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Constant '%s' must have an initializer", var_decl->name);
      return 0;
    }
    if (poisoned) {
      return 0;
    }
    ComptimeValue folded = comptime_none();
    if (!type_checker_eval_comptime(checker, var_decl->initializer,
                                    &folded) ||
        (var_type->kind == TYPE_TYPE && folded.kind != COMPTIME_TYPE_REF) ||
        (var_type->kind == TYPE_FIELD &&
         folded.kind != COMPTIME_FIELD_REF)) {
      type_checker_set_error_at_location(
          checker, var_decl->initializer->location,
          "Constant '%s' initializer must be a compile-time %s value",
          var_decl->name, var_type->name);
      return 0;
    }
    if (symbol_table_lookup_current_scope(checker->symbol_table,
                                          var_decl->name)) {
      type_checker_report_duplicate_declaration(
          checker, declaration->location, var_decl->name);
      return 0;
    }
    Symbol *const_symbol =
        symbol_create(var_decl->name, SYMBOL_CONSTANT, var_type);
    if (!const_symbol) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Failed to create symbol for constant '%s'", var_decl->name);
      return 0;
    }
    const_symbol->comptime_value = folded;
    const_symbol->is_initialized = 1;
    const_symbol->is_immutable = 1;
    const_symbol->decl_line = declaration->location.line;
    const_symbol->decl_column = declaration->location.column;
    const_symbol->decl_file = declaration->location.filename;
    if (!symbol_table_declare(checker->symbol_table, const_symbol)) {
      type_checker_report_duplicate_declaration(
          checker, declaration->location, var_decl->name);
      symbol_destroy(const_symbol);
      return 0;
    }
    return 1;
  }
  *handled = 0;
  return 1;
}

static int type_checker_check_global_initializer(
    TypeChecker *checker, ASTNode *declaration,
    VarDeclaration *var_decl, Type *var_type, Scope *current_scope,
    int *poisoned_io) {
  int poisoned = *poisoned_io;
  /* A global's storage is laid out in the object file, so its value has to be
   * known at compile time. For an aggregate that means an aggregate literal
   * and nothing else -- a call or any other run-time expression has no image
   * to lay out, and there is no module initializer to run one in. Caught here
   * rather than in codegen so the report carries a source location. */
  if (!poisoned && var_decl->initializer && !var_decl->is_extern && var_type &&
      (var_type->kind == TYPE_STRUCT || var_type->kind == TYPE_ARRAY) &&
      var_decl->initializer->type != AST_AGGREGATE_LITERAL &&
      (!current_scope || current_scope->type == SCOPE_GLOBAL)) {
    type_checker_set_error_at_location(
        checker, var_decl->initializer->location,
        "a global of aggregate type must be initialized with an aggregate "
        "literal (%s), whose value is known at compile time; '%s' has type "
        "'%s'",
        var_type->kind == TYPE_STRUCT ? "'{ field: value, ... }'"
                                      : "'[ value, ... ]'",
        var_decl->name, var_type->name ? var_type->name : "?");
    /* Register the binding anyway so later uses do not pile on with
     * "undefined variable"; the declaration itself has already failed. */
    checker->has_error = 1;
    poisoned = 1;
  }

  /* The same rule for a scalar global. Its initializer is folded to bytes in
   * the object file, so it has to be one of the shapes the module lowering
   * can fold (see eval_numeric in mtlc_lower_module.c): a numeric constant
   * expression, `sizeof(T)`, `&name`, or a string literal. Anything else -- a
   * call, `new`, an index or member access, string concatenation -- is a
   * run-time value with nothing to lay out. Rejected here so the report
   * carries a source location instead of failing as an internal compiler
   * error in codegen. */
  if (!poisoned && var_decl->initializer && !var_decl->is_extern && var_type &&
      var_type->kind != TYPE_STRUCT && var_type->kind != TYPE_ARRAY &&
      (!current_scope || current_scope->type == SCOPE_GLOBAL) &&
      /* An integer `const` gets the more specific diagnostic from its own
       * fold below; everything else lands here. */
       !(var_decl->is_const && type_checker_is_numeric_type(var_type)) &&
      !layoutable_global_initializer(checker, var_type, var_decl->initializer,
                                    1)) {
    type_checker_set_error_at_location(
        checker, var_decl->initializer->location,
        "a global's initializer must be known at compile time; '%s' is "
        "initialized with a value that is only available at run time. Move "
        "the initialization into a function, or make the initializer a "
        "constant expression",
        var_decl->name);
    checker->has_error = 1;
    poisoned = 1;
  }
  *poisoned_io = poisoned;
  return 1;
}

static int type_checker_fold_const_value(
    TypeChecker *checker, ASTNode *declaration,
    VarDeclaration *var_decl, Type *var_type, Scope *current_scope,
    int *has_folded_value_io, int poisoned, int *folded_is_float_io,
    long long *folded_integer_value_io, double *folded_float_value_io) {
  int folded_is_float = *folded_is_float_io;
  long long folded_integer_value = *folded_integer_value_io;
  double folded_float_value = *folded_float_value_io;
// A `const` declaration binds an immutable value and must be initialized.
// Numeric consts must fold at compile time. Integer globals use the
// storage free symbol form. Other numeric consts keep normal storage when
// the backend needs an address, but carry the folded value for later const
// expressions.
if (var_decl->is_const) {
  if (!var_decl->initializer) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Constant '%s' must have an initializer", var_decl->name);
    return 0;
  }
  if (!poisoned && type_checker_is_numeric_type(var_type)) {
    long long const_value = 0;
    double float_value = 0.0;
    int is_float = type_checker_is_floating_type(var_type);
    int evaluated = is_float
                        ? type_checker_eval_float_constant_with_checker(
                              checker, var_decl->initializer, &float_value)
                        : type_checker_eval_integer_constant_with_checker(
                              checker, var_decl->initializer, &const_value);
    if (!evaluated) {
      type_checker_set_error_at_location(
          checker, var_decl->initializer->location,
          is_float
              ? "Constant '%s' initializer must be a compile-time "
                "constant expression"
              : "Constant '%s' initializer must be a compile-time integer "
                "constant expression",
          var_decl->name);
      return 0;
    }
    *has_folded_value_io = 1;
    folded_is_float = is_float;
    folded_integer_value = const_value;
    folded_float_value = float_value;
    if (current_scope && current_scope->type == SCOPE_GLOBAL && !is_float) {
      if (symbol_table_lookup_current_scope(checker->symbol_table,
                                            var_decl->name)) {
        type_checker_report_duplicate_declaration(
            checker, declaration->location, var_decl->name);
        return 0;
      }
      Symbol *const_symbol =
          symbol_create(var_decl->name, SYMBOL_CONSTANT, var_type);
      if (!const_symbol) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Failed to create symbol for constant '%s'", var_decl->name);
        return 0;
      }
      const_symbol->data.constant.value = const_value;
      const_symbol->has_constant_value = 1;
      const_symbol->constant_integer_value = const_value;
      const_symbol->is_initialized = 1;
      if (!symbol_table_declare(checker->symbol_table, const_symbol)) {
        type_checker_report_duplicate_declaration(
            checker, declaration->location, var_decl->name);
        symbol_destroy(const_symbol);
        return 0;
      }
      return 2;
    }
    // Local numeric consts and global float consts use normal storage.
  }
  // Non numeric consts use normal storage and the immutable flag below.
}
  *folded_is_float_io = folded_is_float;
  *folded_integer_value_io = folded_integer_value;
  *folded_float_value_io = folded_float_value;
  return 1;
}

static int type_checker_track_local_initialization(
    TypeChecker *checker, ASTNode *declaration,
    VarDeclaration *var_decl, Symbol *var_symbol, Type *var_type) {
if (checker->current_function && !var_decl->is_extern) {
  Scope *declare_scope =
      symbol_table_get_current_scope(checker->symbol_table);
  if (declare_scope && declare_scope->type != SCOPE_GLOBAL) {
    int track_definite_init =
        !var_symbol->is_address_space_binding &&
        !(var_type &&
          (var_type->kind == TYPE_ARRAY || var_type->kind == TYPE_STRUCT ||
           var_type->kind == TYPE_STRING));
    if (track_definite_init) {
      if (!type_checker_init_tracker_declare(
              checker, var_decl->name, var_decl->initializer != NULL)) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Out of memory while tracking initialization state for '%s'",
            var_decl->name);
        return 0;
      }
    }

    if (var_type && var_type->kind == TYPE_POINTER) {
      long long known_extent = type_checker_extract_known_buffer_extent(
          checker, var_decl->initializer);
      long long known_alignment =
          type_checker_extract_known_pointer_alignment(
              checker, var_decl->initializer);
      if (!type_checker_buffer_extent_declare(checker, var_decl->name,
                                              known_extent,
                                              known_alignment)) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Out of memory while tracking buffer extent for '%s'",
            var_decl->name);
        return 0;
      }
    }
  }
  }
  return 1;
}

static int type_checker_declare_variable(
    TypeChecker *checker, ASTNode *declaration,
    VarDeclaration *var_decl, Type *var_type, Scope *current_scope,
    int *poisoned_io) {
  int poisoned = *poisoned_io;
  int has_folded_value = 0;
  int folded_is_float = 0;
  long long folded_integer_value = 0;
  double folded_float_value = 0.0;

  {
    int folded = type_checker_fold_const_value(
        checker, declaration, var_decl, var_type, current_scope,
        &has_folded_value, poisoned, &folded_is_float,
        &folded_integer_value, &folded_float_value);
    if (folded != 1) {
      return folded == 2 ? 1 : 0;
    }
  }

  // Check for duplicate declaration in current scope.
  Symbol *existing = symbol_table_lookup_current_scope(checker->symbol_table,
                                                       var_decl->name);
  if (existing) {
    if (existing->kind != SYMBOL_VARIABLE) {
      type_checker_report_duplicate_declaration_prev(
          checker, declaration->location, var_decl->name, existing);
      return 0;
    }
    if (existing->is_extern != var_decl->is_extern) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Variable '%s' redeclared with conflicting extern/non-extern "
          "linkage",
          var_decl->name);
      return 0;
    }
    if (!var_decl->is_extern) {
      type_checker_report_duplicate_declaration_prev(
          checker, declaration->location, var_decl->name, existing);
      return 0;
    }
    if (!type_checker_types_equal(existing->type, var_type)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern variable '%s' redeclared with conflicting type",
          var_decl->name);
      return 0;
    }
    if (!type_checker_link_name_matches_symbol(existing, var_decl->name,
                                               var_decl->is_extern,
                                               var_decl->link_name)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern variable '%s' redeclared with conflicting link name",
          var_decl->name);
      return 0;
    }
    return 1;
  }

  // Create and declare the symbol
  Symbol *var_symbol =
      symbol_create(var_decl->name, SYMBOL_VARIABLE, var_type);
  if (var_symbol) {
    var_symbol->decl_line = declaration->location.line;
    var_symbol->decl_column = declaration->location.column;
    var_symbol->decl_file = declaration->location.filename;
  }
  if (!var_symbol) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Failed to create symbol for variable '%s'", var_decl->name);
    return 0;
  }

  var_symbol->is_extern = var_decl->is_extern;
  var_symbol->is_immutable = var_decl->is_const;
  /* A `const` written as an aggregate literal is a table a `comptime for` can
     read the rows of, so the literal is kept where the name can reach it. */
  if (var_decl->is_const && var_decl->initializer &&
      var_decl->initializer->type == AST_AGGREGATE_LITERAL) {
    var_symbol->constant_initializer = var_decl->initializer;
  }
  if (has_folded_value) {
    var_symbol->has_constant_value = 1;
    var_symbol->constant_is_float = folded_is_float;
    var_symbol->constant_integer_value = folded_integer_value;
    var_symbol->constant_float_value = folded_float_value;
  }
  var_symbol->is_address_space_binding =
      var_decl->address_space != AST_ADDRESS_SPACE_DEFAULT;
  var_symbol->address_space =
      var_decl->address_space == AST_ADDRESS_SPACE_WORKGROUP
          ? MTLC_ADDRESS_SPACE_WORKGROUP
      : var_decl->address_space == AST_ADDRESS_SPACE_PRIVATE
          ? MTLC_ADDRESS_SPACE_PRIVATE
          : MTLC_ADDRESS_SPACE_DEFAULT;
  if (var_decl->is_extern) {
    const char *effective_link_name = type_checker_decl_link_name(
        var_decl->name, var_decl->is_extern, var_decl->link_name);
    var_symbol->link_name =
        effective_link_name ? strdup(effective_link_name) : NULL;
    if (!var_symbol->link_name) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Failed to allocate link name for extern variable '%s'",
          var_decl->name);
      symbol_destroy(var_symbol);
      return 0;
    }
  }

  if (!symbol_table_declare(checker->symbol_table, var_symbol)) {
    type_checker_report_duplicate_declaration_prev(
        checker, declaration->location, var_decl->name,
        symbol_table_lookup_current_scope(checker->symbol_table,
                                          var_decl->name));
    symbol_destroy(var_symbol);
    return 0;
  }

  if (!type_checker_track_local_initialization(checker, declaration,
                                               var_decl, var_symbol,
                                               var_type)) {
    return 0;
  }
  *poisoned_io = poisoned;
  return 1;
}

static int type_checker_process_variable(TypeChecker *checker,
                                   ASTNode *declaration,
                                   int *handled) {
  *handled = 1;
  switch (declaration->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *var_decl = (VarDeclaration *)declaration->data;
    if (!var_decl || !var_decl->name) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid variable declaration");
      return 0;
    }

    if (var_decl->link_name && !var_decl->is_extern) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Link-name suffix is only allowed on extern declarations");
      return 0;
    }

    Scope *current_scope =
        symbol_table_get_current_scope(checker->symbol_table);
    if (var_decl->is_extern &&
        (!current_scope || current_scope->type != SCOPE_GLOBAL)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern declarations are only allowed at top level");
      return 0;
    }

    if (var_decl->is_extern && var_decl->initializer) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern variable '%s' cannot have an initializer", var_decl->name);
      return 0;
    }
    if (var_decl->is_extern && !var_decl->type_name) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern variable '%s' requires an explicit type annotation",
          var_decl->name);
      return 0;
    }

    if (current_scope && current_scope->type != SCOPE_GLOBAL) {
      Symbol *parameter = find_enclosing_parameter(checker, var_decl->name);
      if (parameter) {
        type_checker_report_parameter_shadow(checker, declaration->location,
                                             var_decl->name, parameter);
        return 0;
      }
    }

    Type *var_type = NULL;

    // If type is explicitly specified, resolve it
    if (var_decl->type_name) {
      var_type = type_checker_get_type_by_name(checker, var_decl->type_name);
      if (!var_type) {
        type_checker_report_undefined_symbol(checker, declaration->location,
                                             var_decl->type_name, "type");
        return 0;
      }
    }

    int poisoned = 0;
    if (!type_checker_check_address_space(checker, declaration, var_decl,
                                         current_scope, &var_type) ||
        !type_checker_check_variable_initializer(checker, declaration,
                                                 var_decl, current_scope,
                                                 &var_type, &poisoned)) {
      return 0;
    }
    {
      int const_handled = 0;
      int declared = type_checker_declare_comptime_const(
          checker, declaration, var_decl, var_type, poisoned,
          &const_handled);
      if (const_handled) {
        return declared;
      }
    }
    if (!type_checker_check_global_initializer(checker, declaration, var_decl,
                                               var_type, current_scope,
                                               &poisoned) ||
        !type_checker_declare_variable(checker, declaration, var_decl, var_type,
                                       current_scope, &poisoned)) {
      return 0;
    }
    return poisoned ? 0 : 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static Symbol *type_checker_build_function_symbol(
    TypeChecker *checker, ASTNode *declaration,
    FunctionDeclaration *func_decl, Type *return_type) {
  // Resolve parameter types and check for duplicate parameter names
  Type **param_types = NULL;
  if (func_decl->parameter_count > 0) {
    param_types = malloc(func_decl->parameter_count * sizeof(Type *));
    if (!param_types) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Memory allocation failed for function parameters");
      return 0;
    }

    // Check for duplicate parameter names
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      for (size_t j = i + 1; j < func_decl->parameter_count; j++) {
        if (strcmp(func_decl->parameter_names[i],
                   func_decl->parameter_names[j]) == 0) {
          type_checker_report_duplicate_declaration(
              checker, declaration->location, func_decl->parameter_names[i]);
          free(param_types);
          return 0;
        }
      }
    }

    /* `T[..]` gathers whatever a call passes after the fixed parameters. Only
       the last parameter can, because everything after it would have nothing
       left to take. */
    type_checker_note_gathered_parameter(func_decl);
    for (size_t i = 0; i + 1 < func_decl->parameter_count; i++) {
      const char *written = func_decl->parameter_types[i];
      size_t written_length = written ? strlen(written) : 0;
      if (written_length > 4 &&
          strcmp(written + written_length - 4, "[..]") == 0) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "'%s' gathers the rest of the call's arguments, so it has to be "
            "the last parameter of '%s'",
            func_decl->parameter_names[i], func_decl->name);
        free(param_types);
        return 0;
      }
    }

    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      param_types[i] = type_checker_get_type_by_name(
          checker, func_decl->parameter_types[i]);
      if (!param_types[i]) {
        type_checker_report_undefined_symbol(checker, declaration->location,
                                             func_decl->parameter_types[i],
                                             "type");
        free(param_types);
        return 0;
      }
      if (type_checker_reject_no_runtime_repr(checker, declaration->location,
                                              param_types[i])) {
        free(param_types);
        return 0;
      }
      if (func_decl->is_kernel &&
          !gpu_kernel_parameter_type(param_types[i])) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "GPU kernel '%s' parameter '%s' has unsupported ABI type '%s'; "
            "use a scalar, a pointer, or a record built from those",
            func_decl->name, func_decl->parameter_names[i],
            param_types[i]->name ? param_types[i]->name : "unknown");
        free(param_types);
        return 0;
      }
    }
  }

  // Copy parameter names so function symbols own their metadata.
  char **param_names_copy = NULL;
  if (func_decl->parameter_count > 0) {
    param_names_copy = malloc(func_decl->parameter_count * sizeof(char *));
    if (!param_names_copy) {
      if (param_types)
        free(param_types);
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Memory allocation failed for function parameter names");
      return 0;
    }
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      param_names_copy[i] = strdup(func_decl->parameter_names[i]);
      if (!param_names_copy[i]) {
        for (size_t j = 0; j < i; j++) {
          free(param_names_copy[j]);
        }
        free(param_names_copy);
        if (param_types)
          free(param_types);
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Memory allocation failed for parameter name copy");
        return 0;
      }
    }
  }

  // Create function symbol
  Symbol *func_symbol =
      symbol_create(func_decl->name, SYMBOL_FUNCTION, return_type);
  if (!func_symbol) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Memory allocation failed for function symbol");
    if (param_names_copy) {
      for (size_t i = 0; i < func_decl->parameter_count; i++) {
        free(param_names_copy[i]);
      }
      free(param_names_copy);
    }
    if (param_types)
      free(param_types);
    return 0;
  }

  // Set function-specific data
  func_symbol->data.function.parameter_count = func_decl->parameter_count;
  func_symbol->data.function.parameter_names = param_names_copy;
  func_symbol->data.function.parameter_types = param_types;
  func_symbol->data.function.return_type = return_type;
  func_symbol->is_kernel = func_decl->is_kernel;
  func_symbol->kernel_block[0] = func_decl->kernel_block[0];
  func_symbol->kernel_block[1] = func_decl->kernel_block[1];
  func_symbol->kernel_block[2] = func_decl->kernel_block[2];
  func_symbol->kernel_threads_per_item = func_decl->kernel_threads_per_item;
  func_symbol->is_extern = func_decl->is_extern;
  if (func_decl->is_extern) {
    const char *effective_link_name = type_checker_decl_link_name(
        func_decl->name, func_decl->is_extern, func_decl->link_name);
    func_symbol->link_name =
        effective_link_name ? strdup(effective_link_name) : NULL;
    if (!func_symbol->link_name) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Failed to allocate link name for extern function '%s'",
          func_decl->name);
      symbol_destroy(func_symbol);
      return 0;
    }
  }
  return func_symbol;
}

static int type_checker_check_function_body(
    TypeChecker *checker, ASTNode *declaration,
    FunctionDeclaration *func_decl, Symbol *func_symbol,
    Type *return_type) {
  // Add parameters to the new scope
  Type **active_param_types =
      checker->current_function->data.function.parameter_types;
  if (func_decl->parameter_count > 0) {
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      Symbol *param_symbol =
          symbol_create(func_decl->parameter_names[i], SYMBOL_PARAMETER,
                        active_param_types ? active_param_types[i] : NULL);
      if (!param_symbol) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Failed to create parameter symbol");
        symbol_table_exit_scope(checker->symbol_table);
        return 0;
      }
      if (func_decl->is_kernel && active_param_types &&
          active_param_types[i] &&
          active_param_types[i]->kind == TYPE_POINTER) {
        param_symbol->address_space = MTLC_ADDRESS_SPACE_GLOBAL;
      }
      param_symbol->decl_line = declaration->location.line;
      param_symbol->decl_column = declaration->location.column;
      param_symbol->decl_file = declaration->location.filename;
      if (!symbol_table_declare(checker->symbol_table, param_symbol)) {
        type_checker_report_duplicate_declaration(
            checker, declaration->location, func_decl->parameter_names[i]);
        symbol_destroy(param_symbol);
        type_checker_init_tracker_reset(checker);
        symbol_table_exit_scope(checker->symbol_table);
        return 0;
      }
      if (!type_checker_init_tracker_declare(
              checker, func_decl->parameter_names[i], 1)) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Out of memory while tracking parameter initialization");
        type_checker_init_tracker_reset(checker);
        symbol_table_exit_scope(checker->symbol_table);
        return 0;
      }
      Type *param_type = active_param_types ? active_param_types[i] : NULL;
      if (param_type && param_type->kind == TYPE_POINTER) {
        if (!type_checker_buffer_extent_declare(
                checker, func_decl->parameter_names[i], -1, -1)) {
          type_checker_set_error_at_location(
              checker, declaration->location,
              "Out of memory while tracking pointer parameter extent");
          type_checker_init_tracker_reset(checker);
          symbol_table_exit_scope(checker->symbol_table);
          return 0;
        }
      }
    }
  }

  // Process the function body
  if (func_decl->body &&
      !type_checker_check_statement(checker, func_decl->body)) {
    // Error already reported
    type_checker_init_tracker_reset(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  // Memory diagnostics (use-after-free, dangling stack addresses,
  // constant out-of-bounds accesses, leaks). The scope is still live, so
  // `const` locals resolve for constant-index evaluation.
  if (func_decl->body &&
      !type_checker_check_function_memory(checker, declaration)) {
    type_checker_init_tracker_reset(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  // A function with a non-void return type must contain at least one
  // return statement. This is a simple body-walk (a missing return on
  // some paths is not yet diagnosed); a function with no return at all
  // would otherwise compile and return garbage from RAX/XMM0. `main` is
  // exempt: the entry point falls through to an implicit `return 0`.
  /* A `@naked` function has no frame and no compiled epilogue: its asm block
   * loads the return register and returns itself, so there is no `return`
   * statement to find and no garbage to warn about. */
  if (func_decl->body && return_type &&
      return_type->kind != TYPE_VOID && !func_decl->is_naked &&
      strcmp(func_decl->name, "main") != 0 &&
      !type_checker_ast_contains_node_type(func_decl->body,
                                           AST_RETURN_STATEMENT)) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Function '%s' has non-void return type '%s' but contains no return "
        "statement",
        func_decl->name, return_type->name);
    type_checker_init_tracker_reset(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }
  return 1;
}

static Type *type_checker_function_return_type(
    TypeChecker *checker, ASTNode *declaration,
    FunctionDeclaration *func_decl) {
  // Resolve return type
  Type *return_type = NULL;
  if (func_decl->return_type_count > 0 &&
      !type_checker_ensure_multi_return_type(checker, func_decl,
                                             declaration->location)) {
    /* Only speak in generalities when nothing more specific was said: the
     * builder names the offending return value when it can. */
    if (!checker->has_error) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Could not build the multiple return type for function '%s'",
          func_decl->name);
    }
    return 0;
  }
  if (func_decl->return_type) {
    return_type =
        type_checker_get_type_by_name(checker, func_decl->return_type);
    if (!return_type) {
      type_checker_report_undefined_symbol(checker, declaration->location,
                                           func_decl->return_type, "type");
      return 0;
    }
    if (type_checker_reject_no_runtime_repr(checker, declaration->location,
                                            return_type)) {
      return 0;
    }
  } else {
    return_type = checker->builtin_void;
  }
  if (func_decl->is_kernel && return_type->kind != TYPE_VOID) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "GPU kernel '%s' must return void (remove '-> %s')", func_decl->name,
        return_type->name ? return_type->name : "non-void");
    return 0;
  }
  return return_type;
}

static int type_checker_bind_function_symbol(
    TypeChecker *checker, ASTNode *declaration,
    FunctionDeclaration *func_decl, Symbol *func_symbol,
    int *is_resolving_forward, Symbol **existing_before_out,
    int *handled) {
  *handled = 1;
  Symbol *existing_before = symbol_table_lookup_current_scope(
      checker->symbol_table, func_decl->name);
  *is_resolving_forward =
      (existing_before && existing_before->kind == SYMBOL_FUNCTION &&
       existing_before->is_forward_declaration);

  if (existing_before && existing_before->kind != SYMBOL_FUNCTION) {
    type_checker_report_duplicate_declaration(checker, declaration->location,
                                              func_decl->name);
    symbol_destroy(func_symbol);
    return 0;
  }

  if (existing_before && existing_before->kind == SYMBOL_FUNCTION) {
    if (existing_before->is_extern != func_decl->is_extern) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Function '%s' redeclared with conflicting extern/non-extern "
          "linkage",
          func_decl->name);
      symbol_destroy(func_symbol);
      return 0;
    }
    if ((existing_before->is_extern || func_decl->is_extern) &&
        !type_checker_link_name_matches_symbol(
            existing_before, func_decl->name, func_decl->is_extern,
            func_decl->link_name)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Function '%s' redeclared with conflicting link name",
          func_decl->name);
      symbol_destroy(func_symbol);
      return 0;
    }
  }

  // Forward declaration: no body
  if (!func_decl->body) {
    func_symbol->is_initialized = 0;
    if (!symbol_table_declare_forward(checker->symbol_table, func_symbol)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Invalid or conflicting forward declaration for function '%s'",
          func_decl->name);
      symbol_destroy(func_symbol);
      return 0;
    }
    if (*is_resolving_forward) {
      symbol_destroy(func_symbol);
    }
    return 1;
  }

  func_symbol->is_initialized = 1;
  if (!symbol_table_resolve_forward_declaration(checker->symbol_table,
                                                func_symbol)) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Function definition for '%s' does not match existing declaration",
        func_decl->name);
    symbol_destroy(func_symbol);
    return 0;
  }
  *existing_before_out = existing_before;
  *handled = 0;
  return 1;
}

static int type_checker_process_function(TypeChecker *checker,
                                   ASTNode *declaration,
                                   int *handled) {
  *handled = 1;
  switch (declaration->type) {
  case AST_FUNCTION_DECLARATION: {
    FunctionDeclaration *func_decl = (FunctionDeclaration *)declaration->data;
    if (!func_decl || !func_decl->name) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid function declaration");
      return 0;
    }

    if (func_decl->link_name && !func_decl->is_extern) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Link-name suffix is only allowed on extern declarations");
      return 0;
    }

    if (func_decl->is_rule) {
      Symbol *rule_symbol =
          symbol_table_lookup(checker->symbol_table, func_decl->name);
      Type **rule_params = rule_symbol && rule_symbol->kind == SYMBOL_FUNCTION
                               ? rule_symbol->data.function.parameter_types
                               : NULL;
      Type *rule_return = rule_symbol && rule_symbol->kind == SYMBOL_FUNCTION
                              ? rule_symbol->data.function.return_type
                              : NULL;
      if (!type_checker_validate_rule_signature(checker, declaration, func_decl,
                                                rule_params, rule_return)) {
        return 0;
      }
    }

    Scope *current_scope =
        symbol_table_get_current_scope(checker->symbol_table);
    if (func_decl->is_kernel &&
        (!current_scope || current_scope->type != SCOPE_GLOBAL)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "GPU kernel '%s' must be declared at top level", func_decl->name);
      return 0;
    }
    /* `extern kernel name(params);` is the host-side declaration of a kernel
     * defined in a separately compiled device module. It carries the signature
     * so `dispatch` can check its arguments, and it never has a body. A
     * non-extern `kernel` is a definition and must. */
    if (func_decl->is_kernel && !func_decl->is_extern && !func_decl->body) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "GPU kernel '%s' must have a body (an 'extern kernel' declaration "
          "names one defined in a device module)",
          func_decl->name);
      return 0;
    }
    if (func_decl->is_extern &&
        (!current_scope || current_scope->type != SCOPE_GLOBAL)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern declarations are only allowed at top level");
      return 0;
    }
    if (func_decl->is_extern && func_decl->body) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern function '%s' must not have a body", func_decl->name);
      return 0;
    }

    Type *return_type =
        type_checker_function_return_type(checker, declaration, func_decl);
    if (!return_type) {
      return 0;
    }

    Symbol *func_symbol = type_checker_build_function_symbol(
        checker, declaration, func_decl, return_type);
    if (!func_symbol) {
      return 0;
    }

    int is_resolving_forward = 0;
    Symbol *existing_before = NULL;
    {
      int bound = 0;
      int result = type_checker_bind_function_symbol(
          checker, declaration, func_decl, func_symbol,
          &is_resolving_forward, &existing_before, &bound);
      if (bound) {
        return result;
      }
    }

    if (is_resolving_forward) {
      checker->current_function = existing_before;
      symbol_destroy(func_symbol); // not inserted, existing symbol was updated
      func_symbol = existing_before;
    } else {
      checker->current_function = func_symbol;
    }
    checker->current_function_decl = declaration;

    // Enter a new scope for the function body
    if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_FUNCTION)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Out of memory while entering function scope");
      return 0;
    }
    type_checker_init_tracker_reset(checker);
    if (!type_checker_init_tracker_enter_scope(checker)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Out of memory while initializing flow analysis scope");
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }

    if (!type_checker_check_function_body(checker, declaration, func_decl,
                                         func_symbol, return_type)) {
      return 0;
    }

    type_checker_init_tracker_exit_scope(checker);
    type_checker_init_tracker_reset(checker);

    // Exit the function's scope
    symbol_table_exit_scope(checker->symbol_table);

    // Reset the current function in the type checker
    checker->current_function = NULL;
    checker->current_function_decl = NULL;

    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int type_checker_process_member(TypeChecker *checker,
                                   ASTNode *declaration,
                                   int *handled) {
  *handled = 1;
  switch (declaration->type) {
  case AST_METHOD_DECLARATION:
    // Method declarations are handled within struct processing
    // This case shouldn't normally be reached during standalone processing
    return 1;

  case AST_INLINE_ASM:
    // Top-level inline assembly is permitted.
    return 1;

  case AST_EFFECT_DECLARATION:
    return 1;

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int type_checker_check_multi_assignment(
    TypeChecker *checker, ASTNode *declaration,
    Assignment *assignment, int *handled) {
  *handled = 1;
  if (assignment->target_count > 0) {
    Type *value_type = type_checker_infer_type(checker, assignment->value);
    if (!value_type || value_type->kind != TYPE_STRUCT ||
        value_type->field_count != assignment->target_count) {
      type_checker_set_error_at_location(
          checker, assignment->value->location,
          "Multiple assignment needs a matching multiple return value");
      return 0;
    }

    for (size_t i = 0; i < assignment->target_count; i++) {
      ASTNode *target = assignment->targets[i];
      if (!target || target->type != AST_IDENTIFIER) {
        type_checker_set_error_at_location(
            checker, target ? target->location : declaration->location,
            "Multiple return assignment targets must be identifiers");
        return 0;
      }
      Identifier *identifier = (Identifier *)target->data;
      Symbol *symbol = identifier && identifier->name
                           ? type_checker_resolve_identifier(checker,
                                                             identifier)
                           : NULL;
      if (!symbol || (symbol->kind != SYMBOL_VARIABLE &&
                      symbol->kind != SYMBOL_PARAMETER)) {
        type_checker_report_undefined_symbol(
            checker, target->location,
            identifier && identifier->name ? identifier->name : "<invalid>",
            "variable");
        return 0;
      }
      if (symbol->is_immutable) {
        type_checker_set_error_at_location(
            checker, target->location, "'%s' is a constant and cannot be assigned to",
            identifier->name);
        return 0;
      }
      Type *field_type = value_type->field_types[i];
      if (!type_checker_is_assignable(checker, symbol->type, field_type)) {
        type_checker_report_assign_mismatch(checker, NULL, target->location,
                                            symbol->type, field_type);
        return 0;
      }
      if (checker->current_function && symbol->scope &&
          symbol->scope->type != SCOPE_GLOBAL) {
        type_checker_init_tracker_set_initialized(checker, identifier->name);
      }
    }
    return 1;
  }
  *handled = 0;
  return 1;
}

static int type_checker_check_member_assignment(
    TypeChecker *checker, Assignment *assignment,
    MemberAccess *member) {
  if (!member || !member->object || !member->member) {
    type_checker_set_error_at_location(checker,
                                       assignment->target->location,
                                       "Invalid field assignment target");
    return 0;
  }

  Type *object_type = type_checker_infer_type(checker, member->object);
  if (!object_type) {
    return 0;
  }
  /* Assigning through a pointer-to-struct auto-dereferences (like `->`). */
  if (object_type->kind == TYPE_POINTER && object_type->base_type) {
    object_type = object_type->base_type;
  }

  if (object_type->kind != TYPE_STRUCT &&
      object_type->kind != TYPE_STRING) {
    char error_msg[512];
    snprintf(error_msg, sizeof(error_msg),
             "Cannot assign field '%s' on non-struct/string type '%s'",
             member->member, object_type->name);
    type_checker_set_error_at_location(
        checker, assignment->target->location, error_msg);
    return 0;
  }

  Type *field_type = type_get_field_type(object_type, member->member);
  if (!field_type) {
    char error_msg[512];
    snprintf(error_msg, sizeof(error_msg),
             "Field '%s' not found in type '%s'", member->member,
             object_type->name);
    type_checker_set_error_at_location(
        checker, assignment->target->location, error_msg);
    return 0;
  }

  checker->aggregate_target_type = field_type;
  Type *value_type = type_checker_infer_type(checker, assignment->value);
  checker->aggregate_target_type = NULL;
  if (!value_type) {
    if (!checker->has_error) {
      type_checker_set_error_at_location(
          checker, assignment->value->location,
          "Cannot infer type of assignment value");
    }
    return 0;
  }

  if (!(type_checker_type_accepts_null_pointer(field_type) &&
        type_checker_is_null_pointer_constant(assignment->value)) &&
      !type_checker_is_assignable_from(checker, field_type, value_type,
                                       assignment->value)) {
    type_checker_report_assign_mismatch(checker, assignment->value,
                                        assignment->value->location,
                                        field_type, value_type);
    return 0;
  }

  return 1;
}

static int type_checker_check_target_assignment(
    TypeChecker *checker, ASTNode *declaration,
    Assignment *assignment, int *handled) {
  *handled = 1;
  // Complex assignment target: obj.field = value or arr[i] = value
  if (assignment->target) {
    if (assignment->target->type == AST_MEMBER_ACCESS) {
      MemberAccess *member = (MemberAccess *)assignment->target->data;
      return type_checker_check_member_assignment(checker, assignment,
                                                  member);
    } else if (assignment->target->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *target_index =
          (ArrayIndexExpression *)assignment->target->data;
      if (!target_index || !target_index->array || !target_index->index) {
        type_checker_set_error_at_location(checker,
                                           assignment->target->location,
                                           "Invalid array assignment target");
        return 0;
      }

      Type *target_array_type =
          type_checker_infer_type(checker, target_index->array);
      if (!target_array_type) {
        return 0;
      }
      /* `s[i]` reads a character; it is not a place to put one. A string
       * is a borrowed view, and the bytes it points at are as likely to be
       * a literal in read-only memory as a buffer the program owns. Reach
       * the bytes through `s.chars` when they are genuinely writable. */
      if (target_array_type->kind == TYPE_STRING) {
        type_checker_set_error_at_location(
            checker, assignment->target->location,
            "Cannot assign through a string index: a string is a borrowed "
            "view and its bytes may be read-only");
        if (checker->error_reporter) {
          error_reporter_set_last_label(
              checker->error_reporter,
              "write through 's.chars' when the bytes are yours to change");
        }
        return 0;
      }
      if (target_array_type->kind == TYPE_ARRAY) {
        long long constant_index = 0;
        if (type_checker_eval_integer_constant(target_index->index,
                                               &constant_index)) {
          if (constant_index < 0 ||
              (unsigned long long)constant_index >=
                  (unsigned long long)target_array_type->array_size) {
            type_checker_set_error_at_location(
                checker, target_index->index->location,
                "Array index %lld is out of bounds for '%s' (size %zu)",
                constant_index,
                target_array_type->name ? target_array_type->name : "array",
                target_array_type->array_size);
            return 0;
          }
        }
      }

      Type *element_type =
          type_checker_infer_type(checker, assignment->target);
      if (!element_type) {
        return 0;
      }

      checker->aggregate_target_type = element_type;
      Type *value_type = type_checker_infer_type(checker, assignment->value);
      checker->aggregate_target_type = NULL;
      if (!value_type) {
        if (!checker->has_error) {
          type_checker_set_error_at_location(
              checker, assignment->value->location,
              "Cannot infer type of assignment value");
        }
        return 0;
      }

      if (!(type_checker_type_accepts_null_pointer(element_type) &&
            type_checker_is_null_pointer_constant(assignment->value)) &&
          !type_checker_is_assignable_from(checker, element_type, value_type,
                                           assignment->value)) {
        type_checker_report_assign_mismatch(checker, assignment->value,
                                            assignment->value->location,
                                            element_type, value_type);
        return 0;
      }

      return 1;
    } else if (assignment->target->type == AST_UNARY_EXPRESSION) {
      UnaryExpression *target_unary =
          (UnaryExpression *)assignment->target->data;
      if (!target_unary || !target_unary->operator ||
          strcmp(target_unary->operator, "*") != 0) {
        type_checker_set_error_at_location(checker,
                                           assignment->target->location,
                                           "Invalid assignment target");
        return 0;
      }

      Type *target_type =
          type_checker_infer_type(checker, assignment->target);
      if (!target_type) {
        return 0;
      }

      checker->aggregate_target_type = target_type;
      Type *value_type = type_checker_infer_type(checker, assignment->value);
      checker->aggregate_target_type = NULL;
      if (!value_type) {
        if (!checker->has_error) {
          type_checker_set_error_at_location(
              checker, assignment->value->location,
              "Cannot infer type of assignment value");
        }
        return 0;
      }

      if (!(type_checker_type_accepts_null_pointer(target_type) &&
            type_checker_is_null_pointer_constant(assignment->value)) &&
          !type_checker_is_assignable_from(checker, target_type, value_type,
                                           assignment->value)) {
        type_checker_report_assign_mismatch(checker, assignment->value,
                                            assignment->value->location,
                                            target_type, value_type);
        return 0;
      }

      return 1;
    }

    type_checker_set_error_at_location(checker, assignment->target->location,
                                       "Invalid assignment target");
    return 0;
  }
  *handled = 0;
  return 1;
}

static int type_checker_process_assignment(TypeChecker *checker,
                                   ASTNode *declaration,
                                   int *handled) {
  *handled = 1;
  switch (declaration->type) {
  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)declaration->data;
    if (!assignment || !assignment->value) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid assignment statement");
      return 0;
    }

    {
      int multi_handled = 0;
      int checked = type_checker_check_multi_assignment(
          checker, declaration, assignment, &multi_handled);
      if (multi_handled) {
        return checked;
      }
    }

    {
      int target_handled = 0;
      int checked = type_checker_check_target_assignment(
          checker, declaration, assignment, &target_handled);
      if (target_handled) {
        return checked;
      }
    }

    // Simple variable assignment: name = value
    if (!assignment->variable_name) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid assignment statement");
      return 0;
    }

    // Look up the variable
    Symbol *var_symbol =
        symbol_table_lookup(checker->symbol_table, assignment->variable_name);
    if (!var_symbol) {
      type_checker_report_undefined_symbol(checker, declaration->location,
                                           assignment->variable_name,
                                           "variable");
      return 0;
    }

    if (var_symbol->kind != SYMBOL_VARIABLE &&
        var_symbol->kind != SYMBOL_PARAMETER) {
      char error_msg[512];
      const char *symbol_type =
          (var_symbol->kind == SYMBOL_FUNCTION)   ? "function"
          : (var_symbol->kind == SYMBOL_STRUCT)   ? "struct"
          : (var_symbol->kind == SYMBOL_CONSTANT) ? "constant"
                                                  : "symbol";
      snprintf(error_msg, sizeof(error_msg),
               "'%s' is a %s and cannot be assigned to",
               assignment->variable_name, symbol_type);
      type_checker_set_error_at_location(checker, declaration->location,
                                         error_msg);
      return 0;
    }

    if (var_symbol->is_immutable) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "'%s' is a constant and cannot be assigned to",
          assignment->variable_name);
      return 0;
    }
    if (var_symbol->is_address_space_binding) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "GPU address-space binding '%s' cannot be rebound; assign its "
          "elements instead",
          assignment->variable_name);
      return 0;
    }

    // Infer the type of the assignment value
    checker->aggregate_target_type = var_symbol->type;
    Type *value_type = type_checker_infer_type(checker, assignment->value);
    checker->aggregate_target_type = NULL;
    if (!value_type) {
      if (!checker->has_error) {
        type_checker_set_error_at_location(
            checker, assignment->value->location,
            "Cannot infer type of assignment value");
      }
      return 0;
    }
    if (type_checker_reject_comptime_escape(
            checker, assignment->value->location, value_type)) {
      return 0;
    }

    // Validate assignment compatibility
    if (!(type_checker_type_accepts_null_pointer(var_symbol->type) &&
          type_checker_is_null_pointer_constant(assignment->value)) &&
        !type_checker_is_assignable_from(checker, var_symbol->type, value_type,
                                         assignment->value)) {
      type_checker_report_assign_mismatch(checker, assignment->value,
                                          assignment->value->location,
                                          var_symbol->type, value_type);
      return 0;
    }

    if (checker->current_function && var_symbol->scope &&
        var_symbol->scope->type != SCOPE_GLOBAL) {
      type_checker_init_tracker_set_initialized(checker,
                                                assignment->variable_name);
      if (var_symbol->type && var_symbol->type->kind == TYPE_POINTER) {
        long long known_extent =
            type_checker_extract_known_buffer_extent(checker, assignment->value);
        long long known_alignment = type_checker_extract_known_pointer_alignment(
            checker, assignment->value);
        if (!type_checker_buffer_extent_set(checker, assignment->variable_name,
                                            known_extent, known_alignment)) {
          type_checker_set_error_at_location(
              checker, assignment->value->location,
              "Out of memory while tracking buffer extent for '%s'",
              assignment->variable_name);
          return 0;
        }
      }
    }

    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int type_checker_reject_file_scope(TypeChecker *checker,
                                          ASTNode *declaration) {
    /* An expression or statement reached file scope, where only declarations
       live. Name what it is so the reader can see which line to move. */
    const char *shape = "statement";
    switch (declaration->type) {
    case AST_ASSIGNMENT:
      shape = "assignment";
      break;
    case AST_BINARY_EXPRESSION:
    case AST_UNARY_EXPRESSION:
      shape = "expression";
      break;
    case AST_MEMBER_ACCESS:
      shape = "field read";
      break;
    case AST_INDEX_EXPRESSION:
      shape = "index read";
      break;
    case AST_IF_STATEMENT:
      shape = "'if'";
      break;
    case AST_WHILE_STATEMENT:
    case AST_FOR_STATEMENT:
      shape = "loop";
      break;
    case AST_RETURN_STATEMENT:
      shape = "'return'";
      break;
    case AST_SWITCH_STATEMENT:
      shape = "'switch'";
      break;
    default:
      break;
    }
    type_checker_set_error_at_location(
        checker, declaration->location,
        "This %s cannot stand at file scope. File scope holds declarations "
        "(fn, struct, enum, var, const, import); move it into a function body",
        shape);
    if (checker->error_reporter)
      error_reporter_set_last_label(checker->error_reporter,
                                    "this needs a function to run in");
    return 0;
}

int type_checker_process_declaration(TypeChecker *checker,
                                     ASTNode *declaration) {
  static int (*const PROCESSORS[])(TypeChecker *, ASTNode *, int *) = {
      type_checker_process_deferred,
      type_checker_process_variable,
      type_checker_process_function,
      type_checker_process_member,
      type_checker_process_assignment};
  size_t processor;

  if (!checker || !declaration) {
    return 0;
  }

  for (processor = 0; processor < sizeof(PROCESSORS) / sizeof(PROCESSORS[0]);
       processor++) {
    int handled = 0;
    int checked = PROCESSORS[processor](checker, declaration, &handled);
    if (handled) {
      return checked;
    }
  }
  return type_checker_reject_file_scope(checker, declaration);
}
