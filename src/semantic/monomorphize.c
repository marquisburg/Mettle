#include "monomorphize.h"
#include "common.h"
#include "string_intern.h"
#include "common.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name;
  ASTNode **methods;
  size_t method_count;
} TraitDef;

typedef struct {
  char *trait_name;
  char *for_type_name;
  ASTNode **methods;
  size_t method_count;
} TraitImpl;

typedef struct {
  char *generic_name;
  char **type_args;
  size_t type_arg_count;
  char *mangled_name;
  SourceLocation location;
  int emitted;
} Instantiation;

typedef struct {
  char *name;
  ASTNode *node;
  char **type_params;
  char **type_param_traits;
  size_t type_param_count;
  int is_struct; // 1 = struct, 0 = function
} GenericDef;

typedef struct {
  ErrorReporter *reporter;
  int had_error;
  TraitDef *traits;
  size_t trait_count;
  TraitImpl *impls;
  size_t impl_count;
  GenericDef *defs;
  size_t def_count;
  Instantiation *instances;
  size_t instance_count;
  /* The module, so inference can read a called function's return type. It is
     held as the program, whose declaration array moves as instantiations are
     appended. */
  Program *module;
} MonoContext;

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);
static void substitute_types_in_ast(ASTNode *node, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);
static GenericDef *find_generic_def(MonoContext *ctx, const char *name);

static char *mangle_name(const char *base, char **type_args,
                           size_t type_arg_count) {
  size_t len = strlen(base);
  for (size_t i = 0; i < type_arg_count; i++) {
    len += 2 + strlen(type_args[i]) * 4;
  }
  len += 1;

  char *result = malloc(len + 64);
  if (!result)
    return NULL;

  size_t base_len = strlen(base);
  memcpy(result, base, base_len);
  size_t pos = base_len;

  for (size_t i = 0; i < type_arg_count; i++) {
    result[pos++] = '_';
    result[pos++] = '_';
    for (const char *p = type_args[i]; *p; p++) {
      if (*p == '*') {
        memcpy(result + pos, "_ptr", 4);
        pos += 4;
      } else if (*p == '<' || *p == '>' || *p == ',') {
        result[pos++] = '_';
      } else {
        result[pos++] = *p;
      }
    }
  }
  result[pos] = '\0';

  return result;
}

static char *written_generic_name(const char *base, char **type_args,
                                  size_t type_arg_count) {
  size_t len = strlen(base) + 3;
  for (size_t i = 0; i < type_arg_count; i++) {
    len += strlen(type_args[i]) + 2;
  }

  char *result = malloc(len);
  if (!result)
    return NULL;

  size_t pos = (size_t)snprintf(result, len, "%s<", base);
  for (size_t i = 0; i < type_arg_count; i++) {
    pos += (size_t)snprintf(result + pos, len - pos, "%s%s", i ? ", " : "",
                            type_args[i]);
  }
  snprintf(result + pos, len - pos, ">");

  return result;
}

static void mono_report_error(MonoContext *ctx, SourceLocation location,
                              const char *message) {
  if (!ctx) {
    return;
  }

  ctx->had_error = 1;
  if (ctx->reporter) {
    error_reporter_add_error(ctx->reporter, ERROR_SEMANTIC, location, message);
  }
}

static void free_string_array(char **values, size_t count) {
  mettle_free_string_array(values, count);
}

static int mono_has_trait(MonoContext *ctx, const char *trait_name) {
  if (!ctx || !trait_name) {
    return 0;
  }

  for (size_t i = 0; i < ctx->trait_count; i++) {
    if (strcmp(ctx->traits[i].name, trait_name) == 0) {
      return 1;
    }
  }

  return 0;
}

static int mono_add_trait(MonoContext *ctx, const char *trait_name,
                          SourceLocation location) {
  if (!ctx || !trait_name) {
    return 0;
  }

  if (mono_has_trait(ctx, trait_name)) {
    char message[512];
    snprintf(message, sizeof(message), "Duplicate trait declaration '%s'",
             trait_name);
    mono_report_error(ctx, location, message);
    return 0;
  }

  TraitDef *grown =
      realloc(ctx->traits, (ctx->trait_count + 1) * sizeof(TraitDef));
  if (!grown) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for trait declaration");
    return 0;
  }

  ctx->traits = grown;
  ctx->traits[ctx->trait_count].name = strdup(trait_name);
  if (!ctx->traits[ctx->trait_count].name) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for trait declaration");
    return 0;
  }
  ctx->traits[ctx->trait_count].methods = NULL;
  ctx->traits[ctx->trait_count].method_count = 0;

  ctx->trait_count++;
  return 1;
}

static TraitDef *mono_find_trait(MonoContext *ctx, const char *trait_name) {
  if (!ctx || !trait_name) {
    return NULL;
  }

  for (size_t i = 0; i < ctx->trait_count; i++) {
    if (strcmp(ctx->traits[i].name, trait_name) == 0) {
      return &ctx->traits[i];
    }
  }

  return NULL;
}

static ASTNode *mono_find_function_method(ASTNode **methods,
                                          size_t method_count,
                                          const char *name) {
  if (!methods || !name) {
    return NULL;
  }

  for (size_t i = 0; i < method_count; i++) {
    ASTNode *method = methods[i];
    if (!method || method->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    FunctionDeclaration *decl = (FunctionDeclaration *)method->data;
    if (decl && decl->name && strcmp(decl->name, name) == 0) {
      return method;
    }
  }

  return NULL;
}

static int mono_type_implements_trait(MonoContext *ctx, const char *trait_name,
                                      const char *type_name) {
  if (!ctx || !trait_name || !type_name) {
    return 0;
  }

  for (size_t i = 0; i < ctx->impl_count; i++) {
    if (strcmp(ctx->impls[i].trait_name, trait_name) == 0 &&
        strcmp(ctx->impls[i].for_type_name, type_name) == 0) {
      return 1;
    }
  }

  return 0;
}

static char **mono_split_bounds(const char *bounds, size_t *out_count) {
  char **items = NULL;
  size_t count = 0;
  const char *start = bounds;

  if (out_count) {
    *out_count = 0;
  }
  if (!bounds || !out_count) {
    return NULL;
  }

  while (*start) {
    const char *end = strchr(start, '+');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    while (len > 0 && (*start == ' ' || *start == '\t')) {
      start++;
      len--;
    }
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
      len--;
    }

    if (len > 0) {
      char **grown = realloc(items, (count + 1) * sizeof(char *));
      if (!grown) {
        free_string_array(items, count);
        return NULL;
      }
      items = grown;
      items[count] = malloc(len + 1);
      if (!items[count]) {
        free_string_array(items, count);
        return NULL;
      }
      memcpy(items[count], start, len);
      items[count][len] = '\0';
      count++;
    }

    if (!end) {
      break;
    }
    start = end + 1;
  }

  *out_count = count;
  return items;
}

static int mono_validate_bound_traits(MonoContext *ctx, const char *bounds,
                                      const char *generic_name,
                                      SourceLocation location) {
  char **traits = NULL;
  size_t trait_count = 0;
  int ok = 1;

  if (!bounds) {
    return 1;
  }

  traits = mono_split_bounds(bounds, &trait_count);
  if (!traits && trait_count == 0) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic bounds");
    return 0;
  }

  for (size_t i = 0; i < trait_count; i++) {
    if (!mono_has_trait(ctx, traits[i])) {
      char message[512];
      snprintf(message, sizeof(message),
               "Unknown trait '%s' in generic bound on '%s'", traits[i],
               generic_name);
      mono_report_error(ctx, location, message);
      ok = 0;
      break;
    }
  }

  free_string_array(traits, trait_count);
  return ok;
}

static int mono_type_satisfies_bounds(MonoContext *ctx, GenericDef *def,
                                      Instantiation *inst,
                                      const char *bounds, size_t arg_index) {
  char **traits = NULL;
  size_t trait_count = 0;
  int ok = 1;

  if (!bounds) {
    return 1;
  }

  traits = mono_split_bounds(bounds, &trait_count);
  if (!traits && trait_count == 0) {
    mono_report_error(ctx, inst->location,
                      "Failed to allocate storage for generic bounds");
    return 0;
  }

  for (size_t i = 0; i < trait_count; i++) {
    if (!mono_has_trait(ctx, traits[i])) {
      char message[512];
      snprintf(message, sizeof(message),
               "Generic '%s' requires unknown trait '%s'", def->name,
               traits[i]);
      mono_report_error(ctx, inst->location, message);
      ok = 0;
      break;
    }

    if (!mono_type_implements_trait(ctx, traits[i], inst->type_args[arg_index])) {
      char message[512];
      snprintf(message, sizeof(message),
               "Type '%s' does not implement trait '%s' required by '%s'",
               inst->type_args[arg_index], traits[i], def->name);
      mono_report_error(ctx, inst->location, message);
      ok = 0;
      break;
    }
  }

  free_string_array(traits, trait_count);
  return ok;
}

static int mono_add_impl(MonoContext *ctx, const char *trait_name,
                         const char *for_type_name, SourceLocation location) {
  if (!ctx || !trait_name || !for_type_name) {
    return 0;
  }

  if (!mono_has_trait(ctx, trait_name)) {
    char message[512];
    snprintf(message, sizeof(message),
             "Unknown trait '%s' in impl declaration", trait_name);
    mono_report_error(ctx, location, message);
    return 0;
  }

  if (mono_type_implements_trait(ctx, trait_name, for_type_name)) {
    char message[512];
    snprintf(message, sizeof(message), "Duplicate impl '%s for %s'", trait_name,
             for_type_name);
    mono_report_error(ctx, location, message);
    return 0;
  }

  TraitImpl *grown =
      realloc(ctx->impls, (ctx->impl_count + 1) * sizeof(TraitImpl));
  if (!grown) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for impl declaration");
    return 0;
  }

  ctx->impls = grown;
  ctx->impls[ctx->impl_count].trait_name = strdup(trait_name);
  ctx->impls[ctx->impl_count].for_type_name = strdup(for_type_name);
  ctx->impls[ctx->impl_count].methods = NULL;
  ctx->impls[ctx->impl_count].method_count = 0;
  if (!ctx->impls[ctx->impl_count].trait_name ||
      !ctx->impls[ctx->impl_count].for_type_name) {
    free(ctx->impls[ctx->impl_count].trait_name);
    free(ctx->impls[ctx->impl_count].for_type_name);
    mono_report_error(ctx, location,
                      "Failed to allocate storage for impl declaration");
    return 0;
  }

  ctx->impl_count++;
  return 1;
}

static char *mono_sanitize_component(const char *value) {
  size_t len = value ? strlen(value) : 0;
  char *result = malloc(len + 1);
  if (!result) {
    return NULL;
  }

  for (size_t i = 0; i < len; i++) {
    char c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') {
      result[i] = c;
    } else {
      result[i] = '_';
    }
  }
  result[len] = '\0';
  return result;
}

static char *mono_mangle_impl_method_name(const char *trait_name,
                                          const char *for_type_name,
                                          const char *method_name) {
  char *trait_part = mono_sanitize_component(trait_name);
  char *type_part = mono_sanitize_component(for_type_name);
  char *method_part = mono_sanitize_component(method_name);
  char *result = NULL;
  size_t len = 0;

  if (!trait_part || !type_part || !method_part) {
    free(trait_part);
    free(type_part);
    free(method_part);
    return NULL;
  }

  len = strlen("__trait_") + strlen(trait_part) + 2 + strlen(type_part) + 2 +
        strlen(method_part) + 1;
  result = malloc(len);
  if (result) {
    snprintf(result, len, "__trait_%s__%s__%s", trait_part, type_part,
             method_part);
  }

  free(trait_part);
  free(type_part);
  free(method_part);
  return result;
}

static char *mono_substitute_self_type(const char *type_name,
                                       const char *for_type_name,
                                       MonoContext *ctx) {
  char *param = "Self";
  char *arg = (char *)for_type_name;
  return substitute_type_string(type_name, &param, &arg, 1, ctx);
}

static int mono_type_names_equal_with_self(const char *trait_type,
                                           const char *impl_type,
                                           const char *for_type_name,
                                           MonoContext *ctx) {
  char *expected = mono_substitute_self_type(trait_type, for_type_name, ctx);
  char *actual = mono_substitute_self_type(impl_type, for_type_name, ctx);
  int equal = 0;

  if (!expected || !actual) {
    free(expected);
    free(actual);
    return 0;
  }

  equal = strcmp(expected, actual) == 0;
  free(expected);
  free(actual);
  return equal;
}

static int mono_validate_impl_methods(MonoContext *ctx, TraitImpl *impl,
                                      SourceLocation location) {
  TraitDef *trait = NULL;

  if (!ctx || !impl) {
    return 0;
  }

  trait = mono_find_trait(ctx, impl->trait_name);
  if (!trait) {
    return 0;
  }

  for (size_t i = 0; i < trait->method_count; i++) {
    ASTNode *trait_method = trait->methods[i];
    FunctionDeclaration *trait_fn =
        trait_method ? (FunctionDeclaration *)trait_method->data : NULL;
    ASTNode *impl_method = NULL;
    FunctionDeclaration *impl_fn = NULL;

    if (!trait_fn || !trait_fn->name) {
      continue;
    }

    impl_method =
        mono_find_function_method(impl->methods, impl->method_count,
                                  trait_fn->name);
    if (!impl_method) {
      char message[512];
      snprintf(message, sizeof(message),
               "Impl '%s for %s' is missing trait method '%s'",
               impl->trait_name, impl->for_type_name, trait_fn->name);
      mono_report_error(ctx, location, message);
      return 0;
    }

    impl_fn = (FunctionDeclaration *)impl_method->data;
    if (!impl_fn || impl_fn->parameter_count != trait_fn->parameter_count) {
      char message[512];
      snprintf(message, sizeof(message),
               "Impl method '%s' has a signature that does not match trait '%s'",
               trait_fn->name, impl->trait_name);
      mono_report_error(ctx, location, message);
      return 0;
    }

    for (size_t j = 0; j < trait_fn->parameter_count; j++) {
      if (!mono_type_names_equal_with_self(trait_fn->parameter_types[j],
                                           impl_fn->parameter_types[j],
                                           impl->for_type_name, ctx)) {
        char message[512];
        snprintf(message, sizeof(message),
                 "Impl method '%s' parameter %llu does not match trait '%s'",
                 trait_fn->name, (unsigned long long)(j + 1),
                 impl->trait_name);
        mono_report_error(ctx, location, message);
        return 0;
      }
    }

    if ((trait_fn->return_type || impl_fn->return_type) &&
        !mono_type_names_equal_with_self(trait_fn->return_type
                                             ? trait_fn->return_type
                                             : "void",
                                         impl_fn->return_type
                                             ? impl_fn->return_type
                                             : "void",
                                         impl->for_type_name, ctx)) {
      char message[512];
      snprintf(message, sizeof(message),
               "Impl method '%s' return type does not match trait '%s'",
               trait_fn->name, impl->trait_name);
      mono_report_error(ctx, location, message);
      return 0;
    }
  }

  return 1;
}

static ASTNode *mono_create_impl_method_function(MonoContext *ctx,
                                                TraitImpl *impl,
                                                ASTNode *method) {
  ASTNode *clone = NULL;
  FunctionDeclaration *fn = NULL;
  char *mangled = NULL;

  if (!ctx || !impl || !method || method->type != AST_FUNCTION_DECLARATION) {
    return NULL;
  }

  clone = ast_clone_node(method);
  if (!clone) {
    return NULL;
  }

  fn = (FunctionDeclaration *)clone->data;
  if (!fn || !fn->name) {
    ast_destroy_node(clone);
    return NULL;
  }

  mangled = mono_mangle_impl_method_name(impl->trait_name, impl->for_type_name,
                                         fn->name);
  if (!mangled) {
    ast_destroy_node(clone);
    return NULL;
  }

  mettle_free_string(fn->name);
  fn->name = mangled;
  fn->is_exported = 0;

  for (size_t i = 0; i < fn->parameter_count; i++) {
    char *new_type =
        mono_substitute_self_type(fn->parameter_types[i], impl->for_type_name,
                                  ctx);
    if (new_type) {
      mettle_free_string(fn->parameter_types[i]);
      fn->parameter_types[i] = new_type;
    }
  }

  if (fn->return_type) {
    char *new_type =
        mono_substitute_self_type(fn->return_type, impl->for_type_name, ctx);
    if (new_type) {
      mettle_free_string(fn->return_type);
      fn->return_type = new_type;
    }
  }

  if (fn->body) {
    char *param = "Self";
    char *arg = impl->for_type_name;
    substitute_types_in_ast(fn->body, &param, &arg, 1, ctx);
  }

  return clone;
}

static int mono_add_instantiation(MonoContext *ctx, const char *generic_name,
                                  char **type_args, size_t type_arg_count,
                                  SourceLocation location) {
  char *mangled = NULL;
  int found = 0;
  Instantiation *inst = NULL;

  if (!ctx || !generic_name || !type_args) {
    return 0;
  }

  mangled = mangle_name(generic_name, type_args, type_arg_count);
  if (!mangled) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic instantiation");
    return 0;
  }

  for (size_t i = 0; i < ctx->instance_count; i++) {
    if (strcmp(ctx->instances[i].mangled_name, mangled) == 0) {
      found = 1;
      break;
    }
  }

  if (found) {
    free(mangled);
    return 1;
  }

  Instantiation *grown = realloc(ctx->instances,
                                 (ctx->instance_count + 1) *
                                     sizeof(Instantiation));
  if (!grown) {
    free(mangled);
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic instantiation");
    return 0;
  }

  ctx->instances = grown;
  inst = &ctx->instances[ctx->instance_count];
  inst->generic_name = strdup(generic_name);
  inst->type_arg_count = type_arg_count;
  inst->type_args = malloc(type_arg_count * sizeof(char *));
  inst->mangled_name = mangled;
  inst->location = location;
  inst->emitted = 0;

  if (!inst->generic_name || !inst->type_args) {
    free(inst->generic_name);
    free(inst->type_args);
    free(inst->mangled_name);
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic instantiation");
    return 0;
  }

  for (size_t i = 0; i < type_arg_count; i++) {
    inst->type_args[i] = strdup(type_args[i]);
    if (!inst->type_args[i]) {
      free_string_array(inst->type_args, i);
      free(inst->generic_name);
      free(inst->mangled_name);
      mono_report_error(ctx, location,
                        "Failed to allocate storage for generic instantiation");
      return 0;
    }
  }

  ctx->instance_count++;
  return 1;
}

static int parse_generic_type_name(const char *type_str, char **out_base,
                                   char ***out_args, size_t *out_arg_count) {
  const char *lt = strchr(type_str, '<');
  if (!lt)
    return 0;

  size_t base_len = (size_t)(lt - type_str);
  *out_base = malloc(base_len + 1);
  memcpy(*out_base, type_str, base_len);
  (*out_base)[base_len] = '\0';

  const char *start = lt + 1;
  const char *end = type_str + strlen(type_str);

  // Find matching '>'
  const char *gt = NULL;
  int depth = 1;
  for (const char *p = start; *p; p++) {
    if (*p == '<')
      depth++;
    else if (*p == '>') {
      depth--;
      if (depth == 0) {
        gt = p;
        break;
      }
    }
  }

  if (!gt) {
    free(*out_base);
    *out_base = NULL;
    return 0;
  }

  // Parse comma-separated args between start and gt
  char **args = NULL;
  size_t count = 0;
  const char *p = start;

  while (p < gt) {
    while (p < gt && *p == ' ')
      p++;

    const char *arg_start = p;
    int inner_depth = 0;
    while (p < gt) {
      if (*p == '<')
        inner_depth++;
      else if (*p == '>')
        inner_depth--;
      else if (*p == ',' && inner_depth == 0)
        break;
      p++;
    }

    const char *arg_end = p;
    while (arg_end > arg_start && *(arg_end - 1) == ' ')
      arg_end--;

    size_t arg_len = (size_t)(arg_end - arg_start);
    if (arg_len > 0) {
      args = realloc(args, (count + 1) * sizeof(char *));
      args[count] = malloc(arg_len + 1);
      memcpy(args[count], arg_start, arg_len);
      args[count][arg_len] = '\0';
      count++;
    }

    if (p < gt && *p == ',')
      p++;
  }

  // Check for pointer suffix after '>' (e.g., "List<int32>*")
  const char *suffix = gt + 1;
  if (suffix < end && *suffix != '\0') {
    // There's a suffix like "*", we need to incorporate it
    // The base stays the same, but we need the caller to handle the suffix
    // For simplicity, append the suffix to the last arg? No, the suffix
    // applies to the whole generic type. We'll handle this by including
    // the suffix in the base name replacement later.
  }

  *out_args = args;
  *out_arg_count = count;
  return 1;
}

static void record_generic_type_use(MonoContext *ctx, const char *type_name,
                                    SourceLocation location) {
  char *base = NULL;
  char **args = NULL;
  size_t arg_count = 0;

  if (!ctx || !type_name) {
    return;
  }

  if (!parse_generic_type_name(type_name, &base, &args, &arg_count)) {
    return;
  }

  /* A type argument may itself be a generic instantiation, as in
   * `Pair<Box<int32>, int32>`. Substitution already mangles the field type to
   * `Box__int32`, but nothing ever asked for `Box<int32>` to be generated, so
   * the field referred to a struct that did not exist. Record the arguments
   * BEFORE the type that uses them: emission follows list order, and Mettle
   * requires a type to be declared before it is used, so `Box__int32` has to
   * land in the program ahead of the struct whose field names it. */
  for (size_t i = 0; i < arg_count; i++) {
    record_generic_type_use(ctx, args[i], location);
  }

  mono_add_instantiation(ctx, base, args, arg_count, location);
  free(base);
  free_string_array(args, arg_count);
}

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx) {
  if (!type_str)
    return NULL;

  size_t ts_len = strlen(type_str);

  // Strip trailing pointer stars and array suffixes to get the core type
  size_t core_len = ts_len;
  size_t ptr_count = 0;
  char *array_suffix = NULL;

  // Check for array suffix first: "type[N]"
  const char *lbr = NULL;
  int depth = 0;
  for (size_t i = 0; i < ts_len; i++) {
    if (type_str[i] == '<')
      depth++;
    else if (type_str[i] == '>')
      depth--;
    else if (type_str[i] == '[' && depth == 0) {
      lbr = type_str + i;
      break;
    }
  }

  if (lbr) {
    array_suffix = strdup(lbr);
    core_len = (size_t)(lbr - type_str);
  }

  // Strip trailing '*' from core
  while (core_len > 0 && type_str[core_len - 1] == '*') {
    ptr_count++;
    core_len--;
  }

  char *core = malloc(core_len + 1);
  memcpy(core, type_str, core_len);
  core[core_len] = '\0';

  // Check if core is a type parameter
  for (size_t i = 0; i < count; i++) {
    if (strcmp(core, param_names[i]) == 0) {
      free(core);
      size_t result_len = strlen(arg_names[i]) + ptr_count +
                          (array_suffix ? strlen(array_suffix) : 0) + 1;
      char *result = malloc(result_len);
      strcpy(result, arg_names[i]);
      for (size_t j = 0; j < ptr_count; j++)
        strcat(result, "*");
      if (array_suffix) {
        strcat(result, array_suffix);
        free(array_suffix);
      }
      return result;
    }
  }

  // Check if core is a generic type instantiation like "List<T>"
  char *gen_base = NULL;
  char **gen_args = NULL;
  size_t gen_arg_count = 0;
  if (parse_generic_type_name(core, &gen_base, &gen_args, &gen_arg_count)) {
    // Substitute type params within the generic args
    char **subst_args = malloc(gen_arg_count * sizeof(char *));
    for (size_t i = 0; i < gen_arg_count; i++) {
      subst_args[i] =
          substitute_type_string(gen_args[i], param_names, arg_names, count, ctx);
    }

    /* Only a generic this pass owns gets a mangled name. A generic enum is
     * instantiated by the type checker instead, off the name the program
     * wrote, so substituting inside its arguments is the whole job: mangling
     * it here hands the type checker a name it has never heard of, and the
     * function that returned it goes undefined along with the type.
     * rewrite_generic_type_name_in_place makes the same check. */
    int owned = !ctx || find_generic_def(ctx, gen_base) != NULL;
    char *mangled = owned ? mangle_name(gen_base, subst_args, gen_arg_count)
                          : written_generic_name(gen_base, subst_args,
                                                 gen_arg_count);

    // Record this as an instantiation if not already recorded
    if (ctx && owned) {
      int found = 0;
      for (size_t i = 0; i < ctx->instance_count; i++) {
        if (strcmp(ctx->instances[i].mangled_name, mangled) == 0) {
          found = 1;
          break;
        }
      }
      if (!found) {
        SourceLocation internal_location = {0, 0, NULL};
        mono_add_instantiation(ctx, gen_base, subst_args, gen_arg_count,
                               internal_location);
      }
    }

    size_t result_len =
        strlen(mangled) + ptr_count +
        (array_suffix ? strlen(array_suffix) : 0) + 1;
    char *result = malloc(result_len);
    strcpy(result, mangled);
    for (size_t j = 0; j < ptr_count; j++)
      strcat(result, "*");
    if (array_suffix) {
      strcat(result, array_suffix);
      free(array_suffix);
    }

    free(mangled);
    free(gen_base);
    for (size_t i = 0; i < gen_arg_count; i++) {
      free(gen_args[i]);
      free(subst_args[i]);
    }
    free(gen_args);
    free(subst_args);
    free(core);
    return result;
  }

  free(core);
  free(array_suffix);

  return strdup(type_str);
}

static void substitute_types_in_ast(ASTNode *node, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->type_name) {
      char *new_type =
          substitute_type_string(vd->type_name, param_names, arg_names, count, ctx);
      if (new_type) {
        mettle_free_string(vd->type_name);
        vd->type_name = new_type;
      }
    }
    if (vd && vd->initializer) {
      substitute_types_in_ast(vd->initializer, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *ne = (NewExpression *)node->data;
    if (ne && ne->type_name) {
      char *new_type =
          substitute_type_string(ne->type_name, param_names, arg_names, count, ctx);
      if (new_type) {
        mettle_free_string(ne->type_name);
        ne->type_name = new_type;
      }
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *ce = (CallExpression *)node->data;
    if (ce) {
      // Substitute type args in generic function calls
      if (ce->type_arg_count > 0 && ce->type_args) {
        for (size_t i = 0; i < ce->type_arg_count; i++) {
          char *new_arg = substitute_type_string(ce->type_args[i], param_names,
                                                 arg_names, count, ctx);
          if (new_arg) {
            mettle_free_string(ce->type_args[i]);
            ce->type_args[i] = new_arg;
          }
        }
      }
      for (size_t i = 0; i < ce->argument_count; i++) {
        substitute_types_in_ast(ce->arguments[i], param_names, arg_names, count,
                                ctx);
      }
      if (ce->object) {
        substitute_types_in_ast(ce->object, param_names, arg_names, count, ctx);
      }
    }
    break;
  }
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (prog) {
      for (size_t i = 0; i < prog->declaration_count; i++) {
        substitute_types_in_ast(prog->declarations[i], param_names, arg_names,
                                count, ctx);
      }
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value) {
      substitute_types_in_ast(rs->value, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *as = (Assignment *)node->data;
    if (as) {
      if (as->value)
        substitute_types_in_ast(as->value, param_names, arg_names, count, ctx);
      if (as->target)
        substitute_types_in_ast(as->target, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_IF_STATEMENT: {
    IfStatement *is = (IfStatement *)node->data;
    if (is) {
      substitute_types_in_ast(is->condition, param_names, arg_names, count, ctx);
      substitute_types_in_ast(is->then_branch, param_names, arg_names, count, ctx);
      for (size_t i = 0; i < is->else_if_count; i++) {
        substitute_types_in_ast(is->else_ifs[i].condition, param_names,
                                arg_names, count, ctx);
        substitute_types_in_ast(is->else_ifs[i].body, param_names, arg_names,
                                count, ctx);
      }
      if (is->else_branch)
        substitute_types_in_ast(is->else_branch, param_names, arg_names, count,
                                ctx);
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *ws = (WhileStatement *)node->data;
    if (ws) {
      substitute_types_in_ast(ws->condition, param_names, arg_names, count, ctx);
      substitute_types_in_ast(ws->body, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *fs = (ForStatement *)node->data;
    if (fs) {
      if (fs->initializer)
        substitute_types_in_ast(fs->initializer, param_names, arg_names, count,
                                ctx);
      if (fs->condition)
        substitute_types_in_ast(fs->condition, param_names, arg_names, count,
                                ctx);
      if (fs->increment)
        substitute_types_in_ast(fs->increment, param_names, arg_names, count,
                                ctx);
      if (fs->body)
        substitute_types_in_ast(fs->body, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *ss = (SwitchStatement *)node->data;
    if (ss) {
      substitute_types_in_ast(ss->expression, param_names, arg_names, count,
                              ctx);
      for (size_t i = 0; i < ss->case_count; i++) {
        substitute_types_in_ast(ss->cases[i], param_names, arg_names, count,
                                ctx);
      }
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value)
        substitute_types_in_ast(cc->value, param_names, arg_names, count, ctx);
      if (cc->body)
        substitute_types_in_ast(cc->body, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *ce = (CastExpression *)node->data;
    if (ce) {
      if (ce->type_name) {
        char *new_type = substitute_type_string(ce->type_name, param_names,
                                                arg_names, count, ctx);
        if (new_type) {
          mettle_free_string(ce->type_name);
          ce->type_name = new_type;
        }
      }
      substitute_types_in_ast(ce->operand, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *be = (BinaryExpression *)node->data;
    if (be) {
      substitute_types_in_ast(be->left, param_names, arg_names, count, ctx);
      substitute_types_in_ast(be->right, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *ue = (UnaryExpression *)node->data;
    if (ue && ue->operand) {
      substitute_types_in_ast(ue->operand, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *ma = (MemberAccess *)node->data;
    if (ma && ma->object) {
      substitute_types_in_ast(ma->object, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ie = (ArrayIndexExpression *)node->data;
    if (ie) {
      if (ie->array)
        substitute_types_in_ast(ie->array, param_names, arg_names, count, ctx);
      if (ie->index)
        substitute_types_in_ast(ie->index, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *ds = (DeferStatement *)node->data;
    if (ds && ds->statement)
      substitute_types_in_ast(ds->statement, param_names, arg_names, count, ctx);
    break;
  }
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      substitute_types_in_ast(node->children[i], param_names, arg_names, count,
                              ctx);
    }
    break;
  default:
    break;
  }
}

static void collect_generic_defs(ASTNode *program, MonoContext *ctx) {
  Program *prog = (Program *)program->data;
  if (!prog)
    return;

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_TRAIT_DECLARATION) {
      TraitDeclaration *td = (TraitDeclaration *)decl->data;
      if (td && td->name) {
        if (mono_add_trait(ctx, td->name, decl->location)) {
          TraitDef *trait = mono_find_trait(ctx, td->name);
          if (trait) {
            trait->methods = td->methods;
            trait->method_count = td->method_count;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl)
      continue;

    if (decl->type == AST_IMPL_DECLARATION) {
      ImplDeclaration *impl = (ImplDeclaration *)decl->data;
      if (impl && impl->trait_name && impl->for_type_name) {
        size_t old_count = ctx->impl_count;
        if (mono_add_impl(ctx, impl->trait_name, impl->for_type_name,
                          decl->location) &&
            ctx->impl_count > old_count) {
          ctx->impls[ctx->impl_count - 1].methods = impl->methods;
          ctx->impls[ctx->impl_count - 1].method_count = impl->method_count;
          mono_validate_impl_methods(ctx, &ctx->impls[ctx->impl_count - 1],
                                     decl->location);
        }
      }
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      if (sd && sd->type_param_count > 0) {
        GenericDef *new_defs =
            realloc(ctx->defs, (ctx->def_count + 1) * sizeof(GenericDef));
        if (!new_defs) {
          mono_report_error(ctx, decl->location,
                            "Out of memory collecting generic definitions");
          return;
        }
        ctx->defs = new_defs;
        GenericDef *def = &ctx->defs[ctx->def_count];
        def->name = strdup(sd->name);
        def->node = decl;
        def->type_param_count = sd->type_param_count;
        def->type_params = malloc(sd->type_param_count * sizeof(char *));
        def->type_param_traits = malloc(sd->type_param_count * sizeof(char *));
        for (size_t j = 0; j < sd->type_param_count; j++) {
          def->type_params[j] = strdup(sd->type_params[j]);
          def->type_param_traits[j] =
              sd->type_param_traits && sd->type_param_traits[j]
                  ? strdup(sd->type_param_traits[j])
                  : NULL;
          if (!mono_validate_bound_traits(ctx, def->type_param_traits[j],
                                          sd->name, decl->location)) {
            continue;
          }
        }
        def->is_struct = 1;
        ctx->def_count++;
      }
    } else if (decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      if (fd && fd->type_param_count > 0) {
        GenericDef *new_defs =
            realloc(ctx->defs, (ctx->def_count + 1) * sizeof(GenericDef));
        if (!new_defs) {
          mono_report_error(ctx, decl->location,
                            "Out of memory collecting generic definitions");
          return;
        }
        ctx->defs = new_defs;
        GenericDef *def = &ctx->defs[ctx->def_count];
        def->name = strdup(fd->name);
        def->node = decl;
        def->type_param_count = fd->type_param_count;
        def->type_params = malloc(fd->type_param_count * sizeof(char *));
        def->type_param_traits = malloc(fd->type_param_count * sizeof(char *));
        for (size_t j = 0; j < fd->type_param_count; j++) {
          def->type_params[j] = strdup(fd->type_params[j]);
          def->type_param_traits[j] =
              fd->type_param_traits && fd->type_param_traits[j]
                  ? strdup(fd->type_param_traits[j])
                  : NULL;
          if (!mono_validate_bound_traits(ctx, def->type_param_traits[j],
                                          fd->name, decl->location)) {
            continue;
          }
        }
        def->is_struct = 0;
        ctx->def_count++;
      }
    }
  }
}

static void collect_type_instantiations(ASTNode *node, MonoContext *ctx) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->type_name) {
      record_generic_type_use(ctx, vd->type_name, node->location);
    }
    if (vd && vd->initializer)
      collect_type_instantiations(vd->initializer, ctx);
    break;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *ne = (NewExpression *)node->data;
    if (ne && ne->type_name) {
      record_generic_type_use(ctx, ne->type_name, node->location);
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *ce = (CallExpression *)node->data;
    if (ce && ce->type_arg_count > 0 && ce->function_name) {
      mono_add_instantiation(ctx, ce->function_name, ce->type_args,
                             ce->type_arg_count, node->location);
    }
    if (ce) {
      for (size_t i = 0; i < ce->argument_count; i++)
        collect_type_instantiations(ce->arguments[i], ctx);
      if (ce->object)
        collect_type_instantiations(ce->object, ctx);
    }
    break;
  }
  case AST_FUNCTION_DECLARATION: {
    FunctionDeclaration *fd = (FunctionDeclaration *)node->data;
    if (fd && fd->type_param_count == 0 && fd->body)
      collect_type_instantiations(fd->body, ctx);
    // Also check parameter types for generic type uses
    if (fd && fd->type_param_count == 0) {
      for (size_t i = 0; i < fd->parameter_count; i++) {
        if (fd->parameter_types[i]) {
          record_generic_type_use(ctx, fd->parameter_types[i], node->location);
        }
      }
      if (fd->return_type) {
        record_generic_type_use(ctx, fd->return_type, node->location);
      }
    }
    break;
  }
  case AST_STRUCT_DECLARATION: {
    /* A field type and a method body are both places a generic type can be
     * named. The template's own are skipped: they still name type parameters,
     * and their instantiations are collected from the monomorphized copy. */
    StructDeclaration *sd = (StructDeclaration *)node->data;
    if (sd && sd->type_param_count == 0) {
      for (size_t i = 0; i < sd->field_count; i++) {
        record_generic_type_use(ctx, sd->field_types[i], node->location);
      }
      for (size_t i = 0; i < sd->method_count; i++) {
        ASTNode *method = sd->methods[i];
        FunctionDeclaration *md =
            method ? (FunctionDeclaration *)method->data : NULL;
        if (!md) {
          continue;
        }
        for (size_t p = 0; p < md->parameter_count; p++) {
          record_generic_type_use(ctx, md->parameter_types[p],
                                  method->location);
        }
        record_generic_type_use(ctx, md->return_type, method->location);
        if (md->body) {
          collect_type_instantiations(md->body, ctx);
        }
      }
    }
    break;
  }
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (prog) {
      for (size_t i = 0; i < prog->declaration_count; i++)
        collect_type_instantiations(prog->declarations[i], ctx);
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value)
      collect_type_instantiations(rs->value, ctx);
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *as = (Assignment *)node->data;
    if (as) {
      if (as->value)
        collect_type_instantiations(as->value, ctx);
      if (as->target)
        collect_type_instantiations(as->target, ctx);
    }
    break;
  }
  case AST_IF_STATEMENT: {
    IfStatement *is_stmt = (IfStatement *)node->data;
    if (is_stmt) {
      collect_type_instantiations(is_stmt->condition, ctx);
      collect_type_instantiations(is_stmt->then_branch, ctx);
      for (size_t i = 0; i < is_stmt->else_if_count; i++) {
        collect_type_instantiations(is_stmt->else_ifs[i].condition, ctx);
        collect_type_instantiations(is_stmt->else_ifs[i].body, ctx);
      }
      if (is_stmt->else_branch)
        collect_type_instantiations(is_stmt->else_branch, ctx);
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *ws = (WhileStatement *)node->data;
    if (ws) {
      collect_type_instantiations(ws->condition, ctx);
      collect_type_instantiations(ws->body, ctx);
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *fs = (ForStatement *)node->data;
    if (fs) {
      if (fs->initializer)
        collect_type_instantiations(fs->initializer, ctx);
      if (fs->condition)
        collect_type_instantiations(fs->condition, ctx);
      if (fs->increment)
        collect_type_instantiations(fs->increment, ctx);
      if (fs->body)
        collect_type_instantiations(fs->body, ctx);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *ss = (SwitchStatement *)node->data;
    if (ss) {
      collect_type_instantiations(ss->expression, ctx);
      for (size_t i = 0; i < ss->case_count; i++)
        collect_type_instantiations(ss->cases[i], ctx);
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value)
        collect_type_instantiations(cc->value, ctx);
      if (cc->body)
        collect_type_instantiations(cc->body, ctx);
    }
    break;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *ce = (CastExpression *)node->data;
    if (ce && ce->operand)
      collect_type_instantiations(ce->operand, ctx);
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *be = (BinaryExpression *)node->data;
    if (be) {
      collect_type_instantiations(be->left, ctx);
      collect_type_instantiations(be->right, ctx);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *ue = (UnaryExpression *)node->data;
    if (ue && ue->operand)
      collect_type_instantiations(ue->operand, ctx);
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *ma = (MemberAccess *)node->data;
    if (ma && ma->object)
      collect_type_instantiations(ma->object, ctx);
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ie = (ArrayIndexExpression *)node->data;
    if (ie) {
      if (ie->array)
        collect_type_instantiations(ie->array, ctx);
      if (ie->index)
        collect_type_instantiations(ie->index, ctx);
    }
    break;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *ds = (DeferStatement *)node->data;
    if (ds && ds->statement)
      collect_type_instantiations(ds->statement, ctx);
    break;
  }
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      collect_type_instantiations(node->children[i], ctx);
    }
    break;
  default:
    break;
  }
}

static GenericDef *find_generic_def(MonoContext *ctx, const char *name);

static void rewrite_generic_type_name_in_place(char **slot,
                                               const char **type_params,
                                               const char **concrete_types,
                                               size_t type_param_count,
                                               MonoContext *ctx) {
  char *type_str = *slot;
  size_t len = strlen(type_str);
  size_t ptr_count = 0;
  char *base = NULL;
  char **args = NULL;
  size_t arg_count = 0;
  const char *array_suffix = NULL;
  int depth = 0;

  (void)type_params;
  (void)concrete_types;
  (void)type_param_count;

  /* An array suffix belongs to the field, not to the type argument: the `[3]`
   * of `Inner<int32>[3]` has to survive the rewrite, and the `[4]` of
   * `Inner<int32[4]>` is part of the argument and must not be mistaken for it. */
  for (size_t i = 0; i < len; i++) {
    if (type_str[i] == '<') {
      depth++;
    } else if (type_str[i] == '>') {
      depth--;
    } else if (type_str[i] == '[' && depth == 0) {
      array_suffix = type_str + i;
      len = i;
      break;
    }
  }

  while (len > 0 && type_str[len - 1] == '*') {
    ptr_count++;
    len--;
  }

  char *core = malloc(len + 1);
  memcpy(core, type_str, len);
  core[len] = '\0';

  if (parse_generic_type_name(core, &base, &args, &arg_count)) {
    if (ctx && !find_generic_def(ctx, base)) {
      free(base);
      for (size_t i = 0; i < arg_count; i++)
        free(args[i]);
      free(args);
      free(core);
      return;
    }
    char *mangled = mangle_name(base, args, arg_count);
    size_t new_len = strlen(mangled) + ptr_count +
                     (array_suffix ? strlen(array_suffix) : 0) + 1;
    char *new_type = malloc(new_len);
    strcpy(new_type, mangled);
    for (size_t i = 0; i < ptr_count; i++)
      strcat(new_type, "*");
    if (array_suffix)
      strcat(new_type, array_suffix);
    mettle_free_string(*slot);
    *slot = new_type;
    free(mangled);
    free(base);
    for (size_t i = 0; i < arg_count; i++)
      free(args[i]);
    free(args);
  }
  free(core);
}

static void rewrite_generic_references(ASTNode *node, MonoContext *ctx) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->type_name) {
      rewrite_generic_type_name_in_place(&vd->type_name, NULL, NULL, 0, ctx);
    }
    if (vd && vd->initializer)
      rewrite_generic_references(vd->initializer, ctx);
    break;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *ne = (NewExpression *)node->data;
    if (ne && ne->type_name) {
      char *base = NULL;
      char **args = NULL;
      size_t arg_count = 0;
      if (parse_generic_type_name(ne->type_name, &base, &args, &arg_count)) {
        char *mangled = mangle_name(base, args, arg_count);
        mettle_free_string(ne->type_name);
        ne->type_name = mangled;
        free(base);
        for (size_t i = 0; i < arg_count; i++)
          free(args[i]);
        free(args);
      }
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *ce = (CallExpression *)node->data;
    if (ce && ce->type_arg_count > 0 && ce->function_name) {
      char *written = written_generic_name(ce->function_name, ce->type_args,
                                           ce->type_arg_count);
      char *mangled =
          mangle_name(ce->function_name, ce->type_args, ce->type_arg_count);
      mettle_free_string(ce->function_name);
      ce->function_name = mangled;
      if (written) {
        mettle_free_string(ce->written_name);
        ce->written_name = written;
      }
      for (size_t i = 0; i < ce->type_arg_count; i++)
        mettle_free_string(ce->type_args[i]);
      free(ce->type_args);
      ce->type_args = NULL;
      ce->type_arg_count = 0;
    }
    if (ce) {
      for (size_t i = 0; i < ce->argument_count; i++)
        rewrite_generic_references(ce->arguments[i], ctx);
      if (ce->object)
        rewrite_generic_references(ce->object, ctx);
    }
    break;
  }
  case AST_FUNCTION_DECLARATION: {
    FunctionDeclaration *fd = (FunctionDeclaration *)node->data;
    if (fd && fd->type_param_count == 0) {
      for (size_t i = 0; i < fd->parameter_count; i++) {
        if (fd->parameter_types[i]) {
          rewrite_generic_type_name_in_place(&fd->parameter_types[i], NULL,
                                             NULL, 0, ctx);
        }
      }
      if (fd->return_type) {
        rewrite_generic_type_name_in_place(&fd->return_type, NULL, NULL, 0, ctx);
      }
      if (fd->body)
        rewrite_generic_references(fd->body, ctx);
    }
    break;
  }
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *sd = (StructDeclaration *)node->data;
    if (sd && sd->type_param_count == 0) {
      for (size_t i = 0; i < sd->field_count; i++) {
        if (sd->field_types[i]) {
          rewrite_generic_type_name_in_place(&sd->field_types[i], NULL, NULL,
                                             0, ctx);
        }
      }
      for (size_t i = 0; i < sd->method_count; i++) {
        ASTNode *method = sd->methods[i];
        FunctionDeclaration *md =
            method ? (FunctionDeclaration *)method->data : NULL;
        if (!md) {
          continue;
        }
        for (size_t p = 0; p < md->parameter_count; p++) {
          if (md->parameter_types[p]) {
            rewrite_generic_type_name_in_place(&md->parameter_types[p], NULL,
                                               NULL, 0, ctx);
          }
        }
        if (md->return_type) {
          rewrite_generic_type_name_in_place(&md->return_type, NULL, NULL, 0, ctx);
        }
        if (md->body) {
          rewrite_generic_references(md->body, ctx);
        }
      }
    }
    break;
  }
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (prog) {
      for (size_t i = 0; i < prog->declaration_count; i++)
        rewrite_generic_references(prog->declarations[i], ctx);
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value)
      rewrite_generic_references(rs->value, ctx);
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *as = (Assignment *)node->data;
    if (as) {
      if (as->value)
        rewrite_generic_references(as->value, ctx);
      if (as->target)
        rewrite_generic_references(as->target, ctx);
    }
    break;
  }
  case AST_IF_STATEMENT: {
    IfStatement *is_stmt = (IfStatement *)node->data;
    if (is_stmt) {
      rewrite_generic_references(is_stmt->condition, ctx);
      rewrite_generic_references(is_stmt->then_branch, ctx);
      for (size_t i = 0; i < is_stmt->else_if_count; i++) {
        rewrite_generic_references(is_stmt->else_ifs[i].condition, ctx);
        rewrite_generic_references(is_stmt->else_ifs[i].body, ctx);
      }
      if (is_stmt->else_branch)
        rewrite_generic_references(is_stmt->else_branch, ctx);
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *ws = (WhileStatement *)node->data;
    if (ws) {
      rewrite_generic_references(ws->condition, ctx);
      rewrite_generic_references(ws->body, ctx);
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *fs = (ForStatement *)node->data;
    if (fs) {
      if (fs->initializer)
        rewrite_generic_references(fs->initializer, ctx);
      if (fs->condition)
        rewrite_generic_references(fs->condition, ctx);
      if (fs->increment)
        rewrite_generic_references(fs->increment, ctx);
      if (fs->body)
        rewrite_generic_references(fs->body, ctx);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *ss = (SwitchStatement *)node->data;
    if (ss) {
      rewrite_generic_references(ss->expression, ctx);
      for (size_t i = 0; i < ss->case_count; i++)
        rewrite_generic_references(ss->cases[i], ctx);
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value)
        rewrite_generic_references(cc->value, ctx);
      if (cc->body)
        rewrite_generic_references(cc->body, ctx);
    }
    break;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *ce = (CastExpression *)node->data;
    if (ce && ce->operand)
      rewrite_generic_references(ce->operand, ctx);
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *be = (BinaryExpression *)node->data;
    if (be) {
      rewrite_generic_references(be->left, ctx);
      rewrite_generic_references(be->right, ctx);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *ue = (UnaryExpression *)node->data;
    if (ue && ue->operand)
      rewrite_generic_references(ue->operand, ctx);
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *ma = (MemberAccess *)node->data;
    if (ma && ma->object)
      rewrite_generic_references(ma->object, ctx);
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ie = (ArrayIndexExpression *)node->data;
    if (ie) {
      if (ie->array)
        rewrite_generic_references(ie->array, ctx);
      if (ie->index)
        rewrite_generic_references(ie->index, ctx);
    }
    break;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *ds = (DeferStatement *)node->data;
    if (ds && ds->statement)
      rewrite_generic_references(ds->statement, ctx);
    break;
  }
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      rewrite_generic_references(node->children[i], ctx);
    }
    break;
  default:
    break;
  }
}

static GenericDef *find_generic_def(MonoContext *ctx, const char *name) {
  for (size_t i = 0; i < ctx->def_count; i++) {
    if (strcmp(ctx->defs[i].name, name) == 0)
      return &ctx->defs[i];
  }
  return NULL;
}

static int validate_instantiation(GenericDef *def, Instantiation *inst,
                                  MonoContext *ctx) {
  if (!def || !inst || !ctx) {
    return 0;
  }

  if (def->type_param_count != inst->type_arg_count) {
    char message[512];
    snprintf(message, sizeof(message),
             "Generic '%s' expects %zu type arguments but got %zu", def->name,
             def->type_param_count, inst->type_arg_count);
    mono_report_error(ctx, inst->location, message);
    return 0;
  }

  for (size_t i = 0; i < def->type_param_count; i++) {
    const char *bounds =
        def->type_param_traits ? def->type_param_traits[i] : NULL;
    if (!mono_type_satisfies_bounds(ctx, def, inst, bounds, i)) {
      return 0;
    }
  }

  return 1;
}

static ASTNode *create_monomorphized_struct(GenericDef *def,
                                            Instantiation *inst,
                                            MonoContext *ctx) {
  ASTNode *clone = ast_clone_node(def->node);
  if (!clone)
    return NULL;

  StructDeclaration *sd = (StructDeclaration *)clone->data;

  /* Substituting a field type can discover a further instantiation, which
   * reallocs ctx->instances and leaves `inst` dangling. The arrays these name
   * are allocated per instantiation and do not move, so copying the handles out
   * first is enough -- but nothing may read through `inst` past this point. */
  char **type_args = inst->type_args;
  size_t type_arg_count = inst->type_arg_count;

  // Set the mangled name
  mettle_free_string(sd->name);
  sd->name = strdup(inst->mangled_name);

  // Clear type params (this is now a concrete type)
  for (size_t i = 0; i < sd->type_param_count; i++)
    mettle_free_string(sd->type_params[i]);
  free(sd->type_params);
  for (size_t i = 0; i < sd->type_param_count; i++)
    mettle_free_string(sd->type_param_traits[i]);
  free(sd->type_param_traits);
  sd->type_params = NULL;
  sd->type_param_traits = NULL;
  sd->type_param_count = 0;

  // Substitute type params in field types
  for (size_t i = 0; i < sd->field_count; i++) {
    char *new_type = substitute_type_string(
        sd->field_types[i], def->type_params, type_args,
        type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(sd->field_types[i]);
      sd->field_types[i] = new_type;
    }
  }

  /* Methods are part of the instantiation, not of the template: their
   * signatures and bodies name the type parameters the same way the fields do,
   * so they take the same substitution before being lifted to top-level
   * functions. */
  for (size_t i = 0; i < sd->method_count; i++) {
    ASTNode *method = sd->methods[i];
    FunctionDeclaration *md =
        method ? (FunctionDeclaration *)method->data : NULL;
    if (!md) {
      continue;
    }

    for (size_t p = 0; p < md->parameter_count; p++) {
      char *new_type = substitute_type_string(
          md->parameter_types[p], def->type_params, type_args,
          type_arg_count, ctx);
      if (new_type) {
        mettle_free_string(md->parameter_types[p]);
        md->parameter_types[p] = new_type;
      }
    }

    if (md->return_type) {
      char *new_type = substitute_type_string(
          md->return_type, def->type_params, type_args,
          type_arg_count, ctx);
      if (new_type) {
        mettle_free_string(md->return_type);
        md->return_type = new_type;
      }
    }

    if (md->body) {
      substitute_types_in_ast(md->body, def->type_params, type_args,
                              type_arg_count, ctx);
    }
  }

  return clone;
}

static ASTNode *create_monomorphized_function(GenericDef *def,
                                              Instantiation *inst,
                                              MonoContext *ctx) {
  ASTNode *clone = ast_clone_node(def->node);
  if (!clone)
    return NULL;

  FunctionDeclaration *fd = (FunctionDeclaration *)clone->data;

  /* As in create_monomorphized_struct: substitution can realloc ctx->instances
   * out from under `inst`, so nothing below reads through it. */
  char **type_args = inst->type_args;
  size_t type_arg_count = inst->type_arg_count;

  // Set the mangled name
  mettle_free_string(fd->name);
  fd->name = strdup(inst->mangled_name);

  // Clear type params
  for (size_t i = 0; i < fd->type_param_count; i++)
    mettle_free_string(fd->type_params[i]);
  free(fd->type_params);
  for (size_t i = 0; i < fd->type_param_count; i++)
    mettle_free_string(fd->type_param_traits[i]);
  free(fd->type_param_traits);
  fd->type_params = NULL;
  fd->type_param_traits = NULL;
  fd->type_param_count = 0;

  // Substitute type params in parameter types
  for (size_t i = 0; i < fd->parameter_count; i++) {
    char *new_type = substitute_type_string(
        fd->parameter_types[i], def->type_params, type_args,
        type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(fd->parameter_types[i]);
      fd->parameter_types[i] = new_type;
    }
  }

  // Substitute type params in return type
  if (fd->return_type) {
    char *new_type = substitute_type_string(
        fd->return_type, def->type_params, type_args,
        type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(fd->return_type);
      fd->return_type = new_type;
    }
  }

  // Substitute type params throughout the function body
  if (fd->body) {
    substitute_types_in_ast(fd->body, def->type_params, type_args,
                            type_arg_count, ctx);
  }

  return clone;
}

typedef struct {
  char *name;
  char *type_name;
} MonoVarBinding;

typedef struct {
  MonoVarBinding *items;
  size_t count;
  size_t capacity;
} MonoVarEnv;

static int mono_env_add(MonoVarEnv *env, char *name, char *type_name) {
  if (!env || !name || !type_name) {
    return 1;
  }

  for (size_t i = 0; i < env->count; i++) {
    if (env->items[i].name && strcmp(env->items[i].name, name) == 0) {
      env->items[i].type_name = type_name;
      return 1;
    }
  }

  if (env->count >= env->capacity) {
    size_t new_capacity = env->capacity ? env->capacity * 2 : 8;
    MonoVarBinding *grown =
        realloc(env->items, new_capacity * sizeof(MonoVarBinding));
    if (!grown) {
      return 0;
    }
    env->items = grown;
    env->capacity = new_capacity;
  }

  env->items[env->count].name = name;
  env->items[env->count].type_name = type_name;
  env->count++;
  return 1;
}

static const char *mono_env_lookup(MonoVarEnv *env, const char *name) {
  if (!env || !name) {
    return NULL;
  }

  for (size_t i = env->count; i > 0; i--) {
    if (env->items[i - 1].name && strcmp(env->items[i - 1].name, name) == 0) {
      return env->items[i - 1].type_name;
    }
  }

  return NULL;
}

static TraitImpl *mono_find_impl_for_method(MonoContext *ctx,
                                            const char *type_name,
                                            const char *method_name,
                                            int *ambiguous) {
  TraitImpl *found = NULL;

  if (ambiguous) {
    *ambiguous = 0;
  }
  if (!ctx || !type_name || !method_name) {
    return NULL;
  }

  for (size_t i = 0; i < ctx->impl_count; i++) {
    TraitImpl *impl = &ctx->impls[i];
    if (!impl->for_type_name || strcmp(impl->for_type_name, type_name) != 0) {
      continue;
    }
    if (!mono_find_function_method(impl->methods, impl->method_count,
                                   method_name)) {
      continue;
    }
    if (found) {
      if (ambiguous) {
        *ambiguous = 1;
      }
      return NULL;
    }
    found = impl;
  }

  return found;
}

static int mono_rewrite_trait_method_call(MonoContext *ctx, ASTNode *node,
                                          MonoVarEnv *env) {
  CallExpression *call = NULL;
  Identifier *object_id = NULL;
  const char *object_type = NULL;
  TraitImpl *impl = NULL;
  char *mangled = NULL;
  ASTNode **new_args = NULL;
  ASTNode *self_arg = NULL;
  int ambiguous = 0;

  if (!ctx || !node || node->type != AST_FUNCTION_CALL || !env) {
    return 1;
  }

  call = (CallExpression *)node->data;
  if (!call || !call->object || call->object->type != AST_IDENTIFIER ||
      !call->function_name) {
    return 1;
  }

  object_id = (Identifier *)call->object->data;
  if (!object_id || !object_id->name) {
    return 1;
  }

  object_type = mono_env_lookup(env, object_id->name);
  if (!object_type) {
    return 1;
  }

  impl = mono_find_impl_for_method(ctx, object_type, call->function_name,
                                   &ambiguous);
  if (ambiguous) {
    char message[512];
    snprintf(message, sizeof(message),
             "Trait method call '%s.%s' is ambiguous for type '%s'",
             object_id->name, call->function_name, object_type);
    mono_report_error(ctx, node->location, message);
    return 0;
  }
  if (!impl) {
    return 1;
  }

  mangled = mono_mangle_impl_method_name(impl->trait_name, impl->for_type_name,
                                         call->function_name);
  self_arg = ast_clone_node(call->object);
  new_args = malloc((call->argument_count + 1) * sizeof(ASTNode *));
  if (!mangled || !self_arg || !new_args) {
    free(mangled);
    if (self_arg) {
      ast_destroy_node(self_arg);
    }
    free(new_args);
    mono_report_error(ctx, node->location,
                      "Failed to rewrite trait method call");
    return 0;
  }

  new_args[0] = self_arg;
  for (size_t i = 0; i < call->argument_count; i++) {
    new_args[i + 1] = call->arguments[i];
  }
  free(call->arguments);
  call->arguments = new_args;
  call->argument_count++;
  ast_add_child(node, self_arg);

  if (!call->written_name) {
    call->written_name = strdup(call->function_name);
  }
  mettle_free_string(call->function_name);
  call->function_name = mangled;
  call->object = NULL;
  return 1;
}

static int mono_rewrite_trait_method_calls_in_node(MonoContext *ctx,
                                                   ASTNode *node,
                                                   MonoVarEnv *env) {
  if (!node || !env) {
    return 1;
  }

  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call) {
      for (size_t i = 0; i < call->argument_count; i++) {
        if (!mono_rewrite_trait_method_calls_in_node(ctx, call->arguments[i],
                                                     env)) {
          return 0;
        }
      }
      if (call->object &&
          !mono_rewrite_trait_method_calls_in_node(ctx, call->object, env)) {
        return 0;
      }
    }
    return mono_rewrite_trait_method_call(ctx, node, env);
  }

  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *decl = (VarDeclaration *)node->data;
    if (decl && decl->initializer &&
        !mono_rewrite_trait_method_calls_in_node(ctx, decl->initializer, env)) {
      return 0;
    }
    if (decl && !mono_env_add(env, decl->name, decl->type_name)) {
      mono_report_error(ctx, node->location,
                        "Failed to track local type for trait method call");
      return 0;
    }
    return 1;
  }

  for (size_t i = 0; i < node->child_count; i++) {
    if (!mono_rewrite_trait_method_calls_in_node(ctx, node->children[i], env)) {
      return 0;
    }
  }

  return 1;
}

static int mono_rewrite_trait_method_calls_in_function(MonoContext *ctx,
                                                       ASTNode *function) {
  FunctionDeclaration *fn = NULL;
  MonoVarEnv env = {0};
  int ok = 1;

  if (!ctx || !function || function->type != AST_FUNCTION_DECLARATION) {
    return 1;
  }

  fn = (FunctionDeclaration *)function->data;
  if (!fn || !fn->body) {
    return 1;
  }

  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (!mono_env_add(&env, fn->parameter_names[i], fn->parameter_types[i])) {
      mono_report_error(ctx, function->location,
                        "Failed to track parameter type for trait method call");
      ok = 0;
      goto cleanup;
    }
  }

  ok = mono_rewrite_trait_method_calls_in_node(ctx, fn->body, &env);

cleanup:
  free(env.items);
  return ok;
}

/* ---- Type-argument inference ---------------------------------------------
 * A call names its type arguments when nothing else can say what they are.
 * When the arguments already say it, `id(7)` is the same call as
 * `id<int32>(7)`, and writing the second adds nothing. Inference runs here,
 * before instantiation, because what it decides is which instantiation to
 * make. It reads the type text the program wrote: a parameter spelled with a
 * type parameter is matched against the argument's static type, and the
 * parameter's binding is whatever the argument put where it sits.
 *
 * It binds only what it is sure of and contradicts nothing else, so a call it
 * cannot read is reported as one to name the arguments on, never guessed at.
 */

typedef struct {
  const char *name;
  char *bound;
} MonoTypeBinding;

static char *mono_text_trim_dup(const char *text) {
  size_t start = 0;
  size_t end;
  char *copy;

  if (!text) {
    return NULL;
  }
  end = strlen(text);
  while (start < end && (text[start] == ' ' || text[start] == '\t')) {
    start++;
  }
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    end--;
  }
  copy = malloc(end - start + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text + start, end - start);
  copy[end - start] = '\0';
  return copy;
}

static int mono_type_param_index(char **params, size_t count,
                                 const char *text) {
  size_t i;
  if (!params || !text) {
    return -1;
  }
  for (i = 0; i < count; i++) {
    if (params[i] && strcmp(params[i], text) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/* The head of `Name<...>`, or NULL when the text is not a generic use. */
static const char *mono_generic_args_start(const char *text) {
  const char *open = text ? strchr(text, '<') : NULL;
  if (!open || open == text) {
    return NULL;
  }
  if (text[strlen(text) - 1] != '>') {
    return NULL;
  }
  return open;
}

static int mono_match_type_text(const char *pattern, const char *actual,
                                char **params, size_t param_count,
                                MonoTypeBinding *bindings);

/* Match `Name<A, B>` against `Name<X, Y>`, argument by argument. */
static int mono_match_generic_args(const char *pattern, const char *actual,
                                   char **params, size_t param_count,
                                   MonoTypeBinding *bindings) {
  const char *p_open = mono_generic_args_start(pattern);
  const char *a_open = mono_generic_args_start(actual);
  const char *p_scan;
  const char *a_scan;
  size_t p_head;
  size_t a_head;

  if (!p_open || !a_open) {
    return 1;
  }
  p_head = (size_t)(p_open - pattern);
  a_head = (size_t)(a_open - actual);
  if (p_head != a_head || strncmp(pattern, actual, p_head) != 0) {
    return 1;
  }

  p_scan = p_open + 1;
  a_scan = a_open + 1;
  while (*p_scan && *a_scan) {
    const char *p_end = p_scan;
    const char *a_end = a_scan;
    int depth = 0;
    char *p_arg;
    char *a_arg;
    int ok;
    while (*p_end && (depth > 0 || (*p_end != ',' && *p_end != '>'))) {
      if (*p_end == '<') {
        depth++;
      } else if (*p_end == '>') {
        depth--;
      }
      p_end++;
    }
    depth = 0;
    while (*a_end && (depth > 0 || (*a_end != ',' && *a_end != '>'))) {
      if (*a_end == '<') {
        depth++;
      } else if (*a_end == '>') {
        depth--;
      }
      a_end++;
    }
    p_arg = mono_text_trim_dup(p_scan);
    a_arg = mono_text_trim_dup(a_scan);
    if (p_arg && a_arg) {
      p_arg[p_end - p_scan] = '\0';
      a_arg[a_end - a_scan] = '\0';
    }
    ok = p_arg && a_arg
             ? mono_match_type_text(p_arg, a_arg, params, param_count, bindings)
             : 1;
    free(p_arg);
    free(a_arg);
    if (!ok) {
      return 0;
    }
    if (*p_end != ',' || *a_end != ',') {
      break;
    }
    p_scan = p_end + 1;
    a_scan = a_end + 1;
  }
  return 1;
}

/* Bind what `pattern` says about `actual`. Returns 0 only on a contradiction
 * between two uses of the same type parameter, which is what makes a call
 * ambiguous. */
static int mono_match_type_text(const char *pattern, const char *actual,
                                char **params, size_t param_count,
                                MonoTypeBinding *bindings) {
  char *p = mono_text_trim_dup(pattern);
  char *a = mono_text_trim_dup(actual);
  int result = 1;
  int index;

  if (!p || !a || !*p || !*a) {
    free(p);
    free(a);
    return 1;
  }

  index = mono_type_param_index(params, param_count, p);
  if (index >= 0) {
    if (bindings[index].bound) {
      result = strcmp(bindings[index].bound, a) == 0;
    } else {
      bindings[index].bound = mettle_strdup(a);
    }
    free(p);
    free(a);
    return result;
  }

  {
    size_t p_len = strlen(p);
    size_t a_len = strlen(a);
    if (p[p_len - 1] == '*' && a[a_len - 1] == '*') {
      p[p_len - 1] = '\0';
      a[a_len - 1] = '\0';
      result = mono_match_type_text(p, a, params, param_count, bindings);
      free(p);
      free(a);
      return result;
    }
    if (p[p_len - 1] == ']' && a[a_len - 1] == ']') {
      char *p_open = strrchr(p, '[');
      char *a_open = strrchr(a, '[');
      if (p_open && a_open) {
        *p_open = '\0';
        *a_open = '\0';
        result = mono_match_type_text(p, a, params, param_count, bindings);
        free(p);
        free(a);
        return result;
      }
    }
  }

  result = mono_match_generic_args(p, a, params, param_count, bindings);
  free(p);
  free(a);
  return result;
}

static char *mono_static_return_type_of_call(MonoContext *ctx,
                                             ASTNode *expr) {
  CallExpression *call = (CallExpression *)expr->data;
  size_t i;
  if (!call || !call->function_name || !ctx->module) {
    return NULL;
  }
  for (i = 0; i < ctx->module->declaration_count; i++) {
    ASTNode *decl = ctx->module->declarations[i];
    FunctionDeclaration *fd;
    if (!decl || decl->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    fd = (FunctionDeclaration *)decl->data;
    if (!fd || !fd->name || fd->type_param_count > 0 ||
        strcmp(fd->name, call->function_name) != 0) {
      continue;
    }
    return fd->return_type ? mettle_strdup(fd->return_type) : NULL;
  }
  return NULL;
}

static char *mono_static_type_of_element(MonoContext *ctx, ASTNode *expr,
                                         MonoVarEnv *env);

/* The type text of an expression, read off the program without checking it.
 * NULL means "this expression does not say", which costs nothing: another
 * argument may still say it, and if none does the call is reported. */
static char *mono_static_type_of(MonoContext *ctx, ASTNode *expr,
                                 MonoVarEnv *env) {
  if (!expr) {
    return NULL;
  }
  switch (expr->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *lit = (NumberLiteral *)expr->data;
    if (!lit) {
      return NULL;
    }
    if (lit->is_char) {
      return mettle_strdup("char");
    }
    return mettle_strdup(lit->is_float ? "float64" : "int32");
  }
  case AST_STRING_LITERAL:
    return mettle_strdup("string");
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)expr->data;
    const char *bound = id ? mono_env_lookup(env, id->name) : NULL;
    return bound ? mettle_strdup(bound) : NULL;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expr->data;
    return cast && cast->type_name ? mettle_strdup(cast->type_name) : NULL;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *ne = (NewExpression *)expr->data;
    char *text;
    size_t len;
    if (!ne || !ne->type_name) {
      return NULL;
    }
    len = strlen(ne->type_name) + 2;
    text = malloc(len);
    if (!text) {
      return NULL;
    }
    snprintf(text, len, "%s*", ne->type_name);
    return text;
  }
  case AST_UNARY_EXPRESSION:
  case AST_INDEX_EXPRESSION:
    return mono_static_type_of_element(ctx, expr, env);
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *bin = (BinaryExpression *)expr->data;
    char *left;
    if (!bin || !bin->operator) {
      return NULL;
    }
    if (strcmp(bin->operator, "==") == 0 || strcmp(bin->operator, "!=") == 0 ||
        strcmp(bin->operator, "<") == 0 || strcmp(bin->operator, ">") == 0 ||
        strcmp(bin->operator, "<=") == 0 || strcmp(bin->operator, ">=") == 0 ||
        strcmp(bin->operator, "&&") == 0 || strcmp(bin->operator, "||") == 0) {
      return NULL;
    }
    left = mono_static_type_of(ctx, bin->left, env);
    if (left) {
      return left;
    }
    return mono_static_type_of(ctx, bin->right, env);
  }
  case AST_FUNCTION_CALL:
    return mono_static_return_type_of_call(ctx, expr);
  default:
    return NULL;
  }
}

static char *mono_static_type_of_element(MonoContext *ctx, ASTNode *expr,
                                         MonoVarEnv *env) {
  switch (expr->type) {
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *un = (UnaryExpression *)expr->data;
    char *inner;
    if (!un || !un->operator) {
      return NULL;
    }
    inner = mono_static_type_of(ctx, un->operand, env);
    if (strcmp(un->operator, "&") == 0) {
      char *text;
      size_t len;
      if (!inner) {
        return NULL;
      }
      len = strlen(inner) + 2;
      text = malloc(len);
      if (text) {
        snprintf(text, len, "%s*", inner);
      }
      free(inner);
      return text;
    }
    if (strcmp(un->operator, "*") == 0) {
      size_t len = inner ? strlen(inner) : 0;
      if (len > 0 && inner[len - 1] == '*') {
        inner[len - 1] = '\0';
        return inner;
      }
      free(inner);
      return NULL;
    }
    return inner;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ix = (ArrayIndexExpression *)expr->data;
    char *base = ix ? mono_static_type_of(ctx, ix->array, env) : NULL;
    size_t len = base ? strlen(base) : 0;
    if (!base || len == 0) {
      free(base);
      return NULL;
    }
    if (base[len - 1] == '*') {
      base[len - 1] = '\0';
      return base;
    }
    if (base[len - 1] == ']') {
      char *open = strrchr(base, '[');
      if (open) {
        *open = '\0';
        return base;
      }
    }
    free(base);
    return NULL;
  }
  default:
    return NULL;
  }
}

static int mono_infer_call_type_args(MonoContext *ctx, ASTNode *node,
                                     MonoVarEnv *env) {
  CallExpression *call = (CallExpression *)node->data;
  GenericDef *def = NULL;
  FunctionDeclaration *fd = NULL;
  MonoTypeBinding *bindings = NULL;
  size_t i;
  int ok = 1;

  if (!call || call->type_arg_count > 0 || !call->function_name) {
    return 1;
  }
  def = find_generic_def(ctx, call->function_name);
  if (!def || def->is_struct || def->type_param_count == 0 || !def->node) {
    return 1;
  }
  fd = (FunctionDeclaration *)def->node->data;
  if (!fd) {
    return 1;
  }

  bindings = calloc(def->type_param_count, sizeof(MonoTypeBinding));
  if (!bindings) {
    return 1;
  }
  for (i = 0; i < def->type_param_count; i++) {
    bindings[i].name = def->type_params[i];
  }

  for (i = 0; i < call->argument_count && i < fd->parameter_count; i++) {
    char *actual = mono_static_type_of(ctx, call->arguments[i], env);
    if (!actual) {
      continue;
    }
    if (!mono_match_type_text(fd->parameter_types[i], actual, def->type_params,
                              def->type_param_count, bindings)) {
      char message[512];
      snprintf(message, sizeof(message),
               "Argument %zu of '%s' asks for a different type parameter "
               "than an earlier argument did. Name the types at the call",
               i + 1, call->function_name);
      mono_report_error(ctx, node->location, message);
      free(actual);
      ok = 0;
      goto done;
    }
    free(actual);
  }

  for (i = 0; i < def->type_param_count; i++) {
    if (!bindings[i].bound) {
      char message[512];
      snprintf(message, sizeof(message),
               "Nothing in this call says what '%s' is in '%s'. Name it at "
               "the call, as '%s<sometype>(...)'",
               bindings[i].name, call->function_name, call->function_name);
      mono_report_error(ctx, node->location, message);
      ok = 0;
      goto done;
    }
  }

  call->type_args = malloc(def->type_param_count * sizeof(char *));
  if (!call->type_args) {
    ok = 0;
    goto done;
  }
  for (i = 0; i < def->type_param_count; i++) {
    call->type_args[i] = bindings[i].bound;
    bindings[i].bound = NULL;
  }
  call->type_arg_count = def->type_param_count;

done:
  for (i = 0; i < def->type_param_count; i++) {
    mettle_free_string(bindings[i].bound);
  }
  free(bindings);
  return ok;
}

static int mono_infer_type_args_in_node(MonoContext *ctx, ASTNode *node,
                                        MonoVarEnv *env) {
  size_t i;

  if (!node) {
    return 1;
  }

  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call) {
      for (i = 0; i < call->argument_count; i++) {
        if (!mono_infer_type_args_in_node(ctx, call->arguments[i], env)) {
          return 0;
        }
      }
      if (call->object &&
          !mono_infer_type_args_in_node(ctx, call->object, env)) {
        return 0;
      }
    }
    if (!mono_infer_call_type_args(ctx, node, env)) {
      return 0;
    }
    return 1;
  }

  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *decl = (VarDeclaration *)node->data;
    if (decl && decl->initializer &&
        !mono_infer_type_args_in_node(ctx, decl->initializer, env)) {
      return 0;
    }
    if (decl && decl->name && decl->type_name &&
        !mono_env_add(env, decl->name, decl->type_name)) {
      return 0;
    }
    return 1;
  }

  for (i = 0; i < node->child_count; i++) {
    if (!mono_infer_type_args_in_node(ctx, node->children[i], env)) {
      return 0;
    }
  }
  return 1;
}

static int mono_infer_type_args_in_function(MonoContext *ctx,
                                            ASTNode *function) {
  FunctionDeclaration *fn = NULL;
  MonoVarEnv env = {0};
  int ok = 1;
  size_t i;

  if (!ctx || !function || function->type != AST_FUNCTION_DECLARATION) {
    return 1;
  }
  fn = (FunctionDeclaration *)function->data;
  if (!fn || !fn->body || fn->type_param_count > 0) {
    return 1;
  }

  for (i = 0; i < fn->parameter_count && ok; i++) {
    if (!mono_env_add(&env, fn->parameter_names[i], fn->parameter_types[i])) {
      ok = 0;
    }
  }
  if (ok) {
    ok = mono_infer_type_args_in_node(ctx, fn->body, &env);
  }
  free(env.items);
  return ok;
}

static int mono_infer_type_args(MonoContext *ctx, ASTNode *program) {
  Program *prog = NULL;
  size_t i;

  if (!ctx || !program || program->type != AST_PROGRAM) {
    return 1;
  }
  prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }
  for (i = 0; i < prog->declaration_count; i++) {
    if (!mono_infer_type_args_in_function(ctx, prog->declarations[i])) {
      return 0;
    }
  }
  return 1;
}

static int mono_rewrite_trait_method_calls(MonoContext *ctx, ASTNode *program) {
  Program *prog = NULL;

  if (!ctx || !program || program->type != AST_PROGRAM) {
    return 1;
  }

  prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
      if (fn && fn->type_param_count == 0 &&
          !mono_rewrite_trait_method_calls_in_function(ctx, decl)) {
        return 0;
      }
    }
  }

  return 1;
}

static int mono_emit_impl_method_functions(MonoContext *ctx, ASTNode *program) {
  Program *prog = NULL;

  if (!ctx || !program || program->type != AST_PROGRAM) {
    return 1;
  }

  prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }

  for (size_t i = 0; i < ctx->impl_count; i++) {
    TraitImpl *impl = &ctx->impls[i];
    for (size_t j = 0; j < impl->method_count; j++) {
      ASTNode *fn = mono_create_impl_method_function(ctx, impl, impl->methods[j]);
      ASTNode **grown = NULL;
      if (!fn) {
        mono_report_error(ctx, (SourceLocation){0, 0, NULL},
                          "Failed to create trait impl method function");
        return 0;
      }
      grown = realloc(prog->declarations,
                      (prog->declaration_count + 1) * sizeof(ASTNode *));
      if (!grown) {
        SourceLocation location = fn->location;
        ast_destroy_node(fn);
        mono_report_error(ctx, location,
                          "Failed to append trait impl method function");
        return 0;
      }
      prog->declarations = grown;
      prog->declarations[prog->declaration_count++] = fn;
      ast_add_child(program, fn);
    }
  }

  return 1;
}

/* Turn every `this` in a method body into `*this`, in place. The node is
 * rewritten rather than replaced so that the parent's payload pointer and its
 * child slot -- which both name this node -- stay correct without the walk
 * needing to know what kind of parent it has. `this.x` becomes `(*this).x` and
 * `return this` becomes `return *this`, which is what the body means once the
 * receiver arrives as a pointer. */
static void mono_this_to_deref(ASTNode *node) {
  if (!node) {
    return;
  }

  if (node->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)node->data;
    if (id && id->name && strcmp(id->name, "this") == 0) {
      ASTNode *inner = ast_create_identifier("this", node->location);
      UnaryExpression *deref = inner ? malloc(sizeof(UnaryExpression)) : NULL;
      char *op = deref ? mettle_strdup("*") : NULL;
      if (!op) {
        free(deref);
        ast_destroy_node(inner);
        return;
      }
      deref->operator= op;
      deref->operand = inner;
      mettle_free_string(id->name);
      free(id);
      node->type = AST_UNARY_EXPRESSION;
      node->data = deref;
      ast_add_child(node, inner);
      return;
    }
  }

  for (size_t i = 0; i < node->child_count; i++) {
    mono_this_to_deref(node->children[i]);
  }
}

typedef struct {
  const char **names;
  size_t count;
  size_t capacity;
} MonoNameSet;

static int mono_name_set_has(const MonoNameSet *set, const char *name) {
  if (!set || !name) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void mono_name_set_add(MonoNameSet *set, const char *name) {
  if (!name || mono_name_set_has(set, name)) {
    return;
  }
  if (set->count >= set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 8;
    const char **grown = realloc(set->names, capacity * sizeof(const char *));
    if (!grown) {
      return;
    }
    set->names = grown;
    set->capacity = capacity;
  }
  set->names[set->count++] = name;
}

/* Which method names some call reaches through a pointer, as `p->m()` -- which
 * the parser has already turned into a call whose receiver is a deref. Only
 * those get a pointer-taking copy, so a program that never calls a method
 * through a pointer emits exactly the functions it did before, and `objdump -t`
 * shows it. The test is the same one the type checker applies, minus the
 * receiver's type, which is not known until later: this set is therefore a
 * superset of what gets resolved, never a subset. */
static void mono_collect_pointer_receiver_methods(ASTNode *node,
                                                  MonoNameSet *set) {
  if (!node) {
    return;
  }
  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call && call->function_name && call->object &&
        call->object->type == AST_UNARY_EXPRESSION) {
      UnaryExpression *unary = (UnaryExpression *)call->object->data;
      if (unary && unary->operator&& strcmp(unary->operator, "*") == 0) {
        mono_name_set_add(set, call->function_name);
      }
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    mono_collect_pointer_receiver_methods(node->children[i], set);
  }
}

/* Build the free function a method call resolves to. `receiver_is_pointer`
 * selects between the two forms the language distinguishes: `s.m(x)` passes the
 * struct by value, `p->m(x)` passes the pointer, so each gets its own lifted
 * function and the call site picks by how the receiver was spelled. The pointer
 * form takes a cloned body with `this` dereferenced throughout; the value form
 * takes the original body, which the struct declaration gives up. */
static ASTNode *mono_lift_one_method(MonoContext *ctx, const char *struct_name,
                                     ASTNode *method, int receiver_is_pointer) {
  FunctionDeclaration *md = (FunctionDeclaration *)method->data;
  const char *suffix = receiver_is_pointer ? MONO_PTR_RECEIVER_SUFFIX : "";
  size_t mangled_len =
      strlen(struct_name) + 1 + strlen(md->name) + strlen(suffix) + 1;
  char *mangled = malloc(mangled_len);
  if (!mangled) {
    mono_report_error(ctx, method->location,
                      "Out of memory lifting a struct method");
    return NULL;
  }
  snprintf(mangled, mangled_len, "%s_%s%s", struct_name, md->name, suffix);

  size_t count = md->parameter_count + 1;
  char **names = calloc(count, sizeof(char *));
  char **types = calloc(count, sizeof(char *));
  if (!names || !types) {
    free(mangled);
    free(names);
    free(types);
    mono_report_error(ctx, method->location,
                      "Out of memory lifting a struct method");
    return NULL;
  }
  names[0] = mettle_strdup("this");
  if (receiver_is_pointer) {
    size_t len = strlen(struct_name) + 2;
    types[0] = malloc(len);
    if (types[0]) {
      snprintf(types[0], len, "%s*", struct_name);
    }
  } else {
    types[0] = mettle_strdup(struct_name);
  }
  int copied_ok = names[0] != NULL && types[0] != NULL;
  for (size_t p = 0; copied_ok && p < md->parameter_count; p++) {
    names[p + 1] = mettle_strdup(md->parameter_names[p]);
    types[p + 1] = mettle_strdup(md->parameter_types[p]);
    copied_ok = names[p + 1] != NULL && types[p + 1] != NULL;
  }

  ASTNode *body = NULL;
  if (copied_ok) {
    body = receiver_is_pointer ? ast_clone_node(md->body) : md->body;
    if (body && receiver_is_pointer) {
      mono_this_to_deref(body);
    }
  }

  ASTNode *fn_node =
      body ? ast_create_node(AST_FUNCTION_DECLARATION, method->location) : NULL;
  FunctionDeclaration *fn =
      fn_node ? calloc(1, sizeof(FunctionDeclaration)) : NULL;
  if (!fn) {
    for (size_t p = 0; p < count; p++) {
      mettle_free_string(names[p]);
      mettle_free_string(types[p]);
    }
    free(names);
    free(types);
    free(mangled);
    if (body && receiver_is_pointer) {
      ast_destroy_node(body);
    }
    if (fn_node) {
      ast_destroy_node(fn_node);
    }
    mono_report_error(ctx, method->location,
                      "Out of memory lifting a struct method");
    return NULL;
  }

  fn->name = mangled;
  fn->parameter_names = names;
  fn->parameter_types = types;
  fn->parameter_count = count;
  fn->return_type = md->return_type ? mettle_strdup(md->return_type) : NULL;
  fn->body = body;
  fn->is_inline = md->is_inline;
  fn->is_inline_contract = md->is_inline_contract;
  fn->is_noinline = md->is_noinline;
  fn->is_pure = md->is_pure;
  fn->reference_twin = md->reference_twin ? strdup(md->reference_twin) : NULL;
  fn->explain_code = md->explain_code ? strdup(md->explain_code) : NULL;
  fn->explain_text = md->explain_text ? strdup(md->explain_text) : NULL;
  fn->is_noalloc = md->is_noalloc;
  fn->is_swappable = md->is_swappable;
  fn->is_naked = md->is_naked;
  fn->is_interrupt = md->is_interrupt;
  fn->simd_mode = md->simd_mode;
  if (!receiver_is_pointer) {
    md->body = NULL; /* ownership moves to the lifted function */
  }
  fn_node->data = fn;
  ast_add_child(fn_node, fn->body);
  return fn_node;
}

/* `struct S { method m(a: T) -> R { ... } }` is called as `s.m(x)`, which the
 * type checker rewrites into a plain call to `S_m(s, x)`. Nothing ever created
 * S_m: the parser stored the method on the struct declaration and every later
 * pass ignored it, so the documented form failed with "Undefined method 'S.m'
 * (expected function 'S_m')" and methods only worked if you hand-wrote the free
 * function yourself.
 *
 * A method lifts once per receiver form the program actually uses: `S_m` takes
 * the struct by value for `s.m(x)`, and `S_m<suffix>` takes `S*` for `p->m(x)`,
 * so a method reached through a pointer can write to its receiver. The struct
 * declaration keeps the method node for diagnostics but gives up its body, which
 * the by-value function takes over; the pointer function gets a clone. */
static int mono_lift_struct_methods(MonoContext *ctx, Program *prog,
                                    ASTNode *program) {
  MonoNameSet pointer_methods = {0};
  int ok = 1;
  size_t original_count = prog->declaration_count;

  mono_collect_pointer_receiver_methods(program, &pointer_methods);

  for (size_t d = 0; d < original_count; d++) {
    ASTNode *decl = prog->declarations[d];
    if (!decl || decl->type != AST_STRUCT_DECLARATION) {
      continue;
    }
    StructDeclaration *sd = (StructDeclaration *)decl->data;
    if (!sd || !sd->name || sd->method_count == 0) {
      continue;
    }
    /* A generic template is removed from the program after instantiation; its
     * methods would name type parameters that no longer exist. */
    if (sd->type_param_count > 0) {
      continue;
    }

    for (size_t m = 0; m < sd->method_count; m++) {
      ASTNode *method = sd->methods[m];
      if (!method || !method->data) {
        continue;
      }
      FunctionDeclaration *md = (FunctionDeclaration *)method->data;
      if (!md || !md->name || !md->body) {
        continue;
      }

      /* Pointer form first: it clones the body, which the value form consumes. */
      for (int ptr = 1; ptr >= 0; ptr--) {
        ASTNode *fn_node = NULL;
        ASTNode **grown = NULL;

        if (ptr && !mono_name_set_has(&pointer_methods, md->name)) {
          continue;
        }

        fn_node = mono_lift_one_method(ctx, sd->name, method, ptr);
        if (!fn_node) {
          ok = 0;
          goto done;
        }

        grown = realloc(prog->declarations,
                        (prog->declaration_count + 1) * sizeof(ASTNode *));
        if (!grown) {
          ast_destroy_node(fn_node);
          mono_report_error(ctx, method->location,
                            "Failed to append a lifted struct method");
          ok = 0;
          goto done;
        }
        prog->declarations = grown;
        prog->declarations[prog->declaration_count++] = fn_node;
        ast_add_child(program, fn_node);
      }
    }
  }

done:
  free(pointer_methods.names);
  return ok;
}

static int mono_emit_instantiation(MonoContext *ctx, Program *prog,
                                   ASTNode *program, size_t index,
                                   int *success) {
  ASTNode *mono_node = NULL;
  GenericDef *def = NULL;
  Instantiation *inst = NULL;
  size_t deps_start = 0;

  if (!ctx || !prog || !program || !success || index >= ctx->instance_count) {
    return 1;
  }

  inst = &ctx->instances[index];
  if (inst->emitted) {
    return 1;
  }

  def = find_generic_def(ctx, inst->generic_name);
  if (!def) {
    return 1;
  }

  if (!validate_instantiation(def, inst, ctx)) {
    *success = 0;
    return 0;
  }

  deps_start = ctx->instance_count;

  if (def->is_struct) {
    mono_node = create_monomorphized_struct(def, inst, ctx);
  } else {
    mono_node = create_monomorphized_function(def, inst, ctx);
  }

  if (!mono_node) {
    return 1;
  }

  for (size_t j = deps_start; j < ctx->instance_count; j++) {
    GenericDef *dep_def = find_generic_def(ctx, ctx->instances[j].generic_name);
    if (!dep_def || !dep_def->is_struct || ctx->instances[j].emitted) {
      continue;
    }
    if (!mono_emit_instantiation(ctx, prog, program, j, success)) {
      return 0;
    }
    if (!*success) {
      return 0;
    }
  }

  prog->declarations =
      realloc(prog->declarations,
              (prog->declaration_count + 1) * sizeof(ASTNode *));
  if (!prog->declarations) {
    *success = 0;
    return 0;
  }
  prog->declarations[prog->declaration_count] = mono_node;
  prog->declaration_count++;
  ast_add_child(program, mono_node);

  /* Not `inst->emitted`: creating this one may have discovered others, which
   * reallocs the array `inst` pointed into. Marking it through the index is
   * what actually records the emission -- through the stale pointer it was
   * written to freed memory, and the instantiation stayed marked unemitted. */
  ctx->instances[index].emitted = 1;
  if (mono_node->type == AST_FUNCTION_DECLARATION &&
      !mono_infer_type_args_in_function(ctx, mono_node)) {
    *success = 0;
    return 0;
  }
  collect_type_instantiations(mono_node, ctx);
  return 1;
}

/* ---- Closure conversion --------------------------------------------------
 * Anonymous `fn(...) { }` expressions parse to AST_LAMBDA_EXPRESSION nodes
 * carrying a FunctionDeclaration payload with a NULL name. This pass lifts each
 * lambda body to a uniquely-named top-level function and records that name on
 * the lambda node (fd->name) so the type checker and IR lowering treat the
 * lambda value as the address of that function. A lambda that references a
 * variable from an enclosing function is a true (capturing) closure, handled in
 * a later step. */

/* A scope's variable names paired with their declared type strings (NULL when
 * the type was inferred and no annotation is available). */
typedef struct {
  char **names;
  char **types;
  size_t count;
  size_t cap;
} CCEnv;

static int cc_env_has(const CCEnv *e, const char *name) {
  if (!name)
    return 0;
  for (size_t i = 0; i < e->count; i++)
    if (strcmp(e->names[i], name) == 0)
      return 1;
  return 0;
}

static const char *cc_env_type(const CCEnv *e, const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < e->count; i++)
    if (strcmp(e->names[i], name) == 0)
      return e->types[i];
  return NULL;
}

static void cc_env_add(CCEnv *e, const char *name, const char *type) {
  if (!name || cc_env_has(e, name))
    return;
  if (e->count == e->cap) {
    size_t ncap = e->cap ? e->cap * 2 : 8;
    char **gn = realloc(e->names, ncap * sizeof(char *));
    char **gt = realloc(e->types, ncap * sizeof(char *));
    if (!gn || !gt) {
      free(gn);
      free(gt);
      return;
    }
    e->names = gn;
    e->types = gt;
    e->cap = ncap;
  }
  e->names[e->count] = strdup(name);
  e->types[e->count] = type ? strdup(type) : NULL;
  e->count++;
}

static void cc_env_free(CCEnv *e) {
  for (size_t i = 0; i < e->count; i++) {
    free(e->names[i]);
    free(e->types[i]);
  }
  free(e->names);
  free(e->types);
  e->names = NULL;
  e->types = NULL;
  e->count = 0;
  e->cap = 0;
}

/* Var-declaration names (with their type strings) inside `node`, not descending
 * into nested lambdas (which open their own scope). */
static void cc_collect_vars(ASTNode *node, CCEnv *env) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION)
    return;
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd)
      cc_env_add(env, vd->name, vd->type_name);
  }
  for (size_t i = 0; i < node->child_count; i++)
    cc_collect_vars(node->children[i], env);
}

static void cc_collect_fn_env(FunctionDeclaration *fd, CCEnv *env) {
  for (size_t i = 0; i < fd->parameter_count; i++)
    cc_env_add(env, fd->parameter_names[i], fd->parameter_types[i]);
  cc_collect_vars(fd->body, env);
}

/* A name used in the lambda body that is bound in an enclosing function (and not
 * rebound inside the lambda) is a capture. Does not descend into nested lambdas.
 * Captures are accumulated in first-reference order in `caps`. */
static void cc_scan_captures(ASTNode *node, const CCEnv *enclosing,
                             const CCEnv *bound, CCEnv *caps) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION)
    return;
  const char *use = NULL;
  if (node->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)node->data;
    use = id ? id->name : NULL;
  } else if (node->type == AST_ASSIGNMENT) {
    Assignment *a = (Assignment *)node->data;
    use = a ? a->variable_name : NULL;
  } else if (node->type == AST_FUNCTION_CALL) {
    CallExpression *c = (CallExpression *)node->data;
    use = c ? c->function_name : NULL;
  }
  if (use && !cc_env_has(bound, use) && cc_env_has(enclosing, use))
    cc_env_add(caps, use, cc_env_type(enclosing, use));
  for (size_t i = 0; i < node->child_count; i++)
    cc_scan_captures(node->children[i], enclosing, bound, caps);
}

/* Growable text buffer for synthesizing source. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} CCBuf;

static void cc_buf_add(CCBuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + n + 1)
      ncap *= 2;
    char *grown = realloc(b->data, ncap);
    if (!grown)
      return;
    b->data = grown;
    b->cap = ncap;
  }
  memcpy(b->data + b->len, s, n + 1);
  b->len += n;
}

static void cc_buf_add_num(CCBuf *b, const char *prefix, size_t n) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%s%zu", prefix, n);
  cc_buf_add(b, tmp);
}

static int cc_cap_index(const CCEnv *caps, const char *name) {
  if (!name)
    return -1;
  for (size_t i = 0; i < caps->count; i++)
    if (strcmp(caps->names[i], name) == 0)
      return (int)i;
  return -1;
}

static ASTNode *cc_env_field_access(int idx, SourceLocation loc) {
  char field[32];
  snprintf(field, sizeof(field), "__cap%d", idx);
  ASTNode *env_id = ast_create_identifier("__env", loc);
  return env_id ? ast_create_member_access(env_id, field, loc) : NULL;
}

/* Rewrite captured-variable references inside a lifted closure body into direct
 * accesses to the heap environment (`__env.__capN`), so reads and writes go
 * through the environment and mutations persist across calls. A captured name is
 * never re-declared inside the lambda body (else it would not be a capture), so
 * every matching reference is the capture. Nested lambdas are not descended into
 * (their captures are handled when they are themselves lifted). Replaced payload
 * nodes are left unfreed - a small, bounded compile-time allocation. */
static void cc_rewrite_captures(ASTNode *node, const CCEnv *caps) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION)
    return;

  if (node->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)node->data;
    int idx = id ? cc_cap_index(caps, id->name) : -1;
    if (idx >= 0) {
      char field[32];
      snprintf(field, sizeof(field), "__cap%d", idx);
      ASTNode *env_id = ast_create_identifier("__env", node->location);
      MemberAccess *ma = env_id ? malloc(sizeof(MemberAccess)) : NULL;
      if (ma) {
        ma->object = env_id;
        ma->member = (char *)string_intern(field);
        node->type = AST_MEMBER_ACCESS;
        node->data = ma;
        node->resolved_type = NULL;
        ast_add_child(node, env_id);
      }
    }
    return;
  }

  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *c = (CallExpression *)node->data;
    int idx = (c && !c->object) ? cc_cap_index(caps, c->function_name) : -1;
    if (idx >= 0) {
      ASTNode *member = cc_env_field_access(idx, node->location);
      FuncPtrCall *fp = member ? malloc(sizeof(FuncPtrCall)) : NULL;
      if (fp) {
        fp->function = member;
        fp->arguments = c->arguments;
        fp->argument_count = c->argument_count;
        fp->effect_signature = NULL;
        node->child_count = 0;
        node->type = AST_FUNC_PTR_CALL;
        node->data = fp;
        node->resolved_type = NULL;
        ast_add_child(node, member);
        for (size_t i = 0; i < fp->argument_count; i++)
          if (fp->arguments[i])
            ast_add_child(node, fp->arguments[i]);
      }
    }
  } else if (node->type == AST_ASSIGNMENT) {
    Assignment *a = (Assignment *)node->data;
    if (a && !a->target && a->variable_name) {
      int idx = cc_cap_index(caps, a->variable_name);
      if (idx >= 0) {
        ASTNode *member = cc_env_field_access(idx, node->location);
        if (member) {
          a->target = member;
          a->variable_name = NULL; /* force the field-assignment path */
          ast_add_child(node, member);
        }
      }
    }
  }

  for (size_t i = 0; i < node->child_count; i++)
    cc_rewrite_captures(node->children[i], caps);
}

static int g_cc_counter;

/* Lift a capturing lambda: synthesize an environment struct, a lifted function
 * (env pointer + user params; captures copied from the env at entry), and a
 * constructor that allocates and populates the env. The lambda value becomes a
 * call to the constructor. Built as source text and parsed, then the original
 * body is grafted onto the lifted function. */
static void cc_emit_capturing(ASTNode *lambda, FunctionDeclaration *fd,
                              CCEnv *caps, ASTNode *program, Program *prog,
                              ErrorReporter *reporter, int *had_error) {
  for (size_t i = 0; i < caps->count; i++) {
    if (!caps->types[i]) {
      char message[256];
      snprintf(message, sizeof(message),
               "cannot capture '%s' in a closure: its type is inferred; add an "
               "explicit type annotation to the variable",
               caps->names[i]);
      if (reporter)
        error_reporter_add_error(reporter, ERROR_SEMANTIC, lambda->location,
                                 message);
      *had_error = 1;
      return;
    }
  }

  int id = g_cc_counter++;
  char env_name[32], lam_name[32], make_name[32], idx[24];
  snprintf(env_name, sizeof(env_name), "__ClosEnv_%d", id);
  snprintf(lam_name, sizeof(lam_name), "__lam_%d", id);
  snprintf(make_name, sizeof(make_name), "__make_lam_%d", id);
  const char *ret = fd->return_type ? fd->return_type : "void";

  CCBuf src = {0};
  /* struct __ClosEnv_N { __code: fn(__ClosEnv_N*, userparams)->R; cap: T; ... } */
  cc_buf_add(&src, "struct ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, " {\n  __code: fn(");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < fd->parameter_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add(&src, fd->parameter_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ";\n");
  for (size_t i = 0; i < caps->count; i++) {
    cc_buf_add(&src, "  ");
    cc_buf_add_num(&src, "__cap", i);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, caps->types[i]);
    cc_buf_add(&src, ";\n");
  }
  cc_buf_add(&src, "}\n");

  /* fn __lam_N(__env: __ClosEnv_N*, userparams) -> R { var cap: T = __env.__capK; ... } */
  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, lam_name);
  cc_buf_add(&src, "(__env: ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < fd->parameter_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add(&src, fd->parameter_names[i]);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, fd->parameter_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  /* Empty body: the original statements are grafted in, with captured-variable
   * references rewritten to `__env.__capN` accesses (see cc_rewrite_captures). */
  cc_buf_add(&src, " {\n}\n");

  /* fn __make_lam_N(cap: T, ...) -> __ClosEnv_N* { var __e = new ...; ... return __e; } */
  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, make_name);
  cc_buf_add(&src, "(");
  for (size_t i = 0; i < caps->count; i++) {
    cc_buf_add_num(&src, "__arg", i);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, caps->types[i]);
    if (i + 1 < caps->count)
      cc_buf_add(&src, ", ");
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "* {\n  var __e: ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "* = new ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, ";\n  __e.__code = &");
  cc_buf_add(&src, lam_name);
  cc_buf_add(&src, ";\n");
  for (size_t i = 0; i < caps->count; i++) {
    cc_buf_add(&src, "  __e.");
    cc_buf_add_num(&src, "__cap", i);
    cc_buf_add(&src, " = ");
    cc_buf_add_num(&src, "__arg", i);
    cc_buf_add(&src, ";\n");
  }
  cc_buf_add(&src, "  return __e;\n}\n");

  if (!src.data) {
    *had_error = 1;
    return;
  }

  Lexer *lx = lexer_create(src.data);
  Parser *ps = lx ? parser_create(lx) : NULL;
  ASTNode *sub = ps ? parser_parse_program(ps) : NULL;
  Program *subp = (sub && sub->type == AST_PROGRAM) ? (Program *)sub->data : NULL;
  if (!subp || subp->declaration_count < 3) {
    if (reporter)
      error_reporter_add_error(reporter, ERROR_SEMANTIC, lambda->location,
                               "internal: failed to synthesize closure");
    *had_error = 1;
    free(src.data);
    return;
  }

  /* Graft the lambda's original body statements onto the lifted function (the
   * second synthesized declaration), after the injected capture locals. */
  ASTNode *lifted = subp->declarations[1];
  ASTNode *body = fd->body;
  fd->body = NULL;
  if (body) {
    for (size_t i = 0; i < lambda->child_count; i++) {
      if (lambda->children[i] == body) {
        for (size_t j = i + 1; j < lambda->child_count; j++)
          lambda->children[j - 1] = lambda->children[j];
        lambda->child_count--;
        break;
      }
    }
  }
  if (body && body->type == AST_PROGRAM && lifted &&
      lifted->type == AST_FUNCTION_DECLARATION) {
    FunctionDeclaration *lf = (FunctionDeclaration *)lifted->data;
    Program *lbody = lf->body ? (Program *)lf->body->data : NULL;
    Program *obody = (Program *)body->data;
    if (lbody && obody && obody->declaration_count > 0) {
      ASTNode **merged =
          realloc(lbody->declarations,
                  (lbody->declaration_count + obody->declaration_count) *
                      sizeof(ASTNode *));
      if (merged) {
        lbody->declarations = merged;
        for (size_t i = 0; i < obody->declaration_count; i++) {
          ASTNode *stmt = obody->declarations[i];
          lbody->declarations[lbody->declaration_count++] = stmt;
          ast_add_child(lf->body, stmt);
        }
        /* Detach the moved statements from the old body so destroying it does
         * not free them. */
        obody->declaration_count = 0;
        body->child_count = 0;
        /* Rewrite captured references in the grafted body to env accesses so
         * mutations persist across calls. */
        for (size_t i = 0; i < lbody->declaration_count; i++)
          cc_rewrite_captures(lbody->declarations[i], caps);
      }
    }
  }
  if (body)
    ast_destroy_node(body);

  /* Move the three synthesized declarations into the program. */
  for (size_t i = 0; i < subp->declaration_count; i++) {
    ASTNode *d = subp->declarations[i];
    ASTNode **grown =
        realloc(prog->declarations,
                (prog->declaration_count + 1) * sizeof(ASTNode *));
    if (!grown) {
      *had_error = 1;
      break;
    }
    prog->declarations = grown;
    prog->declarations[prog->declaration_count++] = d;
    ast_add_child(program, d);
  }
  /* The sub-program shell still references the moved nodes via its own arrays;
   * leave it unfreed (a small, bounded compile-time allocation) rather than
   * risk double-freeing the transferred declarations. */

  /* Record the closure on the lambda node: the value is produced by calling the
   * constructor, passing the current value of each captured variable. */
  fd->name = (char *)string_intern(make_name);
  fd->env_struct_name = (char *)string_intern(env_name);
  fd->captured_count = caps->count;
  fd->captured_names = malloc(caps->count * sizeof(char *));
  fd->captured_types = malloc(caps->count * sizeof(char *));
  for (size_t i = 0; i < caps->count; i++) {
    fd->captured_names[i] = (char *)string_intern(caps->names[i]);
    fd->captured_types[i] = (char *)string_intern(caps->types[i]);
  }

  free(src.data);
}

static void cc_lift_lambda(ASTNode *lambda, ASTNode *program, Program *prog,
                           const CCEnv *enclosing, ErrorReporter *reporter,
                           int *had_error) {
  FunctionDeclaration *fd = (FunctionDeclaration *)lambda->data;
  if (!fd || fd->name)
    return; // already lifted

  CCEnv bound = {0};
  cc_collect_fn_env(fd, &bound);
  CCEnv caps = {0};
  cc_scan_captures(fd->body, enclosing, &bound, &caps);
  cc_env_free(&bound);

  if (caps.count > 0) {
    cc_emit_capturing(lambda, fd, &caps, program, prog, reporter, had_error);
    cc_env_free(&caps);
    return;
  }
  cc_env_free(&caps);

  /* Non-capturing: lift the body to a top-level function and let the lambda
   * value be its address (a thin function pointer). */
  char name[32];
  snprintf(name, sizeof(name), "__lam_%d", g_cc_counter++);

  ASTNode *body = fd->body;
  fd->body = NULL;
  if (body) {
    for (size_t i = 0; i < lambda->child_count; i++) {
      if (lambda->children[i] == body) {
        for (size_t j = i + 1; j < lambda->child_count; j++)
          lambda->children[j - 1] = lambda->children[j];
        lambda->child_count--;
        break;
      }
    }
  }

  ASTNode *fn = ast_create_function_declaration(
      name, fd->parameter_names, fd->parameter_types, fd->parameter_count,
      fd->return_type, body, lambda->location);
  if (!fn) {
    *had_error = 1;
    if (body)
      ast_destroy_node(body);
    return;
  }

  ASTNode **grown =
      realloc(prog->declarations,
              (prog->declaration_count + 1) * sizeof(ASTNode *));
  if (!grown) {
    *had_error = 1;
    ast_destroy_node(fn);
    return;
  }
  prog->declarations = grown;
  prog->declarations[prog->declaration_count++] = fn;
  ast_add_child(program, fn);

  fd->name = (char *)string_intern(name);
}

static void cc_walk(ASTNode *node, const CCEnv *enclosing, ASTNode *program,
                    Program *prog, ErrorReporter *reporter, int *had_error) {
  if (!node)
    return;
  if (node->type == AST_LAMBDA_EXPRESSION) {
    cc_lift_lambda(node, program, prog, enclosing, reporter, had_error);
    return; // the original in-place body is moved out; do not descend
  }
  for (size_t i = 0; i < node->child_count; i++)
    cc_walk(node->children[i], enclosing, program, prog, reporter, had_error);
}

static void cc_process_fn(ASTNode *fnnode, ASTNode *program, Program *prog,
                          ErrorReporter *reporter, int *had_error) {
  FunctionDeclaration *fd = (FunctionDeclaration *)fnnode->data;
  if (!fd || !fd->body)
    return;
  CCEnv locals = {0};
  cc_collect_fn_env(fd, &locals);
  cc_walk(fd->body, &locals, program, prog, reporter, had_error);
  cc_env_free(&locals);
}

int closure_convert_program(ASTNode *program, ErrorReporter *reporter) {
  if (!program || program->type != AST_PROGRAM)
    return 1;
  Program *prog = (Program *)program->data;
  if (!prog)
    return 1;

  g_cc_counter = 0;
  int had_error = 0;

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl)
      continue;
    if (decl->type == AST_FUNCTION_DECLARATION ||
        decl->type == AST_METHOD_DECLARATION) {
      cc_process_fn(decl, program, prog, reporter, &had_error);
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      for (size_t m = 0; sd && m < sd->method_count; m++)
        cc_process_fn(sd->methods[m], program, prog, reporter, &had_error);
    } else if (decl->type == AST_IMPL_DECLARATION) {
      ImplDeclaration *id = (ImplDeclaration *)decl->data;
      for (size_t m = 0; id && m < id->method_count; m++)
        cc_process_fn(id->methods[m], program, prog, reporter, &had_error);
    } else if (decl->type == AST_VAR_DECLARATION) {
      VarDeclaration *vd = (VarDeclaration *)decl->data;
      if (vd && vd->initializer) {
        CCEnv empty = {0};
        cc_walk(vd->initializer, &empty, program, prog, reporter, &had_error);
      }
    }
  }

  return had_error ? 0 : 1;
}

/* ---- Closure adaptation ---------------------------------------------------
 * A thin function value (`&func`, or a non-capturing lambda, which is itself a
 * thin `&__lam_N` once lifted) cannot be assigned where a closure (`Fn(...)->R`)
 * is expected: the closure calling convention passes a hidden environment
 * argument the thin value does not carry. This pass makes that boundary
 * transparent: wherever a thin source flows into an `Fn(...)` -spelled
 * destination (a var declaration, a return statement, or a call argument to a
 * plain top-level function), it is wrapped in a small generated ADAPTER - a
 * one-field heap environment holding the thin function pointer, plus a thunk
 * that calls through it - so the thin value becomes a real closure value.
 * Adapters are deduplicated by signature (one per distinct `(params)->ret`). */

static void cc_detach_child(ASTNode *parent, ASTNode *child) {
  if (!parent || !child)
    return;
  for (size_t i = 0; i < parent->child_count; i++) {
    if (parent->children[i] == child) {
      for (size_t j = i + 1; j < parent->child_count; j++)
        parent->children[j - 1] = parent->children[j];
      parent->child_count--;
      return;
    }
  }
}

typedef struct {
  char **sigs;
  char **ctor_names;
  size_t count;
  size_t cap;
} AdaptCache;

static char *adapt_sig_key(char **param_types, size_t param_count,
                           const char *return_type) {
  CCBuf b = {0};
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&b, ",");
    cc_buf_add(&b, param_types[i]);
  }
  cc_buf_add(&b, "->");
  cc_buf_add(&b, return_type ? return_type : "void");
  return b.data;
}

static const char *adapt_cache_find(const AdaptCache *c, const char *sig) {
  for (size_t i = 0; i < c->count; i++)
    if (strcmp(c->sigs[i], sig) == 0)
      return c->ctor_names[i];
  return NULL;
}

static void adapt_cache_add(AdaptCache *c, const char *sig,
                            const char *ctor_name) {
  if (c->count == c->cap) {
    size_t ncap = c->cap ? c->cap * 2 : 8;
    char **gs = realloc(c->sigs, ncap * sizeof(char *));
    char **gc = realloc(c->ctor_names, ncap * sizeof(char *));
    if (!gs || !gc) {
      free(gs);
      free(gc);
      return;
    }
    c->sigs = gs;
    c->ctor_names = gc;
    c->cap = ncap;
  }
  c->sigs[c->count] = strdup(sig);
  c->ctor_names[c->count] = strdup(ctor_name);
  c->count++;
}

static void adapt_cache_free(AdaptCache *c) {
  for (size_t i = 0; i < c->count; i++) {
    free(c->sigs[i]);
    free(c->ctor_names[i]);
  }
  free(c->sigs);
  free(c->ctor_names);
}

static int g_adapt_counter;

/* Name index over the top_decls snapshot (decl slot+1; 0 = empty). Duplicate
 * names keep every entry: linear probing places same-key entries along one
 * probe path in insertion order, so a lookup that continues to the first
 * empty bucket sees them in snapshot order, exactly what the old linear
 * scans (first name match / first name+arity match) relied on. Consulted per
 * call expression, so a linear scan is O(calls x functions). */
typedef struct {
  size_t *buckets;
  size_t bucket_count;
} AdaptFnIndex;

static AdaptFnIndex g_adapt_fn_index;

static void adapt_fn_index_build(ASTNode **top_decls, size_t top_count) {
  size_t nb = 64;
  while (nb < top_count * 2) {
    nb *= 2;
  }
  g_adapt_fn_index.buckets = calloc(nb, sizeof(size_t));
  g_adapt_fn_index.bucket_count = g_adapt_fn_index.buckets ? nb : 0;
  if (!g_adapt_fn_index.bucket_count) {
    return;
  }
  for (size_t i = 0; i < top_count; i++) {
    FunctionDeclaration *fd = (FunctionDeclaration *)top_decls[i]->data;
    if (!fd || !fd->name) {
      continue;
    }
    size_t b = mettle_fnv1a_hash(fd->name) & (nb - 1);
    while (g_adapt_fn_index.buckets[b]) {
      b = (b + 1) & (nb - 1);
    }
    g_adapt_fn_index.buckets[b] = i + 1;
  }
}

static void adapt_fn_index_free(void) {
  free(g_adapt_fn_index.buckets);
  g_adapt_fn_index.buckets = NULL;
  g_adapt_fn_index.bucket_count = 0;
}

/* First snapshot entry whose name matches; want_arity < 0 matches any arity,
 * otherwise the parameter count must equal want_arity. */
static FunctionDeclaration *adapt_fn_index_find(ASTNode **top_decls,
                                                size_t top_count,
                                                const char *name,
                                                long long want_arity) {
  if (!g_adapt_fn_index.bucket_count) {
    /* Index allocation failed: fall back to the linear scan. */
    for (size_t i = 0; i < top_count; i++) {
      FunctionDeclaration *fd = (FunctionDeclaration *)top_decls[i]->data;
      if (fd && fd->name && strcmp(fd->name, name) == 0 &&
          (want_arity < 0 || (long long)fd->parameter_count == want_arity)) {
        return fd;
      }
    }
    return NULL;
  }
  size_t nb = g_adapt_fn_index.bucket_count;
  size_t b = mettle_fnv1a_hash(name) & (nb - 1);
  while (g_adapt_fn_index.buckets[b]) {
    FunctionDeclaration *fd =
        (FunctionDeclaration *)top_decls[g_adapt_fn_index.buckets[b] - 1]->data;
    if (fd && fd->name && strcmp(fd->name, name) == 0 &&
        (want_arity < 0 || (long long)fd->parameter_count == want_arity)) {
      return fd;
    }
    b = (b + 1) & (nb - 1);
  }
  return NULL;
}

/* Synthesizes struct __Adapt_K { __code: fn(__Adapt_K*, P...)->R; __real:
 * fn(P...)->R; }, fn __athunk_K(__env: __Adapt_K*, p0: P0, ...) -> R { return
 * __env.__real(p0, ...); }, and fn __amake_K(__real: fn(P...)->R) -> __Adapt_K*
 * { ... }. Returns the interned constructor name, or NULL on failure. */
static const char *adapt_emit_adapter(char **param_types, size_t param_count,
                                      const char *return_type,
                                      ASTNode *program, Program *prog,
                                      int *had_error) {
  int id = g_adapt_counter++;
  char struct_name[32], thunk_name[32], make_name[32];
  snprintf(struct_name, sizeof(struct_name), "__Adapt_%d", id);
  snprintf(thunk_name, sizeof(thunk_name), "__athunk_%d", id);
  snprintf(make_name, sizeof(make_name), "__amake_%d", id);
  const char *ret = return_type ? return_type : "void";

  CCBuf src = {0};
  cc_buf_add(&src, "struct ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, " {\n  __code: fn(");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < param_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ";\n  __real: fn(");
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&src, ", ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ";\n}\n");

  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, thunk_name);
  cc_buf_add(&src, "(__env: ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < param_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add_num(&src, "p", i);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, " {\n  ");
  cc_buf_add(&src, strcmp(ret, "void") == 0 ? "" : "return ");
  cc_buf_add(&src, "__env.__real(");
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&src, ", ");
    cc_buf_add_num(&src, "p", i);
  }
  cc_buf_add(&src, ");\n}\n");

  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, make_name);
  cc_buf_add(&src, "(__real: fn(");
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&src, ", ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "* {\n  var __e: ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "* = new ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, ";\n  __e.__code = &");
  cc_buf_add(&src, thunk_name);
  cc_buf_add(&src, ";\n  __e.__real = __real;\n  return __e;\n}\n");

  if (!src.data) {
    *had_error = 1;
    return NULL;
  }

  Lexer *lx = lexer_create(src.data);
  Parser *ps = lx ? parser_create(lx) : NULL;
  ASTNode *sub = ps ? parser_parse_program(ps) : NULL;
  Program *subp = (sub && sub->type == AST_PROGRAM) ? (Program *)sub->data : NULL;
  free(src.data);
  if (!subp || subp->declaration_count < 3) {
    *had_error = 1;
    return NULL;
  }

  for (size_t i = 0; i < subp->declaration_count; i++) {
    ASTNode *d = subp->declarations[i];
    ASTNode **grown =
        realloc(prog->declarations,
                (prog->declaration_count + 1) * sizeof(ASTNode *));
    if (!grown) {
      *had_error = 1;
      break;
    }
    prog->declarations = grown;
    prog->declarations[prog->declaration_count++] = d;
    ast_add_child(program, d);
  }
  /* The sub-program shell is intentionally left unfreed, as in
   * cc_emit_capturing: a small, bounded compile-time allocation. */

  return string_intern(make_name);
}

static const char *adapt_get_or_create(char **param_types, size_t param_count,
                                       const char *return_type,
                                       AdaptCache *cache, ASTNode *program,
                                       Program *prog, int *had_error) {
  char *sig = adapt_sig_key(param_types, param_count, return_type);
  if (!sig) {
    *had_error = 1;
    return NULL;
  }
  const char *existing = adapt_cache_find(cache, sig);
  if (existing) {
    free(sig);
    return existing;
  }
  const char *ctor =
      adapt_emit_adapter(param_types, param_count, return_type, program, prog,
                        had_error);
  if (ctor) {
    adapt_cache_add(cache, sig, ctor);
  }
  free(sig);
  return ctor;
}

static int adapt_type_is_closure_string(const char *s) {
  return s && strlen(s) >= 4 && strncmp(s, "Fn(", 3) == 0;
}

/* Recognizes a "thin" source expression eligible for adaptation: `&plainFunc`
 * (a top-level function found by name) or a non-capturing lambda (already
 * lifted by closure_convert_program). On success, fills the borrowed
 * (not owned by the caller) param_types/param_count/return_type describing the
 * source's signature. */
static int adapt_thin_signature(ASTNode *expr, ASTNode **top_decls,
                                size_t top_count, char ***out_param_types,
                                size_t *out_param_count,
                                char **out_return_type) {
  if (!expr)
    return 0;
  if (expr->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *un = (UnaryExpression *)expr->data;
    if (!un || !un->operator || strcmp(un->operator, "&") != 0 || !un->operand ||
        un->operand->type != AST_IDENTIFIER) {
      return 0;
    }
    Identifier *id = (Identifier *)un->operand->data;
    if (!id || !id->name)
      return 0;
    FunctionDeclaration *fd =
        adapt_fn_index_find(top_decls, top_count, id->name, -1);
    if (fd) {
      *out_param_types = fd->parameter_types;
      *out_param_count = fd->parameter_count;
      *out_return_type = fd->return_type;
      return 1;
    }
    return 0;
  }
  if (expr->type == AST_LAMBDA_EXPRESSION) {
    FunctionDeclaration *fd = (FunctionDeclaration *)expr->data;
    if (!fd || fd->captured_count > 0)
      return 0;
    *out_param_types = fd->parameter_types;
    *out_param_count = fd->parameter_count;
    *out_return_type = fd->return_type;
    return 1;
  }
  return 0;
}

/* If `dest_type_str` is a closure boundary (`Fn(...)`) and `*slot` is a thin,
 * adaptable source, wraps `*slot` in a generated adapter call in place. */
static void adapt_wrap_if_needed(ASTNode **slot, ASTNode *owner,
                                 const char *dest_type_str,
                                 ASTNode **top_decls, size_t top_count,
                                 AdaptCache *cache, ASTNode *program,
                                 Program *prog, int *had_error) {
  if (!slot || !*slot || !owner || !adapt_type_is_closure_string(dest_type_str))
    return;
  char **param_types = NULL;
  size_t param_count = 0;
  char *return_type = NULL;
  if (!adapt_thin_signature(*slot, top_decls, top_count, &param_types,
                            &param_count, &return_type)) {
    return;
  }
  const char *ctor = adapt_get_or_create(param_types, param_count, return_type,
                                        cache, program, prog, had_error);
  if (!ctor) {
    *had_error = 1;
    return;
  }
  ASTNode *inner = *slot;
  ASTNode *wrapper = ast_create_closure_adapt(inner, ctor, param_types,
                                              param_count, return_type,
                                              inner->location);
  if (!wrapper) {
    *had_error = 1;
    return;
  }
  cc_detach_child(owner, inner);
  ast_add_child(owner, wrapper);
  *slot = wrapper;
}

typedef struct {
  const char *name;
  const char *type_name;
} AdaptLocalType;

typedef struct {
  AdaptLocalType *items;
  size_t count;
  size_t capacity;
} AdaptLocals;

static void adapt_locals_collect(ASTNode *node, AdaptLocals *locals) {
  if (!node) {
    return;
  }
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->name && vd->type_name) {
      if (locals->count == locals->capacity) {
        size_t grown = locals->capacity ? locals->capacity * 2 : 8;
        AdaptLocalType *items = (AdaptLocalType *)realloc(
            locals->items, grown * sizeof(AdaptLocalType));
        if (!items) {
          return;
        }
        locals->items = items;
        locals->capacity = grown;
      }
      locals->items[locals->count].name = vd->name;
      locals->items[locals->count].type_name = vd->type_name;
      locals->count++;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    adapt_locals_collect(node->children[i], locals);
  }
}

static const char *adapt_local_type(const AdaptLocals *locals,
                                    const char *name) {
  if (!locals || !name) {
    return NULL;
  }
  for (size_t i = locals->count; i-- > 0;) {
    if (locals->items[i].name && strcmp(locals->items[i].name, name) == 0) {
      return locals->items[i].type_name;
    }
  }
  return NULL;
}

static size_t mono_base_type_length(const char *type_str);

static const char *adapt_field_type(Program *prog, const char *struct_type,
                                    const char *field) {
  size_t base_length;
  if (!prog || !struct_type || !field) {
    return NULL;
  }
  base_length = mono_base_type_length(struct_type);
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    StructDeclaration *sd = NULL;
    if (!decl || decl->type != AST_STRUCT_DECLARATION) {
      continue;
    }
    sd = (StructDeclaration *)decl->data;
    if (!sd || !sd->name || strlen(sd->name) != base_length ||
        strncmp(sd->name, struct_type, base_length) != 0) {
      continue;
    }
    for (size_t f = 0; f < sd->field_count; f++) {
      if (sd->field_names[f] && strcmp(sd->field_names[f], field) == 0) {
        return sd->field_types[f];
      }
    }
    return NULL;
  }
  return NULL;
}

static const char *adapt_expression_type(ASTNode *node,
                                         const AdaptLocals *locals,
                                         Program *prog) {
  if (!node) {
    return NULL;
  }
  switch (node->type) {
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)node->data;
    return id ? adapt_local_type(locals, id->name) : NULL;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
    return index ? adapt_expression_type(index->array, locals, prog) : NULL;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)node->data;
    if (unary && unary->operator&& strcmp(unary->operator, "*") == 0) {
      return adapt_expression_type(unary->operand, locals, prog);
    }
    return NULL;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)node->data;
    if (!member || !member->member) {
      return NULL;
    }
    return adapt_field_type(
        prog, adapt_expression_type(member->object, locals, prog),
        member->member);
  }
  default:
    return NULL;
  }
}

static const char *adapt_assignment_target_type(const Assignment *assign,
                                                const AdaptLocals *locals,
                                                Program *prog) {
  if (!assign) {
    return NULL;
  }
  if (!assign->target) {
    return adapt_local_type(locals, assign->variable_name);
  }
  return adapt_expression_type(assign->target, locals, prog);
}

static void adapt_walk(ASTNode *node, const char *current_return_type,
                       ASTNode **top_decls, size_t top_count,
                       AdaptCache *cache, ASTNode *program, Program *prog,
                       int *had_error, const AdaptLocals *locals) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION ||
      node->type == AST_CLOSURE_ADAPT_EXPRESSION)
    return;

  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->initializer) {
      adapt_wrap_if_needed(&vd->initializer, node, vd->type_name, top_decls,
                           top_count, cache, program, prog, had_error);
    }
  } else if (node->type == AST_RETURN_STATEMENT) {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value) {
      adapt_wrap_if_needed(&rs->value, node, current_return_type, top_decls,
                           top_count, cache, program, prog, had_error);
      if (rs->values && rs->value_count > 0) {
        rs->values[0] = rs->value;
      }
    }
  } else if (node->type == AST_ASSIGNMENT) {
    Assignment *assign = (Assignment *)node->data;
    if (assign && assign->value) {
      adapt_wrap_if_needed(
          &assign->value, node,
          adapt_assignment_target_type(assign, locals, prog), top_decls,
          top_count, cache, program, prog, had_error);
    }
  } else if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call && !call->object && call->function_name) {
      FunctionDeclaration *fd =
          adapt_fn_index_find(top_decls, top_count, call->function_name,
                              (long long)call->argument_count);
      if (fd) {
        for (size_t a = 0; a < call->argument_count; a++) {
          adapt_wrap_if_needed(&call->arguments[a], node,
                               fd->parameter_types[a], top_decls, top_count,
                               cache, program, prog, had_error);
        }
      }
    }
  }

  for (size_t i = 0; i < node->child_count; i++)
    adapt_walk(node->children[i], current_return_type, top_decls, top_count,
              cache, program, prog, had_error, locals);
}

static void adapt_process_fn(ASTNode *fnnode, ASTNode **top_decls,
                             size_t top_count, AdaptCache *cache,
                             ASTNode *program, Program *prog, int *had_error) {
  FunctionDeclaration *fd = (FunctionDeclaration *)fnnode->data;
  AdaptLocals locals = {0};
  if (!fd || !fd->body)
    return;
  adapt_locals_collect(fd->body, &locals);
  adapt_walk(fd->body, fd->return_type, top_decls, top_count, cache, program,
            prog, had_error, &locals);
  free(locals.items);
}

int closure_adapt_program(ASTNode *program, ErrorReporter *reporter) {
  (void)reporter; /* adaptation failures are internal allocation errors only;
                    * signature mismatches surface later as ordinary type
                    * errors once the wrapped node is type-checked. */
  if (!program || program->type != AST_PROGRAM)
    return 1;
  Program *prog = (Program *)program->data;
  if (!prog)
    return 1;

  g_adapt_counter = 0;
  int had_error = 0;

  /* Snapshot of callable top-level function signatures (by name), taken before
   * any adapters are synthesized: adapters are never themselves user-callable
   * boundary targets, so they do not need to be discoverable here. */
  ASTNode **top_decls = malloc(prog->declaration_count * sizeof(ASTNode *));
  size_t top_count = 0;
  if (!top_decls && prog->declaration_count > 0) {
    return 0;
  }
  for (size_t i = 0; i < prog->declaration_count; i++) {
    if (prog->declarations[i] &&
        prog->declarations[i]->type == AST_FUNCTION_DECLARATION) {
      top_decls[top_count++] = prog->declarations[i];
    }
  }

  AdaptCache cache = {0};
  adapt_fn_index_build(top_decls, top_count);

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl)
      continue;
    if (decl->type == AST_FUNCTION_DECLARATION ||
        decl->type == AST_METHOD_DECLARATION) {
      adapt_process_fn(decl, top_decls, top_count, &cache, program, prog,
                       &had_error);
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      for (size_t m = 0; sd && m < sd->method_count; m++)
        adapt_process_fn(sd->methods[m], top_decls, top_count, &cache,
                         program, prog, &had_error);
    } else if (decl->type == AST_IMPL_DECLARATION) {
      ImplDeclaration *id = (ImplDeclaration *)decl->data;
      for (size_t m = 0; id && m < id->method_count; m++)
        adapt_process_fn(id->methods[m], top_decls, top_count, &cache,
                         program, prog, &had_error);
    } else if (decl->type == AST_VAR_DECLARATION) {
      VarDeclaration *vd = (VarDeclaration *)decl->data;
      if (vd && vd->initializer) {
        adapt_wrap_if_needed(&vd->initializer, decl, vd->type_name, top_decls,
                             top_count, &cache, program, prog, &had_error);
      }
    }
  }

  adapt_fn_index_free();
  free(top_decls);
  adapt_cache_free(&cache);
  return had_error ? 0 : 1;
}

/* The base of a field type, with any `*` and `[N]` suffix dropped: `Box__int32`
 * for all of `Box__int32`, `Box__int32*` and `Box__int32[4]`. */
static size_t mono_base_type_length(const char *type_str) {
  size_t length = strlen(type_str);
  const char *bracket = strchr(type_str, '[');

  if (bracket) {
    length = (size_t)(bracket - type_str);
  }
  while (length > 0 && type_str[length - 1] == '*') {
    length--;
  }
  return length;
}

static int mono_name_is(const char *name, const char *type_str,
                        size_t base_length) {
  return name && strlen(name) == base_length &&
         strncmp(name, type_str, base_length) == 0;
}

static size_t mono_find_struct_index(Program *prog, const char *type_str,
                                     size_t base_length) {
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    StructDeclaration *sd = NULL;
    if (!decl || decl->type != AST_STRUCT_DECLARATION) {
      continue;
    }
    sd = (StructDeclaration *)decl->data;
    if (sd && mono_name_is(sd->name, type_str, base_length)) {
      return i;
    }
  }
  return (size_t)-1;
}

static int mono_is_instantiated_struct(MonoContext *ctx, const char *type_str,
                                       size_t base_length) {
  for (size_t i = 0; i < ctx->instance_count; i++) {
    if (ctx->instances[i].emitted &&
        mono_name_is(ctx->instances[i].mangled_name, type_str, base_length)) {
      GenericDef *def = find_generic_def(ctx, ctx->instances[i].generic_name);
      return def && def->is_struct;
    }
  }
  return 0;
}

static void mono_move_declaration(Program *prog, size_t from, size_t to) {
  ASTNode *node = prog->declarations[from];

  if (from <= to) {
    return;
  }
  memmove(&prog->declarations[to + 1], &prog->declarations[to],
          (from - to) * sizeof(ASTNode *));
  prog->declarations[to] = node;
}

/* Instantiations are appended as they are discovered, which puts them after the
 * code that asked for them. That is invisible for a local, whose type may name
 * a struct declared later, but Mettle requires a struct field's type to be
 * declared first, so `struct Holder { b: Box<int32>; }` named a type that came
 * later in the program and failed with "Unknown type 'Box__int32'".
 *
 * Move each one to just before the first field that names it. Everything it
 * depends on -- its type arguments, and whatever its template's fields name --
 * is already declared before that field, since the field could name them too,
 * so the new position is after its own dependencies and before its use. One
 * forward sweep settles it: a declaration is only ever moved to the index being
 * examined, which leaves the prefix behind that index untouched, and the
 * moved-to index is re-examined so that a struct pulled forward can pull its
 * own dependencies forward ahead of itself.
 *
 * That settles each instantiation with at most one move, so the move budget is
 * their count. Two structs whose fields name each other exhaust it instead of
 * chasing each other forever: no order satisfies them, which the type checker
 * then reports the way it does for two hand-written structs. */
static void mono_order_instantiations_before_use(MonoContext *ctx,
                                                 Program *prog) {
  size_t index = 0;
  size_t budget = 0;

  if (!ctx || !prog || ctx->instance_count == 0) {
    return;
  }
  budget = ctx->instance_count;

  while (index < prog->declaration_count) {
    ASTNode *decl = prog->declarations[index];
    StructDeclaration *sd = NULL;
    int moved = 0;

    if (decl && decl->type == AST_STRUCT_DECLARATION) {
      sd = (StructDeclaration *)decl->data;
    }
    for (size_t f = 0; sd && f < sd->field_count; f++) {
      const char *field_type = sd->field_types[f];
      size_t base_length = field_type ? mono_base_type_length(field_type) : 0;
      size_t declared_at = 0;

      if (base_length == 0 ||
          !mono_is_instantiated_struct(ctx, field_type, base_length)) {
        continue;
      }
      declared_at = mono_find_struct_index(prog, field_type, base_length);
      if (declared_at != (size_t)-1 && declared_at > index && budget > 0) {
        mono_move_declaration(prog, declared_at, index);
        budget--;
        moved = 1;
        break;
      }
    }

    if (!moved) {
      index++;
    }
  }
}

int monomorphize_program(ASTNode *program, ErrorReporter *reporter) {
  if (!program || program->type != AST_PROGRAM)
    return 1;

  Program *prog = (Program *)program->data;
  if (!prog)
    return 1;

  MonoContext ctx = {0};
  int success = 1;
  ctx.reporter = reporter;

  // Step 1: Collect generic definitions
  collect_generic_defs(program, &ctx);
  if (ctx.had_error) {
    success = 0;
    goto cleanup;
  }

  if (!mono_emit_impl_method_functions(&ctx, program)) {
    success = 0;
    goto cleanup;
  }

  if (ctx.def_count > 0) {
    /* Step 1.5: fill in the type arguments a call did not name, from what its
       arguments already say. It runs before collection because what it decides
       is which instantiations there are to collect. */
    ctx.module = prog;
    if (!mono_infer_type_args(&ctx, program)) {
      success = 0;
      goto cleanup;
    }

    // Step 2: Collect all instantiations from non-generic code
    collect_type_instantiations(program, &ctx);

    // Step 3: Generate monomorphized definitions, iterating until no new
    // instantiations are discovered (handles transitive generic usage).
    size_t processed = 0;
    while (processed < ctx.instance_count) {
      size_t current_count = ctx.instance_count;
      for (size_t i = processed; i < current_count; i++) {
        if (!mono_emit_instantiation(&ctx, prog, program, i, &success)) {
          goto cleanup;
        }
        if (!success) {
          goto cleanup;
        }
      }
      processed = current_count;
    }

    // Step 4: Rewrite all generic references to use mangled names
    rewrite_generic_references(program, &ctx);

    /* Field types are mangled by now, so an instantiation used as a field can
     * be recognized and moved ahead of the struct that names it. */
    mono_order_instantiations_before_use(&ctx, prog);
  }

  if (!mono_rewrite_trait_method_calls(&ctx, program)) {
    success = 0;
    goto cleanup;
  }

  /* After instantiation, so a monomorphized struct's methods are lifted too. */
  if (!mono_lift_struct_methods(&ctx, prog, program)) {
    success = 0;
    goto cleanup;
  }

  // Step 5: Remove generic (template) definitions from the program
  size_t write_idx = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    int is_generic = 0;
    int is_compile_time_trait_decl = 0;

    if (decl && decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      if (sd && sd->type_param_count > 0)
        is_generic = 1;
    } else if (decl && decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      if (fd && fd->type_param_count > 0)
        is_generic = 1;
    } else if (decl &&
               (decl->type == AST_TRAIT_DECLARATION ||
                decl->type == AST_IMPL_DECLARATION)) {
      is_compile_time_trait_decl = 1;
    }

    if (!is_generic && !is_compile_time_trait_decl) {
      prog->declarations[write_idx++] = decl;
    }
  }
  prog->declaration_count = write_idx;

cleanup:
  // Clean up context
  for (size_t i = 0; i < ctx.trait_count; i++) {
    free(ctx.traits[i].name);
  }
  free(ctx.traits);

  for (size_t i = 0; i < ctx.impl_count; i++) {
    free(ctx.impls[i].trait_name);
    free(ctx.impls[i].for_type_name);
  }
  free(ctx.impls);

  for (size_t i = 0; i < ctx.def_count; i++) {
    free(ctx.defs[i].name);
    for (size_t j = 0; j < ctx.defs[i].type_param_count; j++)
      free(ctx.defs[i].type_params[j]);
    free(ctx.defs[i].type_params);
    for (size_t j = 0; j < ctx.defs[i].type_param_count; j++)
      free(ctx.defs[i].type_param_traits[j]);
    free(ctx.defs[i].type_param_traits);
  }
  free(ctx.defs);

  for (size_t i = 0; i < ctx.instance_count; i++) {
    free(ctx.instances[i].generic_name);
    free(ctx.instances[i].mangled_name);
    for (size_t j = 0; j < ctx.instances[i].type_arg_count; j++)
      free(ctx.instances[i].type_args[j]);
    free(ctx.instances[i].type_args);
  }
  free(ctx.instances);

  return success && !ctx.had_error;
}
