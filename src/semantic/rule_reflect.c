#include "rule_reflect.h"
#include "import_resolver.h"
#include "type_checker_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  char *qualified;
  const char *module;
  IRRuleSite site;
  const char **callees;
  size_t callee_count;
  size_t callee_capacity;
  const char **matches;
  size_t match_count;
  size_t match_capacity;
  const char **param_types;
  size_t param_count;
  const char *return_type;
  int is_extern;
  int is_exported;
  int is_recursive;
  int is_address_taken;
  int has_indirect_calls;
  int is_noalloc;
  int is_pure;
  int is_inline;
  int is_swappable;
  int is_kernel;
  const char *declared_name;
  const char **effects;
  size_t effect_count;
  const char **requires;
  size_t require_count;
  const char **forbids;
  size_t forbid_count;
  const char **provides;
  size_t provide_count;
  int scc_index;
  int scc_lowlink;
  int scc_on_stack;
} RFunction;

typedef struct {
  const char *name;
  char *qualified;
  const char *module;
  IRRuleSite site;
  const char *kind;
  const char *base;
  long long size;
  long long align;
  long long layout;
  const Type *type;
} RType;

typedef struct {
  unsigned char *bytes;
  size_t size;
  size_t capacity;
  size_t *pointers;
  size_t pointer_count;
  size_t pointer_capacity;
  unsigned char *pool;
  size_t pool_size;
  size_t pool_capacity;
  size_t *string_fields;
  size_t *string_offsets;
  size_t string_count;
  size_t string_capacity;
  int failed;
} Image;

typedef struct {
  TypeChecker *checker;
  RFunction *functions;
  size_t function_count;
  size_t function_capacity;
  RType *types;
  size_t type_count;
  size_t type_capacity;
  const char **modules;
  size_t module_count;
  size_t module_capacity;
  char **owned;
  size_t owned_count;
  size_t owned_capacity;
  RFunction *current;
} Reflect;

static const char *strip_import_prefix(const char *name) {
  const char *p = name ? name : "";
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

static char *qualify(const char *module, const char *name) {
  size_t length = strlen(module) + strlen(name) + 2;
  char *out = malloc(length);
  if (!out) {
    return NULL;
  }
  if (module[0]) {
    snprintf(out, length, "%s.%s", module, name);
  } else {
    snprintf(out, length, "%s", name);
  }
  return out;
}

static const char *module_of(const char *filename) {
  const char *module = filename ? import_resolver_module_for_file(filename)
                                : NULL;
  return module ? module : "";
}

static int grow(void **items, size_t *capacity, size_t count,
                size_t item_size) {
  if (count < *capacity) {
    return 1;
  }
  size_t next = *capacity ? *capacity * 2 : 16;
  void *grown = realloc(*items, next * item_size);
  if (!grown) {
    return 0;
  }
  *items = grown;
  *capacity = next;
  return 1;
}

static int note_module(Reflect *reflect, const char *module) {
  if (!module || !module[0]) {
    return 1;
  }
  for (size_t i = 0; i < reflect->module_count; i++) {
    if (strcmp(reflect->modules[i], module) == 0) {
      return 1;
    }
  }
  if (!grow((void **)&reflect->modules, &reflect->module_capacity,
            reflect->module_count, sizeof(const char *))) {
    return 0;
  }
  reflect->modules[reflect->module_count++] = module;
  return 1;
}

static RFunction *find_function(Reflect *reflect, const char *declared_name) {
  if (!declared_name) {
    return NULL;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    if (strcmp(reflect->functions[i].declared_name, declared_name) == 0) {
      return &reflect->functions[i];
    }
  }
  return NULL;
}

static int add_unique(const char ***items, size_t *count, size_t *capacity,
                      const char *text) {
  for (size_t i = 0; i < *count; i++) {
    if (strcmp((*items)[i], text) == 0) {
      return 1;
    }
  }
  if (!grow((void **)items, capacity, *count, sizeof(const char *))) {
    return 0;
  }
  (*items)[(*count)++] = text;
  return 1;
}

static int add_callee(RFunction *function, const char *qualified) {
  return add_unique(&function->callees, &function->callee_count,
                    &function->callee_capacity, qualified);
}

static const char *own_string(Reflect *reflect, char *text) {
  if (!text) {
    return NULL;
  }
  if (!grow((void **)&reflect->owned, &reflect->owned_capacity,
            reflect->owned_count, sizeof(char *))) {
    free(text);
    return NULL;
  }
  reflect->owned[reflect->owned_count++] = text;
  return text;
}

static const char *type_display_name(const Type *type);

static int add_match(Reflect *reflect, RFunction *function, const Type *owner,
                     const char *variant) {
  if (!owner || !variant) {
    return 1;
  }
  const char *shown = strip_import_prefix(variant);
  const char *owner_name = type_display_name(owner);
  size_t length = strlen(owner_name) + strlen(shown) + 2;
  char *text = malloc(length);
  if (!text) {
    return 0;
  }
  snprintf(text, length, "%s.%s", owner_name, shown);
  for (size_t i = 0; i < function->match_count; i++) {
    if (strcmp(function->matches[i], text) == 0) {
      free(text);
      return 1;
    }
  }
  const char *kept = own_string(reflect, text);
  return kept && add_unique(&function->matches, &function->match_count,
                            &function->match_capacity, kept);
}

static int walk_body(Reflect *reflect, ASTNode *node) {
  if (!node) {
    return 1;
  }
  RFunction *current = reflect->current;
  if (node->type == AST_FUNCTION_CALL && node->data) {
    CallExpression *call = (CallExpression *)node->data;
    if (call->is_indirect_call) {
      current->has_indirect_calls = 1;
    } else if (call->function_name) {
      RFunction *callee = find_function(reflect, call->function_name);
      const char *spelling =
          callee ? callee->qualified : strip_import_prefix(call->function_name);
      if (strncmp(spelling, "__", 2) != 0 && !add_callee(current, spelling)) {
        return 0;
      }
    }
  } else if (node->type == AST_FUNC_PTR_CALL) {
    current->has_indirect_calls = 1;
  } else if (node->type == AST_MATCH_STATEMENT && node->data) {
    MatchStatement *match = (MatchStatement *)node->data;
    const Type *owner = match->expression ? match->expression->resolved_type
                                          : NULL;
    if (owner && owner->kind == TYPE_POINTER && owner->base_type) {
      owner = owner->base_type;
    }
    for (size_t i = 0; i < match->arm_count; i++) {
      if (!match->arms[i].is_default &&
          !add_match(reflect, current, owner, match->arms[i].variant_name)) {
        return 0;
      }
    }
  } else if (node->type == AST_SWITCH_STATEMENT && node->data) {
    SwitchStatement *sw = (SwitchStatement *)node->data;
    for (size_t i = 0; i < sw->case_count; i++) {
      CaseClause *clause =
          sw->cases[i] && sw->cases[i]->data ? (CaseClause *)sw->cases[i]->data
                                             : NULL;
      if (!clause || clause->is_default || !clause->value ||
          clause->value->type != AST_MEMBER_ACCESS || !clause->value->data) {
        continue;
      }
      MemberAccess *member = (MemberAccess *)clause->value->data;
      const Type *owner = clause->value->resolved_type;
      if (owner && owner->kind == TYPE_ENUM &&
          !add_match(reflect, current, owner, member->member)) {
        return 0;
      }
    }
  } else if (node->type == AST_IDENTIFIER && node->data) {
    Identifier *identifier = (Identifier *)node->data;
    RFunction *named = find_function(reflect, identifier->name);
    if (named) {
      named->is_address_taken = 1;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!walk_body(reflect, node->children[i])) {
      return 0;
    }
  }
  return 1;
}

static int collect_function(Reflect *reflect, ASTNode *decl) {
  FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
  if (!fd || !fd->name || fd->is_test || fd->is_rule || fd->rewrite_role) {
    return 1;
  }
  if (!grow((void **)&reflect->functions, &reflect->function_capacity,
            reflect->function_count, sizeof(RFunction))) {
    return 0;
  }
  RFunction *function = &reflect->functions[reflect->function_count++];
  memset(function, 0, sizeof(*function));
  function->declared_name = fd->name;
  function->name = strip_import_prefix(fd->name);
  function->module = module_of(decl->location.filename);
  function->qualified = qualify(function->module, function->name);
  if (!function->qualified || !note_module(reflect, function->module)) {
    return 0;
  }
  function->site.file = decl->location.filename;
  function->site.line = decl->location.line;
  function->site.column = decl->location.column;
  function->param_types = (const char **)fd->parameter_types;
  function->param_count = fd->parameter_count;
  function->return_type = fd->return_type ? fd->return_type : "void";
  function->is_extern = fd->is_extern;
  function->is_exported = fd->is_exported;
  function->is_noalloc = fd->is_noalloc;
  function->is_pure = fd->is_pure;
  function->is_inline = fd->is_inline;
  function->is_swappable = fd->is_swappable;
  function->is_kernel = fd->is_kernel;
  function->forbids = (const char **)fd->effects_forbids;
  function->forbid_count = fd->effects_forbids_count;
  function->provides = (const char **)fd->effects_provides;
  function->provide_count = fd->effects_provides_count;
  function->effects = (const char **)fd->effects_with;
  function->effect_count = fd->effects_with_count;
  function->requires = (const char **)fd->effects_requires;
  function->require_count = fd->effects_requires_count;
  return 1;
}

static int collect_type(Reflect *reflect, ASTNode *decl) {
  const char *name = NULL;
  size_t type_params = 0;
  if (decl->type == AST_STRUCT_DECLARATION && decl->data) {
    StructDeclaration *sd = (StructDeclaration *)decl->data;
    name = sd->name;
    type_params = sd->type_param_count;
  } else if (decl->type == AST_ENUM_DECLARATION && decl->data) {
    EnumDeclaration *ed = (EnumDeclaration *)decl->data;
    name = ed->name;
    type_params = ed->type_param_count;
  } else if (decl->type == AST_TYPE_DECLARATION && decl->data) {
    name = ((TypeDeclaration *)decl->data)->name;
  }
  if (!name || type_params > 0) {
    return 1;
  }
  Type *type = type_checker_get_type_by_name(reflect->checker, name);
  if (!type) {
    return 1;
  }
  if (!type->refined_base && type->kind != TYPE_STRUCT &&
      type->kind != TYPE_ENUM && type->kind != TYPE_TAGGED_ENUM) {
    return 1;
  }
  if (!grow((void **)&reflect->types, &reflect->type_capacity,
            reflect->type_count, sizeof(RType))) {
    return 0;
  }
  RType *entry = &reflect->types[reflect->type_count++];
  memset(entry, 0, sizeof(*entry));
  entry->name = strip_import_prefix(name);
  entry->module = module_of(decl->location.filename);
  entry->qualified = qualify(entry->module, entry->name);
  if (!entry->qualified || !note_module(reflect, entry->module)) {
    return 0;
  }
  entry->site.file = decl->location.filename;
  entry->site.line = decl->location.line;
  entry->site.column = decl->location.column;
  entry->kind = type->refined_base            ? "declared"
                : type->kind == TYPE_STRUCT   ? "struct"
                : type->kind == TYPE_ENUM     ? "enum"
                                              : "tagged_enum";
  entry->base = type->refined_base ? type_display_name(type->refined_base) : "";
  entry->size = (long long)type->size;
  entry->align = (long long)type->alignment;
  entry->layout = type_checker_layout_digest(type);
  entry->type = type;
  return 1;
}

typedef struct {
  int *stack;
  size_t depth;
  int next_index;
} Tarjan;

static size_t function_index(Reflect *reflect, RFunction *function) {
  return (size_t)(function - reflect->functions);
}

static RFunction *find_by_qualified(Reflect *reflect, const char *qualified) {
  for (size_t i = 0; i < reflect->function_count; i++) {
    if (strcmp(reflect->functions[i].qualified, qualified) == 0) {
      return &reflect->functions[i];
    }
  }
  return NULL;
}

static void tarjan_visit(Reflect *reflect, Tarjan *state, RFunction *v) {
  v->scc_index = state->next_index;
  v->scc_lowlink = state->next_index;
  state->next_index++;
  state->stack[state->depth++] = (int)function_index(reflect, v);
  v->scc_on_stack = 1;
  for (size_t i = 0; i < v->callee_count; i++) {
    RFunction *w = find_by_qualified(reflect, v->callees[i]);
    if (!w) {
      continue;
    }
    if (w->scc_index < 0) {
      tarjan_visit(reflect, state, w);
      if (w->scc_lowlink < v->scc_lowlink) {
        v->scc_lowlink = w->scc_lowlink;
      }
    } else if (w->scc_on_stack && w->scc_index < v->scc_lowlink) {
      v->scc_lowlink = w->scc_index;
    }
  }
  if (v->scc_lowlink == v->scc_index) {
    size_t members = 0;
    size_t first = state->depth;
    while (first > 0) {
      first--;
      members++;
      if (state->stack[first] == (int)function_index(reflect, v)) {
        break;
      }
    }
    int self_edge = 0;
    for (size_t i = 0; i < v->callee_count; i++) {
      if (strcmp(v->callees[i], v->qualified) == 0) {
        self_edge = 1;
      }
    }
    while (state->depth > first) {
      RFunction *w = &reflect->functions[state->stack[--state->depth]];
      w->scc_on_stack = 0;
      w->is_recursive = members > 1 || self_edge;
    }
  }
}

static int mark_recursion(Reflect *reflect) {
  Tarjan state;
  state.stack = calloc(reflect->function_count + 1, sizeof(int));
  state.depth = 0;
  state.next_index = 0;
  if (!state.stack) {
    return 0;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    reflect->functions[i].scc_index = -1;
    reflect->functions[i].scc_lowlink = -1;
    reflect->functions[i].scc_on_stack = 0;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    if (reflect->functions[i].scc_index < 0) {
      tarjan_visit(reflect, &state, &reflect->functions[i]);
    }
  }
  free(state.stack);
  return 1;
}

static size_t image_reserve(Image *image, size_t size, size_t align) {
  size_t offset = image->size;
  if (align > 1) {
    offset = (offset + align - 1) / align * align;
  }
  size_t end = offset + size;
  if (end > image->capacity) {
    size_t next = image->capacity ? image->capacity : 4096;
    while (next < end) {
      next *= 2;
    }
    unsigned char *grown = realloc(image->bytes, next);
    if (!grown) {
      image->failed = 1;
      return 0;
    }
    memset(grown + image->capacity, 0, next - image->capacity);
    image->bytes = grown;
    image->capacity = next;
  }
  image->size = end;
  return offset;
}

static void image_put_u64(Image *image, size_t offset, unsigned long long v) {
  if (offset + 8 <= image->size) {
    memcpy(image->bytes + offset, &v, 8);
  }
}

static void image_put_bool(Image *image, size_t offset, int v) {
  if (offset < image->size) {
    image->bytes[offset] = v ? 1 : 0;
  }
}

static void image_put_pointer(Image *image, size_t offset, size_t target) {
  image_put_u64(image, offset, (unsigned long long)target);
  if (!grow((void **)&image->pointers, &image->pointer_capacity,
            image->pointer_count, sizeof(size_t))) {
    image->failed = 1;
    return;
  }
  image->pointers[image->pointer_count++] = offset;
}

static void image_put_string(Image *image, size_t offset, const char *text) {
  size_t length = text ? strlen(text) : 0;
  if (length == 0) {
    image_put_u64(image, offset, 0);
    image_put_u64(image, offset + 8, 0);
    return;
  }
  size_t pool_offset = image->pool_size;
  size_t end = pool_offset + length + 1;
  if (end > image->pool_capacity) {
    size_t next = image->pool_capacity ? image->pool_capacity : 4096;
    while (next < end) {
      next *= 2;
    }
    unsigned char *grown = realloc(image->pool, next);
    if (!grown) {
      image->failed = 1;
      return;
    }
    image->pool = grown;
    image->pool_capacity = next;
  }
  memcpy(image->pool + pool_offset, text, length + 1);
  image->pool_size = end;
  if (!grow((void **)&image->string_fields, &image->string_capacity,
            image->string_count, sizeof(size_t))) {
    image->failed = 1;
    return;
  }
  size_t *offsets = realloc(image->string_offsets,
                            image->string_capacity * sizeof(size_t));
  if (!offsets) {
    image->failed = 1;
    return;
  }
  image->string_offsets = offsets;
  image->string_fields[image->string_count] = offset;
  image->string_offsets[image->string_count] = pool_offset;
  image->string_count++;
  image_put_u64(image, offset + 8, (unsigned long long)length);
}

static const Type *lookup_qualified(TypeChecker *checker,
                                    const char *qualified) {
  for (size_t i = 0; i < checker->type_table_count; i++) {
    const Type *type = checker->type_table[i];
    if (type && type->qualified_name &&
        strcmp(type->qualified_name, qualified) == 0) {
      return type;
    }
  }
  return NULL;
}

static size_t field_offset(const Type *type, const char *field, Image *image) {
  int index = type ? type_get_field_index(type, field) : -1;
  if (index < 0 || !type->field_offsets) {
    image->failed = 1;
    return 0;
  }
  return type->field_offsets[index];
}

static const Type *field_type(const Type *type, const char *field,
                              Image *image) {
  int index = type ? type_get_field_index(type, field) : -1;
  if (index < 0 || !type->field_types) {
    image->failed = 1;
    return NULL;
  }
  return type->field_types[index];
}

static void put_site(Image *image, const Type *site_type, size_t base,
                     const IRRuleSite *site) {
  image_put_string(image, base + field_offset(site_type, "file", image),
                   site->file);
  image_put_u64(image, base + field_offset(site_type, "line", image),
                (unsigned long long)site->line);
  image_put_u64(image, base + field_offset(site_type, "column", image),
                (unsigned long long)site->column);
}

static size_t put_string_array(Image *image, const char *const *items,
                               size_t count) {
  if (count == 0) {
    return 0;
  }
  size_t base = image_reserve(image, count * 16, 8);
  for (size_t i = 0; i < count; i++) {
    image_put_string(image, base + i * 16, items[i]);
  }
  return base;
}

static void put_slice(Image *image, size_t offset, size_t target,
                      size_t count) {
  if (count == 0 || target == 0) {
    image_put_u64(image, offset, 0);
    image_put_u64(image, offset + 8, 0);
    return;
  }
  image_put_pointer(image, offset, target);
  image_put_u64(image, offset + 8, (unsigned long long)count);
}

static int add_site(IRRuleImage *out, size_t *capacity, IRRuleSite site) {
  if (!grow((void **)&out->sites, capacity, out->site_count,
            sizeof(IRRuleSite))) {
    return 0;
  }
  out->sites[out->site_count++] = site;
  return 1;
}

static const char *type_display_name(const Type *type) {
  if (!type) {
    return "?";
  }
  if (type->qualified_name && strchr(type->qualified_name, '/')) {
    return type->qualified_name;
  }
  return strip_import_prefix(type->name);
}

static int build_image(Reflect *reflect, const char *root_file,
                       const char *target, IRRuleImage *out,
                       char **error_message) {
  TypeChecker *checker = reflect->checker;
  const Type *program_type = lookup_qualified(checker, "std/rule.Program");
  const Type *function_type = lookup_qualified(checker, "std/rule.Function");
  const Type *type_info_type = lookup_qualified(checker, "std/rule.TypeInfo");
  const Type *field_info_type =
      lookup_qualified(checker, "std/rule.FieldInfo");
  const Type *site_type = lookup_qualified(checker, "std/rule.Site");
  const Type *effect_info_type =
      lookup_qualified(checker, "std/rule.EffectInfo");
  if (!program_type || !function_type || !type_info_type ||
      !field_info_type || !site_type || !effect_info_type) {
    *error_message = strdup("a @rule needs the Program, Function, TypeInfo, "
                            "FieldInfo, EffectInfo and Site records from "
                            "std/rule");
    return 0;
  }
  Image image;
  memset(&image, 0, sizeof(image));
  size_t site_capacity = 0;

  size_t program_offset = image_reserve(&image, program_type->size,
                                        program_type->alignment);
  size_t functions_offset =
      reflect->function_count
          ? image_reserve(&image, function_type->size * reflect->function_count,
                          function_type->alignment)
          : 0;
  size_t types_offset =
      reflect->type_count
          ? image_reserve(&image, type_info_type->size * reflect->type_count,
                          type_info_type->alignment)
          : 0;

  for (size_t i = 0; i < reflect->function_count && !image.failed; i++) {
    RFunction *fn = &reflect->functions[i];
    size_t base = functions_offset + i * function_type->size;
    image_put_string(&image, base + field_offset(function_type, "name", &image),
                     fn->name);
    image_put_string(&image,
                     base + field_offset(function_type, "qualified", &image),
                     fn->qualified);
    image_put_string(&image,
                     base + field_offset(function_type, "module", &image),
                     fn->module);
    put_site(&image, site_type,
             base + field_offset(function_type, "site", &image), &fn->site);
    size_t callees = put_string_array(&image, fn->callees, fn->callee_count);
    put_slice(&image, base + field_offset(function_type, "callees", &image),
              callees, fn->callee_count);
    size_t matches = put_string_array(&image, fn->matches, fn->match_count);
    put_slice(&image, base + field_offset(function_type, "matches", &image),
              matches, fn->match_count);
    size_t params =
        put_string_array(&image, fn->param_types, fn->param_count);
    put_slice(&image,
              base + field_offset(function_type, "param_types", &image),
              params, fn->param_count);
    image_put_string(&image,
                     base + field_offset(function_type, "return_type", &image),
                     fn->return_type);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_extern", &image),
                   fn->is_extern);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_exported", &image),
                   fn->is_exported);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_recursive", &image),
                   fn->is_recursive);
    image_put_bool(
        &image, base + field_offset(function_type, "is_address_taken", &image),
        fn->is_address_taken);
    image_put_bool(
        &image,
        base + field_offset(function_type, "has_indirect_calls", &image),
        fn->has_indirect_calls);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_noalloc", &image),
                   fn->is_noalloc);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_pure", &image),
                   fn->is_pure);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_inline", &image),
                   fn->is_inline);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_swappable", &image),
                   fn->is_swappable);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_kernel", &image),
                   fn->is_kernel);
    {
      size_t effects = put_string_array(&image, fn->effects, fn->effect_count);
      put_slice(&image, base + field_offset(function_type, "effects", &image),
                effects, fn->effect_count);
      size_t requires =
          put_string_array(&image, fn->requires, fn->require_count);
      put_slice(&image, base + field_offset(function_type, "requires", &image),
                requires, fn->require_count);
      size_t forbids = put_string_array(&image, fn->forbids, fn->forbid_count);
      put_slice(&image, base + field_offset(function_type, "forbids", &image),
                forbids, fn->forbid_count);
      size_t provides =
          put_string_array(&image, fn->provides, fn->provide_count);
      put_slice(&image, base + field_offset(function_type, "provides", &image),
                provides, fn->provide_count);
    }
    if (!add_site(out, &site_capacity, fn->site)) {
      image.failed = 1;
    }
  }

  size_t effects_offset = 0;
  if (checker->effect_count > 0) {
    effects_offset =
        image_reserve(&image, effect_info_type->size * checker->effect_count,
                      effect_info_type->alignment);
    for (size_t i = 0; i < checker->effect_count && !image.failed; i++) {
      const TypeCheckerEffect *effect = &checker->effects[i];
      size_t base = effects_offset + i * effect_info_type->size;
      IRRuleSite site;
      site.file = effect->site.filename;
      site.line = effect->site.line;
      site.column = effect->site.column;
      image_put_string(&image,
                       base + field_offset(effect_info_type, "name", &image),
                       effect->name);
      image_put_string(&image,
                       base + field_offset(effect_info_type, "module", &image),
                       effect->is_builtin ? "" : module_of(effect->site.filename));
      put_site(&image, site_type,
               base + field_offset(effect_info_type, "site", &image), &site);
      image_put_bool(&image,
                     base + field_offset(effect_info_type, "is_builtin", &image),
                     effect->is_builtin);
      if (!effect->is_builtin && !add_site(out, &site_capacity, site)) {
        image.failed = 1;
      }
    }
  }
  put_slice(&image,
            program_offset + field_offset(program_type, "effects", &image),
            effects_offset, checker->effect_count);

  for (size_t i = 0; i < reflect->type_count && !image.failed; i++) {
    RType *entry = &reflect->types[i];
    const Type *type = entry->type;
    size_t base = types_offset + i * type_info_type->size;
    image_put_string(&image,
                     base + field_offset(type_info_type, "name", &image),
                     entry->name);
    image_put_string(&image,
                     base + field_offset(type_info_type, "qualified", &image),
                     entry->qualified);
    image_put_string(&image,
                     base + field_offset(type_info_type, "module", &image),
                     entry->module);
    put_site(&image, site_type,
             base + field_offset(type_info_type, "site", &image),
             &entry->site);
    image_put_string(&image,
                     base + field_offset(type_info_type, "kind", &image),
                     entry->kind);
    image_put_string(&image,
                     base + field_offset(type_info_type, "base", &image),
                     entry->base);
    image_put_u64(&image, base + field_offset(type_info_type, "size", &image),
                  (unsigned long long)entry->size);
    image_put_u64(&image, base + field_offset(type_info_type, "align", &image),
                  (unsigned long long)entry->align);
    image_put_u64(&image,
                  base + field_offset(type_info_type, "layout", &image),
                  (unsigned long long)entry->layout);
    size_t field_count = type->kind == TYPE_STRUCT ? type->field_count : 0;
    size_t fields = 0;
    if (field_count > 0) {
      fields = image_reserve(&image, field_info_type->size * field_count,
                             field_info_type->alignment);
      for (size_t f = 0; f < field_count; f++) {
        size_t fbase = fields + f * field_info_type->size;
        image_put_string(&image,
                         fbase + field_offset(field_info_type, "name", &image),
                         type->field_names ? type->field_names[f] : "");
        image_put_string(
            &image, fbase + field_offset(field_info_type, "type_name", &image),
            type_display_name(type->field_types ? type->field_types[f]
                                                : NULL));
        image_put_u64(&image,
                      fbase + field_offset(field_info_type, "offset", &image),
                      type->field_offsets ? type->field_offsets[f] : 0);
        image_put_u64(&image,
                      fbase + field_offset(field_info_type, "size", &image),
                      type->field_types && type->field_types[f]
                          ? type->field_types[f]->size
                          : 0);
      }
    }
    put_slice(&image, base + field_offset(type_info_type, "fields", &image),
              fields, field_count);
    const char *const *variant_names = NULL;
    size_t variant_count = 0;
    if (type->kind == TYPE_ENUM) {
      variant_names = (const char *const *)type->enum_member_names;
      variant_count = type->enum_member_count;
    } else if (type->kind == TYPE_TAGGED_ENUM) {
      variant_names = (const char *const *)type->tagged_variant_names;
      variant_count = type->tagged_variant_count;
    }
    size_t variants = 0;
    if (variant_count > 0 && variant_names) {
      const char **shown = calloc(variant_count, sizeof(const char *));
      if (!shown) {
        image.failed = 1;
        break;
      }
      for (size_t v = 0; v < variant_count; v++) {
        shown[v] = strip_import_prefix(variant_names[v]);
      }
      variants = put_string_array(&image, shown, variant_count);
      free(shown);
    }
    put_slice(&image, base + field_offset(type_info_type, "variants", &image),
              variants, variant_count);
    if (!add_site(out, &site_capacity, entry->site)) {
      image.failed = 1;
    }
  }

  size_t modules =
      put_string_array(&image, reflect->modules, reflect->module_count);

  put_slice(&image, program_offset + field_offset(program_type, "functions",
                                                   &image),
            functions_offset, reflect->function_count);
  put_slice(&image,
            program_offset + field_offset(program_type, "types", &image),
            types_offset, reflect->type_count);
  put_slice(&image,
            program_offset + field_offset(program_type, "modules", &image),
            modules, reflect->module_count);
  image_put_string(&image,
                   program_offset + field_offset(program_type, "target", &image),
                   target ? target : "");
  image_put_string(&image,
                   program_offset + field_offset(program_type, "file", &image),
                   root_file ? root_file : "");

  size_t pool_base = image_reserve(&image, image.pool_size, 8);
  if (!image.failed && image.pool_size > 0) {
    memcpy(image.bytes + pool_base, image.pool, image.pool_size);
  }
  for (size_t i = 0; i < image.string_count && !image.failed; i++) {
    image_put_pointer(&image, image.string_fields[i],
                      pool_base + image.string_offsets[i]);
  }

  free(image.pool);
  free(image.string_fields);
  free(image.string_offsets);
  if (image.failed) {
    free(image.bytes);
    free(image.pointers);
    *error_message = strdup("could not lay out the program the rules read");
    return 0;
  }
  out->bytes = image.bytes;
  out->size = image.size;
  out->pointer_offsets = image.pointers;
  out->pointer_count = image.pointer_count;
  out->function_count = reflect->function_count;
  out->type_count = reflect->type_count;
  return 1;
}

static void dump_program(Reflect *reflect) {
  fprintf(stderr, "rule program: %zu functions, %zu types, %zu modules\n",
          reflect->function_count, reflect->type_count,
          reflect->module_count);
  for (size_t i = 0; i < reflect->function_count; i++) {
    RFunction *fn = &reflect->functions[i];
    fprintf(stderr, "  fn %s [%s:%zu]%s%s%s%s ->", fn->qualified,
            fn->site.file ? fn->site.file : "?", fn->site.line,
            fn->is_recursive ? " recursive" : "",
            fn->is_address_taken ? " address-taken" : "",
            fn->has_indirect_calls ? " indirect-calls" : "",
            fn->is_extern ? " extern" : "");
    for (size_t c = 0; c < fn->callee_count; c++) {
      fprintf(stderr, " %s", fn->callees[c]);
    }
    for (size_t m = 0; m < fn->match_count; m++) {
      fprintf(stderr, " matches:%s", fn->matches[m]);
    }
    fprintf(stderr, "\n");
  }
  for (size_t i = 0; i < reflect->type_count; i++) {
    RType *entry = &reflect->types[i];
    fprintf(stderr, "  %s %s%s%s size %lld align %lld layout %lld\n",
            entry->kind, entry->qualified, entry->base[0] ? " of " : "",
            entry->base, entry->size, entry->align, entry->layout);
  }
}

static void reflect_free(Reflect *reflect) {
  for (size_t i = 0; i < reflect->function_count; i++) {
    free(reflect->functions[i].qualified);
    free(reflect->functions[i].callees);
    free(reflect->functions[i].matches);
  }
  for (size_t i = 0; i < reflect->owned_count; i++) {
    free(reflect->owned[i]);
  }
  free(reflect->owned);
  for (size_t i = 0; i < reflect->type_count; i++) {
    free(reflect->types[i].qualified);
  }
  free(reflect->functions);
  free(reflect->types);
  free(reflect->modules);
}

static void attach_effects(Reflect *reflect, const IREffectResults *effects) {
  if (!effects) {
    return;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    RFunction *fn = &reflect->functions[i];
    const char **performs = NULL;
    size_t perform_count = 0;
    const char **needs = NULL;
    size_t need_count = 0;
    if (ir_effect_results_lookup(effects, fn->declared_name, &performs,
                                 &perform_count, &needs, &need_count)) {
      fn->effects = performs;
      fn->effect_count = perform_count;
      fn->requires = needs;
      fn->require_count = need_count;
    }
  }
}

int rule_reflect_build(TypeChecker *checker, ASTNode *program,
                       const char *root_file, const char *target,
                       const IREffectResults *effects, IRRuleImage *out,
                       char **error_message) {
  if (!checker || !program || program->type != AST_PROGRAM || !out ||
      !error_message) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  *error_message = NULL;
  Program *prog = (Program *)program->data;
  if (!prog) {
    return 0;
  }
  Reflect reflect;
  memset(&reflect, 0, sizeof(reflect));
  reflect.checker = checker;
  int ok = 1;
  for (size_t i = 0; i < prog->declaration_count && ok; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl) {
      continue;
    }
    if (decl->type == AST_FUNCTION_DECLARATION) {
      ok = collect_function(&reflect, decl);
    } else if (decl->type == AST_STRUCT_DECLARATION ||
               decl->type == AST_ENUM_DECLARATION ||
               decl->type == AST_TYPE_DECLARATION) {
      ok = collect_type(&reflect, decl);
    }
  }
  for (size_t i = 0; i < reflect.function_count && ok; i++) {
    RFunction *fn = &reflect.functions[i];
    ASTNode *decl = NULL;
    for (size_t d = 0; d < prog->declaration_count; d++) {
      ASTNode *candidate = prog->declarations[d];
      if (candidate && candidate->type == AST_FUNCTION_DECLARATION &&
          candidate->data &&
          ((FunctionDeclaration *)candidate->data)->name == fn->declared_name) {
        decl = candidate;
        break;
      }
    }
    if (!decl) {
      continue;
    }
    reflect.current = fn;
    ok = walk_body(&reflect, ((FunctionDeclaration *)decl->data)->body);
  }
  if (ok) {
    ok = mark_recursion(&reflect);
  }
  if (ok) {
    attach_effects(&reflect, effects);
  }
  if (ok && getenv("METTLE_DUMP_RULE_PROGRAM")) {
    dump_program(&reflect);
  }
  if (ok) {
    ok = build_image(&reflect, root_file, target, out, error_message);
  } else if (!*error_message) {
    *error_message = strdup("out of memory while reflecting the program");
  }
  reflect_free(&reflect);
  return ok;
}
